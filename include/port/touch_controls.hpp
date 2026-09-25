#pragma once

#include <dolphin/pad.h>

#include <array>
#include <cstdint>
#include <optional>

// On-screen GameCube controller for phones and tablets, feeding PAD port 1 through Aurora's
// virtual pad (PADSetVirtualStatus). This part is pure logic -- layout, hit testing, multi-touch,
// the floating stick -- so it can be tested without a window; src/port/ui/touch_overlay.cpp draws
// it and routes SDL finger events here.
//
// Everything is in window pixels. The layout is built from the short side of the window, so the
// controls keep their size and spacing from a phone to a tablet.
namespace partyboard::touch {

enum class Control : std::uint8_t {
    Stick,
    A,
    B,
    X,
    Y,
    Z,
    L,
    R,
    Start,
    Menu,
    Count,
};

constexpr std::size_t kControlCount = static_cast<std::size_t>(Control::Count);

struct Circle {
    float x = 0.0f;
    float y = 0.0f;
    float radius = 0.0f;
};

struct Insets {
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float left = 0.0f;
};

struct Layout {
    float width = 0.0f;
    float height = 0.0f;
    // Where the stick rests when nobody holds it; a touch anywhere in the stick zone moves it there.
    std::array<Circle, kControlCount> controls {};
    // The left part of the screen, below the shoulder button, that grabs the stick.
    float stickZoneRight = 0.0f;
    float stickZoneTop = 0.0f;

    const Circle &operator[](Control control) const { return controls[static_cast<std::size_t>(control)]; }
};

Layout make_layout(float width, float height, Insets safe = {});

// What the controller shows and sends this frame.
struct State {
    std::uint16_t buttons = 0;
    std::int8_t stickX = 0;
    std::int8_t stickY = 0;
    std::uint8_t triggerLeft = 0;
    std::uint8_t triggerRight = 0;
    std::array<bool, kControlCount> pressed {};
    // The stick's current centre and knob, in pixels; the centre follows the thumb that grabs it.
    float stickCenterX = 0.0f;
    float stickCenterY = 0.0f;
    float knobX = 0.0f;
    float knobY = 0.0f;
};

class Controller {
public:
    void set_layout(const Layout &layout);
    const Layout &layout() const { return mLayout; }

    // Returns whether the touch landed on a control (and so belongs to the controller).
    bool finger_down(std::uint64_t finger, float x, float y);
    void finger_move(std::uint64_t finger, float x, float y);
    void finger_up(std::uint64_t finger);
    void release_all();
    // Whether this finger is holding one of the controls.
    bool owns(std::uint64_t finger) const;

    const State &state() const { return mState; }
    PADStatus pad_status() const;

    // True once per release of the menu button.
    bool take_menu_request();

private:
    struct Finger {
        std::uint64_t id = 0;
        Control control = Control::Count;
        bool active = false;
    };

    static constexpr std::size_t kMaxFingers = 10;

    Finger *find(std::uint64_t id);
    std::optional<Control> hit(float x, float y) const;
    void update_stick(float x, float y);
    void refresh();

    Layout mLayout;
    std::array<Finger, kMaxFingers> mFingers {};
    State mState;
    bool mMenuRequested = false;
};

} // namespace partyboard::touch
