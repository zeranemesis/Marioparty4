/* Included after the private globals of minigame_seq.c. Read-only lookup
 * tables are omitted. Mutable work, font handles and pause state are covered. */
#ifdef TARGET_PC
#include "port/rollback_sequence.h"
#include "game/init.h"

#define SEQ_SNAPSHOT_FIELDS(X) \
    X(mgSeqOvlPrev) X(seqWorkData) X(seqTimer) X(seqDoneF) X(lbl_801D3D94) \
    X(seqSpeed) X(seqPauseF) X(seqRecordVal) X(pauseWaitF) X(pauseExitF) \
    X(pauseActiveF) X(pauseProcess) X(seqLanguage) X(seqType2SprTbl) \
    X(letterBufTbl) X(winnerNameW)
#define SEQ_COUNT(value) + 1
#define SEQ_BYTES(value) + sizeof(value)
enum { SEQ_FIELD_COUNT = 0 SEQ_SNAPSHOT_FIELDS(SEQ_COUNT) };
typedef struct SequenceSnapshot {
    u32 magic, version, total_size, pointer_size;
    u32 field_sizes[SEQ_FIELD_COUNT];
    u8 payload[0 SEQ_SNAPSHOT_FIELDS(SEQ_BYTES)];
} SequenceSnapshot;
#undef SEQ_COUNT
#undef SEQ_BYTES

size_t PartyBoard_RollbackSequenceSize(void) { return sizeof(SequenceSnapshot); }

BOOL PartyBoard_RollbackSequenceSave(void *destination, size_t capacity)
{
    SequenceSnapshot saved = {0};
    u8 *cursor = saved.payload;
    size_t index = 0;
    if (!destination || capacity != sizeof(saved)) return FALSE;
    saved.magic = 0x53455153u; /* SEQS */
    saved.version = 1;
    saved.total_size = sizeof(saved);
    saved.pointer_size = sizeof(void *);
#define SEQ_SAVE(value) do { \
    saved.field_sizes[index++] = sizeof(value); \
    memcpy(cursor, &(value), sizeof(value)); cursor += sizeof(value); \
} while (0);
    SEQ_SNAPSHOT_FIELDS(SEQ_SAVE)
#undef SEQ_SAVE
    memcpy(destination, &saved, sizeof(saved));
    return TRUE;
}

BOOL PartyBoard_RollbackSequenceLoad(const void *source, size_t size)
{
    SequenceSnapshot saved;
    const u8 *cursor;
    size_t index = 0;
    if (!source || size != sizeof(saved)) return FALSE;
    memcpy(&saved, source, sizeof(saved));
    if (saved.magic != 0x53455153u || saved.version != 1
        || saved.total_size != sizeof(saved) || saved.pointer_size != sizeof(void *)) return FALSE;
#define SEQ_VALIDATE(value) if (saved.field_sizes[index++] != sizeof(value)) return FALSE;
    SEQ_SNAPSHOT_FIELDS(SEQ_VALIDATE)
#undef SEQ_VALIDATE
    /* Validate every field before writing any live state. The local copy also
     * keeps a source buffer alias from changing beneath subsequent reads. */
    cursor = saved.payload;
#define SEQ_LOAD(value) do { memcpy(&(value), cursor, sizeof(value)); cursor += sizeof(value); } while (0);
    SEQ_SNAPSHOT_FIELDS(SEQ_LOAD)
#undef SEQ_LOAD
    return TRUE;
}

BOOL PartyBoard_RollbackSequenceSelfTest(void)
{
    SequenceSnapshot original, start, expected, observed, damaged;
    const s16 oldPause = Hu3DPauseF;
    const u32 oldWait = minimumVcount;
    const float oldWaitF = minimumVcountf;
    BOOL ok = PartyBoard_RollbackSequenceSave(&original, sizeof(original));
    int tick, repeat;
    extern void HuSysVWaitSet(s16 vcount);
    if (!ok) return FALSE;
    memset(seqWorkData, 0, sizeof(seqWorkData));
    seqTimer = 30;
    seqDoneF = 0;
    seqPauseF = 0;
    HuSysVWaitSet(1);
    Hu3DPauseF = 0;
    /* Use the real timer callback in its hidden state. Clamping >99 and
     * clearing the parameter exercise the actual callback without resources,
     * sprite drawing, sound playback or a synthetic replacement callback. */
    seqWorkData[0].seq_no = MG_SEQ_TYPE_TIMER;
    seqWorkData[0].stat = 4;
    seqWorkData[0].state = 1;
    seqWorkData[0].param[0] = 1;
    seqWorkData[0].param[1] = 200;
    ok &= PartyBoard_RollbackSequenceSave(&start, sizeof(start));
    for (repeat = 0; repeat < 4; ++repeat) {
        ok &= PartyBoard_RollbackSequenceLoad(&start, sizeof(start));
        for (tick = 0; tick < 40; ++tick) {
            Hu3DPauseF = tick >= 10 && tick < 20;
            MGSeqMain();
            ok &= seqWorkData[0].timer_val == 99 && seqWorkData[0].param[0] == 0;
            ok &= MGSeqDoneCheck() == (tick == 39);
        }
        ok &= seqTimer == 0;
        ok &= PartyBoard_RollbackSequenceSave(&observed, sizeof(observed));
        if (repeat == 0) expected = observed;
        else ok &= memcmp(&observed, &expected, sizeof(observed)) == 0;
    }
    /* A malformed last field must reject before even the first field changes. */
    damaged = start;
    damaged.field_sizes[SEQ_FIELD_COUNT - 1] ^= 1;
    ok &= !PartyBoard_RollbackSequenceLoad(&damaged, sizeof(damaged));
    ok &= !PartyBoard_RollbackSequenceLoad(&start, sizeof(start) - 1);
    damaged = start; damaged.magic ^= 1;
    ok &= !PartyBoard_RollbackSequenceLoad(&damaged, sizeof(damaged));
    ok &= PartyBoard_RollbackSequenceSave(&observed, sizeof(observed))
        && memcmp(&observed, &expected, sizeof(observed)) == 0;
    ok &= PartyBoard_RollbackSequenceLoad(&original, sizeof(original));
    ok &= PartyBoard_RollbackSequenceSave(&observed, sizeof(observed))
        && memcmp(&observed, &original, sizeof(observed)) == 0;
    Hu3DPauseF = oldPause;
    minimumVcount = oldWait;
    minimumVcountf = oldWaitF;
    OSReport("Native minigame sequences: %s (4 x 40 ticks, actual timer callback, pause/resume, atomic rejection, %zu bytes). No graphical minigame.\n",
        ok ? "PASS" : "FAIL", sizeof(original));
    return ok;
}
#include "port/rollback_scene.h"
bool PartyBoard_RollbackSequenceRegions(PartyBoardRollbackRegionSink sink, void *context)
{
    if (!sink) return false;
#define REGION(value) if (!sink(context, &(value), sizeof(value))) return false;
    SEQ_SNAPSHOT_FIELDS(REGION)
#undef REGION
    return true;
}
void PartyBoard_CheckpointSequenceFixture(void)
{
    memset(seqWorkData, 0, sizeof(seqWorkData));
    seqTimer = 30; seqDoneF = 0; seqPauseF = 0; lbl_801D3D94 = 0;
    seqWorkData[0].seq_no = MG_SEQ_TYPE_TIMER;
    seqWorkData[0].stat = 4; seqWorkData[0].state = 1;
    seqWorkData[0].param[0] = 1; seqWorkData[0].param[1] = 200;
}
#undef SEQ_SNAPSHOT_FIELDS
#endif
