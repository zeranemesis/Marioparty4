#ifndef _PORT_CRASH_UPLOADER_H
#define _PORT_CRASH_UPLOADER_H

/* Where a crash report goes once the player has agreed to send it.
 *
 * PartyBoard does not send anything, and this header is the shape of that
 * decision rather than a workaround for it. The game prepares a submission: a
 * directory holding exactly the files the player consented to, already
 * sanitised. Something else — a separate uploader program, or the player
 * attaching the folder to an issue by hand — does the sending.
 *
 * That is why the interface is a struct of function pointers with a default
 * implementation that writes a folder and returns. There is no token here, no
 * URL, and no place to put either. A backend can be added later by registering
 * a different implementation from a separate binary; nothing in the game has to
 * change, and nothing in the game has to hold a secret.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PartyBoardCrashUploader {
    /* Shown to the player, so they know where their report is going. */
    const char *name;
    /* False when this uploader cannot run right now. The player is not offered
     * a send that cannot happen. */
    bool (*available)(void);
    /* Sends, or prepares, one incident. Returns false and fills `error` on
     * failure; the incident then stays FAILED in the queue and is retryable. */
    bool (*submit)(const char *incidentDirectory, char *error, size_t errorCapacity);
} PartyBoardCrashUploader;

/* The uploader in force. Never null: the default one exports a folder. */
const PartyBoardCrashUploader *PartyBoard_CrashUploader(void);
void PartyBoard_CrashSetUploader(const PartyBoardCrashUploader *uploader);

/* Submits every incident the player marked READY, and moves each to SENT or
 * FAILED. Returns the number submitted. Never touches PENDING or DECLINED. */
unsigned PartyBoard_CrashSubmitReady(void);

/* The default implementation, exposed so a test can name it. It copies the
 * consented files into <incident>/submission/ and writes nothing else; the
 * minidump is copied only when consent for it was given separately. */
bool PartyBoard_CrashExportSubmission(const char *incidentDirectory, char *error,
    size_t errorCapacity);

bool PartyBoard_CrashUploaderRunSelfTest(void);

#ifdef __cplusplus
}
#endif

#endif
