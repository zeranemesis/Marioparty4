#include "game/memory.h"
#include "game/init.h"
#include "dolphin/os.h"
#ifdef TARGET_PC
#include "port/mem_diagnostics.h"
#ifdef TARGET_PC
/* The allocator records its caller so a desync report can name who made an
 * allocation (D30). The intrinsic differs per compiler; TARGET_PC is not a
 * Windows-only target. */
#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define PARTYBOARD_RETURN_ADDRESS() ((uintptr_t)_ReturnAddress())
#elif defined(__GNUC__) || defined(__clang__)
#define PARTYBOARD_RETURN_ADDRESS() ((uintptr_t)__builtin_return_address(0))
#else
#define PARTYBOARD_RETURN_ADDRESS() ((uintptr_t)0)
#endif
#endif
#endif

static u32 HeapSizeTbl[HEAP_MAX] = { 0x240000, 0x140000, 0xA80000, 0x580000, 0 };
static void *HeapTbl[HEAP_MAX];

void HuMemInitAll(void)
{
    s32 i;
    void *ptr;
    u32 free_size;
    for(i=0; i<4; i++) {
#ifdef TARGET_PC
        HeapSizeTbl[i] *= 4;
#endif
        ptr = OSAlloc(HeapSizeTbl[i]);
        if(ptr == NULL) {
            OSReport("HuMem> Failed OSAlloc Size:%d\n", HeapSizeTbl[i]);
            return;
        }
        HeapTbl[i] = HuMemInit(ptr, HeapSizeTbl[i]);
#ifdef TARGET_PC
        /* Bounds for the integrity sweep: a block has to be provably inside
         * its own heap, and the sweep needs to know what to walk. */
        PartyBoard_MemDiagRegisterHeap(i, HeapTbl[i], HeapSizeTbl[i]);
#endif
    }
    free_size = OSCheckHeap(currentHeapHandle);
    OSReport("HuMem> left memory space %dKB(%d)\n", free_size/1024, free_size);
    ptr = OSAlloc(free_size);
    if(ptr == NULL) {
        OSReport("HuMem> Failed OSAlloc left space\n");
        return;
    }
    HeapTbl[4] = HuMemInit(ptr, free_size);
#ifdef TARGET_PC
    PartyBoard_MemDiagRegisterHeap(4, HeapTbl[4], free_size);
#endif
    HeapSizeTbl[4] = free_size;
}

void *HuMemInit(void *ptr, size_t size)
{
    return HuMemHeapInit(ptr, size);
}

void HuMemDCFlushAll()
{
    HuMemDCFlush(2);
    HuMemDCFlush(0);
}

void HuMemDCFlush(HeapID heap)
{
    DCFlushRangeNoSync(HeapTbl[heap], HeapSizeTbl[heap]);
}

void *HuMemDirectMalloc(HeapID heap, size_t size)
{
#ifdef TARGET_PC
    /* The header has always had room for the caller; on PC nothing filled
      it, so every block looked anonymous. See D30. */
    uintptr_t retaddr = PARTYBOARD_RETURN_ADDRESS();
#else
    register u32 retaddr;
    asm {
        mflr retaddr
    }
#endif
    size = (size + 31) & ~0x1F;
    return HuMemMemoryAlloc(HeapTbl[heap], size, retaddr);
}

void *HuMemDirectMallocNum(HeapID heap, size_t size, uintptr_t num)
{
#ifdef TARGET_PC
    /* The header has always had room for the caller; on PC nothing filled
      it, so every block looked anonymous. See D30. */
    uintptr_t retaddr = PARTYBOARD_RETURN_ADDRESS();
#else
    register u32 retaddr;
    asm {
        mflr retaddr
    }
#endif
    size = (size + 31) & ~0x1F;
    return HuMemMemoryAllocNum(HeapTbl[heap], size, num, retaddr);
}

void HuMemDirectFree(void *ptr)
{
#ifdef TARGET_PC
    /* The header has always had room for the caller; on PC nothing filled
      it, so every block looked anonymous. See D30. */
    uintptr_t retaddr = PARTYBOARD_RETURN_ADDRESS();
#else
    register u32 retaddr;
    asm {
        mflr retaddr
    }
#endif
    HuMemMemoryFree(ptr, retaddr);
}

void HuMemDirectFreeNum(HeapID heap, uintptr_t num)
{
#ifdef TARGET_PC
    /* The header has always had room for the caller; on PC nothing filled
      it, so every block looked anonymous. See D30. */
    uintptr_t retaddr = PARTYBOARD_RETURN_ADDRESS();
#else
    register u32 retaddr;
    asm {
        mflr retaddr
    }
#endif
    HuMemMemoryFreeNum(HeapTbl[heap], num, retaddr);
}

size_t HuMemUsedMallocSizeGet(HeapID heap)
{
    return HuMemUsedMemorySizeGet(HeapTbl[heap]);
}

size_t HuMemUsedMallocBlockGet(HeapID heap)
{
    return HuMemUsedMemoryBlockGet(HeapTbl[heap]);
}

size_t HuMemHeapSizeGet(HeapID heap)
{
    return HeapSizeTbl[heap];
}

void *HuMemHeapPtrGet(HeapID heap)
{
    return HeapTbl[heap];
}
