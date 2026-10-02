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
// The game camera itself is cancelled (the headset is the viewpoint). A
// minigame's model floats where the board's screen stands (quest_xr.cpp),
// turned so the player sees it as its camera does once the camera rests
// (quest_camera_follow.hpp). A board stays as placed on the table.

extern "C" {
#include "port/quest_stereo.h"
#include "port/netplay_runtime.h"
#include "game/object.h"
#include "game/board/space.h"
}
#include "port/quest_camera_follow.hpp"
#include "port/quest_scene_fit.hpp"
#include "ui/ui.hpp"

#include <aurora/stereo.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <android/log.h>
#include <sys/system_properties.h>
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
    uint32_t drawHud;             // 1: draw the interface into its image this frame
};

using FrameFn = bool (*)(QuestStereoFrame *frame);
using ImagesFn = bool (*)(void **eyeBuffers, void **hudBuffers, uint32_t capacity, uint32_t *count, uint32_t *width,
    uint32_t *height, uint32_t *generation, uint32_t *hudWidth, uint32_t *hudHeight);
using SubmittedFn = void (*)(uint32_t image, uint64_t tag, int syncFd, bool hasWorld, void *user);
using GenerationFn = uint32_t (*)(void);
using WorldOnlyFn = bool (*)();
using ScreenRequiredFn = void (*)(bool);
using BoardModeFn = void (*)(bool);
using CancelledFn = void (*)(uint32_t image, uint64_t tag);
using ReleaseImagesFn = void (*)(void **buffers, uint32_t count);
using ScreenHiddenFn = bool (*)();
using TableSetupHoldFn = bool (*)();
using FollowCameraFn = bool (*)();
using HiddenAreaFn = uint32_t (*)(uint32_t eye, float *rects, uint32_t capacity, uint32_t *version);

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
    TableSetupHoldFn tableSetupHold = nullptr; // optional
    FollowCameraFn followCamera = nullptr;     // optional: following by default
    HiddenAreaFn hiddenArea = nullptr;         // optional: every pixel drawn
    uint32_t registeredGeneration = 0;
    uint32_t hiddenVersion = 0;
};

Quest sQuest;
bool sActive = false;
bool sCameraViewSet = false;
QuestStereoFrame sFrame;
OMOVL sScene = DLL_NONE;
bool sSceneFitted = false;
float sSceneScale = 1.0f;
float sSceneCenter[3]{};
float sEyeClip[2][16]{};
// Backdrops (see PartyBoard_StereoBackdrop): the eyes in the game camera's view
// space, and the scene's size in game units.
float sEyeInView[2][3]{};
float sSceneExtent = 0.0f;
// A backdrop is an object around the eyes much larger than the scene on the table.
constexpr float kBackdropExtents = 2.5f;

// A minigame's model turned to its camera's point of view; restarted with each scene.
partyboard::quest::CameraFollow sFollow;
bool sFollowStarted = false;
std::chrono::steady_clock::time_point sFollowAt {};

bool board_scene(OMOVL scene)
{
    return scene >= DLL_w01Dll && scene <= DLL_w06Dll;
}

// The scenes drawn as a model on the table: the boards and the minigames.
bool spatial_scene(OMOVL scene)
{
    return board_scene(scene) || (scene >= DLL_m401Dll && scene <= DLL_m463Dll);
}

// The scene's floor, measured from its geometry during its first frames, is
// what stands on the table: the bottom of the lowest large surface (ground,
// arena floor, the sea around an island), in game units.
constexpr int kFloorFrames = 20;
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
            sQuest.tableSetupHold = reinterpret_cast<TableSetupHoldFn>(dlsym(lib, "PartyBoardQuest_TableSetupHold"));
            sQuest.followCamera = reinterpret_cast<FollowCameraFn>(dlsym(lib, "PartyBoardQuest_StereoFollowCamera"));
            sQuest.hiddenArea = reinterpret_cast<HiddenAreaFn>(dlsym(lib, "PartyBoardQuest_StereoHiddenArea"));
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

extern "C" void PartyBoard_StereoBeginCamera(s16 cameraNo)
{
    sActive = false;
    sCameraViewSet = false;
    AuroraStereoWorldOnly(false);
    if (cameraNo != 0) {
        return;
    }
    if (!find_quest()) {
        AuroraStereoHideScreen(false);
        return;
    }
    const OMOVL scene = omCurrentOvlGet();
    const bool spatialScene = spatial_scene(scene);
    // Selection rooms and title scenes combine several cameras and projections;
    // preserve their original composition until each has a dedicated MR layout.
    const bool screenRequired = !spatialScene || partyboard::ui::any_document_visible();
#if defined(__ANDROID__)
    static int loggedScene = -999;
    static bool loggedScreenRequired = false;
    if (loggedScene != static_cast<int>(scene) || loggedScreenRequired != screenRequired) {
        __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest", "Scene %d: %s", static_cast<int>(scene),
            screenRequired ? "classic screen" : "spatial rendering");
        loggedScene = static_cast<int>(scene);
        loggedScreenRequired = screenRequired;
    }
#endif
    sQuest.screenRequired(screenRequired);
    // The headset shows the model instead of the flat screen: do not present the screen.
    AuroraStereoHideScreen(!screenRequired && sQuest.screenHidden != nullptr && sQuest.screenHidden());
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
    }
}

extern "C" void PartyBoard_StereoCameraView(s32 cameraNo, Mtx view)
{
    if (!sActive || cameraNo != 0) {
        return;
    }
    const OMOVL scene = omCurrentOvlGet();
    if (scene != sScene) {
        sScene = scene;
        sSceneFitted = false;
        sSceneScale = 1.0f;
        std::memset(sSceneCenter, 0, sizeof(sSceneCenter));
        sFloorY = INFINITY;
        sFloorFrames = 0;
        sFloorSettled = false;
        sFollowStarted = false;
    }
    sViewToWorld = inverse_affine(view);
    const bool board = board_scene(scene);
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
    // Minigames have very different coordinate scales: fit their initial camera once.
    const bool minigame = scene >= DLL_m401Dll && scene <= DLL_m463Dll;
    if (minigame && !sSceneFitted) {
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
            sSceneFitted = true;
#if defined(__ANDROID__)
            __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest", "Stereo scene %d: initial camera fit %.3fx",
                static_cast<int>(scene), sSceneScale);
#endif
        }
    }
    // A minigame, where the board's screen stands (quest_xr.cpp puts the
    // model's frame there, its +Z toward the player's eyes): turned so the
    // camera looks along -Z, about what it looks at. The player, on +Z, sees
    // the arena from the camera's side and height:
    //   sceneWorld = scale * turn(yaw, pitch) * move(-center)
    // Not following, and on a board: unturned, the lowest floor on the table.
    // A board never moves: sliding or turning it made the player sick.
    const bool follow = sSceneFitted && !board && (sQuest.followCamera == nullptr || sQuest.followCamera());
    float turn[3][3] {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    if (follow) {
        float cameraYaw = NAN, cameraPitch = NAN;
        if (!partyboard::quest::camera_yaw(view, cameraYaw)) {
            cameraYaw = NAN;
        }
        if (!partyboard::quest::camera_pitch(view, cameraPitch)) {
            cameraPitch = NAN;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!sFollowStarted) {
            sFollow.reset(cameraYaw, cameraPitch);
            sFollowStarted = true;
        } else {
            sFollow.update(cameraYaw, cameraPitch, std::chrono::duration<float>(now - sFollowAt).count());
        }
        sFollowAt = now;
        partyboard::quest::camera_turn(sFollow.yaw(), sFollow.pitch(), turn);
    } else {
        sFollowStarted = false; // following again starts from the camera of that moment
    }
    Mat4 sceneWorld{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            sceneWorld.m[r][c] = turn[r][c] * sSceneScale;
        }
        sceneWorld.m[r][3] = -(turn[r][0] * sSceneCenter[0] + turn[r][1] * sSceneCenter[1]
            + turn[r][2] * sSceneCenter[2]) * sSceneScale;
    }
    sceneWorld.m[3][3] = 1.0f;
    // Unturned, the measured floor on the table (the table's height is the
    // anchor's). A minigame in the air turns about its center instead.
    if (!follow && sFloorSettled && std::isfinite(sFloorY)) {
        sceneWorld.m[1][3] = -sFloorY * sSceneScale;
    }
    // Cancel the game camera every frame: the tracked headset supplies the
    // viewpoint, while the board stays at its physical table anchor.
    const Mat4 world = multiply(multiply(from_rows(sFrame.world), sceneWorld),
        inverse_affine(view));
    sSceneExtent = sSceneFitted ? 2800.0f / sSceneScale : 0.0f;
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
    AuroraStereoSetEyeSize(sFrame.eyeWidth, sFrame.eyeHeight);
    AuroraStereoSetHud(sFrame.drawHud != 0);
    AuroraStereoBegin(sFrame.image, clip, sFrame.tag);
    sCameraViewSet = true;
}

extern "C" void PartyBoard_StereoEndCamera(void)
{
    if (sActive && sCameraViewSet && sSceneFitted && !sFloorSettled && ++sFloorFrames >= kFloorFrames) {
        sFloorSettled = true;
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest", "Scene %d floor: %s %.1f (scene center %.1f)",
            static_cast<int>(sScene), std::isfinite(sFloorY) ? "measured" : "none, keeping", sFloorY,
            sSceneCenter[1]);
#endif
    }
    if (sActive) {
        if (sCameraViewSet) {
            AuroraStereoEnd();
            sGamePerf.end(static_cast<int>(omCurrentOvlGet()));
        } else {
            sQuest.cancelled(sFrame.image, sFrame.tag);
        }
        sActive = false;
    }
    AuroraStereoWorldOnly(false);
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

// Before a board or a minigame starts: wait while the headset asks the player
// to lay the controller on the table (the first time, or when the saved table
// is out of reach). An online game never waits: its peers would not.
extern "C" BOOL PartyBoard_QuestHoldOverlay(OMOVL next)
{
    if (!spatial_scene(next) || PartyBoard_NetplayEnabled() || !find_quest() || sQuest.tableSetupHold == nullptr) {
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
    if (!sActive || !sCameraViewSet || sFloorSettled || !(sSceneExtent > 0.0f)) {
        return;
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
    AuroraStereoSetUncut(partyboard::quest::sphere_inside_eye_sides(sEyeClip, center, std::sqrt(radius2)));
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

extern "C" BOOL PartyBoard_StereoSphereVisible(float x, float y, float z, float radius)
{
    if (!sActive || !sCameraViewSet) return TRUE;
    const float center[3]{x, y, z};
    return partyboard::quest::sphere_visible(sEyeClip, center, radius) ? TRUE : FALSE;
}
