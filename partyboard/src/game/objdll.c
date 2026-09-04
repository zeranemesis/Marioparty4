#include "game/object.h"
#include "game/dvd.h"
#include "game/memory.h"

#ifndef __MWERKS__
#include <string.h>
#endif

#if defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

typedef s32 (*DLLProlog)(void);
typedef void (*DLLEpilog)(void);

#ifdef TARGET_PC
typedef void (*DLLObjectSetup)(void);
#endif

omDllData *omDLLinfoTbl[OM_DLL_MAX];

static FileListEntry *omDLLFileList;

#if defined(TARGET_PC) && defined(_WIN32)
#define OM_DLL_SNAPSHOT_MAGIC 0x4F564C53u /* "OVLS" */
#define OM_DLL_SNAPSHOT_VERSION 1u

typedef struct omDllSnapshotHeader {
    u32 magic;
    u32 version;
    uintptr_t module;
    u32 image_size;
    u32 timestamp;
    u32 section_count;
    u32 total_size;
} omDllSnapshotHeader;

typedef struct omDllSnapshotSection {
    u32 virtual_address;
    u32 size;
} omDllSnapshotSection;

static HMODULE omDLLCurrentModuleGet(void)
{
    if (omcurdll < 0 || omcurdll >= OM_DLL_MAX || omDLLinfoTbl[omcurdll] == NULL) {
        return NULL;
    }
    return omDLLinfoTbl[omcurdll]->hModule;
}

static IMAGE_NT_HEADERS *omDLLNtHeadersGet(HMODULE module)
{
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    if (module == NULL) {
        return NULL;
    }
    dos = (IMAGE_DOS_HEADER *)module;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return NULL;
    }
    nt = (IMAGE_NT_HEADERS *)((u8 *)module + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt : NULL;
}

static BOOL omDLLSectionIsSnapshotSafe(const IMAGE_SECTION_HEADER *section)
{
    static const char dataName[] = ".data";
    return memcmp(section->Name, dataName, sizeof(dataName) - 1) == 0
        && section->Misc.VirtualSize != 0
        && (section->Characteristics & IMAGE_SCN_MEM_WRITE) != 0
        && (section->Characteristics & IMAGE_SCN_MEM_DISCARDABLE) == 0;
}
#endif

void omDLLDBGOut(void)
{
	OSReport("DLL DBG OUT\n");
}

void omDLLInit(FileListEntry *ovl_list)
{
	s32 i;
	OSReport("DLL DBG OUT\n");
	for(i=0; i<OM_DLL_MAX; i++) {
		omDLLinfoTbl[i] = NULL;
	}
	omDLLFileList = ovl_list;
}

s32 omDLLStart(s16 overlay, s16 flag)
{
	s32 dllno;
	OSReport("DLLStart %d %d\n", overlay, flag);
	dllno = omDLLSearch(overlay);
	if(dllno >= 0 && !flag) {
		omDllData *dll = omDLLinfoTbl[dllno];
#ifdef TARGET_PC
		OSReport("objdll>Already Loaded %s\n", dll->name);
#else
		OSReport("objdll>Already Loaded %s(%08x %08x)\n", dll->name, dll->module, dll->bss);
		
		omDLLInfoDump(&dll->module->info);
		omDLLHeaderDump(dll->module);
		memset(dll->bss, 0, dll->module->bssSize);
		HuMemDCFlushAll();
		dll->ret = ((DLLProlog)dll->module->prolog)();
#endif
		OSReport("objdll> %s prolog end\n", dll->name);
		return dllno;
	} else {
		for(dllno=0; dllno<OM_DLL_MAX; dllno++) {
			if(omDLLinfoTbl[dllno] == NULL) {
				break;
			}
		}
		if(dllno == OM_DLL_MAX) {
			return -1;
		}
		omDLLLink(&omDLLinfoTbl[dllno], overlay, TRUE);
		return dllno;
	}
}

void omDLLNumEnd(s16 overlay, s16 flag)
{
	s16 dllno;
	if(overlay < 0) {
		OSReport("objdll>omDLLNumEnd Invalid dllno %d\n", overlay);
		return;
	}
	OSReport("objdll>omDLLNumEnd %d %d\n", overlay, flag);
	dllno = omDLLSearch(overlay);
	if(dllno < 0) {
		OSReport("objdll>omDLLNumEnd not found DLL No%d\n", overlay);
		return;
	}
	omDLLEnd(dllno, flag);
}

void omDLLEnd(s16 dllno, s16 flag)
{
	OSReport("objdll>omDLLEnd %d %d\n", dllno, flag);
	if(flag == 1) {
		OSReport("objdll>End DLL:%s\n", omDLLinfoTbl[dllno]->name);
		omDLLUnlink(omDLLinfoTbl[dllno], 1);
		omDLLinfoTbl[dllno] = NULL;
	} else {
		omDllData *dll;
		dll = omDLLinfoTbl[dllno];
#ifdef __MWERKS__
		OSReport("objdll>Call Epilog\n");
		((DLLEpilog)dll->module->epilog)();
#endif
		OSReport("objdll>End DLL stayed:%s\n", omDLLinfoTbl[dllno]->name);
	}
	OSReport("objdll>End DLL finish\n");
}

omDllData *omDLLLink(omDllData **dll_ptr, s16 overlay, s16 flag)
{
	omDllData *dll;
	FileListEntry *dllFile = &omDLLFileList[overlay];
	OSReport("objdll>Link DLL:%s\n", dllFile->name);
	dll = HuMemDirectMalloc(HEAP_SYSTEM, sizeof(omDllData));
	*dll_ptr = dll;
	dll->name = dllFile->name;
#ifdef _WIN32
    dll->hModule = LoadLibrary(dllFile->name);
	if (dll->hModule == NULL) {
		OSReport("objdll>++++++++++++++++ DLL Link Failed\n");
	}
#elif defined(__linux__) || defined(__APPLE__) || defined(__ANDROID__)
	{
		// RPATH has to be set properly in CMake
		dll->handle = dlopen(dllFile->name, RTLD_LAZY);
		if (dll->handle == NULL) {
			OSReport("objdll>++++++++++++++++ DLL Link Failed %s\n", dlerror());
		}
	}
#elif defined(__MWERKS__)
	dll->module = HuDvdDataReadDirect(dllFile->name, HEAP_SYSTEM);
	dll->bss = HuMemDirectMalloc(HEAP_SYSTEM, dll->module->bssSize);
	if(OSLink(&dll->module->info, dll->bss) != TRUE) {
		OSReport("objdll>++++++++++++++++ DLL Link Failed\n");
	}
	omDLLInfoDump(&dll->module->info);
	omDLLHeaderDump(dll->module);
	OSReport("objdll>LinkOK %08x %08x\n", dll->module, dll->bss);
#else
	OSReport("DLL/so loading is not implemented for this platform");
#endif
	if(flag == 1) {
		OSReport("objdll> %s prolog start\n", dllFile->name);
#ifdef _WIN32
		{
		DLLObjectSetup objectSetup = (DLLObjectSetup)GetProcAddress(dll->hModule, "ObjectSetup");
		objectSetup();
		}
#elif defined(__linux__) || defined(__APPLE__)
		DLLObjectSetup objectSetup = (DLLObjectSetup)dlsym(dll->handle, "ObjectSetup");
		objectSetup();
#else
		dll->ret = ((DLLProlog)dll->module->prolog)();
#endif
		OSReport("objdll> %s prolog end\n", dllFile->name);
	}
	return dll;
}

void omDLLUnlink(omDllData *dll_ptr, s16 flag)
{
	OSReport("odjdll>Unlink DLL:%s\n", dll_ptr->name);
#ifdef _WIN32
    FreeLibrary(dll_ptr->hModule);
#elif defined(__linux__) || defined(__APPLE__)
	dlclose(dll_ptr->handle);
#else
	if(flag == 1) {
		OSReport("objdll>Unlink DLL epilog\n");
		((DLLEpilog)dll_ptr->module->epilog)();
		OSReport("objdll>Unlink DLL epilog finish\n");
	}
	if(OSUnlink(&dll_ptr->module->info) != TRUE) {
		OSReport("objdll>+++++++++++++++++ DLL Unlink Failed\n");
	}
	HuMemDirectFree(dll_ptr->bss);
	HuMemDirectFree(dll_ptr->module);
#endif
	HuMemDirectFree(dll_ptr);
}

s32 omDLLSearch(s16 overlay)
{
	s32 i;
	FileListEntry *dllFile = &omDLLFileList[overlay];
	OSReport("Search:%s\n", dllFile->name);
	for(i=0; i<OM_DLL_MAX; i++) {
		omDllData *dll = omDLLinfoTbl[i];
		if(dll != NULL && strcmp(dll->name, dllFile->name) == 0) {
			OSReport("+++++++++++ Find%d: %s\n", i, dll->name);
			return i;
		}
	}
	return -1;
}

#ifdef TARGET_PC
size_t omDLLSnapshotSizeGet(void)
{
#ifdef _WIN32
    HMODULE module = omDLLCurrentModuleGet();
    IMAGE_NT_HEADERS *nt = omDLLNtHeadersGet(module);
    IMAGE_SECTION_HEADER *section;
    size_t size = sizeof(omDllSnapshotHeader);
    u16 index;
    if (nt == NULL) {
        return 0;
    }
    section = IMAGE_FIRST_SECTION(nt);
    for (index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
        if (omDLLSectionIsSnapshotSafe(section)) {
            size += sizeof(omDllSnapshotSection) + section->Misc.VirtualSize;
        }
    }
    return size == sizeof(omDllSnapshotHeader) ? 0 : size;
#else
    return 0;
#endif
}

BOOL omDLLSnapshotSave(void *destination, size_t capacity)
{
#ifdef _WIN32
    HMODULE module = omDLLCurrentModuleGet();
    IMAGE_NT_HEADERS *nt = omDLLNtHeadersGet(module);
    omDllSnapshotHeader *header;
    IMAGE_SECTION_HEADER *section;
    u8 *cursor;
    size_t required = omDLLSnapshotSizeGet();
    u32 count = 0;
    u16 index;
    if (destination == NULL || nt == NULL || required == 0 || capacity != required) {
        return FALSE;
    }
    header = (omDllSnapshotHeader *)destination;
    cursor = (u8 *)destination + sizeof(*header);
    section = IMAGE_FIRST_SECTION(nt);
    for (index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
        omDllSnapshotSection savedSection;
        if (!omDLLSectionIsSnapshotSafe(section)) {
            continue;
        }
        savedSection.virtual_address = section->VirtualAddress;
        savedSection.size = section->Misc.VirtualSize;
        memcpy(cursor, &savedSection, sizeof(savedSection));
        cursor += sizeof(savedSection);
        memcpy(cursor, (u8 *)module + savedSection.virtual_address, savedSection.size);
        cursor += savedSection.size;
        ++count;
    }
    header->magic = OM_DLL_SNAPSHOT_MAGIC;
    header->version = OM_DLL_SNAPSHOT_VERSION;
    header->module = (uintptr_t)module;
    header->image_size = nt->OptionalHeader.SizeOfImage;
    header->timestamp = nt->FileHeader.TimeDateStamp;
    header->section_count = count;
    header->total_size = (u32)required;
    return TRUE;
#else
    (void)destination;
    (void)capacity;
    return FALSE;
#endif
}

BOOL omDLLSnapshotLoad(const void *source, size_t size)
{
#ifdef _WIN32
    const omDllSnapshotHeader *header;
    HMODULE module = omDLLCurrentModuleGet();
    IMAGE_NT_HEADERS *nt = omDLLNtHeadersGet(module);
    const u8 *cursor;
    const u8 *end;
    u32 index;
    if (source == NULL || nt == NULL || size < sizeof(omDllSnapshotHeader)) {
        return FALSE;
    }
    header = (const omDllSnapshotHeader *)source;
    if (header->magic != OM_DLL_SNAPSHOT_MAGIC
        || header->version != OM_DLL_SNAPSHOT_VERSION
        || header->module != (uintptr_t)module
        || header->image_size != nt->OptionalHeader.SizeOfImage
        || header->timestamp != nt->FileHeader.TimeDateStamp
        || header->total_size != size) {
        return FALSE;
    }
    cursor = (const u8 *)source + sizeof(*header);
    end = (const u8 *)source + size;
    for (index = 0; index < header->section_count; ++index) {
        omDllSnapshotSection savedSection;
        IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
        u16 sectionIndex;
        BOOL matched = FALSE;
        if ((size_t)(end - cursor) < sizeof(savedSection)) {
            return FALSE;
        }
        memcpy(&savedSection, cursor, sizeof(savedSection));
        cursor += sizeof(savedSection);
        if ((size_t)(end - cursor) < savedSection.size) {
            return FALSE;
        }
        for (sectionIndex = 0; sectionIndex < nt->FileHeader.NumberOfSections;
             ++sectionIndex, ++section) {
            if (omDLLSectionIsSnapshotSafe(section)
                && section->VirtualAddress == savedSection.virtual_address
                && section->Misc.VirtualSize == savedSection.size) {
                matched = TRUE;
                break;
            }
        }
        if (!matched) {
            return FALSE;
        }
        memcpy((u8 *)module + savedSection.virtual_address, cursor, savedSection.size);
        cursor += savedSection.size;
    }
    return cursor == end;
#else
    (void)source;
    (void)size;
    return FALSE;
#endif
}
#endif

void omDLLInfoDump(OSModuleInfo *module)
{
	OSReport("===== DLL Module Info dump ====\n");
	OSReport("                   ID:0x%08x\n", module->id);
	OSReport("             LinkPrev:0x%08x\n", module->link.prev);
	OSReport("             LinkNext:0x%08x\n", module->link.next);
	OSReport("          Section num:%d\n", module->numSections);
	OSReport("Section info tbl ofst:0x%08x\n", module->sectionInfoOffset);
	OSReport("           nameOffset:0x%08x\n", module->nameOffset);
	OSReport("             nameSize:%d\n", module->nameSize);
	OSReport("              version:0x%08x\n", module->version);
	OSReport("===============================\n");
}

void omDLLHeaderDump(OSModuleHeader *module)
{
	OSReport("==== DLL Module Header dump ====\n");
	OSReport("          bss Size:0x%08x\n", module->bssSize);
	OSReport("        rel Offset:0x%08x\n", module->relOffset);
	OSReport("        imp Offset:0x%08x\n", module->impOffset);
	OSReport("    prolog Section:%d\n", module->prologSection);
	OSReport("    epilog Section:%d\n", module->epilogSection);
	OSReport("unresolved Section:%d\n", module->unresolvedSection);
	OSReport("       prolog func:0x%08x\n", module->prolog);
	OSReport("       epilog func:0x%08x\n", module->epilog);
	OSReport("   unresolved func:0x%08x\n", module->unresolved);
	OSReport("================================\n");
}
