// Meta Quest: the game's 3D world drawn a second time for the headset's eyes,
// as a model standing on the player's table.
//
// Hu3DExec brackets camera 0's 3D layers; each time Hu3DCameraSet computes
// the camera's view matrix, the eyes' clip matrices are made from it, and
// Aurora draws every perspective draw once more per eye with them
// (aurora/stereo.h). An eye's clip matrix takes the game's view space back to
// the game world, then to the room (the model on the table), then to the eye:
//
//   clip = eyeProj * eyeView * world * inverse(gameView)
//
// The headset side (libpartyboard_quest.so, stereo_view.cpp) gives eyeProj,
// eyeView and world for each frame, the shared image to draw into, and takes
// the image back once the GPU has drawn it. On a phone the library is not
// loaded and nothing here does anything.
//
// On a board, the game camera itself is cancelled (the headset is the
// viewpoint) and the board stays as placed on the table. A minigame is seen
// from its camera's point of view, as on TV (quest_xr.cpp,
// StereoView::set_window): its camera is kept, and its view space mapped into
// a window where the game's screen stands, what the camera looks at in the
// window's plane; in 3D in the room by default, or framed by the window,
// each eye looking through it from where it is. The left grip can also put a
// minigame on the table like a board.

extern "C" {
#include "port/quest_stereo.h"
#include "port/netplay_runtime.h"
#include "dolphin/vi.h"
#include "game/object.h"
#include "game/board/space.h"
#include "game/chrman.h"
#include "game/gamework_data.h"
}
#include "port/quest_scene_fit.hpp"
#include "port/settings.h"
#include "ui/ui.hpp"

#include <aurora/stereo.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <cstring>

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <unistd.h>
#endif

namespace {

// One frame, as PartyBoardQuest_StereoFrame fills it (same layout there).
struct QuestStereoFrame {
    uint32_t image;
    uint64_t tag;
    float eyeView[2][16]; // room (STAGE) -> eye, row-major
    float eyeProj[2][16]; // eye -> GX clip space
    float world[16];      // game world -> room
    float hudWidth, hudHeight;
    uint32_t eyeWidth, eyeHeight; // drawn part of each eye's half (dynamic resolution)
    uint32_t generation;          // of the images the lease is in
    uint32_t drawHud;             // 1: draw the interface if it changed, 2: whatever (the last one was lost), 0: not
    uint32_t foveation;           // the eyes' foveation level (0: none)
    uint32_t window;              // 1: through a window (eyeView, eyeProj, world: the window's); 2: open (world)
    float windowHeight;           // the window's height, meters
    float windowDistance;         // open: the player's distance to the window, meters
};

using FrameFn = bool (*)(QuestStereoFrame *frame);
using ImagesFn = bool (*)(void **eyeBuffers, void **hudBuffers, uint32_t capacity, uint32_t *count, uint32_t *width,
    uint32_t *height, uint32_t *generation, uint32_t *hudWidth, uint32_t *hudHeight);
using SubmittedFn = void (*)(uint32_t image, uint64_t tag, int syncFd, bool hasWorld, bool hudDrawn, void *user);
using GenerationFn = uint32_t (*)(void);
using WorldOnlyFn = bool (*)();
using ScreenRequiredFn = void (*)(bool);
using BoardModeFn = void (*)(bool);
using CancelledFn = void (*)(uint32_t image, uint64_t tag);
using ReleaseImagesFn = void (*)(void **buffers, uint32_t count);
using ScreenHiddenFn = bool (*)();
using HudCropFn = void (*)(float x, float y, float width, float height);
using SceneBoundsFn = void (*)(const float *low, const float *high);
using InsetFn = void (*)(float x, float y, float width, float height);
using HudFovFn = void (*)(float tanHalfFov);
using CameraViewFn = void (*)(bool cameraView);
using SceneFn = void (*)(int scene);
using TableSetupHoldFn = bool (*)();
using HiddenAreaFn = uint32_t (*)(uint32_t eye, float *rects, uint32_t capacity, uint32_t *version);
using FoveationAvailableFn = void (*)(bool available);

struct Quest {
    bool looked = false;
    FrameFn frame = nullptr;
    ImagesFn images = nullptr;
    ReleaseImagesFn releaseImages = nullptr; // the references images() gives
    SubmittedFn submitted = nullptr;
    GenerationFn generation = nullptr;
    CancelledFn cancelled = nullptr;
    ScreenRequiredFn screenRequired = nullptr;
    BoardModeFn boardMode = nullptr;
    WorldOnlyFn worldOnly = nullptr;
    ScreenHiddenFn screenHidden = nullptr; // optional: an older library has none
    HudCropFn hudCrop = nullptr;           // optional: the interface's part to show
    SceneBoundsFn sceneBounds = nullptr;   // optional: what the eyes need to draw (cropped eyes)
    InsetFn inset = nullptr;               // optional: a part of the TV picture shown beside the 3D
    HudFovFn hudFov = nullptr;             // optional: the open view's camera field, for the interface's size
    CameraViewFn cameraView = nullptr;     // optional: this scene from its camera's point of view
    SceneFn scene = nullptr;               // optional: which scene (its own resolution)
    TableSetupHoldFn tableSetupHold = nullptr; // optional
    HiddenAreaFn hiddenArea = nullptr;         // optional: every pixel drawn
    FoveationAvailableFn foveationAvailable = nullptr; // optional: no automatic foveation
    uint32_t registeredGeneration = 0;
    uint32_t hiddenVersion = 0;
};

Quest sQuest;
bool sActive = false;
bool sCameraViewSet = false;
// The cameras drawn for the eyes: the first one active (the scene's main
// view), and with a minigame's window any other over the same part of the
// screen (a layer drawn over it), mapped as the first was this frame.
int sCurrentCamera = -1;
bool sSecondary = false;
bool sFrameRecording = false; // the first camera's eyes frame is being recorded
float sWindowScale = 0.0f, sWindowDistance = 0.0f;
QuestStereoFrame sFrame;
OMOVL sScene = DLL_NONE;
bool sSceneFitted = false;
// This frame's eyes look through the window (QuestStereoFrame::window).
bool sWindow = false;
float sSceneScale = 1.0f;
float sSceneCenter[3]{};
// A minigame or a menu on the table, turned about the vertical so the player
// sees it from the side its camera looked from when it started (radians).
float sSceneTurn = 0.0f;
// A minigame or a menu on the table follows what its camera looks at (game
// world units, smoothed): Manta Rings' camera follows the divers along a
// course, and on the table the action left the scene's fixed place
// ("the point of view does not follow", 2026-10-10).
float sFollow[3]{};
bool sFollowSet = false;
// A menu keeps the fit of its first showing for the session: back from a
// minigame its camera was zoomed on its panel, the menu was fitted on that
// and its room stood around the player (2026-10-10).
struct SceneFit {
    float scale;
    float center[3];
    float turn;
};
std::map<int, SceneFit> sMenuFits;
float sEyeClip[2][16]{};
// The game camera's view space -> the room, this camera's (begin_eyes), and
// the room-space box around every object offered for drawing this frame
// (ObjectVisible), before culling: the headset narrows the eyes' fields of
// view to it, next frame (StereoView: cropped eyes). An object entering the
// view widens it at once, as it is counted even when culled.
float sViewToRoom[4][4]{};
float sBoundsLow[3]{INFINITY, INFINITY, INFINITY};
float sBoundsHigh[3]{-INFINITY, -INFINITY, -INFINITY};
// Larger objects (meters, in the room) are left out: a sea or a sky around
// the table would make the box the whole view.
constexpr float kBoundsMaxRadius = 1.5f;
constexpr float kBoundsMaxRadiusOpen = 15.0f;
// Backdrops (see PartyBoard_StereoBackdrop): the eyes in the game camera's view
// space, and the scene's size in game units.
float sEyeInView[2][3]{};
float sSceneExtent = 0.0f;
// A backdrop is an object around the eyes much larger than the scene on the table.
constexpr float kBackdropExtents = 2.5f;
// A minigame in 3D from its camera's point of view: a sky is an object with
// the game camera deep inside (within kOpenSkyCore of its radius from its
// center) and at least this many times the camera's distance to what it
// looks at (a dome, a skybox): skies measured 47 to 84 times; Avalanche!'s
// course 8.6 times seen from its intro's camera, and objects of 12 times
// left holes in Paratrooper Plunge and Manta Rings (2026-10-06).
constexpr float kOpenSkyPerDistance = 25.0f;
constexpr float kOpenSkyCore = 0.5f;
float sOpenSkyRadius = 0.0f;


bool board_scene(OMOVL scene)
{
    return scene >= DLL_w01Dll && scene <= DLL_w06Dll;
}

// The menus before a board, 3D rooms with their own cameras: seen in 3D from
// their camera's point of view like a minigame (asked for 2026-10-09). Their
// sprites (cursors, panels, text) are the interface over the screen.
// `adb shell setprop debug.partyboard.menus3d 0` keeps them flat, read again
// every 2 s.
bool menu_scene(OMOVL scene)
{
    const bool menu = scene == DLL_modeseldll || scene == DLL_mentDll || scene == DLL_mgmodedll
        || scene == DLL_option || scene == DLL_present || scene == DLL_selmenuDLL;
    if (!menu) return false;
#if defined(__ANDROID__)
    static bool enabled = true;
    static auto readAt = std::chrono::steady_clock::time_point {};
    const auto now = std::chrono::steady_clock::now();
    if (now - readAt >= std::chrono::seconds(2)) {
        readAt = now;
        char value[PROP_VALUE_MAX] = {};
        __system_property_get("debug.partyboard.menus3d", value);
        const bool wanted = value[0] != '0';
        if (wanted != enabled) {
            __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest", "Menus %s (debug.partyboard.menus3d)",
                wanted ? "in 3D" : "flat");
        }
        enabled = wanted;
    }
    return enabled;
#else
    return true;
#endif
}

// The size of a minigame or a menu on the table, times its camera's fit.
// Each menu has its own: its camera's fit says little of its room's size.
// The minigame menu's room surrounded the player, filled both eyes and cost
// 10 ms of GPU an image for 61 draws (2026-10-10); at 0.15, chosen in the
// headset, it stands on the table as a model (6 ms); the mode selection was
// too small at that size. Read every 2 s, to settle them by eye:
// `adb shell setprop debug.partyboard.size_74 0.5` (the scene's number, as
// "Scene N" in the log), `menu_size` and `minigame_size` for the others.
float table_size(OMOVL scene)
{
    if (board_scene(scene)) return 1.0f;
    const bool menu = menu_scene(scene);
    static OMOVL readScene = DLL_NONE;
    static float size = 1.0f;
#if defined(__ANDROID__)
    static auto readAt = std::chrono::steady_clock::time_point {};
    const auto now = std::chrono::steady_clock::now();
    if (scene != readScene || now - readAt >= std::chrono::seconds(2)) {
        readScene = scene;
        readAt = now;
        const auto read = [](const char *name, float fallback) {
            char value[PROP_VALUE_MAX] = {};
            __system_property_get(name, value);
            const float v = value[0] != 0 ? std::strtof(value, nullptr) : fallback;
            return v > 0.02f && v < 4.0f ? v : fallback;
        };
        // Settled in the headset: the minigame menu, the mode selection.
        float fallback = menu ? 0.5f : 1.0f;
        if (scene == DLL_mgmodedll) fallback = 0.15f;
        if (scene == DLL_modeseldll) fallback = 0.5f;
        fallback = read(menu ? "debug.partyboard.menu_size" : "debug.partyboard.minigame_size", fallback);
        char name[48];
        std::snprintf(name, sizeof(name), "debug.partyboard.size_%d", static_cast<int>(scene));
        const float chosen = read(name, fallback);
        if (chosen != size) {
            __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest", "Scene %d on the table at %.2f of its fit",
                static_cast<int>(scene), chosen);
        }
        size = chosen;
    }
    return size;
#else
    (void)menu;
    return 1.0f;
#endif
}

bool minigame_scene(OMOVL scene)
{
    return scene >= DLL_m401Dll && scene <= DLL_m463Dll;
}

// How a minigame shows: from its camera's point of view, as on TV (behind
// the characters, the camera's framing), or on the TV screen. Not on the
// table: there the player's head replaced the camera and the TV framing was
// lost (taken out 2026-10-10, "I am not behind the characters"); menus and
// boards stay on the table. `adb shell setprop debug.partyboard.view_<scene>
// camera|screen` (the scene's number, as "Scene N" in the log) overrides one
// live, read every 2 s.
enum class SceneView { Camera, Screen };

SceneView minigame_view(OMOVL scene)
{
    // Mario Medley copies its frame into the pictures it is played with: in
    // 3D it was broken, unplayable (2026-10-06, 2026-10-10).
    SceneView view = scene == DLL_m405Dll ? SceneView::Screen : SceneView::Camera;
#if defined(__ANDROID__)
    static OMOVL readScene = DLL_NONE;
    static SceneView chosen = SceneView::Camera;
    static bool overridden = false;
    static auto readAt = std::chrono::steady_clock::time_point {};
    const auto now = std::chrono::steady_clock::now();
    if (scene != readScene || now - readAt >= std::chrono::seconds(2)) {
        readAt = now;
        char name[48];
        std::snprintf(name, sizeof(name), "debug.partyboard.view_%d", static_cast<int>(scene));
        char value[PROP_VALUE_MAX] = {};
        __system_property_get(name, value);
        const bool was = overridden;
        const SceneView before = chosen;
        overridden = true;
        if (std::strcmp(value, "camera") == 0) chosen = SceneView::Camera;
        else if (std::strcmp(value, "screen") == 0) chosen = SceneView::Screen;
        else overridden = false;
        if (overridden && (scene != readScene || !was || before != chosen)) {
            __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest", "Scene %d shown %s (%s)",
                static_cast<int>(scene), value, name);
        }
        readScene = scene;
    }
    if (overridden) view = chosen;
#endif
    return view;
}

bool flat_minigame(OMOVL scene)
{
    return minigame_scene(scene) && minigame_view(scene) == SceneView::Screen;
}

// Seen from its camera's point of view even with the minigames on the table.
bool camera_view_minigame(OMOVL scene)
{
    return minigame_scene(scene) && minigame_view(scene) == SceneView::Camera;
}

// The scenes drawn in 3D: the boards on the table; the minigames and the
// menus from their camera's point of view (or on the table too).
bool spatial_scene(OMOVL scene)
{
    return board_scene(scene) || (minigame_scene(scene) && !flat_minigame(scene)) || menu_scene(scene);
}

// The scene's floor, measured from its geometry during its first frames, is
// what stands on the table: the bottom of the lowest large surface (ground,
// arena floor, the sea around an island), in game units.
constexpr int kFloorFrames = 20;
// A minigame's or a menu's scene is fitted on its camera this many frames in.
constexpr int kFitAfterFrames = 3;
int sSceneFrames = 0;
constexpr float kFloorSpan = 0.3f; // of the scene's extent, for a surface to count
float sFloorY = INFINITY;
int sFloorFrames = 0;
bool sFloorSettled = false;

struct Mat4 {
    float m[4][4];
};

// The game camera's view space -> game world, for the floor measurement.
Mat4 sViewToWorld {};

Mat4 multiply(const Mat4 &a, const Mat4 &b)
{
    Mat4 out {};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c] + a.m[r][3] * b.m[3][c];
        }
    }
    return out;
}

Mat4 from_rows(const float rows[16])
{
    Mat4 out;
    std::memcpy(out.m, rows, sizeof(out.m));
    return out;
}

// The inverse of an affine GX matrix (3x4, column vectors).
Mat4 inverse_affine(Mtx view)
{
    const float a = view[0][0], b = view[0][1], c = view[0][2];
    const float d = view[1][0], e = view[1][1], f = view[1][2];
    const float g = view[2][0], h = view[2][1], i = view[2][2];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    const float s = det != 0.0f ? 1.0f / det : 0.0f;
    Mat4 out {};
    out.m[0][0] = (e * i - f * h) * s;
    out.m[0][1] = (c * h - b * i) * s;
    out.m[0][2] = (b * f - c * e) * s;
    out.m[1][0] = (f * g - d * i) * s;
    out.m[1][1] = (a * i - c * g) * s;
    out.m[1][2] = (c * d - a * f) * s;
    out.m[2][0] = (d * h - e * g) * s;
    out.m[2][1] = (b * g - a * h) * s;
    out.m[2][2] = (a * e - b * d) * s;
    for (int r = 0; r < 3; ++r) {
        out.m[r][3] = -(out.m[r][0] * view[0][3] + out.m[r][1] * view[1][3] + out.m[r][2] * view[2][3]);
    }
    out.m[3][3] = 1.0f;
    return out;
}

bool find_quest()
{
#if defined(__ANDROID__)
    if (!sQuest.looked) {
        sQuest.looked = true;
        // Loaded by the Java side (QuestVr) on a headset only.
        void *lib = dlopen("libpartyboard_quest.so", RTLD_NOW | RTLD_NOLOAD);
        if (lib != nullptr) {
            sQuest.frame = reinterpret_cast<FrameFn>(dlsym(lib, "PartyBoardQuest_StereoFrame"));
            sQuest.images = reinterpret_cast<ImagesFn>(dlsym(lib, "PartyBoardQuest_StereoImages"));
            sQuest.releaseImages = reinterpret_cast<ReleaseImagesFn>(dlsym(lib, "PartyBoardQuest_StereoReleaseImages"));
            sQuest.submitted = reinterpret_cast<SubmittedFn>(dlsym(lib, "PartyBoardQuest_StereoSubmitted"));
            sQuest.generation = reinterpret_cast<GenerationFn>(dlsym(lib, "PartyBoardQuest_StereoGeneration"));
            sQuest.worldOnly = reinterpret_cast<WorldOnlyFn>(dlsym(lib, "PartyBoardQuest_StereoWorldOnly"));
            sQuest.screenRequired = reinterpret_cast<ScreenRequiredFn>(dlsym(lib, "PartyBoardQuest_StereoScreenRequired"));
            sQuest.boardMode = reinterpret_cast<BoardModeFn>(dlsym(lib, "PartyBoardQuest_StereoBoardMode"));
            sQuest.cancelled = reinterpret_cast<CancelledFn>(dlsym(lib, "PartyBoardQuest_StereoCancelled"));
            sQuest.screenHidden = reinterpret_cast<ScreenHiddenFn>(dlsym(lib, "PartyBoardQuest_StereoScreenHidden"));
            sQuest.hudCrop = reinterpret_cast<HudCropFn>(dlsym(lib, "PartyBoardQuest_StereoHudCrop"));
            sQuest.sceneBounds = reinterpret_cast<SceneBoundsFn>(dlsym(lib, "PartyBoardQuest_StereoSceneBounds"));
            sQuest.inset = reinterpret_cast<InsetFn>(dlsym(lib, "PartyBoardQuest_StereoInset"));
            sQuest.hudFov = reinterpret_cast<HudFovFn>(dlsym(lib, "PartyBoardQuest_StereoHudFov"));
            sQuest.cameraView = reinterpret_cast<CameraViewFn>(dlsym(lib, "PartyBoardQuest_StereoCameraView"));
            sQuest.scene = reinterpret_cast<SceneFn>(dlsym(lib, "PartyBoardQuest_StereoScene"));
            sQuest.tableSetupHold = reinterpret_cast<TableSetupHoldFn>(dlsym(lib, "PartyBoardQuest_TableSetupHold"));
            sQuest.hiddenArea = reinterpret_cast<HiddenAreaFn>(dlsym(lib, "PartyBoardQuest_StereoHiddenArea"));
            sQuest.foveationAvailable =
                reinterpret_cast<FoveationAvailableFn>(dlsym(lib, "PartyBoardQuest_StereoFoveationAvailable"));
            // This thread, the game's, named to the headset's runtime as the
            // application's main thread (optional: an older library has none).
            if (auto gameThread = reinterpret_cast<void (*)(int)>(dlsym(lib, "PartyBoardQuest_GameThread"))) {
                gameThread(static_cast<int>(gettid()));
            }
            // The head's pose again once a frame is recorded (optional: an
            // older library has none). Aurora calls it on this thread.
            AuroraStereoSetLateLatch(
                reinterpret_cast<AuroraStereoLateLatch>(dlsym(lib, "PartyBoardQuest_StereoLateLatch")));
            __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest", "Stereo bridge: symbols %s",
                sQuest.frame && sQuest.images && sQuest.releaseImages && sQuest.submitted && sQuest.generation
                    && sQuest.cancelled ? "ready" : "missing");
        } else {
            // SDL can reach a camera while the headset library is still loading.
            sQuest.looked = false;
        }
    }
    return sQuest.frame != nullptr && sQuest.images != nullptr && sQuest.releaseImages != nullptr
        && sQuest.submitted != nullptr
        && sQuest.generation != nullptr && sQuest.cancelled != nullptr && sQuest.screenRequired != nullptr
        && sQuest.worldOnly != nullptr && sQuest.boardMode != nullptr;
#else
    return false;
#endif
}

// The shared images, once and again whenever the headset makes new ones.
// The pixels the lenses never show, whenever the headset has (new) ones:
// Aurora fills them with the nearest depth before each eye's draws.
void update_hidden_area()
{
    uint32_t version = 0;
    if (sQuest.hiddenArea == nullptr) {
        return;
    }
    sQuest.hiddenArea(0, nullptr, 0, &version);
    if (version == sQuest.hiddenVersion) {
        return;
    }
    static float rects[512 * 4];
    for (uint32_t eye = 0; eye < 2; ++eye) {
        const uint32_t count = std::min<uint32_t>(sQuest.hiddenArea(eye, rects, 512, &version), 512);
        AuroraStereoSetHiddenArea(eye, rects, count);
    }
    sQuest.hiddenVersion = version;
}

bool register_images()
{
    update_hidden_area();
    const uint32_t generation = sQuest.generation();
    if (generation == 0) {
        return false;
    }
    if (generation == sQuest.registeredGeneration) {
        return true;
    }
    // The eyes' images and, apart, the interface's: one of each per ring slot.
    void *eyeBuffers[8] {};
    void *hudBuffers[8] {};
    uint32_t count = 0, width = 0, height = 0, imagesGeneration = 0;
    uint32_t hudWidth = 0, hudHeight = 0;
    if (!sQuest.images(eyeBuffers, hudBuffers, 8, &count, &width, &height, &imagesGeneration, &hudWidth, &hudHeight)
        || count == 0) {
        return false;
    }
    // Dawn takes its own references when it imports the buffers; the ones
    // images() gave for the import go back either way.
    const bool registered = AuroraStereoRegisterImages(eyeBuffers, hudBuffers, count, width, height, hudWidth, hudHeight,
        sQuest.submitted, nullptr);
    sQuest.releaseImages(eyeBuffers, count);
    sQuest.releaseImages(hudBuffers, count);
    if (!registered) {
        return false;
    }
    sQuest.registeredGeneration = imagesGeneration;
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest",
        "Stereo bridge: imported %u images (%ux%u, HUD %ux%u apart), generation %u", count, width, height, hudWidth,
        hudHeight, imagesGeneration);
#endif
    return true;
}

// The game's pace in the headset, logged every 5 seconds ("Game perf"): the time
// between frames drawn for the eyes, and the part of it spent recording their
// draws (camera 0's 3D layers).
struct GamePerf {
    using Clock = std::chrono::steady_clock;
    Clock::time_point windowStart {}, lastFrame {}, recordStart {};
    uint32_t frames = 0, slow = 0;
    double intervalSum = 0, intervalMax = 0, recordSum = 0, recordMax = 0;

    void begin()
    {
        const auto now = Clock::now();
        recordStart = now;
        if (lastFrame != Clock::time_point {}) {
            const double ms = std::chrono::duration<double, std::milli>(now - lastFrame).count();
            if (ms < 1000.0) { // longer: paused
                intervalSum += ms;
                intervalMax = ms > intervalMax ? ms : intervalMax;
                slow += ms > 25.0; // a visible stutter at the game's 60 Hz
                ++frames;
            }
        }
        lastFrame = now;
        if (windowStart == Clock::time_point {}) {
            windowStart = now;
        }
    }

    void end(int scene)
    {
        const auto now = Clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - recordStart).count();
        recordSum += ms;
        recordMax = ms > recordMax ? ms : recordMax;
        const double seconds = std::chrono::duration<double>(now - windowStart).count();
        if (seconds < 5.0 || frames == 0) {
            return;
        }
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest",
            "Game perf: scene=%d %.1f frames/s interval avg=%.2fms max=%.2fms stutters=%u record avg=%.2fms max=%.2fms",
            scene, frames / seconds, intervalSum / frames, intervalMax, slow, recordSum / frames, recordMax);
#endif
        *this = {};
        lastFrame = now;
        windowStart = now;
    }
};

GamePerf sGamePerf;

} // namespace

static void begin_eyes(const Mat4& world);

static bool same_viewport(int a, int b);

// The player this headset plays: the human whose pad is this machine's (its
// seat online, pad 0 offline), else the first human, else player 0.
static int local_player()
{
    const int port = PartyBoard_NetplayEnabled() ? PartyBoard_NetplayLocalPort() : 0;
    for (int player = 0; player < 4; ++player) {
        if (!GWPlayerCfg[player].iscom && GWPlayerCfg[player].pad_idx == port) {
            return player;
        }
    }
    for (int player = 0; player < 4; ++player) {
        if (!GWPlayerCfg[player].iscom) {
            return player;
        }
    }
    return 0;
}

// The camera the eyes take: the first active one, or with a split screen (an
// active camera over another part of the screen) the one looking nearest to
// this headset's player, its own view as on its part of the TV (a team's, in
// two against two). Each headset of a game played together takes its own.
static int primary_camera()
{
    // Photo Finish: camera 0 only photographs the puzzle's picture into a
    // corner of the frame (copied to a texture, then covered in black); the
    // players' views are cameras 1 to 4, one a player. The eyes took camera
    // 0: the picture's scene and its black cover, the interface cut to that
    // corner, no puzzle (2026-10-10).
    if (omCurrentOvlGet() == DLL_m411Dll) {
        const int own = 1 + local_player();
        if (own < HU3D_CAM_MAX && -1.0f != Hu3DCamera[own].fov) {
            return own;
        }
    }
    int first = -1;
    bool split = false;
    for (int camera = 0; camera < HU3D_CAM_MAX; ++camera) {
        if (-1.0f == Hu3DCamera[camera].fov) continue;
        if (first < 0) {
            first = camera;
        } else if (!same_viewport(camera, first)) {
            split = true;
        }
    }
    if (!split) {
        return first;
    }
    HuVecF player{};
    if (!CharModelPosGet(GWPlayerCfg[local_player()].character, &player)) {
        return first;
    }
    // The camera chosen before stays until another looks clearly nearer (a
    // player halfway between two would make the view jump back and forth).
    static int sChosen = -1;
    int nearest = first;
    float nearestDistance = INFINITY, chosenDistance = INFINITY;
    for (int camera = 0; camera < HU3D_CAM_MAX; ++camera) {
        const auto& view = Hu3DCamera[camera];
        if (-1.0f == view.fov) continue;
        const float dx = view.target.x - player.x;
        const float dy = view.target.y - player.y;
        const float dz = view.target.z - player.z;
        const float distance = dx * dx + dy * dy + dz * dz;
        if (!std::isfinite(distance)) continue;
        if (camera == sChosen) chosenDistance = distance;
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearest = camera;
        }
    }
    if (sChosen >= 0 && chosenDistance < INFINITY && chosenDistance <= 1.5f * nearestDistance) {
        return sChosen;
    }
    sChosen = nearest;
    return nearest;
}

static bool same_viewport(int a, int b)
{
    const auto& first = Hu3DCamera[a];
    const auto& second = Hu3DCamera[b];
    return std::abs(first.viewportX - second.viewportX) < 1.0f && std::abs(first.viewportY - second.viewportY) < 1.0f
        && std::abs(first.viewportW - second.viewportW) < 1.0f && std::abs(first.viewportH - second.viewportH) < 1.0f;
}

// The game's own frame while the headset hides it: unseen, but still drawn
// where the game copies it into textures (Three Throw's nets, Mario Medley's
// water and reflections: their world is drawn into that frame too). It was
// drawn at the headset screen's 3840 x 2160 for copies of 640 x 480 at most:
// some 4 ms of GPU an image, the eyes' resolution lowered for it, 70% in
// Three Throw (2026-10-10). Hidden, it is drawn at twice the GameCube's
// resolution (the setting the online game uses at once); shown again, at the
// player's. `adb shell setprop debug.partyboard.flat_scale 3` (0: the
// player's), read every 2 s.
static void flat_frame_scale(bool hidden)
{
    static float applied = -1.0f;
    float hiddenScale = 2.0f;
#if defined(__ANDROID__)
    static float readScale = 2.0f;
    static auto readAt = std::chrono::steady_clock::time_point {};
    const auto now = std::chrono::steady_clock::now();
    if (now - readAt >= std::chrono::seconds(2)) {
        readAt = now;
        char value[PROP_VALUE_MAX] = {};
        __system_property_get("debug.partyboard.flat_scale", value);
        readScale = (value[0] >= '0' && value[0] <= '9') ? std::strtof(value, nullptr) : 2.0f;
    }
    hiddenScale = readScale;
#endif
    const float player = PartyBoard_NetplayEnabled()
        ? 1.0f
        : static_cast<float>(partyboard::getSettings().game.internalResolutionScale.getValue());
    const float wanted = hidden && hiddenScale > 0.0f ? hiddenScale : player;
    if (wanted == applied) return;
    applied = wanted;
    VISetFrameBufferScale(wanted);
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest", "Game frame at %s%.1f (%s)",
        wanted > 0.0f ? "" : "native, ", wanted, hidden ? "hidden by the headset" : "on show");
#endif
}

extern "C" void PartyBoard_StereoBeginCamera(s16 cameraNo)
{
    sActive = false;
    sCameraViewSet = false;
    sSecondary = false;
    AuroraStereoWorldOnly(false);
    const int primary = primary_camera();
    if (cameraNo != primary) {
        // Another camera over the same part of the screen (a layer over the
        // first: a foreground, effects) goes to the eyes too, through the same
        // mapping. One over another part (a split screen's other players)
        // stays in the game's frame: the eyes show the first player's view.
        if (sFrameRecording && sWindow && primary >= 0 && same_viewport(cameraNo, primary)) {
            sActive = true;
            sSecondary = true;
            sCurrentCamera = cameraNo;
        }
        return;
    }
    sFrameRecording = false;
    sCurrentCamera = cameraNo;
    // Only this camera's part of the interface (a split screen's other
    // players' scores and timers are theirs), from the TV's 640 x 480.
    if (sQuest.hudCrop != nullptr) {
        const auto& view = Hu3DCamera[cameraNo];
        bool split = false;
        for (int other = 0; other < HU3D_CAM_MAX; ++other) {
            split = split || (other != cameraNo && -1.0f != Hu3DCamera[other].fov && !same_viewport(other, cameraNo));
        }
        if (split && view.viewportW > 1.0f && view.viewportH > 1.0f) {
            sQuest.hudCrop(view.viewportX / 640.0f, view.viewportY / 480.0f, view.viewportW / 640.0f,
                view.viewportH / 480.0f);
        } else {
            sQuest.hudCrop(0.0f, 0.0f, 1.0f, 1.0f);
        }
    }
    // No panel of the TV picture beside the 3D: Photo Finish's camera 0 was
    // taken for the finished picture (2026-10-06), but its corner of the TV
    // image is covered in black once photographed, then by player 1's view.
    if (sQuest.inset != nullptr) {
        sQuest.inset(0.0f, 0.0f, 0.0f, 0.0f);
    }
    if (!find_quest()) {
        AuroraStereoHideScreen(false);
        return;
    }
    const OMOVL scene = omCurrentOvlGet();
    const bool spatialScene = spatial_scene(scene);
    if (sQuest.cameraView != nullptr) sQuest.cameraView(camera_view_minigame(scene));
    if (sQuest.scene != nullptr) sQuest.scene(static_cast<int>(scene));
    // A menu or a minigame with several cameras (layers, or a split screen) shows its
    // first one in 3D, with the layers over it (every minigame in 3D, asked
    // for 2026-10-06).
    int cameras = 0;
    for (int other = 0; other < HU3D_CAM_MAX; ++other) {
        cameras += -1.0f != Hu3DCamera[other].fov ? 1 : 0;
    }
    // A scene that copies its frame into a texture while the eyes record its
    // camera (Mario Medley's panels, Three Throw) would copy a frame without
    // what only the eyes drew: for the rest of that scene its world is drawn
    // into the game's frame too.
    static OMOVL sCopyScene = DLL_NONE;
    if (AuroraStereoTakeEfbCopied() && sCopyScene != scene) {
        sCopyScene = scene;
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest", "Scene %d copies its frame while drawn for the eyes: "
            "its world drawn into that frame too", static_cast<int>(scene));
#endif
    }
    AuroraStereoSetKeepFlat(scene == sCopyScene);
    const bool screenRequired = !spatialScene || partyboard::ui::any_document_visible();
#if defined(__ANDROID__)
    static int loggedScene = -999;
    static bool loggedScreenRequired = false;
    if (loggedScene != static_cast<int>(scene) || loggedScreenRequired != screenRequired) {
        __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest", "Scene %d: %s (%d cameras, the eyes' from camera %d)",
            static_cast<int>(scene), screenRequired ? "classic screen" : "spatial rendering", cameras, cameraNo);
        loggedScene = static_cast<int>(scene);
        loggedScreenRequired = screenRequired;
    }
#endif
    sQuest.screenRequired(screenRequired);
    // The headset shows the model instead of the flat screen: do not present the screen.
    const bool screenHidden = !screenRequired && sQuest.screenHidden != nullptr && sQuest.screenHidden();
    AuroraStereoHideScreen(screenHidden);
    flat_frame_scale(screenHidden);
    sQuest.boardMode(!screenRequired && scene >= DLL_w01Dll && scene <= DLL_w06Dll);
    if (screenRequired || !register_images()) {
        return;
    }
    AuroraStereoWorldOnly(sQuest.worldOnly());
    sActive = sQuest.frame(&sFrame);
    if (sActive && sFrame.generation != sQuest.registeredGeneration) {
        // New images since they were imported (resolution change): this frame
        // stays flat, the next one imports them.
        sQuest.cancelled(sFrame.image, sFrame.tag);
        sActive = false;
    }
    if (sActive) {
        sGamePerf.begin();
        for (int axis = 0; axis < 3; ++axis) {
            sBoundsLow[axis] = INFINITY;
            sBoundsHigh[axis] = -INFINITY;
        }
    }
}

extern "C" void PartyBoard_StereoCameraView(s32 cameraNo, Mtx view)
{
    if (!sActive || cameraNo != sCurrentCamera) {
        return;
    }
    if (sSecondary) {
        // A layer over the first camera: its view space mapped as the first's.
        Mat4 viewToWindow{};
        for (int axis = 0; axis < 3; ++axis) {
            viewToWindow.m[axis][axis] = sWindowScale;
        }
        viewToWindow.m[2][3] = sWindowScale * sWindowDistance;
        viewToWindow.m[3][3] = 1.0f;
        begin_eyes(multiply(from_rows(sFrame.world), viewToWindow));
        return;
    }
    const OMOVL scene = omCurrentOvlGet();
    if (scene != sScene) {
        sScene = scene;
        sSceneFitted = false;
        sSceneScale = 1.0f;
        sSceneTurn = 0.0f;
        sFollowSet = false;
        std::memset(sSceneCenter, 0, sizeof(sSceneCenter));
        sFloorY = INFINITY;
        sFloorFrames = 0;
        sFloorSettled = false;
        sSceneFrames = 0;
    }
    ++sSceneFrames;
    sViewToWorld = inverse_affine(view);
    const bool board = board_scene(scene);
    const bool window = sFrame.window != 0 && !board;
    const bool framed = window && sFrame.window == 1;
    if (board && !sSceneFitted) {
        const int count = BoardSpaceCountGet(0);
        if (count > 0 && count <= 256) {
            float minimum[3]{INFINITY, INFINITY, INFINITY};
            float maximum[3]{-INFINITY, -INFINITY, -INFINITY};
            int playable = 0;
            for (int index = 1; index <= count; ++index) {
                const auto* space = BoardSpaceGet(0, index);
                if (!space || space->type == 0) continue;
                const float pos[3]{space->pos.x, space->pos.y, space->pos.z};
                if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2])) continue;
                for (int axis = 0; axis < 3; ++axis) {
                    minimum[axis] = (std::min)(minimum[axis], pos[axis]);
                    maximum[axis] = (std::max)(maximum[axis], pos[axis]);
                }
                ++playable;
            }
            if (playable >= 3 && partyboard::quest::board_fit(minimum, maximum, sSceneScale, sSceneCenter)) {
                sSceneFitted = true;
#if defined(__ANDROID__)
                __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest",
                    "Board fit scene=%d spaces=%d scale=%.3f center=%.1f/%.1f/%.1f",
                    static_cast<int>(scene), playable, sSceneScale, sSceneCenter[0], sSceneCenter[1], sSceneCenter[2]);
#endif
            }
        }
    }
    // Minigames and menus have very different coordinate scales: put on the
    // table, fit their initial camera once.
    const bool minigame = minigame_scene(scene) || menu_scene(scene);
    // A few frames in: the first ones can still hold the camera of the
    // scene before (the minigame menu was fitted 30 000 units away, on the
    // camera of the minigame it came back from, 2026-10-10).
    if (minigame && !window && !sSceneFitted && menu_scene(scene)) {
        const auto known = sMenuFits.find(static_cast<int>(scene));
        if (known != sMenuFits.end()) {
            sSceneScale = known->second.scale;
            std::memcpy(sSceneCenter, known->second.center, sizeof(sSceneCenter));
            sSceneTurn = known->second.turn;
            sSceneFitted = true;
        }
    }
    if (minigame && !window && !sSceneFitted && sSceneFrames >= kFitAfterFrames) {
        const auto& camera = Hu3DCamera[cameraNo];
        const float dx = camera.pos.x - camera.target.x;
        const float dy = camera.pos.y - camera.target.y;
        const float dz = camera.pos.z - camera.target.z;
        if (std::isfinite(camera.target.x) && std::isfinite(camera.target.y) && std::isfinite(camera.target.z)
            && partyboard::quest::scene_scale(std::sqrt(dx * dx + dy * dy + dz * dz), camera.fov, camera.aspect,
                sSceneScale)) {
            sSceneCenter[0] = camera.target.x;
            sSceneCenter[1] = camera.target.y;
            sSceneCenter[2] = camera.target.z;
            // The table's +Z points at the player: the camera's horizontal
            // look turned to -Z, the player on the camera's side (on
            // 2026-10-10 the minigames lay as their world did, seen from
            // aside or from behind: "the camera no longer takes the
            // minigame's first point of view").
            const float lookX = camera.target.x - camera.pos.x, lookZ = camera.target.z - camera.pos.z;
            sSceneTurn = lookX * lookX + lookZ * lookZ > 1e-6f ? static_cast<float>(M_PI) - std::atan2(lookX, lookZ) : 0.0f;
            sSceneFitted = true;
            if (menu_scene(scene)) {
                sMenuFits[static_cast<int>(scene)] =
                    SceneFit {sSceneScale, {sSceneCenter[0], sSceneCenter[1], sSceneCenter[2]}, sSceneTurn};
            }
#if defined(__ANDROID__)
            __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest", "Stereo scene %d: initial camera fit %.3fx",
                static_cast<int>(scene), sSceneScale);
#endif
        }
    }
    // On the table and not fitted yet: nothing for the eyes this frame,
    // rather than the scene at an arbitrary size around the player.
    if (minigame && !window && !sSceneFitted) {
        sQuest.cancelled(sFrame.image, sFrame.tag);
        sActive = false;
        return;
    }
    Mat4 world{};
    if (window) {
        // The game camera's view space into the window (+Z toward the
        // player, meters): what the camera looks at in the window's plane,
        // its vertical field of view as tall as the window, so the window
        // frames the scene as the screen did; nearer things stand out of it,
        // farther ones behind it:
        //   window = scale * view + (0, 0, scale * distance)
        // The camera's moves and cuts happen behind the window, which stays put.
        const auto& camera = Hu3DCamera[cameraNo];
        const float dx = camera.pos.x - camera.target.x;
        const float dy = camera.pos.y - camera.target.y;
        const float dz = camera.pos.z - camera.target.z;
        float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        float scale = partyboard::quest::view_to_window_scale(distance, camera.fov, sFrame.windowHeight);
        // Open, the player stands where the camera does: what it looks at
        // where the screen is, at the player's distance, and its field of
        // view as wide in the player's: the same point of view (the window's
        // height made it a smaller picture, "not zoomed enough", 2026-10-06).
        if (!framed && sFrame.windowDistance > 0.0f) {
            scale = sFrame.windowDistance / distance;
        }
        // The interface then spans the camera's field from the player's
        // place too: what the game marks on it from the 3D (Hu3D3Dto2D, a
        // cursor on the piece in hand) lands on that thing. As tall as the
        // screen, it was a smaller picture than the 3D (2026-10-10).
        if (sQuest.hudFov != nullptr && !sSecondary) {
            sQuest.hudFov(!framed && sFrame.windowDistance > 0.0f
                    ? std::tan(camera.fov * 0.5f * static_cast<float>(M_PI) / 180.0f)
                    : 0.0f);
        }
        Mat4 viewToWindow{};
        for (int axis = 0; axis < 3; ++axis) {
            viewToWindow.m[axis][axis] = scale;
        }
        viewToWindow.m[2][3] = scale * distance;
        viewToWindow.m[3][3] = 1.0f;
        world = multiply(from_rows(sFrame.world), viewToWindow);
        sWindowScale = scale;
        sWindowDistance = distance;
        // Framed, the sky and the far scenery are the game's, seen through the
        // window. Open, a sky would surround the player and hide the room: an
        // object around the game camera itself, much larger than its distance
        // to what it looks at (PartyBoard_StereoBackdrop). Not the eyes': an
        // arena's floor holds them too, and it came and went with the head
        // (2026-10-06).
        sSceneExtent = 0.0f;
        sOpenSkyRadius = framed ? 0.0f : kOpenSkyPerDistance * distance;
    } else {
        // A board (or a minigame on the table): unturned, the lowest floor on
        // the table, the game camera cancelled every frame: the tracked
        // headset supplies the viewpoint. A board never moves: sliding or
        // turning it made the player sick.
        //   sceneWorld = scale * turn * move(-center)
        // (a minigame or a menu turned toward the player, sSceneTurn).
        const float c = std::cos(sSceneTurn), sn = std::sin(sSceneTurn);
        const float scale = sSceneScale * table_size(scene);
        // What stands at the table's center: the camera's target followed
        // (a minigame, a menu), turned the same way throughout; a cut (a jump
        // of half the scene) is taken at once, a move eased over a few frames.
        float center[3]{sSceneCenter[0], sSceneCenter[1], sSceneCenter[2]};
        float floorY = sFloorY;
        if (!board && sSceneFitted && minigame_scene(scene)) {
            const auto& camera = Hu3DCamera[cameraNo];
            const float target[3]{camera.target.x, camera.target.y, camera.target.z};
            if (std::isfinite(target[0]) && std::isfinite(target[1]) && std::isfinite(target[2])) {
                const float extent = 2800.0f / sSceneScale;
                float jump2 = 0.0f;
                for (int axis = 0; axis < 3; ++axis) {
                    const float d = target[axis] - sFollow[axis];
                    jump2 += d * d;
                }
                for (int axis = 0; axis < 3; ++axis) {
                    sFollow[axis] = !sFollowSet || jump2 > 0.25f * extent * extent
                        ? target[axis]
                        : sFollow[axis] + (target[axis] - sFollow[axis]) * 0.12f;
                }
                sFollowSet = true;
                // The floor keeps its height under the target.
                floorY = sFollow[1] - (sSceneCenter[1] - (std::isfinite(sFloorY) ? sFloorY : sSceneCenter[1]));
                std::memcpy(center, sFollow, sizeof(center));
            }
        }
        Mat4 sceneWorld{};
        sceneWorld.m[0][0] = c * scale;
        sceneWorld.m[0][2] = sn * scale;
        sceneWorld.m[1][1] = scale;
        sceneWorld.m[2][0] = -sn * scale;
        sceneWorld.m[2][2] = c * scale;
        sceneWorld.m[0][3] = -(c * center[0] + sn * center[2]) * scale;
        sceneWorld.m[1][3] = -center[1] * scale;
        sceneWorld.m[2][3] = -(-sn * center[0] + c * center[2]) * scale;
        sceneWorld.m[3][3] = 1.0f;
        // The measured floor on the table (the table's height is the anchor's).
        if (sFloorSettled && std::isfinite(floorY)) {
            sceneWorld.m[1][3] = -floorY * scale;
        }
        world = multiply(multiply(from_rows(sFrame.world), sceneWorld), inverse_affine(view));
        sSceneExtent = sSceneFitted ? 2800.0f / sSceneScale : 0.0f;
        sOpenSkyRadius = 0.0f;
    }
    sWindow = window;
    begin_eyes(world);
    sFrameRecording = true;
}

// The eyes' clip matrices from `world` (the game camera's view space -> the
// room), the eyes' positions in that view space, and this camera's recording.
static void begin_eyes(const Mat4& world)
{
    const bool framed = sWindow && sFrame.window == 1;
    {
        // Each eye's position (the room -> eye matrix's inverse translation),
        // brought into the game camera's view space: room -> game view.
        Mtx toView;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                toView[r][c] = world.m[r][c];
            }
        }
        const Mat4 roomToView = inverse_affine(toView);
        for (int eye = 0; eye < 2; ++eye) {
            const float *v = sFrame.eyeView[eye];
            const float room[3] {
                -(v[0] * v[3] + v[4] * v[7] + v[8] * v[11]),
                -(v[1] * v[3] + v[5] * v[7] + v[9] * v[11]),
                -(v[2] * v[3] + v[6] * v[7] + v[10] * v[11]),
            };
            for (int axis = 0; axis < 3; ++axis) {
                sEyeInView[eye][axis] = roomToView.m[axis][0] * room[0] + roomToView.m[axis][1] * room[1]
                    + roomToView.m[axis][2] * room[2] + roomToView.m[axis][3];
            }
        }
    }
    float clip[2][16];
    for (int eye = 0; eye < 2; ++eye) {
        const Mat4 eyeClip
            = multiply(from_rows(sFrame.eyeProj[eye]), multiply(from_rows(sFrame.eyeView[eye]), world));
        std::memcpy(clip[eye], eyeClip.m, sizeof(clip[eye]));
    }
    std::memcpy(sEyeClip, clip, sizeof(sEyeClip));
    std::memcpy(sViewToRoom, world.m, sizeof(sViewToRoom));
    AuroraStereoSetEyeSize(sFrame.eyeWidth, sFrame.eyeHeight);
    AuroraStereoSetWindow(framed);
    AuroraStereoSetCoverAlpha(sWindow && !framed);
    AuroraStereoSetHud(sFrame.drawHud != 0);
    if (sFrame.drawHud == 2) {
        AuroraStereoRedrawHud();
    }
    // Whether the renderer can foveate, told once it has its device; the
    // level the headset chose for this frame (StereoView::adapt_resolution).
    static bool sFoveationTold = false;
    if (!sFoveationTold && sQuest.foveationAvailable != nullptr) {
        sQuest.foveationAvailable(AuroraStereoFoveationSupported());
        sFoveationTold = true;
    }
    AuroraStereoSetFoveation(sFrame.foveation);
    AuroraStereoBegin(sFrame.image, clip, sFrame.tag);
    sCameraViewSet = true;
}

extern "C" void PartyBoard_StereoEndCamera(void)
{
    if (sActive && !sSecondary && sCameraViewSet && sSceneFitted && !sFloorSettled && ++sFloorFrames >= kFloorFrames) {
        sFloorSettled = true;
        // A minigame or a menu whose lowest large surface lies far below what
        // its camera looks at stood far above the table (2026-10-10: Manta
        // Rings, the sea bed 12 000 units under the surface where it is
        // played; Booksquirm, 1 070 under for 1 700 across): what the camera
        // looks at stands just above the table instead. Boards keep their
        // measured floor.
        // Every minigame measured that day but one stood too high: on the
        // table, the floor is never deeper than a quarter of the scene under
        // what the camera looks at, nor above it.
        const float extent = sSceneScale > 0.0f ? 2800.0f / sSceneScale : 0.0f;
        if (!board_scene(sScene) && extent > 0.0f) {
            const float lowest = sSceneCenter[1] - 0.25f * extent;
            const float highest = sSceneCenter[1] - 0.05f * extent;
            const float measured = sFloorY;
            if (!std::isfinite(sFloorY) || sFloorY < lowest) sFloorY = lowest;
            if (sFloorY > highest) sFloorY = highest;
#if defined(__ANDROID__)
            if (sFloorY != measured) {
                __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest",
                    "Scene %d floor %.1f too far from its camera's target %.1f: %.1f instead",
                    static_cast<int>(sScene), measured, sSceneCenter[1], sFloorY);
            }
#endif
        }
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest", "Scene %d floor: %s %.1f (scene center %.1f)",
            static_cast<int>(sScene), std::isfinite(sFloorY) ? "measured" : "none, keeping", sFloorY,
            sSceneCenter[1]);
#endif
    }
    if (sActive) {
        if (sCameraViewSet) {
            AuroraStereoEnd();
            if (!sSecondary) {
                sGamePerf.end(static_cast<int>(omCurrentOvlGet()));
                // What this frame offered to draw, for the next one's eyes
                // (none through a window: the window is the frame).
                if (sQuest.sceneBounds != nullptr) {
                    const bool any = (!sWindow || sFrame.window == 2) && sBoundsLow[0] <= sBoundsHigh[0];
                    sQuest.sceneBounds(any ? sBoundsLow : nullptr, any ? sBoundsHigh : nullptr);
                }
            }
        } else if (!sSecondary) {
            sQuest.cancelled(sFrame.image, sFrame.tag);
        }
        sActive = false;
        sSecondary = false;
    }
    AuroraStereoWorldOnly(false);
}

// A toast of the game (ui::push_toast), shown in the headset too: its screen,
// where toasts are drawn, is hidden while the world is in 3D. On a phone the
// headset library is not there and nothing happens.
extern "C" void PartyBoard_QuestToast(const char *title, const char *text, int durationMs)
{
#if defined(__ANDROID__)
    static void (*toast)(const char *, const char *, int) = nullptr;
    static bool looked = false;
    if (!looked) {
        void *lib = dlopen("libpartyboard_quest.so", RTLD_NOW | RTLD_NOLOAD);
        if (lib == nullptr) return; // not loaded yet: look again next time
        looked = true;
        toast = reinterpret_cast<void (*)(const char *, const char *, int)>(dlsym(lib, "PartyBoardQuest_Toast"));
    }
    if (toast != nullptr) toast(title != nullptr ? title : "", text != nullptr ? text : "", durationMs);
#else
    (void)title;
    (void)text;
    (void)durationMs;
#endif
}

extern "C" BOOL PartyBoard_StereoActive(void)
{
    return sActive ? TRUE : FALSE;
}

extern "C" BOOL PartyBoard_StereoBoardPresentation(void)
{
    const OMOVL scene = omCurrentOvlGet();
    return board_scene(scene) && find_quest() && sQuest.worldOnly() ? TRUE : FALSE;
}

// Before a board starts: wait while the headset asks the player to lay the
// controller on the table (the first time, or when the saved table is out of
// reach). A minigame needs no table: it shows where the screen stands (or,
// put on the table, where the screen's table is). An online game never
// waits: its peers would not.
extern "C" BOOL PartyBoard_QuestHoldOverlay(OMOVL next)
{
    if (!board_scene(next) || PartyBoard_NetplayEnabled() || !find_quest() || sQuest.tableSetupHold == nullptr) {
        return FALSE;
    }
    const bool hold = sQuest.tableSetupHold();
#if defined(__ANDROID__)
    static bool loggedHold = false;
    if (hold != loggedHold) {
        __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest", "Scene %d %s the table calibration",
            static_cast<int>(next), hold ? "waits for" : "starts after");
        loggedHold = hold;
    }
#endif
    return hold ? TRUE : FALSE;
}

// The sky, a skybox or a far scenery ring: seen from the headset it would
// surround the player and hide the room, and it fills every pixel of both
// eyes. Recognized as an object whose bounding sphere holds an eye and is much
// larger than the scene on the table; the table and the room replace it.
extern "C" BOOL PartyBoard_StereoBackdrop(float x, float y, float z, float radius)
{
    if (sActive && sCameraViewSet && sOpenSkyRadius > 0.0f) {
        // Around the game camera (its view space's origin), whatever the head does.
        const float core = kOpenSkyCore * radius;
        const bool sky = std::isfinite(radius) && radius >= sOpenSkyRadius && x * x + y * y + z * z < core * core;
#if defined(__ANDROID__)
        static int loggedScene = -1;
        if (sky && loggedScene != static_cast<int>(sScene)) {
            loggedScene = static_cast<int>(sScene);
            __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest", "Scene %d: a sky left out, %.0f units around the "
                "camera (%.1f times its distance to its target)", static_cast<int>(sScene), radius,
                radius * kOpenSkyPerDistance / sOpenSkyRadius);
        }
#endif
        if (sky) return TRUE;
        // The large scenery too (a ground spreading far, a background), so the
        // room shows around the action (asked for 2026-10-10: "remove the
        // ground and the background, else I no longer see my table"): an
        // object this many times larger than the camera's distance to its
        // target. `adb shell setprop debug.partyboard.open_scenery 3` sets the
        // factor live, 0 keeps everything.
        // Not on a course, whose ground is the course itself: Avalanche!'s
        // slope went with the scenery (8.6 times the camera's distance,
        // 2026-10-10), nor Paratrooper Plunge's or Manta Rings'. Per scene:
        // `debug.partyboard.scenery_<scene>`, overriding `open_scenery`.
        // Three Throw too: through its baskets' nets the room showed where its
        // stands had been left out, as holes in the baskets (2026-10-10).
        const bool course = sScene == DLL_m401Dll || sScene == DLL_m406Dll || sScene == DLL_m408Dll
            || sScene == DLL_m410Dll;
        static float factor = 3.0f;
        static OMOVL factorScene = DLL_NONE;
#if defined(__ANDROID__)
        static auto readAt = std::chrono::steady_clock::time_point {};
        const auto now = std::chrono::steady_clock::now();
        if (sScene != factorScene || now - readAt >= std::chrono::seconds(2)) {
            readAt = now;
            factorScene = sScene;
            const auto read = [](const char *name, float fallback) {
                char value[PROP_VALUE_MAX] = {};
                __system_property_get(name, value);
                return (value[0] >= '0' && value[0] <= '9') || value[0] == '.' ? std::strtof(value, nullptr) : fallback;
            };
            char name[48];
            std::snprintf(name, sizeof(name), "debug.partyboard.scenery_%d", static_cast<int>(sScene));
            const float chosen = read(name, read("debug.partyboard.open_scenery", course ? 0.0f : 3.0f));
            if (chosen != factor) {
                __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest",
                    "Scene %d: scenery larger than %.1f times the camera's distance left out (0: none)",
                    static_cast<int>(sScene), chosen);
            }
            factor = chosen;
        }
#else
        factor = course ? 0.0f : 3.0f;
#endif
        const float distance = sOpenSkyRadius / kOpenSkyPerDistance;
        // A ground under the action is kept however large: an object holding
        // the point the camera looks at, (0, 0, -distance) in its view space
        // (Domination's field went with the scenery, 6.6 times the camera's
        // distance, 2026-10-10). Only the scenery away from the action goes.
        const float tz = z + distance;
        const bool underAction = x * x + y * y + tz * tz < radius * radius;
        const bool scenery = factor > 0.0f && std::isfinite(radius) && radius >= factor * distance && !underAction;
#if defined(__ANDROID__)
        static int loggedScenery = -1;
        if (scenery && loggedScenery != static_cast<int>(sScene)) {
            loggedScenery = static_cast<int>(sScene);
            __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest", "Scene %d: scenery left out, %.0f units across "
                "(%.1f times the camera's distance)", static_cast<int>(sScene), radius, radius / distance);
        }
#endif
        return scenery ? TRUE : FALSE;
    }
    if (!sActive || !sCameraViewSet || !(sSceneExtent > 0.0f) || !std::isfinite(radius)
        || radius < kBackdropExtents * sSceneExtent) {
        return FALSE;
    }
    const float center[3] {x, y, z};
    for (int eye = 0; eye < 2; ++eye) {
        float distance2 = 0.0f;
        for (int axis = 0; axis < 3; ++axis) {
            const float d = sEyeInView[eye][axis] - center[axis];
            distance2 += d * d;
        }
        if (distance2 < radius * radius) {
            return TRUE;
        }
    }
    return FALSE;
}

// Each object drawn for the eyes while the floor is measured: its bounds,
// from the object's model-view matrix (as hsfdraw.c draws it).
extern "C" void PartyBoard_StereoObserveBounds(Mtx modelView, const HuVecF *min, const HuVecF *max)
{
    if (!sActive || !sCameraViewSet || sFloorSettled || sWindow || !(sSceneExtent > 0.0f)) {
        return; // from the camera's point of view, no floor goes on the table
    }
    float low[3] {INFINITY, INFINITY, INFINITY};
    float high[3] {-INFINITY, -INFINITY, -INFINITY};
    float viewLow[3] {INFINITY, INFINITY, INFINITY};
    float viewHigh[3] {-INFINITY, -INFINITY, -INFINITY};
    for (int corner = 0; corner < 8; ++corner) {
        const float local[3] {(corner & 1) ? max->x : min->x, (corner & 2) ? max->y : min->y,
            (corner & 4) ? max->z : min->z};
        float view[3], world[3];
        for (int r = 0; r < 3; ++r) {
            view[r] = modelView[r][0] * local[0] + modelView[r][1] * local[1] + modelView[r][2] * local[2]
                + modelView[r][3];
        }
        for (int r = 0; r < 3; ++r) {
            world[r] = sViewToWorld.m[r][0] * view[0] + sViewToWorld.m[r][1] * view[1]
                + sViewToWorld.m[r][2] * view[2] + sViewToWorld.m[r][3];
            low[r] = (std::min)(low[r], world[r]);
            high[r] = (std::max)(high[r], world[r]);
            viewLow[r] = (std::min)(viewLow[r], view[r]);
            viewHigh[r] = (std::max)(viewHigh[r], view[r]);
        }
    }
    if (!std::isfinite(low[1]) || (std::max)(high[0] - low[0], high[2] - low[2]) < kFloorSpan * sSceneExtent) {
        return; // too small to be the ground
    }
    float center[3], radius2 = 0.0f;
    for (int r = 0; r < 3; ++r) {
        center[r] = (viewLow[r] + viewHigh[r]) * 0.5f;
        const float half = (viewHigh[r] - viewLow[r]) * 0.5f;
        radius2 += half * half;
    }
    if (PartyBoard_StereoBackdrop(center[0], center[1], center[2], std::sqrt(radius2))) {
        return; // the sky is not the floor
    }
    sFloorY = (std::min)(sFloorY, low[1]);
}

// The object's bounds in the game camera's view space (as hsfdraw.c draws
// it), as a sphere: its draws go without the eyes' cut when that sphere lies
// wholly inside both eyes' sides. Everything else (particles, sprites, and
// any draw outside ObjDraw) keeps the cut.
extern "C" void PartyBoard_StereoObjectBegin(Mtx modelView, const HuVecF *min, const HuVecF *max)
{
    if (!sActive || !sCameraViewSet || min == nullptr || max == nullptr) {
        return;
    }
    float low[3] {INFINITY, INFINITY, INFINITY};
    float high[3] {-INFINITY, -INFINITY, -INFINITY};
    for (int corner = 0; corner < 8; ++corner) {
        const float local[3] {(corner & 1) ? max->x : min->x, (corner & 2) ? max->y : min->y,
            (corner & 4) ? max->z : min->z};
        for (int r = 0; r < 3; ++r) {
            const float view = modelView[r][0] * local[0] + modelView[r][1] * local[1] + modelView[r][2] * local[2]
                + modelView[r][3];
            low[r] = (std::min)(low[r], view);
            high[r] = (std::max)(high[r], view);
        }
    }
    float center[3], radius2 = 0.0f;
    for (int r = 0; r < 3; ++r) {
        center[r] = (low[r] + high[r]) * 0.5f;
        const float half = (high[r] - low[r]) * 0.5f;
        radius2 += half * half;
    }
    // A minigame keeps the cut: its objects' bounds proved wrong from the
    // camera's place (ObjectVisible), and an uncut draw past them spills into
    // the other eye.
    AuroraStereoSetUncut(!sWindow && partyboard::quest::sphere_inside_eye_sides(sEyeClip, center, std::sqrt(radius2)));
    // Its distance from between the eyes: opaque objects may go to the GPU
    // front to back (debug.partyboard.sort_opaque), for its early depth test.
    float distance2 = 0.0f;
    for (int r = 0; r < 3; ++r) {
        const float d = center[r] - (sEyeInView[0][r] + sEyeInView[1][r]) * 0.5f;
        distance2 += d * d;
    }
    AuroraStereoSetSortKey(std::isfinite(distance2) ? std::sqrt(distance2) : -1.0f);
}

// debug.partyboard.freeze 1 (read every second): main.c consumes the
// simulation's ticks without playing them, so the game holds one image and
// every phase of a headset A/B (tools/quest_campaign.ps1) draws exactly the
// same frame; the board's own motion no longer swamps a few percent.
// Where each pass of the game's main loop spends its time (src/game/main.c's
// marks), every 5 s: waiting for a render slot (aurora_begin_frame), the
// simulation ticks, recording the draws (Hu3DExec, fonts), ending the GX
// frame (HuSysDoneRender: the eyes' pass is built there), the interface and
// Aurora's frame end, and the frame limiter's wait.
extern "C" void PartyBoard_LoopMark(int mark)
{
#if defined(__ANDROID__)
    static const char *const kNames[] = { "slotWait", "simulation", "draw", "doneRender", "uiEnd", "limiter" };
    static std::chrono::steady_clock::time_point last {};
    static int lastMark = -1;
    static double sum[6] {}, max[6] {};
    static uint32_t loops = 0;
    static auto loggedAt = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (mark >= 1 && mark <= 6 && lastMark == mark - 1) {
        const double ms = std::chrono::duration<double, std::milli>(now - last).count();
        sum[mark - 1] += ms;
        max[mark - 1] = std::max(max[mark - 1], ms);
    }
    last = now;
    lastMark = mark;
    if (mark != 6) {
        return;
    }
    ++loops;
    if (now - loggedAt < std::chrono::seconds(5)) {
        return;
    }
    char text[512];
    int length = std::snprintf(text, sizeof(text), "Game loop: %u loops, avg/max ms", loops);
    for (int i = 0; i < 6 && length > 0 && length < static_cast<int>(sizeof(text)); ++i) {
        length += std::snprintf(text + length, sizeof(text) - length, " %s=%.2f/%.2f", kNames[i], sum[i] / loops, max[i]);
        sum[i] = max[i] = 0;
    }
    __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest", "%s", text);
    loops = 0;
    loggedAt = now;
#else
    (void)mark;
#endif
}

extern "C" bool PartyBoard_DebugFreeze(void)
{
#if defined(__ANDROID__)
    static bool frozen = false;
    static auto readAt = std::chrono::steady_clock::time_point {};
    const auto now = std::chrono::steady_clock::now();
    if (now - readAt >= std::chrono::seconds(1)) {
        readAt = now;
        char value[PROP_VALUE_MAX] = {};
        __system_property_get("debug.partyboard.freeze", value);
        const bool wanted = value[0] == '1';
        if (wanted != frozen) {
            __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest", "Simulation %s (debug.partyboard.freeze)",
                wanted ? "frozen" : "running");
        }
        frozen = wanted;
    }
    return frozen;
#else
    return false;
#endif
}

extern "C" void PartyBoard_StereoObjectEnd(void)
{
    AuroraStereoSetUncut(false);
    AuroraStereoSetSortKey(-1.0f);
}

// An object offered for drawing, its bounding sphere in the game camera's
// view space: into the frame's box in the room (sBoundsLow/High).
static void add_to_bounds(float x, float y, float z, float radius)
{
    // On the table, and from the camera's place (open): a minigame's arena
    // stands in the room there too, the room seen around it once its sky is
    // left out. Not through a framed window, which is the frame already.
    if (!sActive || !sCameraViewSet || (sWindow && sFrame.window != 2) || !std::isfinite(radius)) return;
    const auto &m = sViewToRoom;
    const float scale = std::sqrt(m[0][0] * m[0][0] + m[1][0] * m[1][0] + m[2][0] * m[2][0]);
    const float r = radius * scale;
    // From the camera's place the scene is life-sized around the player: only
    // what is larger than the room counts as a sky or a sea.
    if (!(r < (sWindow ? kBoundsMaxRadiusOpen : kBoundsMaxRadius))) return;
    for (int axis = 0; axis < 3; ++axis) {
        const float c = m[axis][0] * x + m[axis][1] * y + m[axis][2] * z + m[axis][3];
        if (!std::isfinite(c)) return;
        sBoundsLow[axis] = (std::min)(sBoundsLow[axis], c - r);
        sBoundsHigh[axis] = (std::max)(sBoundsHigh[axis], c + r);
    }
}

// From the camera's place (a minigame), what lies outside both eyes is culled
// too: Manta Rings drew everything and ran at 48 to 59 images a second
// (2026-10-10). Not Three Throw, whose balls it took away (2026-10-06).
// `debug.partyboard.cull_camera 0`, read every 2 s, turns it off.
static bool cull_from_camera()
{
    if (!sWindow || sFrame.window != 2 || omCurrentOvlGet() == DLL_m410Dll) return false;
#if defined(__ANDROID__)
    static bool enabled = true;
    static auto readAt = std::chrono::steady_clock::time_point {};
    const auto now = std::chrono::steady_clock::now();
    if (now - readAt >= std::chrono::seconds(2)) {
        readAt = now;
        char value[PROP_VALUE_MAX] = {};
        __system_property_get("debug.partyboard.cull_camera", value);
        enabled = value[0] != '0';
    }
    return enabled;
#else
    return true;
#endif
}

extern "C" BOOL PartyBoard_StereoObjectVisible(float x, float y, float z, float radius)
{
    add_to_bounds(x, y, z, radius);
    if (!sActive || !sCameraViewSet) return TRUE;
    if (sWindow ? !cull_from_camera() : !sFloorSettled) return TRUE;
#if defined(__ANDROID__)
    static bool enabled = true;
    static uint32_t tested = 0, culled = 0;
    static auto checkedAt = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - checkedAt >= std::chrono::seconds(5)) {
        if (tested != 0) {
            __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest",
                "Object culling: %u of %u pieces outside both eyes in 5 s (%u%%)%s", culled, tested,
                culled * 100 / tested, enabled ? "" : ", off (debug.partyboard.cull_objects 0)");
        }
        tested = culled = 0;
        checkedAt = now;
        char value[PROP_VALUE_MAX] = {};
        __system_property_get("debug.partyboard.cull_objects", value);
        enabled = value[0] != '0';
    }
    const float center[3]{x, y, z};
    const bool visible = partyboard::quest::sphere_visible(sEyeClip, center, radius);
    ++tested;
    culled += visible ? 0 : 1;
    return visible || !enabled ? TRUE : FALSE;
#else
    const float center[3]{x, y, z};
    return partyboard::quest::sphere_visible(sEyeClip, center, radius) ? TRUE : FALSE;
#endif
}

extern "C" BOOL PartyBoard_StereoSphereVisible(float x, float y, float z, float radius)
{
    add_to_bounds(x, y, z, radius);
    if (!sActive || !sCameraViewSet || (sWindow && !cull_from_camera())) return TRUE;
    const float center[3]{x, y, z};
    return partyboard::quest::sphere_visible(sEyeClip, center, radius) ? TRUE : FALSE;
}
