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

extern "C" {
#include "port/quest_stereo.h"
#include "game/object.h"
#include "game/board/space.h"
}
#include "port/quest_scene_fit.hpp"
#include "ui/ui.hpp"

#include <aurora/stereo.h>

#include <cstdint>
#include <cstring>

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <android/log.h>
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
};

using FrameFn = bool (*)(QuestStereoFrame *frame);
using ImagesFn = bool (*)(void **buffers, uint32_t capacity, uint32_t *count, uint32_t *width, uint32_t *height,
    uint32_t *generation, uint32_t *eyeHeight, uint32_t *hudWidth, uint32_t *hudHeight);
using SubmittedFn = void (*)(uint32_t image, uint64_t tag, int syncFd, bool hasWorld, void *user);
using GenerationFn = uint32_t (*)(void);
using WorldOnlyFn = bool (*)();
using ScreenRequiredFn = void (*)(bool);
using BoardModeFn = void (*)(bool);
using CancelledFn = void (*)(uint32_t image, uint64_t tag);

struct Quest {
    bool looked = false;
    FrameFn frame = nullptr;
    ImagesFn images = nullptr;
    SubmittedFn submitted = nullptr;
    GenerationFn generation = nullptr;
    CancelledFn cancelled = nullptr;
    ScreenRequiredFn screenRequired = nullptr;
    BoardModeFn boardMode = nullptr;
    WorldOnlyFn worldOnly = nullptr;
    uint32_t registeredGeneration = 0;
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

struct Mat4 {
    float m[4][4];
};

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
            sQuest.submitted = reinterpret_cast<SubmittedFn>(dlsym(lib, "PartyBoardQuest_StereoSubmitted"));
            sQuest.generation = reinterpret_cast<GenerationFn>(dlsym(lib, "PartyBoardQuest_StereoGeneration"));
            sQuest.worldOnly = reinterpret_cast<WorldOnlyFn>(dlsym(lib, "PartyBoardQuest_StereoWorldOnly"));
            sQuest.screenRequired = reinterpret_cast<ScreenRequiredFn>(dlsym(lib, "PartyBoardQuest_StereoScreenRequired"));
            sQuest.boardMode = reinterpret_cast<BoardModeFn>(dlsym(lib, "PartyBoardQuest_StereoBoardMode"));
            sQuest.cancelled = reinterpret_cast<CancelledFn>(dlsym(lib, "PartyBoardQuest_StereoCancelled"));
            __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest", "Stereo bridge: symbols %s",
                sQuest.frame && sQuest.images && sQuest.submitted && sQuest.generation && sQuest.cancelled
                    ? "ready" : "missing");
        } else {
            // SDL can reach a camera while the headset library is still loading.
            sQuest.looked = false;
        }
    }
    return sQuest.frame != nullptr && sQuest.images != nullptr && sQuest.submitted != nullptr
        && sQuest.generation != nullptr && sQuest.cancelled != nullptr && sQuest.screenRequired != nullptr
        && sQuest.worldOnly != nullptr && sQuest.boardMode != nullptr;
#else
    return false;
#endif
}

// The shared images, once and again whenever the headset makes new ones.
bool register_images()
{
    const uint32_t generation = sQuest.generation();
    if (generation == 0) {
        return false;
    }
    if (generation == sQuest.registeredGeneration) {
        return true;
    }
    void *buffers[8] {};
    uint32_t count = 0, width = 0, height = 0, imagesGeneration = 0;
    uint32_t eyeHeight = 0, hudWidth = 0, hudHeight = 0;
    if (!sQuest.images(buffers, 8, &count, &width, &height, &imagesGeneration, &eyeHeight, &hudWidth, &hudHeight) || count == 0) {
        return false;
    }
    if (!AuroraStereoRegisterImages(buffers, count, width, height, eyeHeight, hudWidth, hudHeight, sQuest.submitted, nullptr)) {
        return false;
    }
    sQuest.registeredGeneration = imagesGeneration;
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "PartyBoardQuest", "Stereo bridge: imported %u images (%ux%u), generation %u",
        count, width, height, imagesGeneration);
#endif
    return true;
}

} // namespace

extern "C" void PartyBoard_StereoBeginCamera(s16 cameraNo)
{
    sActive = false;
    sCameraViewSet = false;
    AuroraStereoWorldOnly(false);
    if (cameraNo != 0 || !find_quest()) {
        return;
    }
    const OMOVL scene = omCurrentOvlGet();
    const bool spatialScene = (scene >= DLL_w01Dll && scene <= DLL_w06Dll)
        || (scene >= DLL_m401Dll && scene <= DLL_m463Dll);
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
    sQuest.boardMode(!screenRequired && scene >= DLL_w01Dll && scene <= DLL_w06Dll);
    if (screenRequired || !register_images()) {
        return;
    }
    AuroraStereoWorldOnly(sQuest.worldOnly());
    sActive = sQuest.frame(&sFrame);
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
    }
    const bool board = scene >= DLL_w01Dll && scene <= DLL_w06Dll;
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
                    minimum[axis] = std::min(minimum[axis], pos[axis]);
                    maximum[axis] = std::max(maximum[axis], pos[axis]);
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
    Mat4 sceneWorld{};
    sceneWorld.m[0][0] = sceneWorld.m[1][1] = sceneWorld.m[2][2] = sSceneScale;
    sceneWorld.m[3][3] = 1.0f;
    for (int axis = 0; axis < 3; ++axis) {
        sceneWorld.m[axis][3] = -sSceneCenter[axis] * sSceneScale;
    }
    if (minigame && sSceneFitted) {
        sceneWorld.m[1][3] += 600.0f; // arena center 15 cm above table at default scale
    }
    // Cancel the game camera every frame: the tracked headset supplies the
    // viewpoint, while the board stays at its physical table anchor.
    const Mat4 world = multiply(multiply(from_rows(sFrame.world), sceneWorld),
        inverse_affine(view));
    float clip[2][16];
    for (int eye = 0; eye < 2; ++eye) {
        const Mat4 eyeClip
            = multiply(from_rows(sFrame.eyeProj[eye]), multiply(from_rows(sFrame.eyeView[eye]), world));
        std::memcpy(clip[eye], eyeClip.m, sizeof(clip[eye]));
    }
    std::memcpy(sEyeClip, clip, sizeof(sEyeClip));
    AuroraStereoBegin(sFrame.image, clip, sFrame.tag);
    sCameraViewSet = true;
}

extern "C" void PartyBoard_StereoEndCamera(void)
{
    if (sActive) {
        if (sCameraViewSet) {
            AuroraStereoEnd();
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
    return scene >= DLL_w01Dll && scene <= DLL_w06Dll && find_quest() && sQuest.worldOnly() ? TRUE : FALSE;
}

extern "C" BOOL PartyBoard_StereoSphereVisible(float x, float y, float z, float radius)
{
    if (!sActive || !sCameraViewSet) return TRUE;
    const float center[3]{x, y, z};
    return partyboard::quest::sphere_visible(sEyeClip, center, radius) ? TRUE : FALSE;
}
