#ifndef _GAME_MEMORY_H
#define _GAME_MEMORY_H

#include "dolphin/types.h"

// TODO rename to HU_MEMNUM_OVL
#define MEMORY_DEFAULT_NUM 0x10000000

typedef enum {
    HEAP_SYSTEM,
    HEAP_MUSIC,
    HEAP_DATA,
    HEAP_DVD,
    HEAP_MISC,
    HEAP_MAX
} HeapID;

void HuMemInitAll(void);
void *HuMemInit(void *ptr, size_t size);
void HuMemDCFlushAll();
void HuMemDCFlush(HeapID heap);
void *HuMemDirectMalloc(HeapID heap, size_t size);
void *HuMemDirectMallocNum(HeapID heap, size_t size, uintptr_t num);
void HuMemDirectFree(void *ptr);
void HuMemDirectFreeNum(HeapID heap, uintptr_t num);
size_t HuMemUsedMallocSizeGet(HeapID heap);
size_t HuMemUsedMallocBlockGet(HeapID heap);
size_t HuMemHeapSizeGet(HeapID heap);
void *HuMemHeapPtrGet(HeapID heap);

void *HuMemHeapInit(void *ptr, size_t size);
void *HuMemMemoryAlloc(void *heap_ptr, size_t size, uintptr_t retaddr);
void *HuMemMemoryAllocNum(void *heap_ptr, size_t size, uintptr_t num, uintptr_t retaddr);
void HuMemMemoryFree(void *ptr, uintptr_t retaddr);
void HuMemMemoryFreeNum(void *heap_ptr, uintptr_t num, uintptr_t retaddr);
size_t HuMemUsedMemorySizeGet(void *heap_ptr);
s32 HuMemUsedMemoryBlockGet(void *heap_ptr);
size_t HuMemMemorySizeGet(void *ptr);
size_t HuMemMemoryAllocSizeGet(size_t size);
void HuMemHeapDump(void *heap_ptr, s16 status);
#ifdef TARGET_PC
/* Rollback copies a tracked heap only up to the furthest byte the allocator
 * has ever handed out. The heap's memory must be zero when tracking starts. */
void HuMemHighWaterTrack(void *heap_ptr, size_t size);
/* Bytes from the heap start that may have been written; 0 when untracked. */
size_t HuMemHighWaterGet(const void *heap_ptr);
void HuMemHighWaterForget(void *heap_ptr);
#endif

#endif
