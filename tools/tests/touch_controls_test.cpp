// The on-screen controller's logic: layout, hit testing, multi-touch and the floating stick.
// Standalone:
//   c++ -std=c++20 -DTARGET_PC -Iinclude -Iextern/aurora/include
//     tools/tests/touch_controls_test.cpp src/port/touch_controls.cpp
#include "port/touch_controls.hpp"

#include <cstdio>
#include <cstdlib>
#include <tuple>

using namespace partyboard::touch;

namespace {

int failures = 0;

void check(bool condition, const char *what)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

bool inside(const Circle &c, float width, float height)
{
    return c.x - c.radius >= 0 && c.y - c.radius >= 0 && c.x + c.radius <= width && c.y + c.radius <= height;
}

bool overlap(const Circle &a, const Circle &b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy < (a.radius + b.radius) * (a.radius + b.radius);
}

} // namespace

int main()
{
    // A 20:9 phone in landscape, a 4:3 tablet, and a phone with a notch on the left.
    for (const auto &[w, h, inset] : { std::tuple { 2400.0f, 1080.0f, 0.0f }, std::tuple { 2048.0f, 1536.0f, 0.0f },
             std::tuple { 2340.0f, 1080.0f, 90.0f } }) {
        const Layout layout = make_layout(w, h, Insets { .left = inset });
        for (std::size_t i = 0; i < kControlCount; ++i) {
            check(inside(layout.controls[i], w, h), "every control fits on screen");
            check(layout.controls[i].x - layout.controls[i].radius >= inset, "no control under the notch");
            for (std::size_t j = i + 1; j < kControlCount; ++j) {
                check(!overlap(layout.controls[i], layout.controls[j]), "controls do not overlap");
            }
        }
        check(layout[Control::A].radius > layout[Control::B].radius, "A is the big button, as on a GameCube pad");
    }

    Controller pad;
    const Layout layout = make_layout(2400.0f, 1080.0f);
    pad.set_layout(layout);
    check(pad.state().buttons == 0 && pad.state().stickX == 0 && pad.state().stickY == 0, "idle pad is neutral");

    // Nothing is taken outside the controls: the middle of the screen stays free.
    check(!pad.finger_down(1, 1200.0f, 400.0f), "a touch in the middle is not the controller's");

    const Circle a = layout[Control::A];
    check(pad.finger_down(2, a.x, a.y), "touching A is captured");
    check((pad.state().buttons & PAD_BUTTON_A) != 0 && pad.pad_status().analogA == 0xFF, "A is pressed");
    const Circle b = layout[Control::B];
    pad.finger_move(2, b.x, b.y);
    check((pad.state().buttons & PAD_BUTTON_B) != 0 && (pad.state().buttons & PAD_BUTTON_A) == 0, "sliding from A to B presses B");
    pad.finger_up(2);
    check(pad.state().buttons == 0, "lifting releases");

    // Multi-touch: stick and a button at the same time.
    const Circle rest = layout[Control::Stick];
    check(pad.finger_down(3, rest.x + 50.0f, rest.y + 40.0f), "the stick zone is captured");
    pad.finger_move(3, rest.x + 50.0f + rest.radius * 2.0f, rest.y + 40.0f);
    check(pad.state().stickX == 127 && pad.state().stickY == 0, "full right, clamped to the stick's reach");
    pad.finger_move(3, rest.x + 50.0f, rest.y + 40.0f - rest.radius * 0.5f);
    check(pad.state().stickX == 0 && pad.state().stickY > 60 && pad.state().stickY < 67, "half up is about half, up is positive");
    check(pad.finger_down(4, a.x, a.y), "A while holding the stick");
    check((pad.pad_status().button & PAD_BUTTON_A) != 0 && pad.pad_status().stickY > 0, "stick and A together");
    check(pad.finger_down(5, rest.x, rest.y), "a second thumb in the stick zone is swallowed");
    pad.finger_up(5);
    check(pad.state().stickY > 0, "and does not disturb the first");
    pad.finger_up(3);
    check(pad.state().stickX == 0 && pad.state().stickY == 0, "releasing the stick recentres it");
    pad.finger_up(4);

    // Triggers carry an analog value with the digital press.
    const Circle r = layout[Control::R];
    pad.finger_down(6, r.x, r.y);
    check((pad.state().buttons & PAD_TRIGGER_R) != 0 && pad.pad_status().triggerRight > 0, "R sends digital and analog");
    pad.finger_up(6);

    // The menu button asks once, on release, and not at all when dragged away.
    const Circle menu = layout[Control::Menu];
    pad.finger_down(7, menu.x, menu.y);
    check(!pad.take_menu_request(), "menu waits for the release");
    pad.finger_up(7);
    check(pad.take_menu_request() && !pad.take_menu_request(), "menu asked once");
    pad.finger_down(8, menu.x, menu.y);
    pad.finger_move(8, menu.x, menu.y + menu.radius * 4.0f);
    pad.finger_up(8);
    check(!pad.take_menu_request(), "dragging off the menu button cancels it");
    check((pad.pad_status().button & PAD_BUTTON_START) == 0, "menu is not Start");

    pad.finger_down(9, a.x, a.y);
    pad.release_all();
    check(pad.state().buttons == 0, "release_all clears everything");

    if (failures == 0) {
        std::puts("touch controls tests passed");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
