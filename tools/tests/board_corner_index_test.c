/*
 * D1 — can `corner` actually reach 4 or more?
 *
 * What this test is. `BoardSpaceCornerPosGet` (src/game/board/space.c:168) reads
 * `corner_pos[corner]` from a local `s8[4][2]`, and `corner` is bounded nowhere.
 * Its caller `fn_1_52A0` (src/REL/w04Dll/boo_event.c:1060) computes that index
 * from two counts. This program is an exact transcription of that arithmetic,
 * and it enumerates every state the two counts can be in.
 *
 * What this test is NOT. It is a model of the caller, not the caller, and
 * certainly not the game. It can prove that the arithmetic reaches an
 * out-of-range index for some occupancy, and it can say which occupancy is
 * needed. It cannot prove that occupancy occurs in a real match. That question
 * belongs to the detector compiled into BoardSpaceCornerPosGet, which across
 * 142 recorded peer sessions has never fired.
 *
 * So a failure here would mean the arithmetic is safe and the register entry is
 * wrong. A pass means the arithmetic is unsafe and the remaining question is
 * reachability, exactly as the register says.
 *
 * The transcribed loop, from boo_event.c:1047-1060:
 *
 *     var_r27 = 0;
 *     for (i = 0; i < 4; i++) {
 *         sp8[i] = GWPlayer[i].space_curr;
 *         if (lbl_1_bss_B0 == sp8[i]) var_r27 += 1;
 *     }
 *     for (i = 0; i < 4; i++) {
 *         var_r30 = fn_1_2FBC(i);
 *         if (var_r30->unk08 != -1 && var_r30->unk08 != GWSystem.player_curr) {
 *             BoardSpaceCornerPosGet(lbl_1_bss_B0, var_r27++, &arg1[var_r30->unk08]);
 *         }
 *     }
 */

#include <stdio.h>
#include <string.h>

#define PLAYERS 4
#define SLOTS 4
#define CORNER_TABLE 4

struct Outcome {
    int maximumCorner;
    int callsOutOfRange;
    int calls;
};

/* One evaluation of the transcribed loop.
 *
 *   spaceOfPlayer[p]   the space player p stands on
 *   targetSpace        lbl_1_bss_B0, the space the event is drawing around
 *   slotOwner[s]       fn_1_2FBC(s)->unk08, a player index or -1
 *   currentPlayer      GWSystem.player_curr
 */
static struct Outcome evaluate(const int spaceOfPlayer[PLAYERS], int targetSpace,
    const int slotOwner[SLOTS], int currentPlayer)
{
    struct Outcome outcome;
    int corner = 0;
    int i;

    outcome.maximumCorner = -1;
    outcome.callsOutOfRange = 0;
    outcome.calls = 0;

    for (i = 0; i < PLAYERS; i++) {
        if (spaceOfPlayer[i] == targetSpace) corner += 1;
    }
    for (i = 0; i < SLOTS; i++) {
        if (slotOwner[i] != -1 && slotOwner[i] != currentPlayer) {
            const int used = corner++;
            outcome.calls += 1;
            if (used > outcome.maximumCorner) outcome.maximumCorner = used;
            if (used < 0 || used >= CORNER_TABLE) outcome.callsOutOfRange += 1;
        }
    }
    return outcome;
}

static int describe(const char *title, const int spaceOfPlayer[PLAYERS], int targetSpace,
    const int slotOwner[SLOTS], int currentPlayer, int expectOutOfRange, int expectMaximum)
{
    const struct Outcome outcome =
        evaluate(spaceOfPlayer, targetSpace, slotOwner, currentPlayer);
    const int ok = outcome.callsOutOfRange == expectOutOfRange
        && outcome.maximumCorner == expectMaximum;
    printf("  %-52s calls=%d max_corner=%2d out_of_range=%d %s\n", title, outcome.calls,
        outcome.maximumCorner, outcome.callsOutOfRange, ok ? "" : "<-- UNEXPECTED");
    return ok;
}

int main(void)
{
    int ok = 1;
    int worstCorner = -1;
    int reachableStates = 0;
    int unsafeStates = 0;
    int minimumOccupantsForUnsafe = PLAYERS + 1;
    int minimumOccupantsDistinctSlots = PLAYERS + 1;
    int worstCornerDistinctSlots = -1;
    int spaceOfPlayer[PLAYERS];
    int slotOwner[SLOTS];
    int mask, slotState, currentPlayer, i;

    printf("D1 corner index — transcription of boo_event.c:1047-1060\n\n");

    printf("named cases\n");

    /* Nobody on the target space, three other players holding slots: the
     * ordinary case, and it stays inside the table. */
    {
        const int spaces[PLAYERS] = {10, 11, 12, 13};
        const int slots[SLOTS] = {1, 2, 3, -1};
        ok &= describe("nobody on the space, three slots owned", spaces, 99, slots, 0, 0, 2);
    }

    /* All four players on the target space, three slots owned by the three
     * players who are not the current one. This is the case the register
     * describes, and every one of its three calls is out of range. */
    {
        const int spaces[PLAYERS] = {42, 42, 42, 42};
        const int slots[SLOTS] = {1, 2, 3, -1};
        ok &= describe("four on the space, three slots owned", spaces, 42, slots, 0, 3, 6);
    }

    /* The boundary: four occupants and a single slot still overflows, because
     * the count starts at four and the table holds four. */
    {
        const int spaces[PLAYERS] = {42, 42, 42, 42};
        const int slots[SLOTS] = {1, -1, -1, -1};
        ok &= describe("four on the space, one slot owned", spaces, 42, slots, 0, 1, 4);
    }

    /* Three occupants and one slot is the last safe state. */
    {
        const int spaces[PLAYERS] = {42, 42, 42, 7};
        const int slots[SLOTS] = {1, -1, -1, -1};
        ok &= describe("three on the space, one slot owned", spaces, 42, slots, 0, 0, 3);
    }

    /* A slot owned by the current player is skipped, so it cannot push the
     * index up. */
    {
        const int spaces[PLAYERS] = {42, 42, 42, 42};
        const int slots[SLOTS] = {0, 0, 0, 0};
        ok &= describe("four on the space, every slot owned by the current player",
            spaces, 42, slots, 0, 0, -1);
    }

    /* Exhaustive enumeration. Occupancy is a 4-bit mask, each slot is one of
     * five values (-1 or a player), and the current player is one of four. */
    printf("\nexhaustive enumeration\n");
    for (mask = 0; mask < 16; mask++) {
        for (i = 0; i < PLAYERS; i++) spaceOfPlayer[i] = (mask & (1 << i)) ? 42 : 7;
        for (slotState = 0; slotState < 5 * 5 * 5 * 5; slotState++) {
            int scratch = slotState;
            for (i = 0; i < SLOTS; i++) {
                slotOwner[i] = (scratch % 5) - 1;
                scratch /= 5;
            }
            for (currentPlayer = 0; currentPlayer < PLAYERS; currentPlayer++) {
                const struct Outcome outcome =
                    evaluate(spaceOfPlayer, 42, slotOwner, currentPlayer);
                int occupants = 0;
                reachableStates++;
                if (outcome.maximumCorner > worstCorner) worstCorner = outcome.maximumCorner;
                for (i = 0; i < PLAYERS; i++) {
                    if (spaceOfPlayer[i] == 42) occupants++;
                }
                /* A second reading of the same enumeration, restricted to slot
                 * tables where no player owns two slots. Whether a player can
                 * is itself open - defect D2 describes the assignment in
                 * fn_1_52A0 overwriting the fourth slot rather than refusing -
                 * so both thresholds are reported instead of one being assumed.
                 */
                {
                    int duplicate = 0;
                    int a, b;
                    for (a = 0; a < SLOTS && !duplicate; a++) {
                        if (slotOwner[a] == -1) continue;
                        for (b = a + 1; b < SLOTS; b++) {
                            if (slotOwner[b] == slotOwner[a]) { duplicate = 1; break; }
                        }
                    }
                    if (!duplicate) {
                        if (outcome.maximumCorner > worstCornerDistinctSlots) {
                            worstCornerDistinctSlots = outcome.maximumCorner;
                        }
                        if (outcome.callsOutOfRange != 0
                            && occupants < minimumOccupantsDistinctSlots) {
                            minimumOccupantsDistinctSlots = occupants;
                        }
                    }
                }
                if (outcome.callsOutOfRange == 0) continue;
                unsafeStates++;
                if (occupants < minimumOccupantsForUnsafe) minimumOccupantsForUnsafe = occupants;
            }
        }
    }

    printf("  states enumerated                        %d\n", reachableStates);
    printf("  states with an out-of-range read         %d\n", unsafeStates);
    printf("  highest index passed to a 4-entry table  %d\n", worstCorner);
    printf("  fewest occupants needed to overflow      %d\n", minimumOccupantsForUnsafe);
    printf("\n  if no player can own two slots:\n");
    printf("    highest index                          %d\n", worstCornerDistinctSlots);
    printf("    fewest occupants needed to overflow    %d\n", minimumOccupantsDistinctSlots);

    /* The arithmetic must be able to overflow, and the thresholds must be the
     * ones below. If any of this stops holding, the register entry for D1 is
     * describing code that no longer exists. */
    if (unsafeStates == 0) {
        printf("\nFAIL: the transcribed arithmetic never leaves the table.\n");
        ok = 0;
    }
    if (worstCorner < CORNER_TABLE) {
        printf("\nFAIL: the highest index stayed inside the table.\n");
        ok = 0;
    }
    /* The condition the register states is occupants + qualifying slots > 4.
     * With four slots and one player excluded as the current one, that makes
     * ONE occupant enough when a player may own two slots, and TWO when not.
     * An earlier version of this test asserted four, which is the most
     * spectacular case rather than the threshold: the test was wrong and the
     * register was right. Both numbers are asserted now, so the next person to
     * guess is corrected by the test instead of by a rerun. */
    if (minimumOccupantsForUnsafe != 1) {
        printf("\nFAIL: expected one occupant to be the threshold, measured %d.\n",
            minimumOccupantsForUnsafe);
        ok = 0;
    }
    if (minimumOccupantsDistinctSlots != 2) {
        printf("\nFAIL: expected two occupants to be the threshold when no player owns two\n"
               "      slots, measured %d.\n", minimumOccupantsDistinctSlots);
        ok = 0;
    }

    printf("\n%s\n", ok
            ? "PASS: the index leaves the table as soon as the occupants of the target space\n"
              "and the slots owned by other players add up to more than four. That needs one\n"
              "occupant if a player may own two slots, two if not - far short of the four the\n"
              "case list makes it look like. Whether either happens in a real match is NOT\n"
              "decided here; the detector in BoardSpaceCornerPosGet decides that, and across\n"
              "142 recorded peer sessions it has never fired."
            : "FAIL");
    return ok ? 0 : 1;
}
