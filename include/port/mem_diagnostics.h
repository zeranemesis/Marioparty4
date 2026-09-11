#ifndef _PORT_MEM_DIAGNOSTICS_H
#define _PORT_MEM_DIAGNOSTICS_H

/* Integrity checking for the HuMem allocator.
 *
 * Why this exists. Every HuMem heap is carved out of one 64 MB MEM1 block, so a
 * write that runs off the end of one HuMem allocation and into its neighbour
 * stays inside a single, valid host allocation. AddressSanitizer sees nothing
 * wrong, and Full PageHeap would have the same blind spot for the same reason.
 * The only detector that can see inside MEM1 is one that knows the HuMem block
 * format, which is what this is.
 *
 * The goal is one sentence: name the first frame at which a HuMem allocation
 * stops being intact, name the victim block, and name where that block was
 * allocated from.
 *
 * Layout is deliberately NOT changed. docs/humem_layout.md derives the format
 * from the allocator; the two facts this instrumentation rests on are that the
 * payload starts at offset 64 while the header struct is only 40 bytes long,
 * leaving 24 dead bytes per block for a head redzone, and that blocks are
 * physically contiguous, so a payload overflow lands on the next block's header
 * and header validation catches it without needing a tail canary. Nothing moves,
 * which is what keeps the crash as reproducible as it is in a normal build.
 *
 * Off by default. PARTYBOARD_MEM_DIAGNOSTICS=1 turns it on.
 */

#include <stddef.h>

#include "dolphin/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Master switch, read once. Off means the hooks below return immediately. */
bool PartyBoard_MemDiagEnabled(void);

/* Poisoning a freed payload is a separate switch because it rewrites memory the
 * game has stopped using, which is exactly the kind of change that can move the
 * bug being hunted. Start with it off; turn it on only to chase a suspected
 * use-after-free. PARTYBOARD_MEM_POISON_FREE=1. */
bool PartyBoard_MemDiagPoisonFreeEnabled(void);

/* Heap bounds, so a block can be checked for containment and the sweep knows
 * what to walk. Called once per heap from HuMemInitAll. */
void PartyBoard_MemDiagRegisterHeap(s32 heapId, void *base, size_t size);

/* Allocation hooks. `block` is the block header, `requested` the size the caller
 * asked for, `internal` the total block size including the 64-byte header. */
void PartyBoard_MemDiagOnAlloc(void *block, size_t requested, size_t internal, uintptr_t retaddr);
void PartyBoard_MemDiagOnFree(void *block, uintptr_t retaddr);

/* A block is being consumed by a split or a coalesce, so its diagnostic record
 * must stop expecting the old redzones. */
void PartyBoard_MemDiagOnRetire(void *block);

/* Full sweep of every registered heap. Returns true while every heap is intact.
 * On the first violation it writes a report and returns false; the caller stops
 * the simulation rather than letting the damage spread. Called once per accepted
 * simulation tick. */
bool PartyBoard_MemDiagSweep(void);

/* Called once per accepted simulation tick. Runs the sweep and, on the first
   violation, writes the reports and stops the run so the frame at which the
   heap stopped being intact is the frame the session ends on. */
void PartyBoard_MemDiagTick(void);

/* Frames the sweep has certified, for the report and for the supervisor. */
u32 PartyBoard_MemDiagLastGoodFrame(void);
s32 PartyBoard_MemDiagFirstBadFrame(void); /* -1 while intact */

/* True once a corruption has been reported, so the run can be classified as
 * MEMORY_CORRUPTION_DETECTED rather than as an ordinary exit. */
bool PartyBoard_MemDiagCorruptionDetected(void);

/* Sweep cost, for the report: block count and timing. */
void PartyBoard_MemDiagStats(u32 *blocks, u32 *sweeps, double *averageMs, double *worstMs);

/* Suppresses report files while the detector is being tested against known
   corruptions, so a self-test never litters a session directory. */
void PartyBoard_MemDiagSetQuiet(bool quiet);

bool PartyBoard_MemDiagRunSelfTest(void);

/* Implemented in memory.c, where the block structure is visible. Walks one
 * heap's circular list and hands each block to the sink as plain values, so the
 * diagnostics module never needs its own copy of the layout. The sink returns
 * false to stop the walk. `ok` is false when the walk itself had to give up,
 * which is already a corruption. */
typedef bool (*PartyBoardMemBlockSink)(void *context, void *block, s32 size, u8 magic, u8 flag,
    void *prev, void *next, uintptr_t num, uintptr_t retaddr);
bool PartyBoard_MemWalkHeap(void *heapBase, size_t heapSize,
    PartyBoardMemBlockSink sink, void *context);

/* Offsets the diagnostics module needs, reported by memory.c rather than
 * assumed, so a change to the allocator cannot silently invalidate them. */
size_t PartyBoard_MemBlockHeaderSize(void);  /* sizeof(struct memory_block) */
size_t PartyBoard_MemBlockPayloadOffset(void); /* BLOCK_GET_DATA - block */

#ifdef __cplusplus
}
#endif

#endif
