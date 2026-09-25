// Credits: TwilitRealm
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef __ANDROID__
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#endif

#include "imgui/ImGuiEngine.hpp"
#include "iso_validate.hpp"
#include "ui/ui.hpp"

#include "partyboard_version.h"
#include "ui/menu_bar.hpp"
#include "ui/overlay.hpp"
#include "ui/touch_overlay.hpp"
#include "ui/precompile.hpp"
#include "ui/prelaunch.hpp"
#include "ui/preset.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_log.h>
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <dolphin/gx/GXAurora.h>

#include <chrono>
#include <dolphin/os.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>
#include <game/disp.h>
#include <port/config.hpp>
#include <port/dolassets.h>
#include <port/main.h>
#include <port/mods.h>
#include <port/settings.h>
#include <port/netplay_runtime.h>
#include <port/port_version.h>
#include <port/retroachievements.h>

#include <aurora/dvd.h>
#include <aurora/lib/logging.hpp>
#include <game/card.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" int game_main();

using namespace std::string_literals;
using namespace std::string_view_literals;

static bool mainCalled = false;

AuroraInfo auroraInfo;
std::filesystem::path PartyBoard_ConfigPath;

aurora::Module PartyBoardMainLog("partyboard::main");

static u8 selectedLanguage;

u8 OSGetLanguage() {
    return selectedLanguage;
}

static void LanguageInit() {
    // Keep language at 0 (English) if not on a PAL disc.
    // Doubt this matters, but avoid funky shit.
    if (!partyboard::version::isRegionPal()) {
        return;
    }

    // Cache this to avoid funky shenanigans.
    selectedLanguage = static_cast<u8>(partyboard::getSettings().game.language.getValue());
}

bool launchUILoop() {
    while (PartyBoard_IsRunning && !PartyBoard_IsGameLaunched) {
        const AuroraEvent* event = aurora_update();
        while (event != nullptr && event->type != AURORA_NONE) {
            switch (event->type) {
                case AURORA_SDL_EVENT:
                    partyboard::ui::handle_event(event->sdl);
                // partyboard::g_imguiConsole.HandleSDLEvent(event->sdl);
                break;
                case AURORA_DISPLAY_SCALE_CHANGED:
                    // TODO PC
                    // partyboard::ImGuiEngine_Initialize(event->windowSize.scale);
                break;
                case AURORA_EXIT:
                    return false;
            }

            event++;
        }

        if (!aurora_begin_frame()) {
            PartyBoardMainLog.debug("aurora_begin_frame returned false, skipping draw this frame");
            continue;
        }

        partyboard::ui::update();

        // partyboard::g_imguiConsole.PreDraw();
        // partyboard::g_imguiConsole.PostDraw();

        aurora_end_frame();
    }

    return PartyBoard_IsRunning;
}

void aurora_log_callback(AuroraLogLevel level, const char* module, const char *message, unsigned int len)
{
    const char *levelStr = "??";
    FILE *out = stdout;
    switch (level) {
        case LOG_DEBUG:
            levelStr = "DEBUG";
            break;
        case LOG_INFO:
            levelStr = "INFO";
            break;
        case LOG_WARNING:
            levelStr = "WARNING";
            break;
        case LOG_ERROR:
            levelStr = "ERROR";
            out = stderr;
            break;
        case LOG_FATAL:
            levelStr = "FATAL";
            out = stderr;
            break;
    }
#ifdef __ANDROID__
    // stdout and stderr lead nowhere on Android; SDL's log is what reaches logcat.
    (void)out;
    (void)levelStr;
    SDL_LogPriority priority = SDL_LOG_PRIORITY_INFO;
    switch (level) {
        case LOG_DEBUG:
            priority = SDL_LOG_PRIORITY_DEBUG;
            break;
        case LOG_INFO:
            priority = SDL_LOG_PRIORITY_INFO;
            break;
        case LOG_WARNING:
            priority = SDL_LOG_PRIORITY_WARN;
            break;
        case LOG_ERROR:
            priority = SDL_LOG_PRIORITY_ERROR;
            break;
        case LOG_FATAL:
            priority = SDL_LOG_PRIORITY_CRITICAL;
            break;
    }
    SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, priority, "[%s] %s", module, message);
#else
    fprintf(out, "[%s | %s] %s\n", levelStr, module, message);
#endif
    if (level == LOG_FATAL) {
        fflush(out);
        abort();
    }
}

static bool try_parse_backend(std::string_view backend, AuroraBackend& outBackend) {
    if (backend == "auto") {
        outBackend = BACKEND_AUTO;
        return true;
    }
    if (backend == "d3d11") {
        outBackend = BACKEND_D3D11;
        return true;
    }
    if (backend == "d3d12") {
        outBackend = BACKEND_D3D12;
        return true;
    }
    if (backend == "metal") {
        outBackend = BACKEND_METAL;
        return true;
    }
    if (backend == "vulkan") {
        outBackend = BACKEND_VULKAN;
        return true;
    }
    if (backend == "opengl") {
        outBackend = BACKEND_OPENGL;
        return true;
    }
    if (backend == "opengles") {
        outBackend = BACKEND_OPENGLES;
        return true;
    }
    if (backend == "webgpu") {
        outBackend = BACKEND_WEBGPU;
        return true;
    }
    if (backend == "null") {
        outBackend = BACKEND_NULL;
        return true;
    }

    return false;
}

static std::string_view backend_name(AuroraBackend backend) {
    switch (backend) {
        default:
            return "Auto"sv;
        case BACKEND_D3D12:
            return "D3D12"sv;
        case BACKEND_D3D11:
            return "D3D11"sv;
        case BACKEND_METAL:
            return "Metal"sv;
        case BACKEND_VULKAN:
            return "Vulkan"sv;
        case BACKEND_OPENGL:
            return "OpenGL"sv;
        case BACKEND_OPENGLES:
            return "OpenGL ES"sv;
        case BACKEND_WEBGPU:
            return "WebGPU"sv;
        case BACKEND_NULL:
            return "Null"sv;
    }
}

static bool IsBackendAvailable(AuroraBackend backend) {
    if (backend == BACKEND_AUTO) {
        return true;
    }

    size_t availableBackendCount = 0;
    const AuroraBackend* availableBackends = aurora_get_available_backends(&availableBackendCount);
    for (size_t i = 0; i < availableBackendCount; ++i) {
        if (availableBackends[i] == backend) {
            return true;
        }
    }

    return false;
}

static AuroraBackend ResolveDesiredBackend() {
    AuroraBackend desiredBackend = BACKEND_AUTO;

    if (!try_parse_backend(
                   static_cast<const std::string&>(partyboard::getSettings().backend.graphicsBackend),
                   desiredBackend))
    {
        PartyBoardMainLog.warn("Unknown configured backend '{}', falling back to Auto",
                     static_cast<const std::string&>(partyboard::getSettings().backend.graphicsBackend));
        desiredBackend = BACKEND_AUTO;
    }

    if (!IsBackendAvailable(desiredBackend)) {
        PartyBoardMainLog.warn("Requested backend '{}' is unavailable, falling back to Auto",
                     backend_name(desiredBackend));
        desiredBackend = BACKEND_AUTO;
    }

    return desiredBackend;
}


static void migrate_directory(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::create_directories(to, ec);
    if (ec) {
        return;
    }

    for (std::filesystem::recursive_directory_iterator it(
             from, std::filesystem::directory_options::skip_permission_denied, ec);
        it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec) {
            return;
        }

        const auto relativePath = std::filesystem::relative(it->path(), from, ec);
        if (ec) {
            return;
        }

        const auto targetPath = to / relativePath;
        if (it->is_directory(ec)) {
            std::filesystem::create_directories(targetPath, ec);
            if (ec) {
                return;
            }
        } else if (it->is_regular_file(ec) && !std::filesystem::exists(targetPath, ec)) {
            std::filesystem::create_directories(targetPath.parent_path(), ec);
            if (ec) {
                return;
            }
            std::filesystem::copy_file(
                it->path(), targetPath, std::filesystem::copy_options::skip_existing, ec);
            if (ec) {
                return;
            }
        }
    }
}

static std::filesystem::path calculate_config_path() {
#ifdef _WIN32
    // Keep offline menu/save regression tests isolated from the user's normal
    // preferences and memory cards. Normal launches never set this variable.
    if (const auto *path = _wgetenv(L"PARTYBOARD_TEST_PROFILE"); path && *path)
        return std::filesystem::path(path);
    // Explicit online test isolation: normal online/offline preference paths
    // are unchanged unless the test runner supplies a temporary profile.
    if (PartyBoard_NetplayEnabled()) {
        if (const auto *path = _wgetenv(L"PARTYBOARD_NETPLAY_TEST_PROFILE"); path && *path)
            return std::filesystem::path(path);
    }
#endif
#ifdef __APPLE__
#if TARGET_OS_IOS && !TARGET_OS_TV
    const char* documentsPath = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    if (!documentsPath) {
        PartyBoardMainLog.error("Unable to get iOS Documents path: {}", SDL_GetError());
    }

    std::filesystem::path configPath = reinterpret_cast<const char8_t*>(documentsPath);

    char* oldPrefPath = SDL_GetPrefPath("MarioPartyRD", "Party Board");
    if (oldPrefPath) {
        const std::filesystem::path oldConfigPath = reinterpret_cast<const char8_t*>(oldPrefPath);
        SDL_free(oldPrefPath);

        std::error_code ec;
        if (oldConfigPath != configPath && std::filesystem::exists(oldConfigPath, ec)) {
            migrate_directory(oldConfigPath, configPath);
        }
    }

    return configPath;
#endif
#endif

    // On Android this is the app's internal files directory (Context.getFilesDir()):
    // private, kept across updates, removed only on uninstall or "Clear storage".
    // Config, memory cards, achievements and the pipeline cache all live there.
    char* result = SDL_GetPrefPath("MarioPartyRD", "Party Board");
    if (!result) {
        PartyBoardMainLog.error("Unable to get PrefPath: {}", SDL_GetError());
        return {};
    }

    std::filesystem::path configPath = reinterpret_cast<const char8_t*>(result);
    SDL_free(result);
    return configPath;
}

static void EnsureInitialPipelineCache(const std::filesystem::path& configDir) {
#ifdef __ANDROID__
    // The seed is an APK asset, not a file next to the binary (SDL_GetBasePath()
    // is "./" here). Aurora merges it itself through SDL_IOFromFile, which reads
    // assets, so there is nothing to copy.
    (void)configDir;
    return;
#endif
    if (configDir.empty()) {
        return;
    }

    const std::filesystem::path pipelineCachePath = configDir / "pipeline_cache.db";
    if (std::filesystem::exists(pipelineCachePath)) {
        return;
    }

    const char* basePath = SDL_GetBasePath();
    if (basePath == nullptr) {
        PartyBoardMainLog.error("Unable to resolve base path while seeding pipeline cache: {}", SDL_GetError());
        return;
    }

    const std::filesystem::path initialPipelineCachePath =
        std::filesystem::path(basePath) / "initial_pipeline_cache.db";
    if (!std::filesystem::exists(initialPipelineCachePath)) {
        PartyBoardMainLog.error("No bundled initial pipeline cache found at '{}'", initialPipelineCachePath.string());
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(configDir, ec);
    if (ec) {
        PartyBoardMainLog.error("Failed to create config directory '{}' for pipeline cache: {}",
                     configDir.string(), ec.message());
        return;
    }

    std::filesystem::copy_file(initialPipelineCachePath, pipelineCachePath, std::filesystem::copy_options::none, ec);
    if (ec) {
        PartyBoardMainLog.error("Failed to seed pipeline cache from '{}' to '{}': {}",
                     initialPipelineCachePath.string(), pipelineCachePath.string(), ec.message());
        return;
    }

    PartyBoardMainLog.info("Seeded pipeline cache from '{}'", initialPipelineCachePath.string());
}

static constexpr PADDefaultMapping defaultPadMapping = {
    .buttons = {
        {SDL_GAMEPAD_BUTTON_SOUTH, PAD_BUTTON_A},
        {SDL_GAMEPAD_BUTTON_EAST, PAD_BUTTON_B},
        {SDL_GAMEPAD_BUTTON_WEST, PAD_BUTTON_X},
        {SDL_GAMEPAD_BUTTON_NORTH, PAD_BUTTON_Y},
        {SDL_GAMEPAD_BUTTON_START, PAD_BUTTON_START},
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_TRIGGER_Z},
        {PAD_NATIVE_BUTTON_INVALID, PAD_TRIGGER_L},
        {PAD_NATIVE_BUTTON_INVALID, PAD_TRIGGER_R},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, PAD_BUTTON_UP},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PAD_BUTTON_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PAD_BUTTON_LEFT},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_BUTTON_RIGHT},
    },
    .axes = {
        {{SDL_GAMEPAD_AXIS_LEFTX, AXIS_SIGN_POSITIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_LEFT_X_POS},
        {{SDL_GAMEPAD_AXIS_LEFTX, AXIS_SIGN_NEGATIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_LEFT_X_NEG},
        // SDL's gamepad y-axis is inverted from GC's
        {{SDL_GAMEPAD_AXIS_LEFTY, AXIS_SIGN_NEGATIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_LEFT_Y_POS},
        {{SDL_GAMEPAD_AXIS_LEFTY, AXIS_SIGN_POSITIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_LEFT_Y_NEG},
        {{SDL_GAMEPAD_AXIS_RIGHTX, AXIS_SIGN_POSITIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_X_POS},
        {{SDL_GAMEPAD_AXIS_RIGHTX, AXIS_SIGN_NEGATIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_X_NEG},
        // see above
        {{SDL_GAMEPAD_AXIS_RIGHTY, AXIS_SIGN_NEGATIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_Y_POS},
        {{SDL_GAMEPAD_AXIS_RIGHTY, AXIS_SIGN_POSITIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_Y_NEG},
        {{SDL_GAMEPAD_AXIS_LEFT_TRIGGER, AXIS_SIGN_POSITIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_TRIGGER_L},
        {{SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, AXIS_SIGN_POSITIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_TRIGGER_R},
    },
};

static bool online_wait_for_start(bool pumpEvents) {
#ifdef _WIN32
    const auto *disc = _wgetenv(L"PARTYBOARD_ONLINE_DISC");
    if (!PartyBoard_NetplayEnabled() || !disc || !*disc) return true;
    const auto validName = [](const wchar_t *name) {
        return name && std::wstring_view(name).starts_with(L"Local\\PartyBoardOnlineStart-")
            && std::wcslen(name) < 180;
    };
    const auto *readyName = _wgetenv(L"PARTYBOARD_ONLINE_READY");
    const auto *goName = _wgetenv(L"PARTYBOARD_ONLINE_GO");
    const auto *cancelName = _wgetenv(L"PARTYBOARD_ONLINE_CANCEL");
    if (!validName(readyName) || !validName(goName) || !validName(cancelName)) return false;
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName);
    HANDLE go = OpenEventW(SYNCHRONIZE, FALSE, goName);
    HANDLE cancel = OpenEventW(SYNCHRONIZE, FALSE, cancelName);
    bool started = false;
    if (ready && go && cancel && SetEvent(ready)) {
        HANDLE handles[] = {cancel, go};
        const ULONGLONG deadline = GetTickCount64() + 120000;
        while (GetTickCount64() < deadline && PartyBoard_IsRunning) {
            const DWORD result = WaitForMultipleObjects(2, handles, FALSE, 10);
            if (result == WAIT_OBJECT_0 + 1) { started = true; break; }
            if (result != WAIT_TIMEOUT) break;
            if (pumpEvents) {
                const AuroraEvent *event = aurora_update();
                while (event && event->type != AURORA_NONE) {
                    if (event->type == AURORA_EXIT) PartyBoard_IsRunning = false;
                    ++event;
                }
            }
        }
    }
    if (ready) CloseHandle(ready);
    if (go) CloseHandle(go);
    if (cancel) CloseHandle(cancel);
    return started;
#elif defined(__ANDROID__)
    // The lobby runs in the app's ":online" process (platforms/android,
    // online/GameLink.java) and hands over "port:secret". READY is the hello
    // below; GO is one 'G' byte back. The socket is never closed: the lobby
    // learns this game has ended, however it ended, from the end of stream.
    static int link = -1;
    const char *disc = std::getenv("PARTYBOARD_ONLINE_DISC");
    const char *barrier = std::getenv("PARTYBOARD_ONLINE_BARRIER");
    if (!PartyBoard_NetplayEnabled() || !disc || !*disc) return true;
    if (!barrier || !*barrier) return false;
    const char *colon = std::strchr(barrier, ':');
    if (!colon || std::strlen(colon + 1) != 32) return false;
    const long port = std::strtol(barrier, nullptr, 10);
    if (port <= 0 || port > 65535) return false;
    if (link < 0) {
        const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return false;
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
            close(fd);
            PartyBoardMainLog.error("Online lobby unreachable, leaving");
            return false;
        }
        std::string hello = "PBGAME1\n";
        hello.append(colon + 1, 32);
        hello.push_back('R');
        if (send(fd, hello.data(), hello.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(hello.size())) {
            close(fd);
            return false;
        }
        link = fd;
    }
    bool started = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    while (std::chrono::steady_clock::now() < deadline && PartyBoard_IsRunning) {
        pollfd wait { link, POLLIN, 0 };
        const int ready = poll(&wait, 1, 10);
        if (ready < 0) break;
        if (ready > 0) {
            char answer = 0;
            started = recv(link, &answer, 1, 0) == 1 && answer == 'G';
            break;
        }
        if (pumpEvents) {
            const AuroraEvent *event = aurora_update();
            while (event && event->type != AURORA_NONE) {
                if (event->type == AURORA_EXIT) PartyBoard_IsRunning = false;
                ++event;
            }
        }
    }
    PartyBoardMainLog.info("Online start barrier: {}", started ? "go" : "cancelled");
    return started;
#else
    return true;
#endif
}

// Phones and tablets (SDL sends these on Android and iOS; never on desktop).
//
// Pausing is SDL's job, not ours: on Android the next SDL_PumpEvents after the
// activity pauses blocks until it resumes (SDL_HINT_ANDROID_BLOCK_ON_PAUSE,
// on by default) and SDL pauses the audio device with it, so game logic, the
// MusyX output and rendering all stop. Aurora reports it as AURORA_PAUSED /
// AURORA_UNPAUSED, which main.c uses to reset the frame pacer.
//
// What SDL cannot do is keep our data: a backgrounded app can be killed with
// no further notice, and by then nothing runs. This must happen in an event
// watch, because the events themselves are only queued -- the thread blocks
// before the game loop could ever read them. Settings are already written on
// every change and memory cards on every CARD call; this is the last chance.
static bool SDLCALL OnAppLifecycleEvent(void*, SDL_Event* event) {
    switch (event->type) {
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
        case SDL_EVENT_TERMINATING:
            PartyBoardMainLog.info("{}, saving settings",
                event->type == SDL_EVENT_TERMINATING ? "Terminating" : "Entering background");
            partyboard::config::Save();
            fflush(stdout);
            fflush(stderr);
            break;
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            PartyBoardMainLog.info("Back in the foreground");
            break;
        case SDL_EVENT_LOW_MEMORY:
            PartyBoardMainLog.warn("The system reported low memory");
            break;
        default:
            break;
    }
    return true;
}

extern "C" bool PartyBoard_OnlineWaitForStart(void) { return online_wait_for_start(true); }
extern "C" bool PartyBoard_OnlineBarrierProbe(void) { return online_wait_for_start(false); }

extern "C" int port_main(int argc, char* argv[]) {
    // On iOS, when connected to an external monitor, SDLUIKitSceneDelegate scene:willConnectToSession:
    // can call our main function again. Explicitly guard against this reinitialization.
    if (mainCalled) {
        return 0;
    }
    mainCalled = true;

    partyboard::registerSettings();
    partyboard::config::FinishRegistration();

    PartyBoard_ConfigPath = calculate_config_path();

    partyboard::config::LoadFromUserPreferences();
    std::string onlineDisc;
#ifdef _WIN32
    if (PartyBoard_NetplayEnabled()) {
        if (const auto *path = _wgetenv(L"PARTYBOARD_ONLINE_DISC"); path && *path) {
            const auto utf8 = std::filesystem::path(path).u8string();
            onlineDisc.assign(reinterpret_cast<const char *>(utf8.c_str()));
        }
    }
#elif defined(__ANDROID__)
    // Set by PartyBoardActivity from the lobby's launch intent.
    if (PartyBoard_NetplayEnabled()) {
        if (const auto *path = std::getenv("PARTYBOARD_ONLINE_DISC"); path && *path) {
            onlineDisc = path;
        }
    }
#endif

    // The launcher knows which copy of the game it is starting, and it is the
    // one the player's mods were installed against. Without this the game boots
    // whatever backend.isoPath last remembered, so a second disc quietly plays
    // unmodded. Ignored unless it names a file that exists.
    std::string launcherDisc;
    {
#ifdef _WIN32
        const auto *raw = _wgetenv(L"PARTYBOARD_DISC_IMAGE");
#else
        const auto *raw = std::getenv("PARTYBOARD_DISC_IMAGE");
#endif
        if (raw != nullptr && *raw != 0) {
            const std::filesystem::path candidate(raw);
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error)) {
                const auto utf8 = candidate.u8string();
                launcherDisc.assign(reinterpret_cast<const char *>(utf8.c_str()));
            }
            else {
                PartyBoardMainLog.warn("PARTYBOARD_DISC_IMAGE does not name a readable file, ignoring it");
            }
        }
    }

    EnsureInitialPipelineCache(PartyBoard_ConfigPath);
    // TODO: How to handle this?
    //PADSetDefaultMapping(&defaultPadMapping, PAD_TYPE_STANDARD);

    {
        const auto configPathString = PartyBoard_ConfigPath.u8string();
        AuroraConfig config{};
        config.appName = "Party Board";
        config.userPath = reinterpret_cast<const char*>(configPathString.c_str());
        config.vsync = partyboard::getSettings().video.enableVsync;
        config.startFullscreen = partyboard::getSettings().video.enableFullscreen;
        config.windowPosX = -1;
        config.windowPosY = -1;
        config.windowWidth = HU_FB_WIDTH * 2;
        config.windowHeight = HU_FB_HEIGHT * 2;
        config.desiredBackend = ResolveDesiredBackend();
        config.logCallback = &aurora_log_callback;
        config.mem1Size = 64 * 1024 * 1024;
        config.mem2Size = 24 * 1024 * 1024;
        config.allowJoystickBackgroundEvents = PartyBoard_NetplayEnabled() || partyboard::getSettings().game.allowBackgroundInput;
        config.pauseOnFocusLost = !PartyBoard_NetplayEnabled() && partyboard::getSettings().game.pauseOnFocusLost;
        // config.imGuiInitCallback = &aurora_imgui_init_callback;
        // Aurora can write out every decoded texture it loads, which is the one
        // way to look at what a draw actually samples instead of inferring it
        // from code. Opt-in: it writes a DDS per texture into
        // %APPDATA%/Party Board/texture_dumps.
        config.allowTextureDumps = std::getenv("PARTYBOARD_DUMP_TEXTURES") != nullptr;
        auroraInfo = aurora_initialize(argc, argv, &config);
    }
    if (!SDL_AddEventWatch(OnAppLifecycleEvent, nullptr)) {
        PartyBoardMainLog.warn("Unable to watch app lifecycle events: {}", SDL_GetError());
    }

#ifdef PARTY_BOARD_DISCORD
    partyboard::discord::initialize();
#endif

    char windowTitle[100];
    snprintf(windowTitle, sizeof(windowTitle), "PartyBoard %s", PARTY_BOARD_WC_DESCRIBE);
    VISetWindowTitle(windowTitle);

    if (PartyBoard_NetplayEnabled()
        || (!partyboard::getSettings().video.enableAdaptiveWidescreen
            && partyboard::getSettings().video.lockAspectRatio)) {
        AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
    } else {
        AuroraSetViewportPolicy(AURORA_VIEWPORT_STRETCH);
    }
    VISetFrameBufferScale(PartyBoard_NetplayEnabled()
        ? 1
        : partyboard::getSettings().game.internalResolutionScale.getValue());

    // TODO PC
    // partyboard::audio::SetMasterVolume(partyboard::getSettings().audio.masterVolume / 100.0f);
    // partyboard::audio::SetEnableReverb(partyboard::getSettings().audio.enableReverb);
    // partyboard::audio::EnableHrtf = partyboard::getSettings().audio.enableHrtf;

    // Run ImGui UI loop if Aurora couldn't initialize a backend
    if (auroraInfo.backend == BACKEND_NULL) {
        launchUILoop();
        fflush(stdout);
        fflush(stderr);
#ifdef PARTY_BOARD_DISCORD
        partyboard::discord::shutdown();
#endif
        partyboard::ui::shutdown();
        aurora_shutdown();
        return 0;
    }

    partyboard::ui::initialize();
    // Under the overlay, so toasts stay readable over the screen controls.
    partyboard::ui::push_document(std::make_unique<partyboard::ui::TouchOverlay>(), true, true);
    partyboard::ui::push_document(std::make_unique<partyboard::ui::Overlay>(), true, true);
    partyboard::ui::push_document(std::make_unique<partyboard::ui::MenuBar>(), false);

    // Invalidate a bad saved isoPath so that Party Board can't get blocked from starting up.
    // This is only a metadata check; full hash verification is handled by the prelaunch UI.
    bool forcePreLaunchUI = false;
    bool saveConfigBeforePrelaunch = false;

    const std::string p = partyboard::getSettings().backend.isoPath;
    partyboard::iso::DiscInfo discInfo{};
    if (!p.empty() &&
        partyboard::iso::inspect(p.c_str(), discInfo) != partyboard::iso::ValidationError::Success)
    {
        PartyBoardMainLog.info("Saved DVD image path failed validation, clearing configured path: {}", p);
        partyboard::getSettings().backend.isoPath.setValue("");
        partyboard::getSettings().backend.isoVerification.setValue(partyboard::DiscVerificationState::Unknown);
        forcePreLaunchUI = true;
        saveConfigBeforePrelaunch = true;
    }

    partyboard::iso::log_verification_state(
        partyboard::getSettings().backend.isoPath.getValue(),
        partyboard::getSettings().backend.isoVerification.getValue());

    if (partyboard::getSettings().backend.isoPath.getValue().empty() && launcherDisc.empty()) {
        forcePreLaunchUI = true;
    }
    if (forcePreLaunchUI && partyboard::getSettings().backend.skipPreLaunchUI.getValue()) {
        PartyBoardMainLog.info("Prelaunch UI was disabled with no usable DVD image, enabling prelaunch UI");
        partyboard::getSettings().backend.skipPreLaunchUI.setValue(false);
        saveConfigBeforePrelaunch = true;
    }
    if (saveConfigBeforePrelaunch) {
        partyboard::config::Save();
    }

    if (onlineDisc.empty() && launcherDisc.empty() && !partyboard::getSettings().backend.skipPreLaunchUI) {
        partyboard::ui::push_document(std::make_unique<partyboard::ui::Prelaunch>(), true);

        // pre game launch ui main loop
        if (!launchUILoop()) {
            fflush(stdout);
            fflush(stderr);
#ifdef PARTY_BOARD_DISCORD
            partyboard::discord::shutdown();
#endif
            partyboard::ui::shutdown();
            aurora_shutdown();
            return 0;
        }
    }

    // Shaders are compiled before the game boots rather than during it. The cache
    // is loaded at GPU init but compiled asynchronously, and a draw whose pipeline
    // is not ready is dropped rather than delayed, which is what makes a scene's
    // first visit show missing geometry for a few seconds. Doing it here -- after
    // the prelaunch UI has closed, before the disc boots -- trades a one-off wait
    // for no pop-in at all. Opt-out, since it is a wait the player can see.
    if (partyboard::getSettings().game.precompileShaders.getValue()) {
        const u32 total = AuroraPipelinesQueued();
        if (total > 0) {
            auto &screen = static_cast<partyboard::ui::ShaderPrecompile &>(
                partyboard::ui::push_document(std::make_unique<partyboard::ui::ShaderPrecompile>(), true, true));
            const auto started = std::chrono::steady_clock::now();
            u32 remaining = total;
            bool exitRequested = false;
            while (!exitRequested) {
                remaining = AuroraPipelinesQueued();
                screen.set_progress(total - remaining, total);
                if (remaining == 0) {
                    break;
                }
                // A driver that never finishes must not strand the player on this
                // screen; give up and let the rest build in the background, which
                // is exactly the behaviour there was before this existed.
                if (std::chrono::steady_clock::now() - started > std::chrono::minutes(5)) {
                    PartyBoardMainLog.warn(
                        "Gave up waiting on {} shader pipelines, they will finish in the background", remaining);
                    break;
                }
                // Driving a real frame does two jobs: it draws the progress, and on
                // backends with no dedicated pipeline thread it is what advances the
                // compilation at all, since there the worker only runs from
                // end_pipeline_frame().
                const AuroraEvent *event = aurora_update();
                while (event != nullptr && event->type != AURORA_NONE) {
                    if (event->type == AURORA_SDL_EVENT) {
                        partyboard::ui::handle_event(event->sdl);
                    } else if (event->type == AURORA_EXIT) {
                        exitRequested = true;
                        break;
                    }
                    event++;
                }
                if (exitRequested) {
                    break;
                }
                if (!aurora_begin_frame()) {
                    continue;
                }
                partyboard::ui::update();
                aurora_end_frame();
            }
            screen.pop();
            PartyBoardMainLog.info("Precompiled {} of {} shader pipelines before boot", total - remaining, total);
            if (exitRequested) {
                fflush(stdout);
                fflush(stderr);
                partyboard::ui::shutdown();
                aurora_shutdown();
                return 0;
            }
        }
    }

    const char *dvd_source = "the saved path";
    std::string dvd_path = partyboard::getSettings().backend.isoPath.getValue();
    if (!launcherDisc.empty()) {
        dvd_path = launcherDisc;
        dvd_source = "the launcher";
    }
    if (!onlineDisc.empty()) {
        dvd_path = onlineDisc;
        dvd_source = "the online session";
    }

    if (dvd_path.empty()) {
        PartyBoardMainLog.error("No DVD image specified, unable to boot!");
    }
    else {
        PartyBoardMainLog.info("DVD image chosen by {}", dvd_source);
    }
    if (!PartyBoard_IsGameLaunched &&
        partyboard::iso::inspect(dvd_path.c_str(), discInfo) != partyboard::iso::ValidationError::Success)
    {
        PartyBoardMainLog.error("DVD image failed validation: {}", dvd_path);
    }
    PartyBoardMainLog.info("Loading DVD image: {}", dvd_path);
    if (!aurora_dvd_open(dvd_path.c_str())) {
        PartyBoardMainLog.error("Failed to open DVD image: {}", dvd_path);
        if (!onlineDisc.empty()) { partyboard::ui::shutdown(); aurora_shutdown(); return 3; }
        // Do not remember a disc that cannot be opened (e.g. an Android document
        // whose provider hands out a stream that cannot seek), or every launch
        // would boot straight into it again instead of offering the disc picker.
        if (launcherDisc.empty() && dvd_path == partyboard::getSettings().backend.isoPath.getValue()) {
            partyboard::getSettings().backend.isoPath.setValue("");
            partyboard::getSettings().backend.isoVerification.setValue(partyboard::DiscVerificationState::Unknown);
            partyboard::config::Save();
        }
    }

    // Mods must be overlaid before anything reads the FST.
    if (PartyBoard_InitMods() > 0 && !onlineDisc.empty()) {
        PartyBoardMainLog.warn("Mods are active in an online session; every player must run the same mod list");
    }

    PartyBoard_IsGameLaunched = true;

    if (onlineDisc.empty() && !partyboard::getSettings().backend.wasPresetChosen) {
        partyboard::ui::push_document(std::make_unique<partyboard::ui::PresetWindow>());
    }

    partyboard::version::init();
    if (PartyBoard_NetplayEnabled()) {
        const auto &id = partyboard::version::getDiskID();
        char diagnostic[96];
        std::snprintf(diagnostic, sizeof(diagnostic), "boot_disc id=%.4s%.2s", id.gameName, id.company);
        PartyBoard_NetplayTrace(diagnostic);
    }
    LanguageInit();
    // After the disc is known: the achievement set is chosen by its hash.
    PartyBoard_RAInit();

    // OSInit();

    // Reset Data
    // TODO PC
    // static mDoRstData sResetData = {0};
    // mDoRst::setResetData(&sResetData);
    // mDoRst::offReset();
    // mDoRst::setLogoScnFlag(0);

    InitializeDol();

    game_main();

    // partyboard::MoviePlayerShutdown();

    fflush(stdout);
    fflush(stderr);

    // Notifies all CVs and causes threads to exit
    OSResetSystem(OS_RESET_SHUTDOWN, 0, 0);
    PartyBoard_RAShutdown();

#ifdef PARTY_BOARD_DISCORD
    partyboard::discord::shutdown();
#endif
    partyboard::ui::shutdown();
    aurora_shutdown();

    return 0;
}
