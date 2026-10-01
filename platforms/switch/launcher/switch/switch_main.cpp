// Nintendo Switch (libnx) entry point for the PartyBoard GameCube launcher.
//
// Rendering goes through the same EGL boundary as the bring-up probe
// (platforms/switch/source/switch_egl.cpp); the launcher only adds input,
// audio, the system font, and chain-loading of the engine NRO.

#include <atomic>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include <switch.h>
#include <glad/glad.h>

#include "partyboard_switch/egl.hpp"

#include "app.hpp"
#include "paths.hpp"
#include "render.hpp"
#include "settings.hpp"
#include "sound.hpp"

namespace partyboard::launcher {

namespace {

constexpr u32 kAudioFrames = 1024; // 2 ch * 2 bytes * 1024 = exactly one 0x1000 page
constexpr int kAudioBuffers = 3;

class SwitchPlatform final : public Platform {
public:
    bool init(int argc, char** argv) {
        m_layout = sdLayout("sdmc:");
        if (argc > 0 && argv[0] && std::strncmp(argv[0], "sdmc:/", 6) == 0)
            m_selfPath = argv[0];
        else
            m_selfPath = m_layout.launcherPath;

        m_tickStart = armGetSystemTick();

        // PartyBoard's artwork and fonts (res/ in the repository) ship in romfs.
        m_romfsReady = R_SUCCEEDED(romfsInit());
        if (m_romfsReady) {
            m_bold = readFile("romfs:/rodin-db.otf");
            m_display = readFile("romfs:/n64party.otf");
        }

        padConfigureInput(8, HidNpadStyleSet_NpadStandard | HidNpadStyleTag_NpadGc);
        // A single Joy-Con is one player's controller held sideways, as in the
        // game (platforms/switch/sdl3, patch 0004): the controller applet then
        // offers that grip.
        hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
        padInitializeAny(&m_pad);

        m_plReady = R_SUCCEEDED(plInitialize(PlServiceType_User));
        if (m_plReady && R_FAILED(plGetSharedFontByType(&m_font, PlSharedFontType_Standard)))
            m_plReady = false;
        m_psmReady = R_SUCCEEDED(psmInitialize());
        m_setReady = R_SUCCEEDED(setInitialize());

        // Docked: render at 1080p so text stays sharp on a TV.
        const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
        nwindowSetDimensions(nwindowGetDefault(), docked ? 1920 : 1280, docked ? 1080 : 720);
        if (!PartyBoardSwitch_EglInitialize())
            return false;
        if (!gladLoadGL()) {
            std::printf("launcher: gladLoadGL failed\n");
            return false;
        }
        m_width = static_cast<int>(PartyBoardSwitch_FramebufferWidth());
        m_height = static_cast<int>(PartyBoardSwitch_FramebufferHeight());

        m_bank.generate();
        startAudio();
        return m_plReady;
    }

    void shutdown() {
        stopAudio();
        PartyBoardSwitch_EglShutdown();
        if (m_setReady)
            setExit();
        if (m_psmReady)
            psmExit();
        if (m_plReady)
            plExit();
        if (m_romfsReady)
            romfsExit();
    }

    bool beginFrame() override { return appletMainLoop(); }
    void endFrame() override { PartyBoardSwitch_SwapBuffers(); }

    double now() const override {
        return static_cast<double>(armTicksToNs(armGetSystemTick() - m_tickStart)) / 1.0e9;
    }

    InputState input() override {
        padUpdate(&m_pad);
        InputState state;
        state.held = mapButtons(padGetButtons(&m_pad));
        state.pressed = mapButtons(padGetButtonsDown(&m_pad));
        const HidAnalogStickState stick = padGetStickPos(&m_pad, 0);
        state.stickX = static_cast<float>(stick.x) / 32767.0f;
        state.stickY = static_cast<float>(stick.y) / 32767.0f;
        return state;
    }

    ControllerSlots controllers() override {
        ControllerSlots slots;
        for (int i = 0; i < 4; ++i)
            slots.player[i] = hidGetNpadStyleSet(static_cast<HidNpadIdType>(HidNpadIdType_No1 + i)) != 0;
        slots.handheld = hidGetNpadStyleSet(HidNpadIdType_Handheld) != 0;
        return slots;
    }

    SystemStatus status() override {
        SystemStatus status;
        const time_t now = std::time(nullptr);
        if (const std::tm* local = std::localtime(&now)) {
            status.hour = local->tm_hour;
            status.minute = local->tm_min;
        }
        if (m_psmReady) {
            u32 percent = 0;
            if (R_SUCCEEDED(psmGetBatteryChargePercentage(&percent)))
                status.battery = static_cast<int>(percent);
            PsmChargerType charger = PsmChargerType_Unconnected;
            if (R_SUCCEEDED(psmGetChargerType(&charger)))
                status.charging = charger != PsmChargerType_Unconnected;
        }
        return status;
    }

    int framebufferWidth() const override { return m_width; }
    int framebufferHeight() const override { return m_height; }
    unsigned presentFramebuffer() const override { return 0; }
    const char* shaderHeader() const override { return "#version 330 core\n"; }

    FontBlob font(FontWeight weight) override {
        switch (weight) {
        case FontWeight::Regular:
            // Body text in the console's own shared font.
            if (!m_plReady)
                return {};
            return {static_cast<const uint8_t*>(m_font.address), m_font.size, false};
        case FontWeight::Bold:
            return m_bold.empty() ? FontBlob{} : FontBlob{m_bold.data(), m_bold.size(), false};
        case FontWeight::Display:
            return m_display.empty() ? FontBlob{} : FontBlob{m_display.data(), m_display.size(), false};
        }
        return {};
    }

    std::string resourcePath(const std::string& name) override { return "romfs:/" + name; }

    Language systemLanguage() override {
        if (!m_setReady)
            return Language::English;
        u64 code = 0;
        SetLanguage language = SetLanguage_ENUS;
        if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &language)) &&
            (language == SetLanguage_FR || language == SetLanguage_FRCA))
            return Language::French;
        return Language::English;
    }

    const SdLayout& layout() override { return m_layout; }
    std::string selfPath() override { return m_selfPath; }

    void playSound(Sound sound, float gain) override {
        if (m_audioRunning)
            m_mixer.trigger(sound, gain);
    }

    LaunchResult launch(const std::vector<std::string>& args) override {
        if (args.empty())
            return LaunchResult::Failed;
        if (!envHasNextLoad())
            return LaunchResult::Unsupported;
        const std::string argv = joinArgv(args);
        return R_SUCCEEDED(envSetNextLoad(args[0].c_str(), argv.c_str())) ? LaunchResult::Scheduled
                                                                           : LaunchResult::Failed;
    }

    bool showControllerApplet() override {
        HidLaControllerSupportArg arg;
        hidLaCreateControllerSupportArg(&arg);
        arg.hdr.player_count_min = 1;
        arg.hdr.player_count_max = 4;
        arg.hdr.enable_take_over_connection = 1;
        arg.hdr.enable_left_justify = 1;
        arg.hdr.enable_permit_joy_dual = 1;
        HidLaControllerSupportResultInfo info{};
        return R_SUCCEEDED(hidLaShowControllerSupport(&info, &arg));
    }

private:
    static std::vector<uint8_t> readFile(const char* path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    static uint32_t mapButtons(u64 buttons) {
        uint32_t out = 0;
        const struct {
            u64 hid;
            uint32_t button;
        } table[] = {
            {HidNpadButton_A, kButtonA},         {HidNpadButton_B, kButtonB},
            {HidNpadButton_X, kButtonX},         {HidNpadButton_Y, kButtonY},
            {HidNpadButton_L, kButtonL},         {HidNpadButton_R, kButtonR},
            {HidNpadButton_ZL, kButtonZL},       {HidNpadButton_ZR, kButtonZR},
            {HidNpadButton_Plus, kButtonPlus},   {HidNpadButton_Minus, kButtonMinus},
            {HidNpadButton_Up, kButtonUp},       {HidNpadButton_Down, kButtonDown},
            {HidNpadButton_Left, kButtonLeft},   {HidNpadButton_Right, kButtonRight},
        };
        for (const auto& entry : table) {
            if (buttons & entry.hid)
                out |= entry.button;
        }
        return out;
    }

    void startAudio() {
        if (R_FAILED(audoutInitialize()))
            return;
        if (R_FAILED(audoutStartAudioOut())) {
            audoutExit();
            return;
        }
        constexpr size_t bytes = kAudioFrames * 2 * sizeof(int16_t);
        for (int i = 0; i < kAudioBuffers; ++i) {
            m_audioData[i] = static_cast<int16_t*>(aligned_alloc(0x1000, bytes));
            if (!m_audioData[i]) {
                stopAudio();
                return;
            }
            std::memset(m_audioData[i], 0, bytes);
            m_audioBuffers[i] = AudioOutBuffer{};
            m_audioBuffers[i].buffer = m_audioData[i];
            m_audioBuffers[i].buffer_size = bytes;
            m_audioBuffers[i].data_size = bytes;
            m_audioBuffers[i].data_offset = 0;
        }
        m_audioRunning = true;
        if (R_FAILED(threadCreate(&m_audioThread, audioMain, this, nullptr, 0x4000, 0x2B, -2)) ||
            R_FAILED(threadStart(&m_audioThread))) {
            m_audioRunning = false;
            stopAudio();
            return;
        }
        m_audioThreadStarted = true;
    }

    void stopAudio() {
        m_audioRunning = false;
        if (m_audioThreadStarted) {
            threadWaitForExit(&m_audioThread);
            threadClose(&m_audioThread);
            m_audioThreadStarted = false;
        }
        if (m_audioData[0]) {
            audoutStopAudioOut();
            audoutExit();
        }
        for (int16_t*& data : m_audioData) {
            std::free(data);
            data = nullptr;
        }
    }

    void fill(AudioOutBuffer& buffer) {
        m_mixer.mix(static_cast<int16_t*>(buffer.buffer), kAudioFrames);
        audoutAppendAudioOutBuffer(&buffer);
    }

    static void audioMain(void* arg) {
        auto* self = static_cast<SwitchPlatform*>(arg);
        for (AudioOutBuffer& buffer : self->m_audioBuffers)
            self->fill(buffer);
        while (self->m_audioRunning) {
            AudioOutBuffer* released = nullptr;
            u32 count = 0;
            if (R_FAILED(audoutWaitPlayFinish(&released, &count, 100'000'000ull)) || !released)
                continue;
            self->fill(*released);
            while (R_SUCCEEDED(audoutGetReleasedAudioOutBuffer(&released, &count)) && count > 0 && released)
                self->fill(*released);
        }
    }

    SdLayout m_layout;
    std::string m_selfPath;
    u64 m_tickStart = 0;
    PadState m_pad{};
    PlFontData m_font{};
    bool m_plReady = false;
    bool m_romfsReady = false;
    bool m_psmReady = false;
    std::vector<uint8_t> m_bold;    // FOT-NewRodin Pro DB
    std::vector<uint8_t> m_display; // N64 Party
    bool m_setReady = false;
    int m_width = 1280;
    int m_height = 720;

    SoundBank m_bank;
    Mixer m_mixer{m_bank};
    Thread m_audioThread{};
    bool m_audioThreadStarted = false;
    std::atomic<bool> m_audioRunning{false};
    int16_t* m_audioData[kAudioBuffers] = {};
    AudioOutBuffer m_audioBuffers[kAudioBuffers]{};
};

} // namespace

} // namespace partyboard::launcher

int main(int argc, char** argv) {
    using namespace partyboard::launcher;

    SwitchPlatform platform;
    if (!platform.init(argc, argv)) {
        platform.shutdown();
        return EXIT_FAILURE;
    }

    Renderer renderer;
    if (!renderer.init(platform.shaderHeader(), platform.font(FontWeight::Regular), platform.font(FontWeight::Bold),
                       platform.font(FontWeight::Display))) {
        renderer.shutdown();
        platform.shutdown();
        return EXIT_FAILURE;
    }
    {
        App app(platform, renderer);
        app.init();
        while (app.frame()) {
        }
    }
    // Let the last UI sound finish before the loader takes over.
    svcSleepThread(120'000'000ull);
    renderer.shutdown();
    platform.shutdown();
    return EXIT_SUCCESS;
}
