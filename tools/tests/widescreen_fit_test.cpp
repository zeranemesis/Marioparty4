// The adaptive widescreen's arithmetic on every kind of screen.
// Standalone:
//   c++ -std=c++20 -Iinclude tools/tests/widescreen_fit_test.cpp
#include "port/widescreen_fit.hpp"

#include <cmath>
#include <cstdio>

using namespace partyboard::widescreen;

namespace {

int failures = 0;

void check(bool condition, const char *what)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

bool near(float a, float b)
{
    return std::fabs(a - b) < 1e-2f;
}

constexpr float kCenter = 288.0f; // HU_DISP_WIDTH / 2
constexpr float kWidth = kCenter * 2;

// Where a HUD x lands across the window, 0..1.
float on_window(float hudX, float scale, HudSpan span)
{
    return (placed_x(hudX, kCenter, scale, span) - (kCenter - kCenter * scale)) / (kWidth * scale);
}

} // namespace

int main()
{
    check(near(aspect_scale(640, 480), 1.0f), "4:3 is the original");
    check(near(aspect_scale(1920, 1080), 4.0f / 3.0f), "16:9");
    check(near(aspect_scale(1280, 800), 1.2f), "16:10");
    check(near(aspect_scale(3440, 1440), 3440.0f / 1440.0f * 0.75f), "21:9");
    check(near(aspect_scale(5120, 1440), 2.6666667f), "32:9");
    check(near(aspect_scale(2400, 1080), 2400.0f / 1080.0f * 0.75f), "20:9 phone");
    check(near(aspect_scale(5760, 1080), 4.0f), "48:9 triple screen");
    check(near(aspect_scale(7680, 1080), kMaxScale), "wider than 48:9 is capped");
    check(near(aspect_scale(1280, 1024), 1.0f), "5:4 is letterboxed, not cut");
    check(near(aspect_scale(1080, 2400), 1.0f), "portrait is letterboxed, not cut");
    check(near(aspect_scale(0, 0), 1.0f) && near(aspect_scale(1920, 0), 1.0f), "minimised window");

    for (float scale : {aspect_scale(1920, 1080), aspect_scale(3440, 1440), aspect_scale(5120, 1440)}) {
        const HudSpan whole = hud_span(scale, 1920, 0, 1920);
        check(near(whole.mid, 0) && near(whole.span, 1), "no safe area inset");
        // The middle half stays over the 3D scene: on the window it keeps its
        // distance from the centre, as the widened scene does.
        for (float x : {kCenter * 0.5f, kCenter, kCenter * 1.5f}) {
            check(near(placed_x(x, kCenter, scale, whole), x), "the middle half does not move");
            check(near(picture_x(x, kCenter, scale, whole), kCenter + (x - kCenter) / scale), "middle over the scene");
        }
        check(near(on_window(0, scale, whole), 0), "left edge on the window's");
        check(near(on_window(kWidth, scale, whole), 1), "right edge on the window's");
        // A corner panel keeps the margin it had on a 4:3 screen.
        check(near(placed_x(20, kCenter, scale, whole) - (kCenter - kCenter * scale), 20), "left margin kept");
        check(near(kCenter + kCenter * scale - placed_x(kWidth - 20, kCenter, scale, whole), 20), "right margin kept");
        float previous = -1e9f;
        bool grows = true;
        for (float x = -200; x <= kWidth + 200; x += 3) {
            const float placed = placed_x(x, kCenter, scale, whole);
            grows = grows && placed > previous;
            previous = placed;
        }
        check(grows, "nothing crosses over on its way out");
        // A sprite placed over a 3D point lands on it, wherever it is.
        for (float picture : {0.0f, 40.0f, 100.0f, kCenter, 400.0f, 520.0f, kWidth}) {
            const float hud = hud_x(picture, kCenter, scale, whole);
            check(near(picture_x(hud, kCenter, scale, whole), picture), "sprite over a 3D point");
        }
    }

    // A 20:9 phone in landscape with the notch on the left: 80 of 2400 points.
    const float phone = aspect_scale(2400, 1080);
    const HudSpan notch = hud_span(phone, 2400, 80, 2320);
    check(notch.span < 1 && notch.mid > 0, "the HUD moves away from the notch");
    check(near(on_window(0, phone, notch), 80.0f / 2400.0f), "HUD left edge at the end of the notch");
    check(near(on_window(kWidth, phone, notch), 1.0f), "HUD right edge on the window's");
    for (float picture : {0.0f, 50.0f, kCenter, 400.0f, kWidth}) {
        check(near(picture_x(hud_x(picture, kCenter, phone, notch), kCenter, phone, notch), picture),
            "sprite over a 3D point beside a notch");
    }

    // A safe area narrower than 4:3 cannot hold the HUD: the whole window is used.
    const HudSpan tight = hud_span(1.1f, 1000, 100, 800);
    check(near(tight.mid, 0) && near(tight.span, 1), "too narrow a safe area is ignored");
    check(near(hud_span(2, 0, 0, 0).span, 1), "no window size");

    if (failures == 0) std::printf("widescreen_fit_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
