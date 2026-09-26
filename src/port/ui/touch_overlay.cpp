#include "touch_overlay.hpp"

#include "aurora/rmlui.hpp"
#include "input.hpp"
#include "port/main.h"
#include "port/settings.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_touch.h>
#include <SDL3/SDL_video.h>
#include <aurora/lib/webgpu/gpu.hpp>
#include <aurora/lib/window.hpp>
#include <dolphin/pad.h>

#include <algorithm>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace partyboard::ui {
namespace {

    const Rml::String kDocumentSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/touch.rcss" />
</head>
<body>
    <div id="touch-root">
        <div id="stick" class="stick"><div id="knob" class="knob" /></div>
        <div id="a" class="button a">A</div>
        <div id="b" class="button b">B</div>
        <div id="x" class="button">X</div>
        <div id="y" class="button">Y</div>
        <div id="z" class="button shoulder z">Z</div>
        <div id="l" class="button shoulder">L</div>
        <div id="r" class="button shoulder">R</div>
        <div id="start" class="button small">START</div>
        <div id="menu" class="button small menu">&#xe5d2;</div>
    </div>
</body>
</rml>
)RML";

    // Same order as touch::Control.
    constexpr std::array<const char *, touch::kControlCount> kElementIds = {
        "stick", "a", "b", "x", "y", "z", "l", "r", "start", "menu",
    };

    constexpr std::uint32_t kPort = PAD_CHAN0;

    // Only real screens: a laptop trackpad also sends finger events.
    bool direct_touch(const SDL_TouchFingerEvent &event)
    {
        return SDL_GetTouchDeviceType(event.touchID) == SDL_TOUCH_DEVICE_DIRECT;
    }

    constexpr bool mobile_platform()
    {
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_MACCATALYST)
        return true;
#else
        return false;
#endif
    }

    TouchOverlay *sInstance = nullptr;

} // namespace

TouchOverlay::TouchOverlay()
    : Document(kDocumentSource)
{
    sInstance = this;
    if (mDocument == nullptr) {
        return;
    }
    mRoot = mDocument->GetElementById("touch-root");
    mKnob = mDocument->GetElementById("knob");
    for (std::size_t i = 0; i < kElementIds.size(); ++i) {
        mElements[i] = mDocument->GetElementById(kElementIds[i]);
    }
}

TouchOverlay::~TouchOverlay()
{
    if (sInstance == this) {
        sInstance = nullptr;
    }
    PADClearVirtualStatus(kPort);
}

void TouchOverlay::show()
{
    if (mDocument != nullptr) {
        mDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::None, Rml::ScrollFlag::None);
    }
}

bool TouchOverlay::stands_in_for_gamepad() noexcept
{
    const int mode = getSettings().game.touchControls.getValue();
    return mode == 1 || (mode == 0 && mobile_platform());
}

bool TouchOverlay::should_show() const
{
    if (mDocument == nullptr || !PartyBoard_IsGameLaunched || any_document_visible() || is_prelaunch_open()) {
        return false;
    }
    switch (getSettings().game.touchControls.getValue()) {
        case 1:
            return true;
        case 2:
            return false;
        default:
            // A gamepad on port 1 takes over; unplug it and the screen controls come back.
            return mobile_platform() && PADGetIndexForPort(kPort) < 0;
    }
}

void TouchOverlay::set_shown(bool shown)
{
    if (shown == mShown) {
        return;
    }
    mShown = shown;
    if (mRoot != nullptr) {
        mRoot->SetClass("shown", shown);
    }
    if (!shown) {
        // Nothing stays held behind a menu or after a gamepad connects.
        mController.release_all();
        PADClearVirtualStatus(kPort);
    }
}

void TouchOverlay::place(Rml::Element *element, const touch::Circle &circle) const
{
    if (element == nullptr) {
        return;
    }
    const float size = circle.radius * 2.0f;
    element->SetProperty(Rml::PropertyId::Left, Rml::Property((circle.x - circle.radius) * mScaleX, Rml::Unit::PX));
    element->SetProperty(Rml::PropertyId::Top, Rml::Property((circle.y - circle.radius) * mScaleY, Rml::Unit::PX));
    element->SetProperty(Rml::PropertyId::Width, Rml::Property(size * mScaleX, Rml::Unit::PX));
    element->SetProperty(Rml::PropertyId::Height, Rml::Property(size * mScaleY, Rml::Unit::PX));
    // RmlUi takes no percentage radius ("50%" is a syntax error there): round it in pixels.
    element->SetProperty("border-radius", Rml::ToString(circle.radius * mScaleX) + "px");
    element->SetProperty(Rml::PropertyId::LineHeight, Rml::Property(size * mScaleY, Rml::Unit::PX));
}

void TouchOverlay::relayout()
{
    const AuroraWindowSize windowSize = aurora::window::get_window_size();
    auto *context = aurora::rmlui::get_context();
    if (windowSize.width == 0 || windowSize.height == 0 || context == nullptr) {
        return;
    }
    const float width = static_cast<float>(windowSize.width);
    const float height = static_cast<float>(windowSize.height);
    const Rml::Vector2i contextSize = context->GetDimensions();
    if (contextSize.x <= 0 || contextSize.y <= 0 || windowSize.native_fb_width == 0 || windowSize.native_fb_height == 0) {
        return;
    }
    // The UI is drawn only over the game picture, which is pillarboxed on a phone (4:3 in a 20:9
    // screen). The controls used to be laid out over the whole window and then squeezed into the
    // picture's width: drawn in one place, pressed in another. Lay them out over the picture.
    const auto viewport = aurora::webgpu::calculate_present_viewport(windowSize.native_fb_width,
        windowSize.native_fb_height, static_cast<uint32_t>(contextSize.x), static_cast<uint32_t>(contextSize.y));
    const float toWindowX = width / static_cast<float>(windowSize.native_fb_width);
    const float toWindowY = height / static_cast<float>(windowSize.native_fb_height);
    const float originX = viewport.left * toWindowX;
    const float originY = viewport.top * toWindowY;
    const float areaWidth = std::max(1.0f, viewport.width * toWindowX);
    const float areaHeight = std::max(1.0f, viewport.height * toWindowY);
    const float scaleX = static_cast<float>(contextSize.x) / areaWidth;
    const float scaleY = static_cast<float>(contextSize.y) / areaHeight;
    if (width == mLayoutWidth && height == mLayoutHeight && scaleX == mScaleX && scaleY == mScaleY
        && originX == mOriginX && originY == mOriginY)
    {
        return;
    }
    mLayoutWidth = width;
    mLayoutHeight = height;
    mOriginX = originX;
    mOriginY = originY;
    mScaleX = scaleX;
    mScaleY = scaleY;

    // Keep clear of notches and rounded corners, where they reach into the picture.
    touch::Insets safe;
    SDL_Rect safeRect {};
    if (auto *window = aurora::window::get_sdl_window(); window != nullptr && SDL_GetWindowSafeArea(window, &safeRect)) {
        safe.left = std::max(0.0f, static_cast<float>(safeRect.x) - originX);
        safe.top = std::max(0.0f, static_cast<float>(safeRect.y) - originY);
        safe.right = std::max(0.0f, originX + areaWidth - static_cast<float>(safeRect.x + safeRect.w));
        safe.bottom = std::max(0.0f, originY + areaHeight - static_cast<float>(safeRect.y + safeRect.h));
    }
    mController.set_layout(touch::make_layout(areaWidth, areaHeight, safe));
    for (std::size_t i = 0; i < touch::kControlCount; ++i) {
        place(mElements[i], mController.layout().controls[i]);
    }
}

void TouchOverlay::update()
{
    Document::update();
    set_shown(should_show());
    if (!mShown) {
        return;
    }
    relayout();

    const int opacity = std::clamp(getSettings().game.touchControlsOpacity.getValue(), 10, 100);
    if (opacity != mOpacity && mRoot != nullptr) {
        mOpacity = opacity;
        mRoot->SetProperty(Rml::PropertyId::Opacity, Rml::Property(static_cast<float>(opacity) / 100.0f, Rml::Unit::NUMBER));
    }

    if (mController.take_menu_request()) {
        input::open_menu();
        return;
    }

    const PADStatus status = mController.pad_status();
    PADSetVirtualStatus(kPort, &status);

    const auto &state = mController.state();
    for (std::size_t i = 0; i < touch::kControlCount; ++i) {
        if (mElements[i] != nullptr) {
            mElements[i]->SetClass("pressed", state.pressed[i]);
        }
    }
    // The stick base follows the thumb that grabbed it; the knob shows how far it is pushed.
    const touch::Circle &rest = mController.layout()[touch::Control::Stick];
    place(mElements[static_cast<std::size_t>(touch::Control::Stick)], touch::Circle { state.stickCenterX, state.stickCenterY, rest.radius });
    if (mKnob != nullptr) {
        const float knobRadius = rest.radius * 0.45f;
        const float left = rest.radius + (state.knobX - state.stickCenterX) - knobRadius;
        const float top = rest.radius + (state.knobY - state.stickCenterY) - knobRadius;
        mKnob->SetProperty(Rml::PropertyId::Left, Rml::Property(left * mScaleX, Rml::Unit::PX));
        mKnob->SetProperty(Rml::PropertyId::Top, Rml::Property(top * mScaleY, Rml::Unit::PX));
        mKnob->SetProperty(Rml::PropertyId::Width, Rml::Property(knobRadius * 2.0f * mScaleX, Rml::Unit::PX));
        mKnob->SetProperty(Rml::PropertyId::Height, Rml::Property(knobRadius * 2.0f * mScaleY, Rml::Unit::PX));
        mKnob->SetProperty("border-radius", Rml::ToString(knobRadius * mScaleX) + "px");
    }
}

bool TouchOverlay::handle_event(const SDL_Event &event) noexcept
{
    auto *self = sInstance;
    // Android may not cancel the fingers of an app sent to the background: nothing stays held.
    if (self != nullptr && (event.type == SDL_EVENT_WILL_ENTER_BACKGROUND || event.type == SDL_EVENT_DID_ENTER_FOREGROUND)) {
        self->mController.release_all();
        return false;
    }
    if (self == nullptr || !self->mShown) {
        return false;
    }
    switch (event.type) {
        case SDL_EVENT_FINGER_DOWN:
            if (!direct_touch(event.tfinger)) {
                return false;
            }
            return self->mController.finger_down(event.tfinger.fingerID, event.tfinger.x * self->mLayoutWidth - self->mOriginX,
                event.tfinger.y * self->mLayoutHeight - self->mOriginY);
        case SDL_EVENT_FINGER_MOTION:
            if (!self->mController.owns(event.tfinger.fingerID)) {
                return false;
            }
            self->mController.finger_move(event.tfinger.fingerID, event.tfinger.x * self->mLayoutWidth - self->mOriginX,
                event.tfinger.y * self->mLayoutHeight - self->mOriginY);
            return true;
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
            if (!self->mController.owns(event.tfinger.fingerID)) {
                return false;
            }
            self->mController.finger_up(event.tfinger.fingerID);
            return true;
        default:
            return false;
    }
}

} // namespace partyboard::ui
