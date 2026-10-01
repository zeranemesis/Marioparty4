#pragma once

// What the launcher needs from the machine it runs on. The Switch build
// implements it with libnx; the host preview implements it with a headless
// Mesa context and scripted input so the UI can be screenshotted in CI.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "i18n.hpp"
#include "sound.hpp"

namespace partyboard::launcher {

enum Button : uint32_t {
    kButtonA = 1u << 0,
    kButtonB = 1u << 1,
    kButtonX = 1u << 2,
    kButtonY = 1u << 3,
    kButtonL = 1u << 4,
    kButtonR = 1u << 5,
    kButtonZL = 1u << 6,
    kButtonZR = 1u << 7,
    kButtonPlus = 1u << 8,
    kButtonMinus = 1u << 9,
    kButtonUp = 1u << 10,
    kButtonDown = 1u << 11,
    kButtonLeft = 1u << 12,
    kButtonRight = 1u << 13,
};

struct InputState {
    uint32_t held = 0;
    uint32_t pressed = 0;
    float stickX = 0.0f; // left stick, [-1, 1], +y is up
    float stickY = 0.0f;
};

struct ControllerSlots {
    bool player[4] = {};
    bool handheld = false;
};

struct SystemStatus {
    int hour = -1; // -1 when the clock is unavailable
    int minute = -1;
    int battery = -1; // percent, -1 when unknown
    bool charging = false;
};

struct FontBlob {
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool synthesizeBold = false; // embolden outlines when no bold face exists
};

enum class LaunchResult : unsigned char {
    Scheduled,   // the loader will start the engine once the launcher exits
    Unsupported, // this loader cannot chain-load another NRO
    Failed,
};

class Platform {
public:
    virtual ~Platform() = default;

    // False when the system asked the application to close.
    virtual bool beginFrame() = 0;
    virtual void endFrame() = 0;
    virtual double now() const = 0;

    virtual InputState input() = 0;
    virtual ControllerSlots controllers() = 0;
    virtual SystemStatus status() = 0;

    virtual int framebufferWidth() const = 0;
    virtual int framebufferHeight() const = 0;
    // GL framebuffer object the frame is resolved into (0 = window surface).
    virtual unsigned presentFramebuffer() const = 0;
    virtual const char* shaderHeader() const = 0;

    virtual FontBlob font(bool bold) = 0;
    virtual Language systemLanguage() = 0;

    virtual std::vector<std::string> gameDirectories() = 0;
    virtual std::string coversDirectory() = 0;
    virtual std::string settingsPath() = 0;
    virtual std::vector<std::string> engineCandidates() = 0;
    virtual std::string selfPath() = 0;

    virtual void playSound(Sound sound, float gain = 1.0f) = 0;
    virtual LaunchResult launch(const std::vector<std::string>& args) = 0;
    virtual bool showControllerApplet() = 0;
};

} // namespace partyboard::launcher
