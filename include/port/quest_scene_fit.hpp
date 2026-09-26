#pragma once

#include <algorithm>
#include <cmath>

namespace partyboard::quest {
// Camera framing is a fallback for uncalibrated minigames, not a measurement
// of their geometry. Freeze it at scene entry so camera animation cannot move
// or resize the physical diorama. Boards use their playable-space bounds.
// Place the lowest playable surface on the table and center the board in X/Z.
// GX clip volume is -w..w in X/Y and -w..0 in Z. Keep a mesh if either
// eye intersects its bounding sphere; invalid data must not hide geometry.
inline bool sphere_visible(const float clip[2][16], const float center[3], float radius) {
    if (!std::isfinite(radius) || radius < 0) return true;
    for (int axis = 0; axis < 3; ++axis) if (!std::isfinite(center[axis])) return true;
    for (int eye = 0; eye < 2; ++eye) {
        bool inside = true;
        for (int plane = 0; plane < 6; ++plane) {
            float p[4];
            const int row = plane / 2;
            const float sign = plane % 2 == 0 ? 1.0f : -1.0f;
            for (int c = 0; c < 4; ++c) {
                p[c] = (plane == 5 ? 0.0f : clip[eye][12+c]) + sign * clip[eye][row*4+c];
                if (!std::isfinite(p[c])) return true;
            }
            const float distance = p[0]*center[0] + p[1]*center[1] + p[2]*center[2] + p[3];
            const float extent = radius * std::sqrt(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
            if (distance < -extent) { inside = false; break; }
        }
        if (inside) return true;
    }
    return false;
}

inline bool board_fit(const float minimum[3], const float maximum[3], float& scale, float center[3]) {
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) || maximum[axis] < minimum[axis]) return false;
    }
    const float extent = std::max(maximum[0] - minimum[0], maximum[2] - minimum[2]);
    if (!std::isfinite(extent) || extent < 100.0f) return false;
    scale = std::clamp(2800.0f / extent, 0.05f, 16.0f);
    center[0] = minimum[0] + (maximum[0] - minimum[0]) * 0.5f;
    center[1] = minimum[1];
    center[2] = minimum[2] + (maximum[2] - minimum[2]) * 0.5f;
    return true;
}

inline bool scene_scale(float distance, float fovDegrees, float aspect, float& scale) {
    if (!std::isfinite(distance) || !std::isfinite(fovDegrees) || !std::isfinite(aspect)
        || distance < 1.0f || fovDegrees <= 1.0f || fovDegrees >= 175.0f || aspect <= 0.0f) {
        return false;
    }
    const float width = 2.0f * distance * std::tan(fovDegrees * 0.00872664626f) * aspect;
    if (!std::isfinite(width) || width <= 0.0f) {
        return false;
    }
    // At the default 0.00025 m/unit, this yields a roughly 70 cm arena.
    scale = std::clamp(2800.0f / width, 0.25f, 16.0f);
    return true;
}
}
