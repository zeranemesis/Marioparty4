#ifndef PORT_MODS_H
#define PORT_MODS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Overlay the mods selected by CubeShelf on top of the mounted disc image.
 *
 * Must be called after aurora_dvd_open() and before the game reads any file.
 * Returns the number of overlaid files, or 0 when no mod list was supplied.
 */
int PartyBoard_InitMods(void);

/**
 * Number of enabled mod content roots read from the CubeShelf mod list.
 */
int PartyBoard_GetModRootCount(void);

#ifdef __cplusplus
}
#endif

#endif
