#ifndef PARTYBOARD_PORT_NETPLAY_RUNTIME_H
#define PARTYBOARD_PORT_NETPLAY_RUNTIME_H

#include "dolphin/types.h"

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
bool PartyBoard_NetplayAllowsMultipleInstances(void);
/* False means that the current 60 Hz game tick must wait for its peer. */
bool PartyBoard_NetplayTick(void);
void PartyBoard_NetplayShutdown(void);
bool PartyBoard_NetplayRuntimeRunSelfTest(void);

#ifdef __cplusplus
}
#endif

#endif
