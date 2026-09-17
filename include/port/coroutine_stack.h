#ifndef _PORT_COROUTINE_STACK_H
#define _PORT_COROUTINE_STACK_H

// Guarded allocator and usage meter for libco coroutine stacks.
//
// Every HuPrc process runs on a libco coroutine whose stack libco allocates
// with a plain malloc (extern/libco/settings.h falls back to malloc when
// LIBCO_MALLOC is not defined). That stack has no guard page and no canary, and
// an x86-64 frame is much larger than the PowerPC frame the original stack
// sizes were calibrated for. A process that overflows its stack therefore walks
// straight off the bottom of a heap block and corrupts the block header of its
// neighbour, which Windows reports much later, somewhere unrelated, as
// STATUS_HEAP_CORRUPTION (0xC0000374).
//
// This file makes that failure mode visible instead of silent:
//
//   * the stack is its own VirtualAlloc reservation with a PAGE_NOACCESS page
//     immediately BELOW the usable region. x86-64 stacks grow downwards, so an
//     overflow faults on the guard page at the exact instruction that overflows,
//     rather than quietly damaging the heap;
//   * the usable region is filled with a known pattern at creation, so the peak
//     depth a process ever reached can be measured instead of guessed.
//
// This is instrumentation. It does NOT change any stack size: the point is to
// collect evidence first and size the stacks from measurements afterwards.
//
// Both features can be turned off at runtime:
//   PARTYBOARD_COROUTINE_GUARD=0      fall back to malloc/free
//   PARTYBOARD_STACK_WATERMARK=0      skip the pattern fill and the meter

#include <stddef.h>

#include "dolphin/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// LIBCO_MALLOC / LIBCO_FREE replacements. The returned pointer is the base of
// the usable region, which is what co_derive expects, and the guard page sits
// immediately below it.
void *PartyBoard_CoroutineStackAlloc(size_t size);
void PartyBoard_CoroutineStackFree(void *pointer);

// Initialise the process-wide DbgHelp symbol handler once. SymInitialize fails
// if called twice, so everything that needs symbols goes through this.
bool PartyBoard_CoroutineSymbolsReady(void);

bool PartyBoard_CoroutineGuardEnabled(void);
bool PartyBoard_CoroutineWatermarkEnabled(void);

// Fill the usable region with the measurement pattern, skipping the bytes
// libco owns: the stack-pointer slot at the base and the return addresses it
// pushes near the top. Call after co_create, with the size passed to it.
//
// `entryPoint` is the process function libco was given. It is the only stable
// identity a HuPrc process has, so it is stored raw and resolved to a symbol
// name lazily when a report is produced; that keeps the creation path free of
// any symbol lookup.
void PartyBoard_CoroutineStackArm(void *stack, u32 size, const void *entryPoint);

// Peak bytes ever used by this stack, found by scanning upwards from the base
// for the first word that no longer matches the pattern. Returns 0 when the
// stack is unknown or the meter is off.
u32 PartyBoard_CoroutineStackPeak(const void *stack, u32 size);

// Forget a stack that is about to be freed, after folding its peak into the
// global worst-case table.
void PartyBoard_CoroutineStackRetire(const void *stack);

// Describe an address for a crash report: says whether it falls in a guard
// page, in a known stack, or just near one, and names the owning process.
// Returns false when the address has nothing to do with a coroutine stack.
bool PartyBoard_CoroutineStackDescribe(const void *address, char *out, size_t outSize);

// Append the live stacks and the retired worst cases to a report, one line per
// entry, ordered by usage ratio. Returns the number of lines written.
u32 PartyBoard_CoroutineStackSummary(char *out, size_t outSize);

// Write the summary to its own file in the session directory, so a long session
// leaves usage data even when it never crashes.
void PartyBoard_CoroutineStackWriteReport(const char *path);

// Headroom watchdog.
//
// A coroutine stack that actually overflows cannot be measured after the fact:
// the overflow kills the process at that instant, so the stack is never retired
// and never appears in the usage table. Every peak reported by the meter is
// therefore a survivor, which is how an overflow hid behind a comfortable
// maximum of 27 per cent for a whole investigation.
//
// This catches it before it happens. Once per accepted simulation tick it looks
// at the bottom `threshold` bytes of every live armed stack: if the pattern
// there has been overwritten, that stack has less than `threshold` bytes left
// and is about to run out. Only the bottom of each stack is scanned, so the cost
// is bounded by the threshold rather than by the stack size.
//
// Returns false when a stack is below the threshold, having filled `out` with a
// description naming the owning process. Off unless
// PARTYBOARD_STACK_WATCHDOG=1.
bool PartyBoard_CoroutineStackWatch(u32 threshold, char *out, size_t outSize);
bool PartyBoard_CoroutineWatchdogEnabled(void);

bool PartyBoard_CoroutineStackRunSelfTest(void);

#ifdef __cplusplus
}
#endif

#endif
