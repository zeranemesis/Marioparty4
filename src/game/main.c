#include "game/data.h"
#include "game/dvd.h"
#include "game/gamework.h"
#include "game/gamework_data.h"
#include "game/hsfformat.h"
#include "game/hu3d.h"
#include "game/init.h"
#include "game/minigame_seq.h"
#include "game/msm.h"
#include "game/object.h"
#include "game/pad.h"
#include "game/perf.h"
#include "game/printfunc.h"
#include "game/process.h"
#include "game/sprite.h"
#include "game/sreset.h"
#include "game/wipe.h"
#include "version.h"

#ifdef TARGET_PC
#include "game/disp.h"
#include "port/settings.h"
#include "port/imgui.h"
#include "port/frame_interpolation.h"
#include "port/main.h"
#include "port/rollback.h"
#include "port/rollback_clock.h"
#include "port/netplay_runtime.h"
#include "port/dolassets.h"
#include "port/ui.h"
#include "aurora/dvd.h"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <stdlib.h>

const char *__asan_default_options()
{
    return "new_delete_type_mismatch=0,sleep_before_dying=5,allocator_may_return_null=1";
}

bool PartyBoard_IsRunning = TRUE;
bool PartyBoard_IsShuttingDown = FALSE;
bool PartyBoard_IsGameLaunched = FALSE;
bool PartyBoard_RestartRequested = FALSE;
bool PartyBoard_IsSimulationTick = TRUE;
int PartyBoard_SimulationTicksThisFrame = 1;

bool disableFrameLimiter = FALSE;
#endif

extern FileListEntry _ovltbl[];
SHARED_SYM u32 GlobalCounter;
static u32 vcheck;
static u32 vmiss;
static u32 vstall;
static u32 top_pixels_in;
static u32 top_pixels_out;
static u32 bot_pixels_in;
static u32 bot_pixels_out;
static u32 clr_pixels_in;
static u32 total_copy_clks;
static u32 cp_req;
static u32 tc_req;
static u32 cpu_rd_req;
static u32 cpu_wr_req;
static u32 dsp_req;
static u32 io_req;
static u32 vi_req;
static u32 pe_req;
static u32 rf_req;
static u32 fi_req;
s32 HuDvdErrWait;
SHARED_SYM s32 SystemInitF;

void HuSysVWaitSet(s16 vcount);
s16 HuSysVWaitGet(s16 param);

#ifdef TARGET_PC
void PartyBoard_RequestRestart(void)
{
    PartyBoard_RestartRequested = SUPPORTS_PROCESS_RESTART;
    PartyBoard_IsRunning = FALSE;
}

static void PartyBoard_RunGameLogicTick(void)
{
    PartyBoard_IsSimulationTick = TRUE;
    PartyBoard_FrameInterpolationBeginSimulation();
    pfClsScr();
    HuPrcCall(1);
    MGSeqMain();
    PartyBoard_FrameInterpolationEndSimulation();
}

bool PartyBoard_RollbackRunGameLogicTick(const PartyBoardRollbackInput inputs[4], u8 connectedMask)
{
    if (!PartyBoard_RollbackApplyPads(inputs, connectedMask)) return false;
    PartyBoard_RunGameLogicTick();
    return true;
}
#endif

#ifdef TARGET_PC
int game_main(void)
#else
void main(void)
#endif
{
    u32 met0;
    u32 met1;
    s16 i;
    s32 retrace;
#ifdef TARGET_PC
    int simulationTicks;
    int simulationTickIndex;
    int simulatedTicks;
    int finalizedTicks;
    bool simulationAllowed;
    bool netplayErrorShown = false;
    s16 previousVCount;
#endif
#if VERSION_PAL
    s16 temp = 0;
#endif

    HuDvdErrWait = 0;
    SystemInitF = 0;
#if VERSION_NTSC
    HuSysInit(&GXNtsc480IntDf);
#else
    HuSysInit(&GXPal528IntDf);
#endif
    HuPrcInit();
#ifdef TARGET_PC
    if (!PartyBoard_RollbackRunSelfTest()) {
        OSReport("Rollback prototype disabled: deterministic self-test failed.\n");
    }
#endif
    HuPadInit();
    GWInit();
    pfInit();
    GlobalCounter = 0;
    HuSprInit();
    Hu3DInit();
    HuDataInit();
    HuPerfInit();
    HuPerfCreate("USR0", 0xFF, 0xFF, 0xFF, 0xFF);
    HuPerfCreate("USR1", 0, 0xFF, 0xFF, 0xFF);
    WipeInit(RenderMode);

    for (i = 0; i < 4; i++) {
        GWPlayerCfg[i].character = -1;
    }
    
    omMasterInit(0, _ovltbl, DLL_MAX, DLL_bootdll);
    VIWaitForRetrace();

    if (VIGetNextField() == 0) {
        OSReport("VI_FIELD_BELOW\n");
        VIWaitForRetrace();
    }
#ifdef TARGET_PC
    if (!PartyBoard_OnlineWaitForStart()) PartyBoard_IsRunning = FALSE;
    while (PartyBoard_IsRunning) {
#else
    while (1) {
#endif
#ifdef TARGET_PC
        const AuroraEvent *event = aurora_update();
        bool exiting = false;
        while (event != NULL && event->type != AURORA_NONE) {
            if (event->type == AURORA_EXIT) {
                exiting = true;
                break;
            }
            if (event->type == AURORA_PAUSED || event->type == AURORA_UNPAUSED) {
                // Discard wall-clock time spent paused. Otherwise the first
                // visible frame can try to catch up and skip a transition.
                frame_pacer_reset();
            }
            if (event->type == AURORA_SDL_EVENT) {
                ui_handle_sdl_event(&event->sdl);
                if (partyboard_settings_enableTurboKeybind()) {
                    if (event->sdl.type == SDL_EVENT_KEY_DOWN) {
                        if (event->sdl.key.scancode == SDL_SCANCODE_TAB) {
                            disableFrameLimiter = TRUE;
                        }
                    } else if (event->sdl.type == SDL_EVENT_KEY_UP) {
                        if (event->sdl.key.scancode == SDL_SCANCODE_TAB) {
                            disableFrameLimiter = FALSE;
                        }
                    }
                }
            }
            ++event;
        }
        if (exiting) {
            break;
        }
#endif
        retrace = VIGetRetraceCount();
        if (HuSoftResetButtonCheck() != 0 || HuDvdErrWait != 0) {
            continue;
        }
        HuPerfZero();

        HuPerfBegin(2);
#ifdef TARGET_PC
        if (!aurora_begin_frame()) {
            frame_pacer_reset();
            continue;
        }
        /* aurora_begin_frame can wait for a render slot when the selected FPS
         * exceeds GPU throughput. Refresh SDL after that wait; the scheduled
         * 60 Hz tick polls PAD from this fresh state immediately before the
         * game consumes it. Events remain queued for aurora_update next loop. */
        SDL_PumpEvents();
        simulationTicks = frame_pacer_simulation_tick();
        simulatedTicks = 0;
        finalizedTicks = 0;
        PartyBoard_SimulationTicksThisFrame = 0;
        PartyBoard_IsSimulationTick = FALSE;
        PartyBoard_FrameInterpolationSetEnabled(frame_pacer_interpolation_enabled());
#endif
        HuSysBeforeRender();
        GXSetGPMetric(GX_PERF0_CLIP_VTX, GX_PERF1_VERTICES);
        GXClearGPMetric();
        GXSetVCacheMetric(GX_VC_ALL);
        GXClearVCacheMetric();
        GXClearPixMetric();
        GXClearMemMetric();

        HuPerfBegin(0);
        Hu3DPreProc();
#ifdef TARGET_PC
        HuSysVWaitSet(1);
        for (simulationTickIndex = 0; simulationTickIndex < simulationTicks;
             ++simulationTickIndex) {
            if (simulatedTicks != 0) {
                // Publish the preceding tick before the next checkpoint is
                // captured by PAD/netplay polling.
                msmMusFdoutEnd();
                GlobalCounter++;
                finalizedTicks++;
            }
            simulationAllowed = HuPadPollSimulationTick();
            if (simulationAllowed) {
#endif
                HuPadRead();
#ifdef TARGET_PC
                PartyBoard_RunGameLogicTick();
                PartyBoard_NetplayCommitTick();
                simulatedTicks++;
#else
                pfClsScr();
                HuPrcCall(1);
                MGSeqMain();
#endif
#ifdef TARGET_PC
            }
#endif
#ifdef TARGET_PC
            // Consume the scheduled wall-clock tick even while netplay waits.
            // Otherwise the frame pacer accumulates an ever-growing catch-up
            // debt and races the game after the remote input finally arrives.
            frame_pacer_commit_simulation_tick();
        }
        PartyBoard_SimulationTicksThisFrame = simulatedTicks;
        PartyBoard_IsSimulationTick = simulatedTicks != 0;
        PartyBoard_FrameInterpolationSetStep(frame_pacer_interpolation_step());
        previousVCount = HuSysVWaitGet(0);
        HuSysVWaitSet((s16)simulatedTicks);
#endif
        HuPerfBegin(1);
        Hu3DExec();
        if (
#ifdef TARGET_PC
            PartyBoard_IsSimulationTick
#else
            TRUE
#endif
        ) {
            HuDvdErrorWatch();
        }
        WipeExecAlways();
        HuPerfEnd(0);

        pfDrawFonts();
#ifdef TARGET_PC
        HuSysVWaitSet(previousVCount);
#endif
        HuPerfEnd(1);

        if (
#ifdef TARGET_PC
            PartyBoard_IsSimulationTick && finalizedTicks < simulatedTicks
#else
            TRUE
#endif
        ) {
            msmMusFdoutEnd();
        }
        HuSysDoneRender(retrace);
        GXReadGPMetric(&met0, &met1);
        GXReadVCacheMetric(&vcheck, &vmiss, &vstall);
        GXReadPixMetric(&top_pixels_in, &top_pixels_out, &bot_pixels_in, &bot_pixels_out, &clr_pixels_in, &total_copy_clks);
        GXReadMemMetric(&cp_req, &tc_req, &cpu_rd_req, &cpu_wr_req, &dsp_req, &io_req, &vi_req, &pe_req, &rf_req, &fi_req);
        HuPerfEnd(2);
        if (
#ifdef TARGET_PC
            PartyBoard_IsSimulationTick && finalizedTicks < simulatedTicks
#else
            TRUE
#endif
        ) {
            GlobalCounter++;
        }

#ifdef TARGET_PC
        static bool netplayWaitingShown = false;
        if (PartyBoard_NetplayWaiting() && !netplayWaitingShown) {
            ui_push_toast("info", "En attente de l'autre joueur", "La partie est en pause. Reprise automatique si la synchronisation revient avant 2 minutes.", 8000);
            netplayWaitingShown = true;
        } else if (netplayWaitingShown && !PartyBoard_NetplayWaiting() && !PartyBoard_NetplayHasError()) {
            ui_push_toast("info", "Connexion retablie", "La partie reprend.", 3000);
            netplayWaitingShown = false;
        }
        if (!netplayErrorShown && PartyBoard_NetplayHasError()) {
            ui_push_toast("error", "Online session stopped", PartyBoard_NetplayError(), 60000);
            netplayErrorShown = true;
        }
        ui_update();
        aurora_end_frame();
        if (!disableFrameLimiter || PartyBoard_NetplayEnabled()) {
            frame_limiter();
        }
#endif
    }

#ifdef TARGET_PC
    return 0;
#endif
}

void HuSysVWaitSet(s16 vcount)
{
    minimumVcount = vcount;
    minimumVcountf = vcount;
}

s16 HuSysVWaitGet(s16 param)
{
    return (s16)minimumVcount;
}

s32 rnd_seed = 0x0000D9ED;

s32 rand8(void)
{
    rnd_seed = (rnd_seed * 0x41C64E6D) + 0x3039;
    return (u8)(((rnd_seed + 1) >> 16) & 0xFF);
}

#ifdef TARGET_PC
s32 rand8_state_get(void)
{
    return rnd_seed;
}

void rand8_state_set(s32 state)
{
    rnd_seed = state;
}

extern u32 frand_state_get(void);
extern void frand_state_set(u32 state);

void PartyBoard_RollbackClockSave(PartyBoardRollbackClock *state) {
    if (!state) return;
    state->version = 1;
    state->frandSeed = frand_state_get();
    state->rand8Seed = rnd_seed;
    state->globalCounter = GlobalCounter;
}

BOOL PartyBoard_RollbackClockLoad(const PartyBoardRollbackClock *state) {
    if (!state || state->version != 1) return FALSE;
    frand_state_set(state->frandSeed);
    rnd_seed = state->rand8Seed;
    GlobalCounter = state->globalCounter;
    return TRUE;
}

BOOL PartyBoard_RollbackClockSelfTest(void) {
    PartyBoardRollbackClock saved, changed, observed;
    BOOL ok;
    PartyBoard_RollbackClockSave(&saved);
    changed = saved;
    changed.frandSeed ^= 0x12345678u;
    changed.rand8Seed ^= 0x1234;
    changed.globalCounter ^= 0xabcdefu;
    ok = PartyBoard_RollbackClockLoad(&changed);
    PartyBoard_RollbackClockSave(&observed);
    ok = ok && observed.frandSeed == changed.frandSeed
        && observed.rand8Seed == changed.rand8Seed && observed.globalCounter == changed.globalCounter;
    changed.version = 0;
    ok = ok && !PartyBoard_RollbackClockLoad(&changed);
    PartyBoard_RollbackClockSave(&observed);
    ok = ok && observed.frandSeed == changed.frandSeed
        && observed.rand8Seed == changed.rand8Seed && observed.globalCounter == changed.globalCounter;
    PartyBoard_RollbackClockLoad(&saved);
    PartyBoard_RollbackClockSave(&observed);
    return ok && observed.frandSeed == saved.frandSeed
        && observed.rand8Seed == saved.rand8Seed && observed.globalCounter == saved.globalCounter;
}
#endif

#ifdef TARGET_PC
#include "port/rollback_scene.h"
bool PartyBoard_RollbackClockRegions(PartyBoardRollbackRegionSink sink, void *context)
{
    if (!sink) return false;
#define REGION(value) if (!sink(context, &(value), sizeof(value))) return false;
    REGION(rnd_seed) REGION(GlobalCounter) REGION(minimumVcount) REGION(minimumVcountf)
#undef REGION
    return PartyBoard_RollbackRandomRegions(sink, context);
}
#endif
