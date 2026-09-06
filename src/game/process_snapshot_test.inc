#ifdef TARGET_PC
#include <stdlib.h>

static u32 snapshotTestInput;
static u32 snapshotTestOutput[2];

static void SnapshotTestWorkerA(void)
{
    volatile u32 accumulated = 17;
    while (1) {
        accumulated = accumulated * 33u + snapshotTestInput;
        snapshotTestOutput[0] = accumulated;
        HuPrcSleep(1 + (snapshotTestInput & 3));
    }
}

static void SnapshotTestWorkerB(void)
{
    volatile u32 accumulated = 91;
    while (1) {
        accumulated ^= (accumulated << 7) + snapshotTestInput;
        snapshotTestOutput[1] = accumulated;
        HuPrcVSleep();
    }
}

static void SnapshotTestAdvance(Process *second, u32 first, u32 end)
{
    u32 tick;
    for (tick = first; tick < end; ++tick) {
        snapshotTestInput = tick * 5 + tick / 3;
        second->stat = tick % 13 < 4 ? PROCESS_STAT_PAUSE : 0;
        HuPrcCall(1);
    }
}

BOOL HuPrcSnapshotExecutionSelfTest(void)
{
    Process a = {0}, b = {0}, savedA, savedB, expectedA, expectedB;
    u8 heapA[8] = {0}, heapB[8] = {0};
    u8 *snapshot = NULL;
    const cothread_t previousScheduler = processthread;
    const u8 previousArg = thread_arg;
    const u64 previousGeneration = processTopologyGeneration;
    size_t size = 0;
    u32 savedOutput[2], expectedOutput[2];
    BOOL ok = TRUE;
    int repeat;
    /* Never replace a running game's scheduler. Stack and process fixtures
     * use the real libco, HuPrcCall, sleep and pause implementations. */
    if (processtop != NULL || processcur != NULL || processcnt != 0) return FALSE;
    a.thread = co_create(65536, SnapshotTestWorkerA);
    b.thread = co_create(65536, SnapshotTestWorkerB);
    if (!a.thread || !b.thread) { ok = FALSE; goto cleanup; }
    a.thread_size = b.thread_size = 65536;
    a.next = &b; b.prev = &a;
    heapA[4] = heapB[4] = 165;
    a.heap = heapA; b.heap = heapB;
    processtop = &a; processcnt = 2;
    snapshotTestOutput[0] = snapshotTestOutput[1] = 0;
    SnapshotTestAdvance(&b, 0, 7);
    size = HuPrcSnapshotSizeGet();
    snapshot = malloc(size);
    if (!snapshot || !HuPrcSnapshotSave(snapshot, size)) { ok = FALSE; goto cleanup; }
    savedA = a; savedB = b;
    memcpy(savedOutput, snapshotTestOutput, sizeof(savedOutput));
    SnapshotTestAdvance(&b, 7, 87);
    expectedA = a; expectedB = b;
    memcpy(expectedOutput, snapshotTestOutput, sizeof(expectedOutput));
    for (repeat = 0; repeat < 3; ++repeat) {
        /* Descriptors normally live in the SYSTEM heap snapshot. Copy them
         * explicitly here because these two fixture descriptors are local. */
        a = savedA; b = savedB;
        memcpy(snapshotTestOutput, savedOutput, sizeof(savedOutput));
        if (!HuPrcSnapshotLoad(snapshot, size)) { ok = FALSE; break; }
        SnapshotTestAdvance(&b, 7, 87);
        ok &= memcmp(snapshotTestOutput, expectedOutput, sizeof(expectedOutput)) == 0;
        ok &= a.exec == expectedA.exec && a.sleep_time == expectedA.sleep_time
            && b.exec == expectedB.exec && b.stat == expectedB.stat;
    }
cleanup:
    free(snapshot);
    if (a.thread) co_delete(a.thread);
    if (b.thread) co_delete(b.thread);
    processtop = processcur = NULL; processcnt = 0;
    processthread = previousScheduler; thread_arg = previousArg;
    ok &= processTopologyGeneration == previousGeneration;
    OSReport("Native scheduler replay: %s (2 real coroutines, 3 x 80 replayed ticks, sleep/pause/local-stack state, %zu bytes). Fixture tasks.\n",
        ok ? "PASS" : "FAIL", size);
    return ok;
}
#endif

#ifdef TARGET_PC
#include "port/rollback_scene.h"
static void CheckpointWorker(void)
{
    u32 *state = HuPrcCurrentGet()->user_data;
    volatile u32 local = 17;
    while (1) {
        local = local * 33u + state[0];
        state[1] = local;
        HuPrcSleep(1 + (state[0] & 3));
    }
}
static void CheckpointWorkerEveryTick(void)
{
    u32 *state = HuPrcCurrentGet()->user_data;
    volatile u32 local = 91;
    while (1) {
        local ^= (local << 7) + state[0];
        state[2] = local;
        HuPrcVSleep();
    }
}
/* Test-only harness: real stacks and scheduler, descriptors in the supplied
 * native arena. No running game or global heap root is replaced. */
bool PartyBoard_CheckpointWithFixtureTasks(void *heap,
    bool (*run)(void *context, u32 *state), void *context)
{
    Process *a = NULL, *b = NULL;
    u32 *state;
    bool ok = false;
    cothread_t oldScheduler = processthread;
    u8 oldArg = thread_arg;
    if (!heap || !run || processtop || processcur || processcnt) return false;
    a = HuMemMemoryAlloc(heap, sizeof(*a), 0);
    b = HuMemMemoryAlloc(heap, sizeof(*b), 0);
    state = HuMemMemoryAlloc(heap, 3 * sizeof(*state), 0);
    if (!a || !b || !state) return false;
    memset(a, 0, sizeof(*a)); memset(b, 0, sizeof(*b)); memset(state, 0, 3 * sizeof(*state));
    a->thread = co_create(65536, CheckpointWorker);
    b->thread = co_create(65536, CheckpointWorkerEveryTick);
    if (!a->thread || !b->thread) goto cleanup;
    a->thread_size = b->thread_size = 65536;
    a->heap = b->heap = heap; a->user_data = b->user_data = state;
    a->next = b; b->prev = a;
    processtop = a; processcnt = 2;
    ++processTopologyGeneration;
    HuPrcCall(1);
    ok = run(context, state);
cleanup:
    if (a->thread) co_delete(a->thread);
    if (b->thread) co_delete(b->thread);
    processtop = processcur = NULL; processcnt = 0;
    processthread = oldScheduler; thread_arg = oldArg;
    ++processTopologyGeneration;
    return ok;
}
#endif
