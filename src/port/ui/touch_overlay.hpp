#pragma once

#include "document.hpp"

#include "port/touch_controls.hpp"

#include <array>

// Draws the on-screen controller (port/touch_controls.hpp) and hands it the SDL finger events.
// It shows only in game, with no menu open, and -- in its automatic setting -- only on phones and
// tablets that have no gamepad on port 1. Its presses reach the game through Aurora's virtual pad.
namespace partyboard::ui {

class TouchOverlay : public Document {
public:
    TouchOverlay();
    ~TouchOverlay() override;

    void show() override;
    void update() override;

    // Returns whether the controller took the event, which then goes nowhere else.
    static bool handle_event(const SDL_Event &event) noexcept;
    // Whether the screen controller stands in for a missing gamepad on port 1 on this device.
    static bool stands_in_for_gamepad() noexcept;

private:
    bool should_show() const;
    void set_shown(bool shown);
    void relayout();
    void place(Rml::Element *element, const touch::Circle &circle) const;

    touch::Controller mController;
    std::array<Rml::Element *, touch::kControlCount> mElements {};
    Rml::Element *mRoot = nullptr;
    Rml::Element *mKnob = nullptr;
    float mScaleX = 1.0f;
    float mScaleY = 1.0f;
    float mLayoutWidth = 0.0f;
    float mLayoutHeight = 0.0f;
    // Where the game picture (and so the UI) starts in the window.
    float mOriginX = 0.0f;
    float mOriginY = 0.0f;
    int mOpacity = -1;
    bool mShown = false;
};

} // namespace partyboard::ui
