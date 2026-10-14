#ifndef PORT_MODS_H
#define PORT_MODS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Use this mod list instead of the PARTYBOARD_MOD_LIST environment variable.
 * Launchers that can only pass arguments (the Switch launcher's --mod-list)
 * use this. Must be called before PartyBoard_InitMods(); null or "" clears it.
 */
void PartyBoard_SetModListPath(const char* path);

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
