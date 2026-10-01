// Credits: TwilitRealm

#ifndef PARTY_BOARD_CONFIG_H
#define PARTY_BOARD_CONFIG_H

#ifndef __cplusplus
#include <stdbool.h>
#endif
#include <card.h>

#ifdef __cplusplus
#include "port/config_var.hpp"

#include <os/OSRtc.h>

namespace partyboard {

using namespace config;

enum class GameLanguage : u8 {
    English = OS_LANGUAGE_ENGLISH,
    German = OS_LANGUAGE_GERMAN,
    French = OS_LANGUAGE_FRENCH,
    Spanish = OS_LANGUAGE_SPANISH,
    Italian = OS_LANGUAGE_ITALIAN,
};

enum class DiscVerificationState : u8 {
    Unknown = 0,
    Success,
    HashMismatch,
};

namespace config {
template <>
struct ConfigEnumRange<GameLanguage> {
    static constexpr auto min = GameLanguage::English;
    static constexpr auto max = GameLanguage::Italian;
};

template <>
struct ConfigEnumRange<DiscVerificationState> {
    static constexpr auto min = DiscVerificationState::Unknown;
    static constexpr auto max = DiscVerificationState::HashMismatch;
};

}

// Persistent user settings

struct UserSettings {
    // Program settings

    struct {
        // Video
        ConfigVar<bool> enableFullscreen;
        ConfigVar<bool> enableVsync;
        ConfigVar<int> targetFrameRate;
        ConfigVar<bool> lockAspectRatio;
        ConfigVar<bool> enableAdaptiveWidescreen;
        ConfigVar<bool> enableFpsOverlay;
        ConfigVar<int> fpsOverlayCorner;
        // Hardware ray traced ambient occlusion and shadows (DXR). Windows +
        // RTX only; silently inert elsewhere. Separate terms of one pass: it
        // runs when either is on. See docs/RAYTRACING.md.
        ConfigVar<bool> enableRayTracedAo;
        ConfigVar<bool> enableRayTracedShadows;
        ConfigVar<bool> enableRayTracedReflections;
        // Trace resolution and ray counts as one level, 0 cheapest. The cost of
        // the pass measured between 1.8 and 6.6 ms across real boards at the
        // middle level, so this is what a card with less headroom turns down.
        ConfigVar<int> rayTracingQuality;
        // How the internal frame is fitted to the window: 0 bilinear, 1 area,
        // 2 FSR 1. FSR 1 only acts when the internal frame is the smaller one.
        ConfigVar<int> upscaler;
    } video;

    struct {
        // Audio
        ConfigVar<int> masterVolume;
        ConfigVar<int> mainMusicVolume;
        ConfigVar<int> subMusicVolume;
        ConfigVar<int> soundEffectsVolume;
        ConfigVar<int> fanfareVolume;
        ConfigVar<bool> enableReverb;
        ConfigVar<bool> enableHrtf;
        ConfigVar<bool> menuSounds;
    } audio;

    // Game settings

    struct {
        ConfigVar<GameLanguage> language;

        // QoL
        ConfigVar<bool> enableQuickTransform;

        // Preferences
        ConfigVar<bool> pauseOnFocusLost;
        ConfigVar<bool> enableAchievementToasts;
        ConfigVar<bool> enableControllerToasts;

        // Graphics
        ConfigVar<int> internalResolutionScale;
        ConfigVar<int> shadowResolutionMultiplier;
        ConfigVar<bool> precompileShaders;

        // Audio

        // Input
        ConfigVar<bool> allowBackgroundInput;
        // On-screen controller: 0 = automatic (phones and tablets without a gamepad), 1 = always, 2 = never.
        ConfigVar<int> touchControls;
        ConfigVar<int> touchControlsOpacity; // percent

        // Cheats
        ConfigVar<bool> infiniteHearts;
        ConfigVar<bool> unlockAllMinigames;
        ConfigVar<bool> unlockBowsersGnarlyParty;

        // Technical

        // Controls
        ConfigVar<bool> enableTurboKeybind;

        // Tools
        ConfigVar<bool> speedrunMode;
        ConfigVar<bool> recordingMode;
    } game;

    struct {
        ConfigVar<std::string> isoPath;
        ConfigVar<DiscVerificationState> isoVerification;
        ConfigVar<std::string> graphicsBackend;
        ConfigVar<bool> skipPreLaunchUI;
        ConfigVar<bool> skipBootSequence;
        ConfigVar<bool> showPipelineCompilation;
        ConfigVar<bool> wasPresetChosen;
        ConfigVar<bool> enableCrashReporting;
        ConfigVar<bool> checkForUpdates;
        ConfigVar<int> cardFileType;
        ConfigVar<bool> enableAdvancedSettings;
    } backend;

    struct {
        // RetroAchievements (src/port/retroachievements.cpp). Only the session
        // token is kept, never the password.
        ConfigVar<bool> enabled;
        ConfigVar<std::string> username;
        ConfigVar<std::string> token;
    } retroAchievements;

    struct {
        // The name shown to the other players in an online lobby.
        ConfigVar<std::string> nickname;
    } online;
};

UserSettings& getSettings();

void registerSettings();

}
#endif // __cplusplus

#ifdef __cplusplus
extern "C" {
#endif

CARDFileType partyboard_settings_card_file_type(void);
bool partyboard_settings_enableTurboKeybind(void);
bool partyboard_settings_skipBootSequence(void);
bool partyboard_settings_unlock_all_minigames(void);
bool partyboard_settings_unlock_bowsers_gnarly_party(void);
bool partyboard_settings_adaptive_widescreen(void);

#ifdef __cplusplus
}
#endif

#endif // PARTY_BOARD_CONFIG_H
