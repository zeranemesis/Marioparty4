#include "port/touch_controls.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace partyboard::touch {
namespace {

    // Full deflection, as Aurora's keyboard bindings send it.
    constexpr float kStickMax = 127.0f;
    // Analog trigger value sent with a digital L/R press, as Aurora does for pads.
    constexpr std::uint8_t kTriggerPressed = 180;
    // A held face button may be slid off onto its neighbour; anything further releases it.
    constexpr float kButtonReleaseSlack = 1.6f;

    constexpr std::uint16_t button_bit(Control control)
    {
        switch (control) {
            case Control::A:
                return PAD_BUTTON_A;
            case Control::B:
                return PAD_BUTTON_B;
            case Control::X:
                return PAD_BUTTON_X;
            case Control::Y:
                return PAD_BUTTON_Y;
            case Control::Z:
                return PAD_TRIGGER_Z;
            case Control::L:
                return PAD_TRIGGER_L;
            case Control::R:
                return PAD_TRIGGER_R;
            case Control::Start:
                return PAD_BUTTON_START;
            default:
                return 0;
        }
    }

    constexpr bool is_face_button(Control control)
    {
        return control == Control::A || control == Control::B || control == Control::X || control == Control::Y;
    }

    float distance(float ax, float ay, float bx, float by)
    {
        return std::hypot(ax - bx, ay - by);
    }

} // namespace

Layout make_layout(float width, float height, Insets safe)
{
    Layout layout;
    layout.width = width;
    layout.height = height;

    const float left = safe.left;
    const float right = width - safe.right;
    const float top = safe.top;
    const float bottom = height - safe.bottom;
    // Sized from the short side: a phone in landscape and a tablet get the same thumb reach.
    const float u = std::max(1.0f, std::min(bottom - top, right - left));

    auto set = [&](Control control, float x, float y, float radius) {
        layout.controls[static_cast<std::size_t>(control)] = Circle { x, y, radius * u };
    };

    // GameCube face: a big A, B below-left, X to the right, Y above.
    const float ax = right - 0.24f * u;
    const float ay = bottom - 0.26f * u;
    set(Control::A, ax, ay, 0.095f);
    set(Control::B, ax - 0.18f * u, ay + 0.08f * u, 0.065f);
    set(Control::X, ax + 0.16f * u, ay - 0.05f * u, 0.06f);
    set(Control::Y, ax - 0.05f * u, ay - 0.18f * u, 0.06f);

    set(Control::L, left + 0.13f * u, top + 0.12f * u, 0.07f);
    set(Control::R, right - 0.13f * u, top + 0.12f * u, 0.07f);
    set(Control::Z, right - 0.13f * u, top + 0.30f * u, 0.055f);
    set(Control::Start, (left + right) * 0.5f, bottom - 0.09f * u, 0.055f);
    set(Control::Menu, (left + right) * 0.5f, top + 0.08f * u, 0.045f);

    set(Control::Stick, left + 0.25f * u, bottom - 0.27f * u, 0.14f);
    layout.stickZoneRight = left + (right - left) * 0.45f;
    layout.stickZoneTop = top + 0.25f * u;
    return layout;
}

void Controller::set_layout(const Layout &layout)
{
    mLayout = layout;
    release_all();
}

Controller::Finger *Controller::find(std::uint64_t id)
{
    for (auto &finger : mFingers) {
        if (finger.active && finger.id == id) {
            return &finger;
        }
    }
    return nullptr;
}

std::optional<Control> Controller::hit(float x, float y) const
{
    // Buttons first, nearest centre wins where two circles touch.
    std::optional<Control> best;
    float bestDistance = 0.0f;
    for (std::size_t i = 0; i < kControlCount; ++i) {
        const auto control = static_cast<Control>(i);
        if (control == Control::Stick) {
            continue;
        }
        const Circle &c = mLayout.controls[i];
        const float d = distance(x, y, c.x, c.y);
        if (d <= c.radius && (!best || d < bestDistance)) {
            best = control;
            bestDistance = d;
        }
    }
    if (best) {
        return best;
    }
    if (x <= mLayout.stickZoneRight && y >= mLayout.stickZoneTop) {
        return Control::Stick;
    }
    return std::nullopt;
}

bool Controller::finger_down(std::uint64_t id, float x, float y)
{
    if (find(id) != nullptr) {
        return true;
    }
    const auto control = hit(x, y);
    if (!control) {
        return false;
    }
    // One thumb per stick: a second finger in the zone is simply ignored.
    if (*control == Control::Stick) {
        for (const auto &finger : mFingers) {
            if (finger.active && finger.control == Control::Stick) {
                return true;
            }
        }
    }
    Finger *slot = nullptr;
    for (auto &finger : mFingers) {
        if (!finger.active) {
            slot = &finger;
            break;
        }
    }
    if (slot == nullptr) {
        return true;
    }
    *slot = Finger { id, *control, true };
    if (*control == Control::Stick) {
        // The stick is floating: its centre jumps to wherever the thumb lands.
        mState.stickCenterX = x;
        mState.stickCenterY = y;
        update_stick(x, y);
    }
    refresh();
    return true;
}

void Controller::finger_move(std::uint64_t id, float x, float y)
{
    Finger *finger = find(id);
    if (finger == nullptr) {
        return;
    }
    if (finger->control == Control::Stick) {
        update_stick(x, y);
        refresh();
        return;
    }
    const Circle &held = mLayout[finger->control];
    if (distance(x, y, held.x, held.y) <= held.radius * kButtonReleaseSlack) {
        return;
    }
    // Sliding a thumb from one face button to the next presses the next one, as on a real pad.
    const auto next = hit(x, y);
    if (is_face_button(finger->control) && next && is_face_button(*next)) {
        finger->control = *next;
    }
    else {
        // Dragged away: released without effect, which is also how a menu tap is cancelled.
        finger->active = false;
    }
    refresh();
}

void Controller::finger_up(std::uint64_t id)
{
    Finger *finger = find(id);
    if (finger == nullptr) {
        return;
    }
    if (finger->control == Control::Menu) {
        mMenuRequested = true;
    }
    finger->active = false;
    refresh();
}

void Controller::release_all()
{
    mFingers = {};
    refresh();
}

bool Controller::owns(std::uint64_t id) const
{
    for (const auto &finger : mFingers) {
        if (finger.active && finger.id == id) {
            return true;
        }
    }
    return false;
}

void Controller::update_stick(float x, float y)
{
    const Circle &rest = mLayout[Control::Stick];
    const float radius = std::max(rest.radius, 1.0f);
    float dx = x - mState.stickCenterX;
    float dy = y - mState.stickCenterY;
    const float length = std::hypot(dx, dy);
    if (length > radius) {
        dx *= radius / length;
        dy *= radius / length;
    }
    mState.knobX = mState.stickCenterX + dx;
    mState.knobY = mState.stickCenterY + dy;
    mState.stickX = static_cast<std::int8_t>(std::lround(dx / radius * kStickMax));
    // Screen y grows downwards; a GameCube stick pushed up is positive.
    mState.stickY = static_cast<std::int8_t>(std::lround(-dy / radius * kStickMax));
}

void Controller::refresh()
{
    mState.buttons = 0;
    mState.pressed = {};
    bool stickHeld = false;
    for (const auto &finger : mFingers) {
        if (!finger.active) {
            continue;
        }
        mState.pressed[static_cast<std::size_t>(finger.control)] = true;
        mState.buttons |= button_bit(finger.control);
        stickHeld |= finger.control == Control::Stick;
    }
    if (!stickHeld) {
        const Circle &rest = mLayout[Control::Stick];
        mState.stickCenterX = mState.knobX = rest.x;
        mState.stickCenterY = mState.knobY = rest.y;
        mState.stickX = mState.stickY = 0;
    }
    mState.triggerLeft = (mState.buttons & PAD_TRIGGER_L) != 0 ? kTriggerPressed : 0;
    mState.triggerRight = (mState.buttons & PAD_TRIGGER_R) != 0 ? kTriggerPressed : 0;
}

PADStatus Controller::pad_status() const
{
    PADStatus status;
    std::memset(&status, 0, sizeof(status));
    status.button = mState.buttons;
    status.stickX = mState.stickX;
    status.stickY = mState.stickY;
    status.triggerLeft = mState.triggerLeft;
    status.triggerRight = mState.triggerRight;
    status.analogA = (mState.buttons & PAD_BUTTON_A) != 0 ? 0xFF : 0;
    status.analogB = (mState.buttons & PAD_BUTTON_B) != 0 ? 0xFF : 0;
    status.err = PAD_ERR_NONE;
    return status;
}

bool Controller::take_menu_request()
{
    const bool requested = mMenuRequested;
    mMenuRequested = false;
    return requested;
}

} // namespace partyboard::touch
