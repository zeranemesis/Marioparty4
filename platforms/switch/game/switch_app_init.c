/*
 * libnx runs userAppInit() before main() and userAppExit() after it (weak
 * hooks in its runtime): the game's process environment on the Switch.
 */
#include <switch.h>

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

void userAppInit(void)
{
    /* res/ (UI documents, fonts, images) ships in the NRO's romfs, and the
       port opens it through paths relative to the working directory. */
    romfsInit();
    chdir("romfs:/");

    /* Netplay, the online companion and RetroAchievements use BSD sockets. */
    socketInitializeDefault();
    /* When started with nxlink, stdout and stderr (OSReport, logs) go to the
       host; otherwise this fails quietly. */
    nxlinkStdio();

    /* SDL's preference path (config, saves, caches) follows XDG_DATA_HOME. */
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/partyboard", 0777);
    setenv("XDG_DATA_HOME", "sdmc:/switch/partyboard", 0);
}

void userAppExit(void)
{
    socketExit();
    romfsExit();
}
