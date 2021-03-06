#include "PeLdr.h"
#include "Debug.h"
#include "PEB.h"

#include <strsafe.h>

#pragma warning(disable: 4995)
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='x86' publicKeyToken='6595b64144ccf1df' language='*'\"")

static
BOOL PeLdrExecuteEP(PE_LDR_PARAM *pe)
{
	DWORD	dwOld;
	DWORD	dwEP;

	// TODO: Fix permission as per section flags
	if(!VirtualProtect((LPVOID) pe->dwMapBase, pe->pNtHeaders->OptionalHeader.SizeOfImage,
		PAGE_EXECUTE_READWRITE, &dwOld)) {
		DMSG("Failed to change mapping protection");
		return FALSE;
	}

	dwEP = pe->dwMapBase + pe->pNtHeaders->OptionalHeader.AddressOfEntryPoint;
	DMSG("Executing Entry Point: 0x%08x", dwEP);

	__asm {
		mov eax, dwEP
		call eax
		int 3
	}

	return TRUE;
}

static
BOOL PeLdrApplyRelocations(PE_LDR_PARAM *pe)
{
	UINT_PTR					iRelocOffset;
	DWORD						x;
	DWORD						dwTmp;
	PIMAGE_BASE_RELOCATION		pBaseReloc;
	PIMAGE_RELOC				pReloc;

	if(pe->dwMapBase == pe->pNtHeaders->OptionalHeader.ImageBase) {
		DMSG("Relocation not required");
		return TRUE;
	}

	if(!pe->pNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size) {
		DMSG("PE required relocation but no relocatiom information found");
		return FALSE;
	}

	iRelocOffset = pe->dwMapBase - pe->pNtHeaders->OptionalHeader.ImageBase;
	pBaseReloc = (PIMAGE_BASE_RELOCATION) 
		(pe->dwMapBase + 
		pe->pNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress);

	while(pBaseReloc->SizeOfBlock) {
		x = pe->dwMapBase + pBaseReloc->VirtualAddress;
		dwTmp = (pBaseReloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(IMAGE_RELOC);
		pReloc = (PIMAGE_RELOC) (((DWORD) pBaseReloc) + sizeof(IMAGE_BASE_RELOCATION));

		while(dwTmp--) {
			switch(pReloc->type) {
				case IMAGE_REL_BASED_DIR64:
					*((UINT_PTR*)(x + pReloc->offset)) += iRelocOffset;
					break;	
				case IMAGE_REL_BASED_HIGHLOW:
					*((DWORD*)(x + pReloc->offset)) += (DWORD) iRelocOffset;
					break;

				case IMAGE_REL_BASED_HIGH:
					*((WORD*)(x + pReloc->offset)) += HIWORD(iRelocOffset);
					break;

				case IMAGE_REL_BASED_LOW:
					*((WORD*)(x + pReloc->offset)) += LOWORD(iRelocOffset);
					break;

				case IMAGE_REL_BASED_ABSOLUTE:
					break;

				default:
					DMSG("Unknown relocation type: 0x%08x", pReloc->type);
					break;
			}

			pReloc += 1;
		}

		pBaseReloc = (PIMAGE_BASE_RELOCATION)(((DWORD) pBaseReloc) + pBaseReloc->SizeOfBlock);
	}

	return TRUE;
}

static
BOOL PeLdrProcessIAT(PE_LDR_PARAM *pe)
{
	BOOL						ret = FALSE;
	PIMAGE_IMPORT_DESCRIPTOR	pImportDesc;
	PIMAGE_THUNK_DATA			pThunkData;
	PIMAGE_THUNK_DATA			pThunkDataOrig;
	PIMAGE_IMPORT_BY_NAME		pImportByName;
	PIMAGE_EXPORT_DIRECTORY		pExportDir;
	DWORD						flError = 0;
	DWORD						dwTmp;
	BYTE						*pLibName;
	HMODULE						hMod;

	DMSG("Processing IAT");

	do {
		pImportDesc = (PIMAGE_IMPORT_DESCRIPTOR)(pe->dwMapBase +
			pe->pNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

		if(!pImportDesc) {
			DMSG("IAT not found");
			break;
		}

		while((pImportDesc->Name != 0) && (!flError)) {
			pLibName = (BYTE*) (pe->dwMapBase + pImportDesc->Name);
			DMSG("Loading Library and processing Imports: %s", (CHAR*) pLibName);

			if(pImportDesc->ForwarderChain != -1) {
				DMSG("FIXME: Cannot handle Import Forwarding currently");
				flError = 1;
				break;
			}

			hMod = LoadLibraryA((CHAR*) pLibName);
			if(!hMod) {
				DMSG("Failed to load library: %s", pLibName);
				flError = 1;
				break;
			}

			pThunkData = (PIMAGE_THUNK_DATA)(pe->dwMapBase + pImportDesc->FirstThunk);
			if(pImportDesc->Characteristics == 0)
				/* Borland compilers doesn't produce Hint Table */
				pThunkDataOrig = pThunkData;
			else
				/* Hint Table */
				pThunkDataOrig = (PIMAGE_THUNK_DATA)(pe->dwMapBase + pImportDesc->Characteristics);

			while(pThunkDataOrig->u1.AddressOfData != 0) {
				if(pThunkDataOrig->u1.Ordinal & IMAGE_ORDINAL_FLAG) {
					/* Import via. Export Ordinal */
					PIMAGE_DOS_HEADER		_dos;
					PIMAGE_NT_HEADERS		_nt;

					_dos = (PIMAGE_DOS_HEADER) hMod;
					_nt = (PIMAGE_NT_HEADERS) (((DWORD) hMod) + _dos->e_lfanew);

					pExportDir = (PIMAGE_EXPORT_DIRECTORY) 
						(((DWORD) hMod) + _nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
					dwTmp = (((DWORD) hMod) + pExportDir->AddressOfFunctions) + (((IMAGE_ORDINAL(pThunkDataOrig->u1.Ordinal) - pExportDir->Base)) * sizeof(DWORD));
					dwTmp = ((DWORD) hMod) + *((DWORD*) dwTmp);

					pThunkData->u1.Function = dwTmp;
				}
				else {
					pImportByName = (PIMAGE_IMPORT_BY_NAME)
						(pe->dwMapBase + pThunkDataOrig->u1.AddressOfData);
					pThunkData->u1.Function = (DWORD) GetProcAddress(hMod, (LPCSTR) pImportByName->Name);

					if(!pThunkData->u1.Function) {
						DMSG("Failed to resolve API: %s!%s", 
							(CHAR*)pLibName, (CHAR*)pImportByName->Name);
						flError = 1;
						break;
					}
				}

				pThunkDataOrig++;
				pThunkData++;
			}

			pImportDesc++;
		}

		if(!flError)
			ret = TRUE;

	} while(0);
	return ret;
}

static
BOOL PeLdrMapImage(PE_LDR_PARAM *pe)
{
	DWORD						dwProcessBase;
	DWORD						i;
	_PPEB						peb;
	MEMORY_BASIC_INFORMATION	mi;
	PIMAGE_SECTION_HEADER		pSectionHeader;
	BOOL						ret = FALSE;

	NTSTATUS	(NTAPI *NtUnmapViewOfSection)
					(HANDLE, LPVOID) = NULL;
	if(!pe)
		return ret;

	DMSG("Mapping PE File");

	pe->pDosHeader = (PIMAGE_DOS_HEADER) pe->dwImage;
	if(pe->pDosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
		DMSG("DOS Signature invalid");
		return ret;
	}

	pe->pNtHeaders = (PIMAGE_NT_HEADERS) 
		(PIMAGE_NT_HEADERS)(((DWORD) pe->dwImage) + pe->pDosHeader->e_lfanew);
	if(pe->pNtHeaders->Signature != IMAGE_NT_SIGNATURE) {
		DMSG("NT Signature mismatch");
		return ret;
	}

	peb = (_PPEB)__readfsdword(0x30);
	dwProcessBase = (DWORD) peb->lpImageBaseAddress;

	DMSG("Current process base: 0x%08x", dwProcessBase);

	NtUnmapViewOfSection = 
		(NTSTATUS (NTAPI *)(HANDLE, LPVOID))
			GetProcAddress(GetModuleHandle(TEXT("ntdll.dll")), "ZwUnmapViewOfSection");
	if(!NtUnmapViewOfSection)
		DMSG("Failed to resolve address of NtUnmapViewOfSection");

	do {
		DMSG("Target PE Load Base: 0x%08x Image Size: 0x%08x",
			pe->pNtHeaders->OptionalHeader.ImageBase,
			pe->pNtHeaders->OptionalHeader.SizeOfImage);

		// Find the size of our mapping
		i = dwProcessBase;
		while(VirtualQuery((LPVOID) i, &mi, sizeof(mi))) {
			if(mi.State == MEM_FREE)
				break;

			i += mi.RegionSize;
		}

		if((pe->pNtHeaders->OptionalHeader.ImageBase >= dwProcessBase) && 
			(pe->pNtHeaders->OptionalHeader.ImageBase < i)) {
			EMSG("Cannot load PE in same base address as the loader");
			// TODO: Relocate ourself so as to load Target PE in same address
		}
		else {
			pe->dwMapBase = (DWORD) VirtualAlloc((LPVOID) pe->pNtHeaders->OptionalHeader.ImageBase,
				pe->pNtHeaders->OptionalHeader.SizeOfImage + 1,
				MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

			if(!pe->dwMapBase)
				EMSG("Failed to allocate PE ImageBase: 0x%08x", 
					pe->pNtHeaders->OptionalHeader.ImageBase);
		}

		if(!pe->dwMapBase) {
			if(!pe->pNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size) {
				EMSG("Failed to map required memory address, need relocation to continue");
				break;
			}
			else {
				pe->dwMapBase = (DWORD) VirtualAlloc(NULL, 
					pe->pNtHeaders->OptionalHeader.SizeOfImage + 1,
					MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			}
		}

		if(!pe->dwMapBase) {
			EMSG("Failed to map memory for Target PE");
			break;
		}

		DMSG("Allocated memory for Target PE: 0x%08x", pe->dwMapBase);
		
		DMSG("Copying Headers");
		CopyMemory((LPVOID) pe->dwMapBase, (LPVOID) pe->dwImage,
			pe->pNtHeaders->OptionalHeader.SizeOfHeaders);

		DMSG("Copying Sections");
		pSectionHeader = IMAGE_FIRST_SECTION(pe->pNtHeaders);
		for(i = 0; i < pe->pNtHeaders->FileHeader.NumberOfSections; i++) {
			DMSG("  Copying Section: %s", (CHAR*) pSectionHeader[i].Name);

			CopyMemory(
				(LPVOID)(pe->dwMapBase + pSectionHeader[i].VirtualAddress),
				(LPVOID)(pe->dwImage + pSectionHeader[i].PointerToRawData),
				pSectionHeader[i].SizeOfRawData
			);
		}

		ret = TRUE;
	} while(0);

	return ret;
}

static 
BOOL PeLdrLoadImage(PE_LDR_PARAM *pe)
{
	HANDLE	hFile = NULL;
	DWORD	dwSize;
	DWORD	dwTmp;
	DWORD	dwOff;
	BOOL	ret = FALSE;

	if(!pe)
		goto out;

	DMSG("Reading PE File");

	hFile = CreateFile(pe->pTargetPath, GENERIC_READ, 
		FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if(hFile == INVALID_HANDLE_VALUE) {
		DMSG("Failed to open PE File");
		goto out;
	}

	pe->dwImageSizeOnDisk = dwSize = GetFileSize(hFile, NULL);
	pe->dwImage = (DWORD) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwSize + 1);
	if(!pe->dwImage) {
		DMSG("Failed to allocate memory for PE File");
		goto out;
	}

	dwTmp = dwOff = 0;
	while(dwSize) {
		ReadFile(hFile, (LPVOID)(pe->dwImage + dwOff), dwSize, &dwTmp, NULL);

		if(!dwTmp)
			break;

		dwOff += dwTmp;
		dwSize -= dwTmp;
	}

	if(dwSize) {
		DMSG("Failed to read PE File");
		goto out;
	}

	ret = TRUE;

out:
	if(hFile)
		CloseHandle(hFile);

	return ret;
}

BOOL PeLdrStart(PE_LDR_PARAM *pe)
{
	if(!PeLdrLoadImage(pe))
		return FALSE;
	if(!PeLdrMapImage(pe))
		return FALSE;
	if(!PeLdrProcessIAT(pe))
		return FALSE;
	if(!PeLdrApplyRelocations(pe))
		return FALSE;
	if(!PeLdrExecuteEP(pe))
		return FALSE;

	return TRUE;
}

BOOL PeLdrSetExecutablePath(PE_LDR_PARAM *pe, TCHAR *pExecutable)
{
	if(!pe)
		return FALSE;

	pe->pTargetPath = (TCHAR*) HeapAlloc(GetProcessHeap(), 
		HEAP_ZERO_MEMORY, (lstrlen(pExecutable) + 1) * sizeof(TCHAR));
	if(!pe->pTargetPath) {
		DMSG("Failed to allocate memory for pTargetPath");
		return FALSE;
	}

	lstrcpy(pe->pTargetPath, pExecutable);
	return TRUE;
}

VOID PeLdrInit(PE_LDR_PARAM *pe)
{
	ZeroMemory(pe, sizeof(PE_LDR_PARAM));
}