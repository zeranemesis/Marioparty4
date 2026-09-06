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
/* Companion-controlled start barrier, called only after game initialization. */
bool PartyBoard_OnlineWaitForStart(void);
bool PartyBoard_NetplayAllowsMultipleInstances(void);
/* False means that the current 60 Hz game tick must wait for its peer. */
bool PartyBoard_NetplayTick(void);
/* Called immediately after physical PADRead, before PADClamp/bookkeeping.
 * False leaves the game tick uncommitted. Startup advertises occupied ports
 * without starting a network timeline before the game has initialized. */
bool PartyBoard_NetplayPreparePads(PADStatus status[4], u32 *rumble, bool startup);
/* Completes a rollback-prepared tick after the main game logic ran. Lockstep
 * and offline ticks treat this as a no-op. */
bool PartyBoard_NetplayCommitTick(void);
void PartyBoard_NetplayControlMotor(u32 port, u32 command);
bool PartyBoard_NetplayPadRunProbe(void);
bool PartyBoard_NetplayHasError(void);
bool PartyBoard_NetplayWaiting(void);
const char *PartyBoard_NetplayError(void);
void PartyBoard_NetplayShutdown(void);
bool PartyBoard_NetplayRuntimeRunSelfTest(void);

#ifdef __cplusplus
}
#endif

#endif
