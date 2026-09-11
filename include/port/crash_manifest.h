#ifndef _PORT_CRASH_MANIFEST_H
#define _PORT_CRASH_MANIFEST_H

/* The tester-facing half of crash reporting: a machine-readable manifest, a
 * bounded local queue, and consent.
 *
 * docs/crash_report_user_pipeline.md holds the format and the reasoning. The
 * three boundaries that shape this file:
 *
 *   PartyBoard never sends anything. It writes files. A separate uploader reads
 *   the queue, and it is the only thing that touches a network.
 *
 *   No secret ships with the game. There is no token here, and no place to put
 *   one.
 *
 *   Nothing leaves the machine without a yes, given per incident and separately
 *   for the report and for the minidump.
 *
 * Everything written here is sanitised first: a Windows user name, a personal
 * path, an ISO location and the like are rewritten before they can reach a
 * file that is meant to be sent to someone else.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the player has agreed to for one incident. Both start false, and they
 * are asked for separately: the minidump carries thread stacks and the memory
 * they point at, which the text report does not. */
typedef struct PartyBoardCrashConsent {
    bool report;
    bool minidump;
    bool asked;
} PartyBoardCrashConsent;

/* Queue state of one incident. DECLINED is kept on disk, never resent, and
 * never asked about again. */
typedef enum PartyBoardCrashQueueState {
    PARTYBOARD_CRASH_PENDING = 0,
    PARTYBOARD_CRASH_DECLINED = 1,
    PARTYBOARD_CRASH_READY = 2,
    PARTYBOARD_CRASH_SENT = 3,
    PARTYBOARD_CRASH_FAILED = 4
} PartyBoardCrashQueueState;

/* Rewrites a string so it can be shown to someone else: the Windows user name
 * becomes <user>, a path under the profile becomes %LOCALAPPDATA% or
 * C:\Users\<user>\..., and the install directory becomes <install>. Returns the
 * number of characters written, excluding the terminator. Safe on any string,
 * including one with no path in it at all. */
size_t PartyBoard_CrashSanitizeText(const char *input, char *output, size_t capacity);

/* Builds the fingerprint for an incident from facts that are the same on every
 * machine. Never a pid, an ASLR address, a timestamp or a handle. `symbol`,
 * `source` and `owningProcess` may be NULL when unresolved. */
size_t PartyBoard_CrashBuildFingerprint(const char *defectClass, int32_t gameContext,
    const char *symbol, const char *source, const char *module, const char *moduleOffset,
    const char *owningProcess, char *output, size_t capacity);

/* Writes manifest.json for the incident whose report is `reportPath`. The
 * report is parsed for the facts, so the manifest can never disagree with it.
 * `directory` is the incident directory; NULL means beside the report.
 * Returns false and writes nothing if the report cannot be read. */
bool PartyBoard_CrashWriteManifest(const char *reportPath, const char *minidumpPath,
    const char *directory);

/* The queue. Bounded so a crash loop cannot fill a disk: at most 20 incidents
 * and 200 MB, oldest SENT first, then oldest DECLINED. A READY incident is
 * never dropped to make room. */
bool PartyBoard_CrashQueueAdd(const char *incidentDirectory);
bool PartyBoard_CrashQueueSetState(const char *incidentDirectory,
    PartyBoardCrashQueueState state);
bool PartyBoard_CrashQueueSetConsent(const char *incidentDirectory,
    const PartyBoardCrashConsent *consent, const char *userNote);
/* How many incidents are waiting to be asked about. Zero is the normal case and
 * costs one directory read at startup. */
uint32_t PartyBoard_CrashQueuePendingCount(void);
bool PartyBoard_CrashQueuePrune(void);

bool PartyBoard_CrashManifestRunSelfTest(void);

#ifdef __cplusplus
}
#endif

#endif
