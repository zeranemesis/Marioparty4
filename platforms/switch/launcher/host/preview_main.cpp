// Headless host preview of the launcher.
//
// Runs the real App/Renderer on a Mesa EGL surfaceless context (GLES 3.0),
// driven by a deterministic 60 Hz clock and scripted input, and writes PNG
// screenshots. It is how the UI is reviewed without a console:
//
//   partyboard_launcher_preview --demo-sd /tmp/sd --out shots
//       --script "wait 1; shot shelf; press right; wait 0.4; shot next"
//
// Script commands (separated by ';'):
//   wait <seconds>          advance the clock
//   press <button>          press for one frame (a b x y l r zl zr plus minus up down left right)
//   hold <button> <seconds> hold while the clock advances
//   shot <name>             write <out>/<name>.png
//   lang fr|en              system language reported to the launcher
//   battery <percent>       battery level shown in the header
//   players <n>             number of connected controllers
//
// --icon <file.png> renders the 256x256 homebrew menu icon instead.
// --log-sounds prints every sound cue with its timestamp.

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "app.hpp"
#include "demo_library.hpp"
#include "image.hpp"
#include "paths.hpp"
#include "render.hpp"
#include "settings.hpp"

using namespace partyboard::launcher;

namespace {

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

class HeadlessGl {
public:
    bool init(int width, int height) {
        auto getPlatformDisplay =
            reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        m_display = getPlatformDisplay ? getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
                                       : eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (m_display == EGL_NO_DISPLAY || !eglInitialize(m_display, nullptr, nullptr)) {
            std::fprintf(stderr, "preview: no EGL display\n");
            return false;
        }
        eglBindAPI(EGL_OPENGL_ES_API);
        const EGLint configAttribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                        EGL_NONE};
        EGLConfig config = nullptr;
        EGLint count = 0;
        if (!eglChooseConfig(m_display, configAttribs, &config, 1, &count) || count == 0) {
            const EGLint fallback[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
            eglChooseConfig(m_display, fallback, &config, 1, &count);
        }
        const EGLint contextAttribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
        m_context = eglCreateContext(m_display, config, EGL_NO_CONTEXT, contextAttribs);
        if (m_context == EGL_NO_CONTEXT || !eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_context)) {
            std::fprintf(stderr, "preview: cannot create a GLES 3 context\n");
            return false;
        }
        std::printf("preview: GL_RENDERER = %s\n", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

        m_width = width;
        m_height = height;
        glGenFramebuffers(1, &m_fbo);
        glGenRenderbuffers(1, &m_color);
        glBindRenderbuffer(GL_RENDERBUFFER, m_color);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_color);
        return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }

    Image capture() const {
        Image image;
        image.width = m_width;
        image.height = m_height;
        image.rgba.resize(size_t(m_width) * m_height * 4);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
        // GL rows start at the bottom.
        const size_t stride = size_t(m_width) * 4;
        std::vector<uint8_t> row(stride);
        for (int y = 0; y < m_height / 2; ++y) {
            uint8_t* a = image.rgba.data() + size_t(y) * stride;
            uint8_t* b = image.rgba.data() + size_t(m_height - 1 - y) * stride;
            std::memcpy(row.data(), a, stride);
            std::memcpy(a, b, stride);
            std::memcpy(b, row.data(), stride);
        }
        for (size_t i = 3; i < image.rgba.size(); i += 4)
            image.rgba[i] = 255;
        return image;
    }

    unsigned framebuffer() const { return m_fbo; }
    int width() const { return m_width; }
    int height() const { return m_height; }

private:
    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLContext m_context = EGL_NO_CONTEXT;
    unsigned m_fbo = 0, m_color = 0;
    int m_width = 0, m_height = 0;
};

class HostPlatform final : public Platform {
public:
    HostPlatform(HeadlessGl& gl, std::string root) : m_gl(gl), m_layout(sdLayout(root)) {
        m_regular = readFile("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
        m_bold = readFile("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf");
    }

    bool beginFrame() override { return true; }
    void endFrame() override {
        ++m_frame;
        m_pressed = 0;
    }
    double now() const override { return static_cast<double>(m_frame) / 60.0; }

    InputState input() override {
        InputState state;
        state.held = m_held | m_pressed;
        state.pressed = m_pressed;
        return state;
    }
    ControllerSlots controllers() override {
        ControllerSlots slots;
        for (int i = 0; i < 4; ++i)
            slots.player[i] = i < m_players;
        return slots;
    }
    SystemStatus status() override { return {14, 32, m_battery, false}; }

    int framebufferWidth() const override { return m_gl.width(); }
    int framebufferHeight() const override { return m_gl.height(); }
    unsigned presentFramebuffer() const override { return m_gl.framebuffer(); }
    const char* shaderHeader() const override {
        return "#version 300 es\nprecision highp float;\nprecision highp int;\n";
    }

    FontBlob font(bool bold) override {
        const std::vector<uint8_t>& data = bold ? m_bold : m_regular;
        return data.empty() ? FontBlob{} : FontBlob{data.data(), data.size(), false};
    }
    Language systemLanguage() override { return m_language; }

    std::vector<std::string> gameDirectories() override { return m_layout.gameDirectories; }
    std::string coversDirectory() override { return m_layout.coversDirectory; }
    std::string settingsPath() override { return m_layout.settingsPath; }
    std::vector<std::string> engineCandidates() override { return m_layout.engineCandidates; }
    std::string selfPath() override { return m_layout.launcherPath; }

    void playSound(Sound sound, float gain) override {
        if (logSounds)
            std::printf("preview: t=%.3f sound %d gain %.2f\n", now(), static_cast<int>(sound), gain);
    }
    LaunchResult launch(const std::vector<std::string>& args) override {
        std::printf("preview: launch argv = %s\n", joinArgv(args).c_str());
        launched = true;
        return LaunchResult::Scheduled;
    }
    bool showControllerApplet() override { return false; }

    uint32_t m_held = 0;
    uint32_t m_pressed = 0;
    int m_players = 1;
    int m_battery = 87;
    Language m_language = Language::French;
    bool launched = false;
    bool logSounds = false;

private:
    HeadlessGl& m_gl;
    SdLayout m_layout;
    std::vector<uint8_t> m_regular;
    std::vector<uint8_t> m_bold;
    uint64_t m_frame = 0;
};

uint32_t parseButton(const std::string& name) {
    static const std::map<std::string, uint32_t> names = {
        {"a", kButtonA},       {"b", kButtonB},          {"x", kButtonX},       {"y", kButtonY},
        {"l", kButtonL},       {"r", kButtonR},          {"zl", kButtonZL},     {"zr", kButtonZR},
        {"plus", kButtonPlus}, {"minus", kButtonMinus},  {"up", kButtonUp},     {"down", kButtonDown},
        {"left", kButtonLeft}, {"right", kButtonRight},
    };
    const auto it = names.find(name);
    return it == names.end() ? 0 : it->second;
}

int renderIcon(HeadlessGl& gl, HostPlatform& platform, const std::string& path) {
    Renderer r;
    if (!r.init(platform.shaderHeader(), platform.font(false), platform.font(true)))
        return 1;
    r.beginFrame(gl.width(), gl.height(), gl.framebuffer(), 2.0f);
    // The canvas is 1280x720 virtual units; the icon is its central square.
    constexpr float S = Renderer::kHeight;
    const float x0 = (Renderer::kWidth - S) * 0.5f;
    r.rect(0, 0, Renderer::kWidth, Renderer::kHeight, rgb(0x0E0A2C));
    r.softRect(x0 + 40, 40, S - 80, S - 80, S * 0.4f, 160.0f, withAlpha(rgb(0x5A49D6), 0.95f));

    r.begin3D(x0, 40, S, S * 0.72f);
    const Vec3 eye{5.1f, 5.6f, 5.1f};
    const Mat4 vp = Mat4::perspective(30.0f * kPi / 180.0f, 1.0f / 0.72f, 0.1f, 50.0f) *
                    Mat4::lookAt(eye, {0.0f, 0.2f, 0.0f}, {0.0f, 1.0f, 0.0f});
    const int cells[8][2] = {{1, 0}, {0, 0}, {0, 1}, {0, 2}, {1, 2}, {2, 2}, {2, 1}, {1, 1}};
    for (int i = 0; i < 8; ++i) {
        const Vec3 p{static_cast<float>(cells[i][0] - 1), 0.47f, static_cast<float>(cells[i][1] - 1)};
        const bool core = i == 7;
        r.cube(vp, Mat4::translate(p) * Mat4::scale(core ? 1.0f : 0.94f), eye,
               core ? rgb(0x8B7DFF) : rgb(0x5B4EE0), core ? 0.18f : 0.0f);
    }
    r.end3D();
    r.text(FontWeight::Bold, 104.0f, Renderer::kWidth * 0.5f, S * 0.70f, "PartyBoard", rgb(0xFFFFFF), Align::Center);
    r.text(FontWeight::Bold, 40.0f, Renderer::kWidth * 0.5f, S * 0.86f, "GAMECUBE", rgb(0xB9B0FF), Align::Center, 14.0f);
    r.endFrame();

    Image full = gl.capture();
    // Crop the central square.
    Image square;
    square.width = square.height = gl.height();
    square.rgba.resize(size_t(square.width) * square.height * 4);
    const int left = (gl.width() - gl.height()) / 2;
    for (int y = 0; y < square.height; ++y)
        std::memcpy(square.rgba.data() + size_t(y) * square.width * 4,
                    full.rgba.data() + (size_t(y) * full.width + left) * 4, size_t(square.width) * 4);
    r.shutdown();
    return writePng(path, square) ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    std::string root = "/tmp/partyboard-sd";
    std::string out = ".";
    std::string script = "wait 1.5; shot shelf";
    std::string icon;
    std::string demo;
    bool demoEngine = true;
    bool logSounds = false;
    int width = 1280, height = 720;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string{}; };
        if (arg == "--root")
            root = next();
        else if (arg == "--demo-sd")
            demo = root = next();
        else if (arg == "--no-engine")
            demoEngine = false;
        else if (arg == "--log-sounds")
            logSounds = true;
        else if (arg == "--out")
            out = next();
        else if (arg == "--script")
            script = next();
        else if (arg == "--icon")
            icon = next();
        else if (arg == "--size")
            std::sscanf(next().c_str(), "%dx%d", &width, &height);
    }

    if (!icon.empty()) {
        width = 256 * 1280 / 720 + 1;
        height = 256;
    }

    HeadlessGl gl;
    if (!gl.init(width, height))
        return 1;

    if (!demo.empty())
        demo::makeDemoSdCard(demo, demoEngine);

    HostPlatform platform(gl, root);
    platform.logSounds = logSounds;
    if (!icon.empty())
        return renderIcon(gl, platform, icon);

    makeDirectories(out);
    Renderer renderer;
    if (!renderer.init(platform.shaderHeader(), platform.font(false), platform.font(true)))
        return 1;

    // Commands that configure the platform must apply before App::init.
    std::vector<std::vector<std::string>> commands;
    {
        std::stringstream all(script);
        std::string item;
        while (std::getline(all, item, ';')) {
            std::stringstream words(item);
            std::vector<std::string> command;
            std::string word;
            while (words >> word)
                command.push_back(word);
            if (!command.empty())
                commands.push_back(command);
        }
    }

    int status = 0;
    {
        App app(platform, renderer);
        bool initialised = false;
        auto step = [&]() {
            if (!initialised) {
                app.init();
                initialised = true;
            }
            return app.frame();
        };
        bool running = true;
        for (const auto& command : commands) {
            if (!running)
                break;
            const std::string& verb = command[0];
            if (verb == "lang" && command.size() > 1) {
                platform.m_language = command[1] == "en" ? Language::English : Language::French;
            } else if (verb == "battery" && command.size() > 1) {
                platform.m_battery = std::atoi(command[1].c_str());
            } else if (verb == "players" && command.size() > 1) {
                platform.m_players = std::atoi(command[1].c_str());
            } else if (verb == "wait" && command.size() > 1) {
                const int frames = static_cast<int>(std::lround(std::atof(command[1].c_str()) * 60.0));
                for (int f = 0; f < frames && running; ++f)
                    running = step();
            } else if (verb == "press" && command.size() > 1) {
                platform.m_pressed = parseButton(command[1]);
                running = step();
            } else if (verb == "hold" && command.size() > 2) {
                platform.m_held = parseButton(command[1]);
                const int frames = static_cast<int>(std::lround(std::atof(command[2].c_str()) * 60.0));
                for (int f = 0; f < frames && running; ++f)
                    running = step();
                platform.m_held = 0;
            } else if (verb == "shot" && command.size() > 1) {
                if (!initialised)
                    running = step();
                const std::string path = out + "/" + command[1] + ".png";
                if (!writePng(path, gl.capture())) {
                    std::fprintf(stderr, "preview: cannot write %s\n", path.c_str());
                    status = 1;
                } else {
                    std::printf("preview: wrote %s\n", path.c_str());
                }
            } else {
                std::fprintf(stderr, "preview: unknown command '%s'\n", verb.c_str());
                status = 1;
            }
        }
        if (!running)
            std::printf("preview: launcher exited%s\n", platform.launched ? " after scheduling the engine" : "");
    }
    renderer.shutdown();
    return status;
}
