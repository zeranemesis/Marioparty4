/*
 * Standalone runner for the crash manifest and uploader self-tests.
 *
 * Both modules depend on nothing from the game — the standard library, the
 * filesystem and a JSON header — so they can be built and run on their own,
 * including while a campaign is holding dol.dll open. The same self-tests also
 * run inside partyboard.exe under --netplay-self-test; this exists so a change
 * to the privacy or consent rules can be checked in seconds rather than after
 * a full link.
 */

#include "port/crash_manifest.h"
#include "port/crash_uploader.h"

#include <cstdio>

int main(void)
{
    int failures = 0;

    std::printf("--- crash manifest ---\n");
    if (!PartyBoard_CrashManifestRunSelfTest()) ++failures;

    std::printf("--- crash uploader ---\n");
    if (!PartyBoard_CrashUploaderRunSelfTest()) ++failures;

    if (failures != 0) {
        std::printf("\n%d self-test(s) failed.\n", failures);
        return 1;
    }
    std::printf("\nBoth passed.\n");
    return 0;
}
