#ifndef PORT_VERSION_RUNTIME_H
#define PORT_VERSION_RUNTIME_H

/**
 * Region checks for code that depends on the data of the loaded disc.
 *
 * The PC port compiles the game once (VERSION=1, USA) but can boot a PAL disc,
 * whose files, messages and layouts differ. These macros replace
 * VERSION_PAL / VERSION_NTSC / VERSION_ENG where the branch concerns disc data:
 * on PC they ask the loaded disc, everywhere else they are the compile-time
 * constants, so the GameCube build is unchanged.
 *
 * Do not use them for 50/60 Hz timing branches: the port always runs the game
 * logic at 60 Hz, so the NTSC timing stays correct for every disc.
 * See docs/pal_runtime_conversion.md.
 */

#include "version.h"

#ifdef TARGET_PC
#include "types.h"
#include "port/port_version.h"

/* A JP build keeps its compile-time behaviour; a USA build asks the disc. */
#define VERSION_RT_PAL (!VERSION_JP && partyboard_version_is_pal())
#define VERSION_RT_ENG (VERSION_ENG && !partyboard_version_is_pal())
#else
#define VERSION_RT_PAL VERSION_PAL
#define VERSION_RT_ENG VERSION_ENG
#endif

#define VERSION_RT_NTSC (!VERSION_RT_PAL)

#endif
