#include "port/settings.h"
#include "port/config.hpp"
#include "port/widescreen.h"
#include "port/netplay_runtime.h"
#include "port/widescreen_fit.hpp"
#include "game/disp.h"

#include "dolphin/gx/GXAurora.h"

#include <SDL3/SDL_video.h>
#include <aurora/lib/window.hpp>

namespace partyboard {

// Phones are 19.5:9 to 21:9: stretching the 4:3 picture across one widens
// everything by about two thirds, and the bars either side are where the
// on-screen controls sit. Desktop keeps filling the window. Only a default:
// a choice the player made, or a preset they picked, is kept.
#ifdef __ANDROID__
constexpr bool kLockAspectRatioByDefault = true;
#else
constexpr bool kLockAspectRatioByDefault = false;
#endif

UserSettings g_userSettings = {
    .video = {
        .enableFullscreen {"video.enableFullscreen", false},
        .enableVsync {"video.enableVsync", true},
        .targetFrameRate {"video.targetFrameRate", 60},
        .lockAspectRatio {"video.lockAspectRatio", kLockAspectRatioByDefault},
        .enableAdaptiveWidescreen {"video.enableAdaptiveWidescreen", false},
        .enableFpsOverlay {"game.enableFpsOverlay", false},
        .fpsOverlayCorner {"game.fpsOverlayCorner", 0},
    },

    .audio = {
        .masterVolume {"audio.masterVolume", 80},
        .mainMusicVolume {"audio.mainMusicVolume", 100},
        .subMusicVolume {"audio.subMusicVolume", 100},
        .soundEffectsVolume {"audio.soundEffectsVolume", 100},
        .fanfareVolume {"audio.fanfareVolume", 100},
        .enableReverb {"audio.enableReverb", true},
        .enableHrtf {"audio.enableHrtf", false},
        .menuSounds {"audio.menuSounds", true},
    },

    .game = {
        .language { "game.language", GameLanguage::English },

        // Quality of Life
        .enableQuickTransform {"game.enableQuickTransform", false},

        // Preferences
        .pauseOnFocusLost {"game.pauseOnFocusLost", false},
        .enableAchievementToasts {"game.enableAchievementToasts", true},
        .enableControllerToasts {"game.enableControllerToasts", true},

        // Graphics
        .internalResolutionScale {"game.internalResolutionScale", 0},
        .shadowResolutionMultiplier {"game.shadowResolutionMultiplier", 1},
        .precompileShaders {"game.precompileShaders", true},

        // Audio

        // Input
        .allowBackgroundInput {"game.allowBackgroundInput", true},
        .touchControls {"game.touchControls", 0},
        .touchControlsOpacity {"game.touchControlsOpacity", 60},

        // Cheats
        .infiniteHearts {"game.infiniteHearts", false},
        .unlockAllMinigames {"game.unlockAllMinigames", false},
        .unlockBowsersGnarlyParty {"game.unlockBowsersGnarlyParty", false},

        // Technical

        // Controls
        .enableTurboKeybind {"game.enableTurboKeybind", false},

        // Tools
        .speedrunMode {"game.speedrunMode", false},
        .recordingMode {"game.recordingMode", false}
    },

    .backend = {
        .isoPath {"backend.isoPath", ""},
        .isoVerification {"backend.isoVerification", DiscVerificationState::Unknown},
        .graphicsBackend {"backend.graphicsBackend", "auto"},
        .skipPreLaunchUI {"backend.skipPreLaunchUI", false},
        .skipBootSequence {"backend.skipBootSequence", false},
        .showPipelineCompilation {"backend.showPipelineCompilation", false},
        .wasPresetChosen {"backend.wasPresetChosen", false},
        .enableCrashReporting {"backend.enableCrashReporting", true},
        .checkForUpdates {"backend.checkForUpdates", true},
        .cardFileType {"backend.cardFileType", static_cast<int>(CARD_RAWIMAGE)},
        .enableAdvancedSettings {"backend.enableAdvancedSettings", false},
    },
    .retroAchievements = {
        .enabled {"retroAchievements.enabled", true},
        .username {"retroAchievements.username", ""},
        .token {"retroAchievements.token", ""},
    },
    .online = {
        .nickname {"online.nickname", ""},
    },
};

UserSettings& getSettings() {
    return g_userSettings;
}

void registerSettings() {
    // Video
    Register(g_userSettings.video.enableFullscreen);
    Register(g_userSettings.video.enableVsync);
    Register(g_userSettings.video.targetFrameRate);
    Register(g_userSettings.video.lockAspectRatio);
    Register(g_userSettings.video.enableAdaptiveWidescreen);
    Register(g_userSettings.video.enableFpsOverlay);
    Register(g_userSettings.video.fpsOverlayCorner);

    // Audio
    Register(g_userSettings.audio.masterVolume);
    Register(g_userSettings.audio.mainMusicVolume);
    Register(g_userSettings.audio.subMusicVolume);
    Register(g_userSettings.audio.soundEffectsVolume);
    Register(g_userSettings.audio.fanfareVolume);
    Register(g_userSettings.audio.enableReverb);
    Register(g_userSettings.audio.enableHrtf);
    Register(g_userSettings.audio.menuSounds);

    // Game
    Register(g_userSettings.game.language);
    Register(g_userSettings.game.enableQuickTransform);
    Register(g_userSettings.game.pauseOnFocusLost);
    Register(g_userSettings.game.internalResolutionScale);
    Register(g_userSettings.game.precompileShaders);
    Register(g_userSettings.game.shadowResolutionMultiplier);
    Register(g_userSettings.game.enableAchievementToasts);
    Register(g_userSettings.game.enableControllerToasts);
    Register(g_userSettings.game.enableTurboKeybind);
    Register(g_userSettings.game.speedrunMode);
    Register(g_userSettings.game.recordingMode);
    Register(g_userSettings.game.infiniteHearts);
    Register(g_userSettings.game.unlockAllMinigames);
    Register(g_userSettings.game.unlockBowsersGnarlyParty);
    Register(g_userSettings.game.allowBackgroundInput);
    Register(g_userSettings.game.touchControls);
    Register(g_userSettings.game.touchControlsOpacity);

    Register(g_userSettings.backend.isoPath);
    Register(g_userSettings.backend.isoVerification);
    Register(g_userSettings.backend.graphicsBackend);
    Register(g_userSettings.backend.skipPreLaunchUI);
    Register(g_userSettings.backend.skipBootSequence);
    Register(g_userSettings.backend.showPipelineCompilation);
    Register(g_userSettings.backend.wasPresetChosen);
    Register(g_userSettings.backend.enableCrashReporting);
    Register(g_userSettings.backend.checkForUpdates);
    Register(g_userSettings.backend.cardFileType);
    Register(g_userSettings.backend.enableAdvancedSettings);

    Register(g_userSettings.retroAchievements.enabled);
    Register(g_userSettings.retroAchievements.username);
    Register(g_userSettings.retroAchievements.token);

    Register(g_userSettings.online.nickname);
}

}

extern "C" {

CARDFileType partyboard_settings_card_file_type(void)
{
    return (CARDFileType)partyboard::getSettings().backend.cardFileType.getValue();
}

bool partyboard_settings_enableTurboKeybind(void)
{
    return partyboard::getSettings().game.enableTurboKeybind;
}

bool partyboard_settings_skipBootSequence(void)
{
    // Both peers must run the same startup ticks regardless of offline options.
    return !PartyBoard_NetplayEnabled()
        && partyboard::getSettings().backend.skipBootSequence.getValue();
}

bool partyboard_settings_unlock_all_minigames(void)
{
    // Availability is a gameplay rule: the same A press must not launch a
    // minigame for one peer and hit a locked entry for the other. Online
    // sessions expose the same catalogue, regardless of local cheats/saves.
    // This is an effective value only; offline preferences are not written.
    return PartyBoard_NetplayEnabled()
        || partyboard::getSettings().game.unlockAllMinigames.getValue();
}

bool partyboard_settings_unlock_bowsers_gnarly_party(void)
{
    return partyboard::getSettings().game.unlockBowsersGnarlyParty.getValue();
}

bool partyboard_settings_adaptive_widescreen(void)
{
    return !PartyBoard_NetplayEnabled()
        && partyboard::getSettings().video.enableAdaptiveWidescreen.getValue();
}

bool PartyBoard_WidescreenEnabled(void)
{
    return partyboard_settings_adaptive_widescreen();
}

float PartyBoard_WidescreenAspectScale(void)
{
    SDL_Window *window = aurora::window::get_sdl_window();
    int width = 0;
    int height = 0;

    if (!PartyBoard_WidescreenEnabled() || window == nullptr || !SDL_GetWindowSize(window, &width, &height)) {
        return 1.0f;
    }

    // Narrower than 4:3 (a 5:4 monitor, a portrait screen, a window dragged
    // narrow) is letterboxed rather than cut. The window decides, not the
    // framebuffer: letterboxing makes that one 4:3.
    AuroraSetViewportPolicy(static_cast<s64>(width) * 3 < static_cast<s64>(height) * 4 ? AURORA_VIEWPORT_FIT
                                                                                    : AURORA_VIEWPORT_STRETCH);
    return partyboard::widescreen::aspect_scale(static_cast<float>(width), static_cast<float>(height));
}

static partyboard::widescreen::HudSpan WidescreenHudSpan(float scale)
{
    SDL_Window *window = aurora::window::get_sdl_window();
    int width = 0;
    int height = 0;
    SDL_Rect safe {};
    if (scale == 1.0f || window == nullptr || !SDL_GetWindowSize(window, &width, &height)
        || !SDL_GetWindowSafeArea(window, &safe)) {
        return {};
    }
    return partyboard::widescreen::hud_span(scale, static_cast<float>(width), static_cast<float>(safe.x),
                                            static_cast<float>(safe.w));
}

bool PartyBoard_WidescreenBackdrop(float width, float height)
{
    // Three quarters of the height: some backdrops leave room for borders.
    return width >= HU_DISP_WIDTHF && height >= HU_DISP_HEIGHTF * 0.75f;
}

void PartyBoard_WidescreenStretchHudMatrix(Mtx matrix)
{
    const float scale = PartyBoard_WidescreenAspectScale();
    for (int column = 0; column < 3; ++column) {
        matrix[0][column] *= scale;
    }
}

float PartyBoard_WidescreenHudShift(float hudX)
{
    const float scale = PartyBoard_WidescreenAspectScale();
    if (scale == 1.0f) {
        return 0.0f;
    }
    return partyboard::widescreen::placed_x(hudX, HU_DISP_CENTERX, scale, WidescreenHudSpan(scale)) - hudX;
}

float PartyBoard_WidescreenHudX(float pictureX)
{
    const float scale = PartyBoard_WidescreenAspectScale();
    if (scale == 1.0f) {
        return pictureX;
    }
    return partyboard::widescreen::hud_x(pictureX, HU_DISP_CENTERX, scale, WidescreenHudSpan(scale));
}

float PartyBoard_WidescreenPictureX(float hudX)
{
    const float scale = PartyBoard_WidescreenAspectScale();
    if (scale == 1.0f) {
        return hudX;
    }
    return partyboard::widescreen::picture_x(hudX, HU_DISP_CENTERX, scale, WidescreenHudSpan(scale));
}

float PartyBoard_WidescreenPerspectiveAspect(float originalAspect)
{
    return originalAspect * PartyBoard_WidescreenAspectScale();
}

void PartyBoard_WidescreenOrthoBounds(float originalLeft, float originalRight,
                                      float *renderLeft, float *renderRight)
{
    const float center = (originalLeft + originalRight) * 0.5f;
    const float halfWidth = (originalRight - originalLeft) * 0.5f *
                            PartyBoard_WidescreenAspectScale();
    *renderLeft = center - halfWidth;
    *renderRight = center + halfWidth;
}
}
