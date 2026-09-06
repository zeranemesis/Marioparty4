/* Actual snapshot implementations, synthetic coroutine buffers and loaded-PE
 * fixture. Unused game entry points are removed by /Gy /OPT:REF. */
#include <stdint.h>
#include <stdlib.h>
#include "../../src/game/process.c"
#include "../../src/game/objdll.c"

s32 omcurdll;
static int scheduler;
static cothread_t active = &scheduler;
int co_serializable(void) { return 1; }
cothread_t co_active(void) { return active; }
/* Unused game services: abort if snapshot tests accidentally invoke them. */
cothread_t co_create(unsigned int size, void (*entry)(void)) { abort(); return NULL; }
void co_delete(cothread_t thread) { abort(); }
void co_switch(cothread_t thread) { abort(); }
/* omDLLInit logs while invalidating a previous module lifetime. */
void OSReport(const char *message, ...) { (void)message; }
void *HuMemDirectMalloc(HeapID heap, size_t size) { abort(); return NULL; }
void HuMemDirectFree(void *ptr) { abort(); }
void *HuMemHeapInit(void *ptr, size_t size) { abort(); return NULL; }
void *HuMemMemoryAlloc(void *heap, size_t size, uintptr_t retaddr) { abort(); return NULL; }
void HuMemMemoryFree(void *ptr, uintptr_t retaddr) { abort(); }
size_t HuMemMemoryAllocSizeGet(size_t size) { abort(); return 0; }
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
typedef struct RegionAudit { size_t count, bytes; void *first; } RegionAudit;
static bool auditRegion(void *context, void *address, size_t size)
{
    RegionAudit *audit = context;
    if (!address || !size) return false;
    if (!audit->count) audit->first = address;
    ++audit->count; audit->bytes += size;
    return true;
}
static bool rejectRegion(void *context, void *address, size_t size) { return false; }

static int processes(void)
{
    Process a = {0}, b = {0};
    unsigned char ta[64], tb[64], saved[1024], damaged[1024];
    size_t size, second;
    HuPrcSnapshotEntry entry;
    HuPrcSnapshotHeader header;
    a.next = &b; b.prev = &a;
    a.thread = ta; b.thread = tb;
    a.thread_size = sizeof(ta); b.thread_size = sizeof(tb);
    processtop = &a; processcur = NULL; processcnt = 2;
    {
        RegionAudit audit = {0};
        CHECK(PartyBoard_RollbackProcessRegions(auditRegion, &audit));
        CHECK(audit.count == 6 && audit.first == ta && audit.bytes > sizeof(ta) + sizeof(tb));
        CHECK(!PartyBoard_RollbackProcessRegions(NULL, &audit));
        CHECK(!PartyBoard_RollbackProcessRegions(rejectRegion, &audit));
        processcur = &a;
        CHECK(!PartyBoard_RollbackProcessRegions(auditRegion, &audit));
        processcur = NULL; active = ta;
        CHECK(!PartyBoard_RollbackProcessRegions(auditRegion, &audit));
        active = &scheduler; processthread = tb;
        CHECK(!PartyBoard_RollbackProcessRegions(auditRegion, &audit));
        processthread = &scheduler;
    }
    memset(ta, 0x11, sizeof(ta)); memset(tb, 0x22, sizeof(tb));
    size = HuPrcSnapshotSizeGet();
    CHECK(size != 0 && HuPrcSnapshotSave(saved, sizeof(saved)));
    second = sizeof(header) + sizeof(entry) + sizeof(ta);
    memset(ta, 0x33, sizeof(ta)); memset(tb, 0x44, sizeof(tb));
    memcpy(damaged, saved, size);
    memcpy(&entry, damaged + second, sizeof(entry));
    entry.process = 1; /* Must never dereference the snapshot's fake pointer. */
    memcpy(damaged + second, &entry, sizeof(entry));
    CHECK(!HuPrcSnapshotLoad(damaged, size));
    CHECK(ta[0] == 0x33 && tb[0] == 0x44);
    memcpy(damaged, saved, size);
    memcpy(&header, saved, sizeof(header)); header.process_current = 1;
    memcpy(damaged, &header, sizeof(header));
    CHECK(!HuPrcSnapshotLoad(damaged, size));
    CHECK(ta[0] == 0x33 && tb[0] == 0x44);
    CHECK(!HuPrcSnapshotLoad(saved, size - 1));
    CHECK(ta[0] == 0x33 && tb[0] == 0x44);
    active = ta;
    CHECK(!HuPrcSnapshotLoad(saved, size));
    active = &scheduler;
    b.next = &a;
    CHECK(HuPrcSnapshotSizeGet() == 0);
    CHECK(!HuPrcSnapshotLoad(saved, size));
    b.next = NULL;
    b.prev = NULL;
    CHECK(HuPrcSnapshotSizeGet() == 0);
    b.prev = &a;
    CHECK(HuPrcSnapshotLoad(saved, size));
    CHECK(ta[0] == 0x11 && tb[0] == 0x22);
    processtop = NULL; processcnt = 0;
    return 0;
}

static int overlays(void)
{
    unsigned char module[4096] = {0}, saved[1024], damaged[1024];
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)module;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(module + 128);
    IMAGE_SECTION_HEADER *sections;
    omDllData dll = {0};
    omDllSnapshotHeader header;
    omDllSnapshotSection section;
    size_t size, second;
    dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 128;
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(nt->OptionalHeader);
    nt->FileHeader.NumberOfSections = 2;
    nt->OptionalHeader.SizeOfImage = sizeof(module);
    sections = IMAGE_FIRST_SECTION(nt);
    memcpy(sections[0].Name, ".data", 6); memcpy(sections[1].Name, ".data", 6);
    sections[0].VirtualAddress = 2048; sections[1].VirtualAddress = 2112;
    sections[0].Misc.VirtualSize = sections[1].Misc.VirtualSize = 64;
    sections[0].Characteristics = sections[1].Characteristics = IMAGE_SCN_MEM_WRITE;
    dll.hModule = (HMODULE)module; omcurdll = 0; omDLLinfoTbl[0] = &dll;
    memset(module + 2048, 0x11, 64); memset(module + 2112, 0x22, 64);
    size = omDLLSnapshotSizeGet();
    {
        RegionAudit audit = {0};
        CHECK(PartyBoard_RollbackModuleRegions(auditRegion, &audit));
        CHECK(audit.count == 2 && audit.bytes == 128 && audit.first == module + 2048);
        CHECK(!PartyBoard_RollbackModuleRegions(NULL, &audit));
        CHECK(!PartyBoard_RollbackModuleRegions(rejectRegion, &audit));
        sections[1].VirtualAddress = 4090; audit.count = 0;
        CHECK(!PartyBoard_RollbackModuleRegions(auditRegion, &audit));
        CHECK(audit.count == 0); /* Reject invalid last section before enumeration. */
        sections[1].VirtualAddress = 2112;
    }
    CHECK(size != 0 && omDLLSnapshotSave(saved, size));
    memset(module + 2048, 0x33, 128);
    second = sizeof(header) + sizeof(section) + 64;
    memcpy(damaged, saved, size);
    memcpy(&section, damaged + second, sizeof(section)); section.virtual_address = 2048;
    memcpy(damaged + second, &section, sizeof(section));
    CHECK(!omDLLSnapshotLoad(damaged, size)); /* duplicate first section */
    CHECK(module[2048] == 0x33 && module[2112] == 0x33);
    memcpy(damaged, saved, size);
    memcpy(&header, saved, sizeof(header)); header.section_count = 1;
    memcpy(damaged, &header, sizeof(header));
    CHECK(!omDLLSnapshotLoad(damaged, size));
    CHECK(module[2048] == 0x33 && module[2112] == 0x33);
    CHECK(!omDLLSnapshotLoad(saved, size - 1));
    CHECK(module[2048] == 0x33 && module[2112] == 0x33);
    sections[1].VirtualAddress = 4090;
    CHECK(omDLLSnapshotSizeGet() == 0);
    CHECK(!omDLLSnapshotLoad(saved, size));
    sections[1].VirtualAddress = 2112;
    CHECK(omDLLSnapshotLoad(saved, size));
    CHECK(module[2048] == 0x11 && module[2112] == 0x22);
    memcpy(module + 2500, saved, size);
    CHECK(!omDLLSnapshotLoad(module + 2500, size));
    /* Identical image/base/timestamp after a new lifecycle is still stale. */
    omDLLInit(NULL);
    omDLLinfoTbl[0] = &dll;
    memset(module + 2048, 0x55, 128);
    CHECK(!omDLLSnapshotLoad(saved, size));
    CHECK(module[2048] == 0x55 && module[2112] == 0x55);
    CHECK(omDLLSnapshotSave(saved, size));
    memset(module + 2048, 0x66, 128);
    CHECK(omDLLSnapshotLoad(saved, size));
    CHECK(module[2048] == 0x55 && module[2112] == 0x55);
    return 0;
}
int main(void)
{
    if (processes() || overlays()) return 1;
    printf("PASS: %u native snapshot validation checks\n", checks);
    return 0;
}
