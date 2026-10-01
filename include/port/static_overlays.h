#ifndef PORT_STATIC_OVERLAYS_H
#define PORT_STATIC_OVERLAYS_H

/*
 * Overlays (the game's RELs) linked into the executable, for platforms with no
 * dynamic loader (the Switch: PARTYBOARD_STATIC_OVERLAYS). Elsewhere each
 * overlay is a shared library that objdll.c opens and closes.
 *
 * Opening a shared library gives the overlay fresh globals, and the game
 * relies on it (as it did on the GameCube, where OSLink reloaded the module
 * from the disc): a minigame starts from its initial data, not from the last
 * round's. A built-in overlay keeps its globals for the whole run, so linking
 * one restores its initialised data and zeroes its bss, which the build gathers
 * into one section each per overlay.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PartyBoardStaticOverlay {
    const char *name; /* as listed in _ovltbl, e.g. "m401Dll.so" */
    void (*objectSetup)(void);
    unsigned char *dataStart; /* writable initialised data */
    unsigned char *dataEnd;
    unsigned char *bssStart;
    unsigned char *bssEnd;
    unsigned char *pristine; /* the data as it was before the first link */
} PartyBoardStaticOverlay;

/* The table the build generates. */
extern PartyBoardStaticOverlay PartyBoard_StaticOverlays[];
extern const unsigned PartyBoard_StaticOverlayCount;

/*
 * Finds the overlay `name`, gives it the data and bss of a fresh load and
 * returns it, or returns NULL when it is not built in.
 */
PartyBoardStaticOverlay *PartyBoard_StaticOverlayLink(const char *name);

#ifdef __cplusplus
}
#endif

#endif
