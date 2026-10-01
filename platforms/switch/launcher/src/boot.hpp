#pragma once

// The start-up sequence played between "Play" and the engine, in the spirit
// of the GameCube boot: a small cube drops in, rolls a G-shaped path leaving
// blocks behind, then jumps into the centre as the chime rings and the camera
// swings to an isometric view of the finished emblem.
//
// The timeline is a pure function of time so it can be unit-tested and
// rendered frame-exactly by the host preview.

#include <vector>

#include "math.hpp"
#include "render.hpp"
#include "sound.hpp"

namespace partyboard::launcher {

class Platform;

struct BootCube {
    Mat4 model;
    Color color;
    float emissive = 0.0f;
};

struct BootFrame {
    std::vector<BootCube> cubes;
    Vec3 eye;
    Mat4 viewProjection;
    float fadeIn = 0.0f;   // 0 = black, 1 = scene visible
    float flash = 0.0f;    // white flash after the slam
    float logoAlpha = 0.0f;
    float blackout = 0.0f; // final fade to black
};

struct BootCue {
    double time;
    Sound sound;
    float gain;
};

class BootAnimation {
public:
    static constexpr double kSlamTime = 2.52;
    static constexpr double kSkipTo = 4.85;
    static constexpr double kDuration = 5.35;

    void start(double now);
    // Holding ZR before the slam switches to the playful variant.
    void update(double now, bool alternateHeld, Platform& platform);
    void draw(Renderer& renderer, double now) const;
    void skip(double now);

    bool finished(double now) const { return now - m_start >= kDuration; }
    double elapsed(double now) const { return now - m_start; }
    bool alternate() const { return m_alternate; }

    static BootFrame evaluate(double t, bool alternate, float aspect);
    static std::vector<BootCue> cues(bool alternate);

private:
    double m_start = 0.0;
    double m_lastT = -1.0;
    bool m_alternate = false;
};

} // namespace partyboard::launcher
