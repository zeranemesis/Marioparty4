#include "game/memory.h"
#include "dolphin/os.h"
#ifdef TARGET_PC
#include "port/mem_diagnostics.h"
#include "port/netplay_runtime.h"
#include <stdio.h>
u32 PartyBoard_NetplayFrameForDiagnostics(void);
#include <stdlib.h>
#include <string.h>
#endif

#ifdef SKIP_HU_ALLOC
#include <stdlib.h>
#endif

#if INTPTR_MAX == INT32_MAX
#define MEM_ALLOC_SIZE(size) (((size) + 63) & ~0x1F)
#define DATA_GET_BLOCK(ptr) ((struct memory_block *)(((char *)(ptr)) - 32))
#define BLOCK_GET_DATA(block) (((char *)(block)) + 32)
#define BLOCK_ALIGNMENT 32u
#else
#define MEM_ALLOC_SIZE(size) (((size - 1) / 32 + 1) * 32 + 64)
#define DATA_GET_BLOCK(ptr) ((struct memory_block *)(((char *)(ptr)) - 64))
#define BLOCK_GET_DATA(block) (((char *)(block)) + 64)
#define BLOCK_ALIGNMENT 64u
#endif

struct  memory_block {
    s32 size;
    u8 magic;
    u8 flag;
    struct memory_block *prev;
    struct memory_block *next;
    uintptr_t num;
    uintptr_t retaddr;
};

static void *HuMemMemoryAlloc2(void *heap_ptr, size_t size, uintptr_t num, uintptr_t retaddr);

#ifdef TARGET_PC
/* PARTYBOARD_MEM_FILL: byte written over every payload handed out, as a
 * decimal value, or empty/unset for no fill. See docs/netplay_defect_register.md,
 * the 2026-09-12 experiment. Read once: getenv must not be called from the
 * allocator on every allocation. */
/* PARTYBOARD_ALLOC_TRACE=<first>-<last>: log every allocation made between
 * those two simulation frames, with its heap, its size and its caller. Empty
 * or unset logs nothing. See the 2026-09-12 HEAP_DATA divergence at frame
 * 6649, which repeats to the byte. */
static int HuMemTraceWindow(u32 *first, u32 *last)
{
    static int resolved = -2;
    static u32 from = 0, to = 0;
    if (resolved == -2) {
        const char *value = getenv("PARTYBOARD_ALLOC_TRACE");
        resolved = 0;
        if (value && *value) {
            const char *dash = strchr(value, 45);
            if (dash) {
                from = (u32)atoi(value);
                to = (u32)atoi(dash + 1);
                resolved = 1;
            }
        }
    }
    *first = from;
    *last = to;
    return resolved;
}

static int HuMemFillByte(void)
{
    static int resolved = -2;
    if (resolved == -2) {
        const char *value = getenv("PARTYBOARD_MEM_FILL");
        resolved = (value && *value) ? (atoi(value) & 0xFF) : -1;
        if (resolved >= 0) {
            OSReport("[MEM FILL] payloads filled with 0x%02X\n", resolved);
        }
    }
    return resolved;
}
#endif

void *HuMemHeapInit(void *ptr, size_t size)
{
    struct memory_block *block = ptr;
    block->size = (s32)size;
    block->magic = 205;
    block->flag = 0;
    block->prev = block;
    block->next = block;
    block->num = -256;
    block->retaddr = 0xCDCDCDCD;
    return block;
}

void *HuMemMemoryAllocNum(void *heap_ptr, size_t size, uintptr_t num, uintptr_t retaddr)
{
    return HuMemMemoryAlloc2(heap_ptr, size, num, retaddr);
}

void *HuMemMemoryAlloc(void *heap_ptr, size_t size, uintptr_t retaddr)
{
    return HuMemMemoryAlloc2(heap_ptr, size, -256, retaddr);
}

static void *HuMemMemoryAlloc2(void *heap_ptr, size_t size, uintptr_t num, uintptr_t retaddr)
{
    s32 alloc_size = (s32)MEM_ALLOC_SIZE(size);
#ifdef SKIP_HU_ALLOC
    return malloc(alloc_size);
#endif
    struct memory_block *block = heap_ptr;
    do {
        if (!block->flag && block->size >= alloc_size) {
            if (block->size - alloc_size > BLOCK_ALIGNMENT) {
#ifdef TARGET_PC
                struct memory_block *new_block = (struct memory_block *)((char *)block + alloc_size);
#else
                struct memory_block *new_block = (struct memory_block *)(((u32)block) + alloc_size);
#endif
#ifdef TARGET_PC
                PartyBoard_MemDiagOnRetire(new_block);
#endif
                new_block->size = block->size - alloc_size;
                new_block->magic = 205;
                new_block->flag = 0;
                new_block->retaddr = retaddr;
                block->next->prev = new_block;
                new_block->next = block->next;
                block->next = new_block;
                new_block->prev = block;
                block->size = alloc_size;
            }
            block->flag = 1;
            block->magic = 165;
            block->num = num;
            block->retaddr = retaddr;
#ifdef TARGET_PC
            /* Arms the head redzone in the dead bytes between the header and
             * the payload, and records the allocation in the table that lives
             * outside MEM1. No layout change: see docs/humem_layout.md. */
            PartyBoard_MemDiagOnAlloc(block, size, (size_t)block->size, retaddr);
            {
                const int fill = HuMemFillByte();
                if (fill >= 0) {
                    memset(BLOCK_GET_DATA(block), fill, size);
                }
            }
            {
                u32 traceFirst = 0, traceLast = 0;
                if (HuMemTraceWindow(&traceFirst, &traceLast)) {
                    const u32 frame = PartyBoard_NetplayFrameForDiagnostics();
                    if (frame >= traceFirst && frame <= traceLast) {
                        char trace[128];
                        snprintf(trace, sizeof(trace),
                            "alloc f=%u size=%u block=%d num=%u caller=%08x",
                            frame, (unsigned)size, (int)block->size,
                            (unsigned)num, (unsigned)retaddr);
                        PartyBoard_NetplayTrace(trace);
                    }
                }
            }
#endif
            return BLOCK_GET_DATA(block);
        }
        block = block->next;
    } while (block != heap_ptr);
    OSReport("HuMem>memory alloc error %08x(%08X): Call %08x\n", size, num, retaddr);
    HuMemHeapDump(heap_ptr, -1);
    return NULL;
}

void HuMemMemoryFreeNum(void *heap_ptr, uintptr_t num, uintptr_t retaddr)
{
    struct memory_block *block = heap_ptr;
    do {
        struct memory_block *block_next = block->next;
        if (block->flag && block->num == num) {
            HuMemMemoryFree(BLOCK_GET_DATA(block), retaddr);
        }
        block = block_next;
    } while (block != heap_ptr);
}

static void HuMemTailMemoryAlloc2() // Required for string literal
{
    OSReport("memory allocation(tail) error.\n");
}

void HuMemMemoryFree(void *ptr, uintptr_t retaddr)
{
#ifdef SKIP_HU_ALLOC
    free(ptr);
    return;
#endif
    struct memory_block *block;
    if (!ptr) {
        return;
    }
    block = DATA_GET_BLOCK(ptr);
    if (block->magic != 165) {
        OSReport("HuMem>memory free error. %08x( call %08x)\n", ptr, retaddr);
        return;
    }
#ifdef TARGET_PC
    /* Only for a free the allocator accepts, so a rejected double free
     * never touches memory. */
    PartyBoard_MemDiagOnFree(block, retaddr);
#endif
    if (block->prev < block && !block->prev->flag) {
#ifdef TARGET_PC
        /* Absorbed by its predecessor; the record must not outlive it. */
        PartyBoard_MemDiagOnRetire(block);
#endif
        block->flag = 0;
        block->magic = 205;
        block->next->prev = block->prev;
        block->prev->next = block->next;
        block->prev->size += block->size;
        block = block->prev;
    }
    if (block->next > block && !block->next->flag) {
#ifdef TARGET_PC
        PartyBoard_MemDiagOnRetire(block->next);
#endif
        block->next->next->prev = block;
        block->size += block->next->size;
        block->next = block->next->next;
    }
    block->flag = 0;
    block->magic = 205;
    block->retaddr = retaddr;
}

size_t HuMemUsedMemorySizeGet(void *heap_ptr)
{
    struct memory_block *block = heap_ptr;
    size_t size = 0;
    do {
        if (block->flag == 1) {
            size += block->size;
        }
        block = block->next;
    } while (block != heap_ptr);
    return size;
}

s32 HuMemUsedMemoryBlockGet(void *heap_ptr)
{
    struct memory_block *block = heap_ptr;
    s32 num_blocks = 0;
    do {
        if (block->flag == 1) {
            num_blocks++;
        }
        block = block->next;
    } while (block != heap_ptr);
    return num_blocks;
}

size_t HuMemMemoryAllocSizeGet(size_t size)
{
    return MEM_ALLOC_SIZE(size);
}

void HuMemHeapDump(void *heap_ptr, s16 status)
{
    struct memory_block *block = heap_ptr;
    size_t size = 0;
    size_t inactive_size = 0;
    s32 num_blocks = 0;
    s32 num_unused_blocks = 0;
    u8 dump_type;

    if (status < 0) {
        dump_type = 10;
    }
    else if (status == 0) {
        dump_type = 0;
    }
    else {
        dump_type = 1;
    }
    OSReport("======== HuMem heap dump %08x ========\n", heap_ptr);
    OSReport("MCB-----+Size----+MG+FL+Prev----+Next----+UNum----+Body----+Call----\n");
    do {
        if (dump_type == 10 || block->flag == dump_type) {
            OSReport("%08x %08x %02x %02x %08x %08x %08x %08x %08x\n", block, block->size, block->magic, block->flag, block->prev, block->next,
                block->num, BLOCK_GET_DATA(block), block->retaddr);
        }
        if (block->flag == 1) {
            size += block->size;
            num_blocks++;
        }
        else {
            inactive_size += block->size;
            num_unused_blocks++;
        }

        block = block->next;
    } while (block != heap_ptr);
    OSReport("MCB:%d(%d/%d) MEM:%08x(%08x/%08x)\n", num_blocks + num_unused_blocks, num_blocks, num_unused_blocks, size + inactive_size, size,
        inactive_size);
    OSReport("======== HuMem heap dump %08x end =====\n", heap_ptr);
}

size_t HuMemMemorySizeGet(void *ptr)
{
    struct memory_block *block;
    if (!ptr) {
        return 0;
    }
    block = DATA_GET_BLOCK(ptr);
    if (block->flag == 1 && block->magic == 165) {
        return block->size - BLOCK_ALIGNMENT;
    }
    else {
        return 0;
    }
}

#ifdef TARGET_PC
#include "port/netplay_state.h"

/* Allocator bookkeeping only. Block addresses differ legitimately between two
 * machines; the number of live blocks and the bytes they occupy do not, and a
 * divergence there is an early, precise sign of a different code path. */
void PartyBoard_NetplayHeapState(PartyBoardNetplayStateSink sink, void *context)
{
    static const HeapID heaps[3] = { HEAP_SYSTEM, HEAP_DATA, HEAP_DVD };
    int i;
#define WORD(value) sink(context, #value, (uint32_t)(value))
    for (i = 0; i < 3; i++) {
        const HeapID heap = heaps[i];
        const int ready = HuMemHeapPtrGet(heap) != NULL;
        WORD(heap);
        WORD(ready);
        if (!ready) {
            continue; /* Self-tests run before the arenas exist. */
        }
        WORD(HuMemHeapSizeGet(heap));
        WORD(HuMemUsedMallocSizeGet(heap));
        WORD(HuMemUsedMallocBlockGet(heap));
    }
#undef WORD
}

/* D30. The counts above say THAT two peers hold a different number of
 * blocks; they cannot say which allocations, nor who made them. This walks
 * the same heaps block by block and hands out the caller that the allocator
 * already records in every header, so two censuses diff to the exact
 * allocation sites that only one machine visited.
 *
 * Diagnostics only, called from the report writer after a divergence is
 * already established - never from the hashing path, and never on a hot
 * frame. It allocates nothing and mutates nothing. */
/* Test-only, opt-in, and never called unless --netplay-inject-heap is on the
 * command line. Allocates from HEAP_SYSTEM and deliberately keeps the blocks,
 * so one peer holds more live blocks than the other and the HEAPS subsystem
 * diverges on purpose. The allocation site is here, in the allocator's own
 * translation unit, so the census reports a stable address in dol.dll. */
void PartyBoard_NetplayHeapInjectLeak(int blocks, uint32_t bytes)
{
    int i;
    for (i = 0; i < blocks; i++) {
        void *leaked = HuMemDirectMalloc(HEAP_SYSTEM, (size_t)bytes);
        (void)leaked; /* Kept on purpose: the divergence IS the point. */
    }
}

void PartyBoard_NetplayHeapCensus(PartyBoardHeapBlockSink sink, void *context)
{
    static const HeapID heaps[3] = { HEAP_SYSTEM, HEAP_DATA, HEAP_DVD };
    int i;
    if (!sink) {
        return;
    }
    for (i = 0; i < 3; i++) {
        const HeapID heap = heaps[i];
        struct memory_block *start = HuMemHeapPtrGet(heap);
        struct memory_block *block = start;
        if (!start) {
            continue; /* Arena not created yet. */
        }
        do {
            if (block->flag == 1) {
                sink(context, (int)heap, (uint32_t)block->size,
                    (uint32_t)block->num, block->retaddr);
            }
            block = block->next;
        } while (block != start);
    }
}
#endif

#ifdef TARGET_PC
/* Reported rather than assumed, so a change to the block structure cannot
 * silently invalidate the diagnostics module's idea of the layout. */
size_t PartyBoard_MemBlockHeaderSize(void)
{
    return sizeof(struct memory_block);
}

size_t PartyBoard_MemBlockPayloadOffset(void)
{
    return (size_t)(BLOCK_GET_DATA((struct memory_block *)0));
}

/* Walks one heap's circular list and hands each block to the sink as plain
 * values. Living here keeps the block layout in exactly one place.
 *
 * The walk is defensive by necessity: it is used to find corruption, so it
 * cannot trust the links it follows. It refuses a next pointer outside the
 * heap or misaligned, and it bounds the iteration count. Returning false means
 * the list itself could not be walked, which is already a corruption. */
bool PartyBoard_MemWalkHeap(void *heapBase, size_t heapSize,
    PartyBoardMemBlockSink sink, void *context)
{
    struct memory_block *block = heapBase;
    struct memory_block *start = heapBase;
    u32 guard = 0;
    if (!heapBase || !sink) {
        return FALSE;
    }
    do {
        struct memory_block *next = block->next;
        if (!sink(context, block, block->size, block->magic, block->flag, block->prev, next,
                block->num, block->retaddr)) {
            return TRUE;
        }
        /* The walk is used to FIND corruption, so it must never trust a link it
         * is about to follow. A next pointer outside the heap, misaligned or
         * too close to the end to hold a header is a corruption in itself, and
         * following it would fault instead of reporting. */
        if (!next || ((uintptr_t)next & 7u) != 0) {
            return FALSE;
        }
        if ((uintptr_t)next < (uintptr_t)heapBase
            || (uintptr_t)next + sizeof(struct memory_block) > (uintptr_t)heapBase + heapSize) {
            return FALSE;
        }
        block = next;
        if (++guard > 4000000u) {
            return FALSE;
        }
    } while (block != start);
    return TRUE;
}
#endif
