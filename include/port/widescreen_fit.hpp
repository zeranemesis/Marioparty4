#pragma once

#include <algorithm>
#include <cmath>

namespace partyboard::widescreen {
// The adaptive widescreen widens the original 4:3 picture to the window and
// moves the HUD near its sides out to the new edges. Pure arithmetic, tested
// by widescreen_fit_test.

constexpr float kOriginalAspect = 4.0f / 3.0f;
// 48:9, three 16:9 screens side by side. A wider window is stretched past it.
constexpr float kMaxScale = 4.0f;

// How many times wider than 4:3 the window is. A narrower window gets 1 and
// is letterboxed: widening by less than 1 would cut off the sides and the HUD.
inline float aspect_scale(float width, float height) {
    if (!(width > 0.0f) || !(height > 0.0f)) return 1.0f;
    const float scale = width / height / kOriginalAspect;
    if (!std::isfinite(scale)) return 1.0f;
    return std::clamp(scale, 1.0f, kMaxScale);
}

// Where the original HUD's edges go across the window, both measured from
// their middle (-0.5..0.5): window = mid + hud * span. The whole window unless
// a notch or rounded corners take a side; the safe area then keeps it clear.
struct HudSpan {
    float mid = 0.0f;
    float span = 1.0f;
};

inline HudSpan hud_span(float scale, float windowWidth, float safeX, float safeWidth) {
    if (!(windowWidth > 0.0f) || !(safeWidth > 0.0f) || !std::isfinite(scale)) return {};
    const float lo = std::clamp(safeX / windowWidth, 0.0f, 1.0f);
    const float hi = std::clamp((safeX + safeWidth) / windowWidth, lo, 1.0f);
    // HUD elements keep their size, so they need at least the 4:3 width.
    if (scale * (hi - lo) < 1.0f) return {};
    return {(lo + hi - 1.0f) * 0.5f, hi - lo};
}

// Where HUD x (0..2 * centerX) is drawn in the widened HUD projection, which
// runs from centerX - centerX * scale to centerX + centerX * scale. The middle
// half stays where it was, over the 3D scene it belongs to (the scene keeps
// its size and gains room on the sides); the outer quarter on each side moves
// out with the window's edge, keeping its margin, and the HUD between follows
// part way.
inline float placed_x(float hudX, float centerX, float scale, HudSpan span) {
    const float width = 2.0f * centerX * scale;
    const float left = centerX - centerX * scale + width * (0.5f + span.mid - span.span * 0.5f);
    const float right = centerX - centerX * scale + width * (0.5f + span.mid + span.span * 0.5f);
    const float edge = hudX < centerX ? hudX + left : hudX + right - 2.0f * centerX;
    const float weight = std::clamp((std::fabs(hudX - centerX) / centerX - 0.5f) * 4.0f, 0.0f, 1.0f);
    return hudX + (edge - hudX) * weight;
}

// The HUD x of picture x (0..2 * centerX across the widened 3D scene), and
// back: a sprite placed there is drawn over that point.
inline float picture_x(float hudX, float centerX, float scale, HudSpan span) {
    return centerX + (placed_x(hudX, centerX, scale, span) - centerX) / scale;
}

inline float hud_x(float pictureX, float centerX, float scale, HudSpan span) {
    // placed_x only ever grows with hudX: bisect.
    const float target = centerX + (pictureX - centerX) * scale;
    float lo = centerX - centerX * 8.0f - std::fabs(target);
    float hi = centerX + centerX * 8.0f + std::fabs(target);
    for (int i = 0; i < 40; ++i) {
        const float midX = (lo + hi) * 0.5f;
        (placed_x(midX, centerX, scale, span) < target ? lo : hi) = midX;
    }
    return (lo + hi) * 0.5f;
}

} // namespace partyboard::widescreen
