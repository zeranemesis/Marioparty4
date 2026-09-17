#ifndef _PORT_CRASH_REPORT_H
#define _PORT_CRASH_REPORT_H

// Crash and abnormal-termination reporting.
//
// Purpose: an instance that disappears without the user asking for it must
// always leave a report that names the frame, the overlay, the gameplay context
// and the netplay state it died in. A supervisor exit code of 0 says nothing
// about the game's own exit code, and must never be read as a success.
//
// Two mechanisms, because one is not enough:
//
//   1. An exception handler writes a full report plus a minidump. This catches
//      access violations, illegal instructions, stack-overflow guard-page hits
//      and the other ordinary structured exceptions.
//
//   2. A live state file is rewritten every few frames. This covers the
//      terminations that CANNOT be intercepted in-process: Windows raises
//      STATUS_HEAP_CORRUPTION (0xC0000374) through __fastfail, which bypasses
//      both SetUnhandledExceptionFilter and vectored exception handlers by
//      design. When that happens the state file is the only in-process evidence
//      left, and the supervisor completes the picture from the Windows
//      Application event log and the WER minidump.
//
// Nothing here ever tries to resume a crashed process. The handler captures,
// writes, and lets the original exception kill the process.

#include "dolphin/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Breadcrumb categories. Kept short: they are written to a compact ring and
// read by a human looking for the last notable event before the crash.
#define PARTYBOARD_CRASH_CAT_OVERLAY "OVERLAY"
#define PARTYBOARD_CRASH_CAT_PROCESS "PROCESS"
#define PARTYBOARD_CRASH_CAT_BOARD "BOARD"
#define PARTYBOARD_CRASH_CAT_DATA "DATA"
#define PARTYBOARD_CRASH_CAT_CONTEXT "CONTEXT"
#define PARTYBOARD_CRASH_CAT_MINIGAME "MINIGAME"
#define PARTYBOARD_CRASH_CAT_NET "NET"
#define PARTYBOARD_CRASH_CAT_AUDIO "AUDIO"
#define PARTYBOARD_CRASH_CAT_WARN "WARN"
#define PARTYBOARD_CRASH_CAT_STACK "STACK"

// Simulation and network facts, refreshed by the netplay runtime once per
// accepted simulation tick. Every field is a value the game thread owns, so
// sampling it costs nothing and is safe to read from a crashing context.
typedef struct PartyBoardCrashSimState {
    u32 simulationFrame;
    u32 networkFrame;
    s32 gameContext;
    s32 overlay;
    s32 minigame;
    s32 overlayPrevious;
    u32 overlayTransitionFrame;

    // Progress, as opposed to activity. A run that reaches its frame budget
    // while stuck in a menu and a run that plays twelve turns look identical
    // from frame counts alone, and every scenario until now was gated on frames
    // only. These two are what tell them apart. Both are already in the
    // canonical hash, so they are agreed between peers; they were simply never
    // published anywhere a harness could read them.
    s32 boardTurn;      // GWSystem.turn
    s32 boardMaxTurn;   // GWSystem.max_turn
    s32 boardId;        // GWSystem.board

    u64 lastStateHash;
    u32 lastStateHashFrame;

    u32 frand;
    u32 rand8;
    u32 boardRand;
    u32 frandCalls;
    u32 rand8Calls;
    u32 boardRandCalls;

    u32 received;
    u32 rejected;
    u32 repaired;
    u32 sendErrors;
    u32 packetAgeMs;
    u32 stalledTicks;
    u32 maximumStalledTicks;
    u32 txSequence;
    s32 randomSynchronized;
    s32 configMismatch;
    u32 contextMismatchFrames;
    s32 localReady;
    s32 remoteReady;

    u16 localButtons;
    u16 remoteButtons;
    s8 localStickX;
    s8 localStickY;
    s8 remoteStickX;
    s8 remoteStickY;

    s32 rollbackActive;
    u32 rollbackCount;
    u32 rollbackReplayed;
    u32 rollbackPredicted;

    // Driven by the MusyX audio thread. DIAGNOSTIC ONLY: never hashed, because
    // an audio difference that no gameplay reads is not a gameplay divergence.
    s32 musStatus[4];
} PartyBoardCrashSimState;

// Install the handler and open the session directory. Call this as early as
// possible in main(), before anything that could fault. Only the first call has
// an effect.
//
// `sessionDir` may be NULL: the directory is then taken from
// PARTYBOARD_CRASH_DIR, or from the directory of PARTYBOARD_NET_DIAGNOSTIC, so
// reports land beside the session even for a launcher that sets only the latter.
// `peerIndex` may be -1 and is then taken from PARTYBOARD_CRASH_PEER.
void PartyBoard_CrashReportInit(const char *sessionDir, s32 peerIndex, const char *role);

// Fill in the peer identity once the command line has been parsed. The handler
// is already armed by then, so this only improves what a report says; it never
// gates protection.
void PartyBoard_CrashSetPeer(s32 peerIndex, const char *role);

// Record a notable event. Compact by design: the ring keeps the most recent
// entries only, and this must never be called once per frame for routine work.
void PartyBoard_CrashBreadcrumb(const char *category, const char *format, ...);

/* The simulation frame the crash reporter last saw, for anything that wants to
 * stamp an event with it without keeping its own copy. 0 before the first tick,
 * and possibly one frame stale if the game thread holds the lock - which is the
 * right trade for a marker that must never block the simulation. */
u32 PartyBoard_CrashSimulationFrame(void);

// Refresh the simulation/network snapshot. Called once per accepted tick.
void PartyBoard_CrashUpdateSimState(const PartyBoardCrashSimState *state);

// Lock-free reads of the two fields a diagnostic needs to date an event. Safe
// from any thread, including the MusyX audio thread, which must never block on
// the game thread. Diagnostic only: never hash these, and never branch gameplay
// on them.
u32 PartyBoard_CrashCurrentFrame(void);
s32 PartyBoard_CrashCurrentOverlay(void);

// Rewrite the live state file if enough frames have passed. Called once per
// accepted tick; the write itself is throttled internally.
void PartyBoard_CrashHeartbeat(void);

// The directory reports are written to, resolved once by
// PartyBoard_CrashReportInit. Empty means the working directory. The startup
// scan needs this so it looks where the reporter actually wrote.
const char *PartyBoard_CrashReportDirectory(void);

// A shutdown the user asked for: the window's close button, Alt+F4, or a menu
// quit that reached the normal exit path. After this, termination is expected.
void PartyBoard_CrashNoteUserShutdown(const char *how);

// A shutdown the supervisor asked for, so it is not counted against the game.
void PartyBoard_CrashNoteSupervisorShutdown(void);

// The game reached its own normal end of run.
void PartyBoard_CrashNoteNormalExit(void);

// True once any of the three notes above has been made.
bool PartyBoard_CrashShutdownExpected(void);

// Write a report for a termination that is not an exception: a netplay desync
// stop, a peer that vanished, a refused configuration. Returns the report path
// or NULL.
const char *PartyBoard_CrashReportAbnormal(const char *reason, const char *detail);

// TEST ONLY. Deliberately raises a real exception so the whole chain can be
// verified end to end: handler, writer thread, minidump, report contents. The
// process is EXPECTED to die; nothing here is reachable from gameplay.
//
//   "guard"  write below a guarded coroutine stack, which must fault on its
//            guard page and be reported as a stack overflow with the owning
//            process named. This is the mechanism the HuPrc overflow
//            hypothesis is tested with.
//   "null"   read through a null pointer, for a plain access violation.
void PartyBoard_CrashReportProvoke(const char *mode);

bool PartyBoard_CrashReportRunSelfTest(void);

#ifdef __cplusplus
}
#endif

#endif
