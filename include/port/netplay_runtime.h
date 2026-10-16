#ifndef PARTYBOARD_PORT_NETPLAY_RUNTIME_H
#define PARTYBOARD_PORT_NETPLAY_RUNTIME_H

#include "dolphin/pad.h"

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Experimental two-player LAN runtime. Its safe default synchronizes only
 * minigames; --netplay-full extends lockstep to menus and boards. It is enabled
 * only by command-line arguments, so the normal offline game remains untouched. */
bool PartyBoard_NetplayConfigureFromArgs(int argc, char **argv);
bool PartyBoard_NetplayEnabled(void);
// The netplay frame, for tagging diagnostics; defined in netplay_runtime.cpp.
u32 PartyBoard_NetplayFrameForDiagnostics(void);
/* One named boot/menu diagnostic event; no-op outside a network session. */
void PartyBoard_NetplayTrace(const char *event);
/* Rendered frames since boot. Reported, never hashed: it is a property of
   this machine, not of the game state. See docs/netplay_defect_register.md. */
extern unsigned int PartyBoard_RenderedFrames;
void PartyBoard_NetplayWalkMenu(int ready);
/* Non-zero when a draw hook may only run on a frame carrying a simulation
   tick. See D23 in docs/netplay_defect_register.md. */
int PartyBoard_HookTickGateEnabled(void);
/* Test-only override of the party turn count, 0 when unset. */
int PartyBoard_NetplayMaxTurnsOverride(void);
/* Companion-controlled start barrier, called only after game initialization. */
bool PartyBoard_OnlineWaitForStart(void);
bool PartyBoard_NetplayAllowsMultipleInstances(void);
/* False means that the current 60 Hz game tick must wait for its peer. */
bool PartyBoard_NetplayTick(void);
/* Called immediately after physical PADRead, before PADClamp/bookkeeping.
 * False leaves the game tick uncommitted. Startup advertises occupied ports
 * without starting a network timeline before the game has initialized. */
bool PartyBoard_NetplayPreparePads(PADStatus status[4], u32 *rumble, bool startup);
/* After accepted game logic: captures a canonical lockstep state, or completes
 * an experimental rollback-prepared tick. Offline remains a no-op. */
bool PartyBoard_NetplayCommitTick(void);
void PartyBoard_NetplayControlMotor(u32 port, u32 command);
bool PartyBoard_NetplayPadRunProbe(void);
bool PartyBoard_NetplayHasError(void);
bool PartyBoard_NetplayWaiting(void);
/* True once a packet from the peer has ever been accepted. */
bool PartyBoard_NetplayPeerSeen(void);
const char *PartyBoard_NetplayError(void);
void PartyBoard_NetplayShutdown(void);
bool PartyBoard_NetplayRuntimeRunSelfTest(void);

/* Online inactivity (netplay_idle.hpp). The seat the board is counting down,
 * or -1; seconds left, that seat's character (0..7, -1 unknown) and whether it
 * is this machine's own seat. */
int PartyBoard_NetplayIdleCountdown(int *seconds, int *character, bool *local);
/* A seat replaced by a CPU since the last call, or -1. Each replacement is
 * reported once; *local says whether it is this machine's own seat. */
int PartyBoard_NetplayIdleTakeKick(int *character, bool *local);

/* Game side, in pad.c: the pad of the human whose board turn it is (-1 outside
 * a party board or during a CPU's turn), a pad's character, and turning a pad's
 * player into a CPU of normal level for the rest of the party. */
s32 PartyBoard_NetplayTurnPad(void);
/* The nickname of a seat as the companion passed it (--netplay-name), UTF-8, or
 * NULL when it gave none. Display only: never hashed, never synchronized. */
const char *PartyBoard_NetplaySeatName(int seat);
/* Where the head of the player on that pad is on the picture, as fractions of
 * its width and height (0..1, widescreen included). False outside a party
 * board, while that player's model is hidden, or off screen. */
bool PartyBoard_NetplayPadHeadOnScreen(s32 pad, float *x, float *y);
/* Test-only (--netplay-probe-names): the name tags sit in a fixed row, so their
 * names and drawing can be checked without walking menus to a board. */
bool PartyBoard_NetplayNameProbe(void);
/* The character on each pad (0..7, -1 none) once a party is under way, for the
 * launcher's room; false in the menus, before anyone has chosen. */
bool PartyBoard_NetplayPadCharacters(int characters[4]);
s32 PartyBoard_NetplayPadCharacter(s32 pad);
void PartyBoard_NetplayPadBecomesCom(s32 pad);

#ifdef __cplusplus
}
#endif

#endif
