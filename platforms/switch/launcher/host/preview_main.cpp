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
// --res <dir> reads PartyBoard's artwork and fonts from <dir> (default: res/).
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

#include <sys/stat.h>

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
    HostPlatform(HeadlessGl& gl, std::string root, std::string resources, std::string assets)
        : m_gl(gl), m_layout(sdLayout(root)), m_resources(std::move(resources)), m_assets(std::move(assets)) {
        // The console uses its shared system font for body text; Inter is the
        // closest stand-in among PartyBoard's own fonts.
        m_fonts[0] = readFile(m_resources + "/Inter-Regular.ttf");
        m_fonts[1] = readFile(m_resources + "/FOT-NewRodin Pro DB.otf");
        m_fonts[2] = readFile(m_resources + "/N64Party.otf");
        if (m_fonts[0].empty())
            m_fonts[0] = readFile("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
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

    FontBlob font(FontWeight weight) override {
        const std::vector<uint8_t>& data = m_fonts[static_cast<int>(weight)];
        return data.empty() ? FontBlob{} : FontBlob{data.data(), data.size(), false};
    }
    Language systemLanguage() override { return m_language; }
    // The launcher's own assets (catalogue, CubeShelf covers) shadow res/.
    std::string resourcePath(const std::string& name) override {
        const std::string asset = m_assets + "/" + name;
        struct stat st{};
        return stat(asset.c_str(), &st) == 0 ? asset : m_resources + "/" + name;
    }

    const SdLayout& layout() override { return m_layout; }
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
    std::string m_resources;
    std::string m_assets;
    std::vector<uint8_t> m_fonts[3];
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
    if (!r.init(platform.shaderHeader(), platform.font(FontWeight::Regular), platform.font(FontWeight::Bold),
                platform.font(FontWeight::Display)))
        return 1;
    auto load = [&](const char* name) {
        Image image;
        return loadPng(platform.resourcePath(name), image, 2048)
                   ? r.createTexture(image.width, image.height, image.rgba.data(), true)
                   : Texture{};
    };
    Texture art = load("prelaunch-bg.png");
    Texture logo = load("logo.png");

    r.beginFrame(gl.width(), gl.height(), gl.framebuffer(), 2.0f);
    // The canvas is 1280x720 virtual units; the icon is its central square:
    // the Mario Party 4 cast from the pre-launch art and the PartyBoard logo.
    constexpr float S = Renderer::kHeight;
    const float x0 = (Renderer::kWidth - S) * 0.5f;
    r.rect(0, 0, Renderer::kWidth, Renderer::kHeight, rgb(0x2B1D5E));
    if (art) {
        const float vSpan = 0.86f;
        const float uSpan = vSpan * static_cast<float>(art.height) / static_cast<float>(art.width);
        r.image(art, x0, 0, S, S, {}, 0.0f, 0.665f - uSpan * 0.5f, 0.04f, 0.665f + uSpan * 0.5f, 0.04f + vSpan);
    }
    r.gradientRect(x0, S * 0.45f, S, S * 0.55f, withAlpha(rgb(0x0B0620), 0.0f), withAlpha(rgb(0x0B0620), 0.0f),
                   withAlpha(rgb(0x0B0620), 0.95f), withAlpha(rgb(0x0B0620), 0.95f));
    if (logo) {
        const float lw = S - 60.0f;
        const float lh = lw * static_cast<float>(logo.height) / static_cast<float>(logo.width);
        r.image(logo, x0 + 30.0f, S - lh - 60.0f, lw, lh);
    }
    r.endFrame();
    r.destroyTexture(art);
    r.destroyTexture(logo);

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
    std::string resources = PARTYBOARD_LAUNCHER_RES_DIR;
    std::string assets = PARTYBOARD_LAUNCHER_ASSETS_DIR;
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
        else if (arg == "--res")
            resources = next();
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

    HostPlatform platform(gl, root, resources, assets);
    platform.logSounds = logSounds;
    if (!icon.empty())
        return renderIcon(gl, platform, icon);

    makeDirectories(out);
    Renderer renderer;
    if (!renderer.init(platform.shaderHeader(), platform.font(FontWeight::Regular), platform.font(FontWeight::Bold), platform.font(FontWeight::Display)))
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
