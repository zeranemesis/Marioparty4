#ifndef _PORT_BOARD_COVERAGE_H
#define _PORT_BOARD_COVERAGE_H

/* Which board mechanics a run actually exercised.
 *
 * Why this exists. docs/netplay_validation_matrix.md has a row per board
 * mechanic - shop, star, Boo, lottery, item, battle, fortune, Bowser, warp,
 * mushroom, block, last-five-turns, dice, CPU - and the only thing deciding
 * whether a row moves from UNTESTED to PARTIAL has been someone's recollection
 * of what happened during a session. That is not evidence. A two-hour game can
 * easily never land on a Bowser space, and nobody would notice; the matrix would
 * be updated anyway, because the session "covered the board".
 *
 * So each mechanic says so itself, the first time it runs. The run's own stdout
 * then carries the list, the harness reads it back, and the matrix is updated
 * from what the game did rather than from what anyone remembers.
 *
 * Cost. One already-set static flag test per call site after the first hit, and
 * nothing else: no allocation, no formatting, no lock. The mark is a print and
 * touches no game state, so it cannot affect determinism - which matters,
 * because this runs inside the synchronized simulation on both peers.
 *
 * Deliberately NOT behind a diagnostic switch. A marker that has to be turned on
 * is a marker that will be off during the one session that mattered.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Names are fixed strings, matched by the harness. Keep them in step with
 * docs/netplay_validation_matrix.md. */
void PartyBoard_BoardCoverageMark(const char *subsystem);

/* Every mechanic seen so far, space-separated, for the crash report and the
 * end-of-run summary. Returns the number of distinct mechanics. */
int PartyBoard_BoardCoverageSummary(char *out, unsigned capacity);

/* One static flag per call site: after the first hit this is a test of a local
 * that is already in cache, which is as close to free as a marker gets. */
#define PARTYBOARD_BOARD_COVERAGE(name)                                                            \
    do {                                                                                           \
        static int partyboardCoverageSeen = 0;                                                     \
        if (!partyboardCoverageSeen) {                                                             \
            partyboardCoverageSeen = 1;                                                            \
            PartyBoard_BoardCoverageMark(name);                                                    \
        }                                                                                          \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif
