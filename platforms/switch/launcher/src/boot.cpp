#include "boot.hpp"

#include <algorithm>
#include <cmath>

#include "platform.hpp"

namespace partyboard::launcher {

namespace {

struct Cell {
    int col;
    int row;
};

// A G on a 3x3 floor, traced as one stroke and finished by the core cube in
// the centre: the top-right cell stays open, like the mouth of the letter.
constexpr Cell kPath[] = {{1, 0}, {0, 0}, {0, 1}, {0, 2}, {1, 2}, {2, 2}, {2, 1}, {1, 1}};
constexpr int kRolls = static_cast<int>(sizeof(kPath) / sizeof(kPath[0])) - 1;

constexpr double kFadeLength = 0.35;
constexpr double kDropStart = 0.35;
constexpr double kRollStart = 0.95;
constexpr double kRollStep = 0.17;
constexpr double kHopStart = kRollStart + kRolls * kRollStep; // 2.14
constexpr double kSettleLength = 1.1;
constexpr double kLogoStart = 3.0;

constexpr Color kTrailColor = rgb(0x5B4EE0);
constexpr Color kLeaderColor = rgb(0x8B7DFF);
constexpr Color kPartyColors[4] = {rgb(0xFF4D6D), rgb(0x3B82FF), rgb(0xFFD23B), rgb(0x3BD67A)};

Vec3 cellCenter(Cell c) { return {static_cast<float>(c.col - 1), 0.5f, static_cast<float>(c.row - 1)}; }

float hop(float v, float height) { return height * 4.0f * v * (1.0f - v); }

// Height of the leader above the floor while it drops in and settles.
float dropHeight(double t) {
    const double u = t - kDropStart;
    if (u < 0.0)
        return 8.0f;
    if (u < 0.32) {
        const float v = static_cast<float>(u / 0.32);
        return 6.0f * (1.0f - v * v);
    }
    if (u < 0.50)
        return hop(static_cast<float>((u - 0.32) / 0.18), 0.9f);
    if (u < 0.60)
        return hop(static_cast<float>((u - 0.50) / 0.10), 0.25f);
    return 0.0f;
}

Color hueColor(float t) {
    const float r = 0.5f + 0.5f * std::cos(6.2831853f * (t + 0.0f));
    const float g = 0.5f + 0.5f * std::cos(6.2831853f * (t + 0.33f));
    const float b = 0.5f + 0.5f * std::cos(6.2831853f * (t + 0.67f));
    return {0.35f + 0.65f * r, 0.35f + 0.65f * g, 0.35f + 0.65f * b, 1.0f};
}

Mat4 leaderModel(double t, bool alternate) {
    if (t < kRollStart)
        return Mat4::translate(cellCenter(kPath[0]) + Vec3{0.0f, dropHeight(t), 0.0f});

    if (t < kHopStart) {
        const int step = std::min(kRolls - 1, static_cast<int>((t - kRollStart) / kRollStep));
        const float u = phase(t, kRollStart + step * kRollStep, kRollStep);
        const Vec3 a = cellCenter(kPath[step]);
        const Vec3 b = cellCenter(kPath[step + 1]);
        const float dx = b.x - a.x;
        const float dz = b.z - a.z;
        const Vec3 pivot{(a.x + b.x) * 0.5f, 0.0f, (a.z + b.z) * 0.5f};
        const float angle = 0.5f * kPi * easeInOutCubic(u);
        const Vec3 lift{0.0f, 0.12f * std::sin(kPi * u), 0.0f};
        return Mat4::translate(pivot + lift) * Mat4::rotate(angle, {dz, 0.0f, -dx}) * Mat4::translate(a - pivot);
    }

    const Vec3 centre = cellCenter(kPath[kRolls]);
    const float v = phase(t, kHopStart, BootAnimation::kSlamTime - kHopStart);
    if (v < 1.0f) {
        const float e = easeInOutCubic(v);
        Mat4 spin = Mat4::rotate(2.0f * kPi * e, {0.0f, 1.0f, 0.0f});
        if (alternate)
            spin = spin * Mat4::rotate(2.0f * kPi * e, {1.0f, 0.0f, 0.0f});
        return Mat4::translate(centre + Vec3{0.0f, hop(v, 1.6f), 0.0f}) * spin;
    }

    // Settled core: a gentle breath once the chime has rung.
    const float since = static_cast<float>(t - BootAnimation::kSlamTime);
    const float squash = 1.0f - 0.12f * std::exp(-since * 9.0f) * std::cos(since * 28.0f);
    const float breath = 1.0f + 0.03f * std::sin(since * 3.2f);
    Mat4 scale = Mat4::scale(breath);
    scale.m[5] *= squash;
    return Mat4::translate(centre + Vec3{0.0f, (squash * breath - 1.0f) * 0.5f, 0.0f}) * scale;
}

} // namespace

BootFrame BootAnimation::evaluate(double t, bool alternate, float aspect) {
    BootFrame frame;
    frame.fadeIn = phase(t, 0.0, kFadeLength);
    frame.blackout = phase(t, kSkipTo, kDuration - kSkipTo);
    frame.logoAlpha = easeOutCubic(phase(t, kLogoStart, 0.6));
    const float flashT = phase(t, kSlamTime, 0.6);
    frame.flash = t >= kSlamTime ? 0.55f * (1.0f - flashT) * (1.0f - flashT) : 0.0f;

    // Trail cubes pop up behind the leader as it leaves each cell.
    for (int k = 0; k < kRolls; ++k) {
        const double spawn = kRollStart + k * kRollStep;
        if (t < spawn)
            break;
        const float s = 0.94f * std::max(0.0f, easeOutBack(phase(t, spawn, 0.24)));
        Vec3 position = cellCenter(kPath[k]);
        position.y = 0.5f * s;
        if (t > kSlamTime + 0.9) {
            // Ripple once the emblem is complete.
            position.y += 0.06f * std::sin(static_cast<float>(t - kSlamTime) * 4.0f - static_cast<float>(k) * 0.7f) *
                          std::exp(-static_cast<float>(t - kSlamTime - 0.9) * 0.8f);
        }
        BootCube cube;
        cube.model = Mat4::translate(position) * Mat4::scale(s);
        cube.color = alternate ? kPartyColors[k % 4] : kTrailColor;
        cube.emissive = t >= kSlamTime ? 0.25f * frame.flash : 0.0f;
        frame.cubes.push_back(cube);
    }

    if (t >= kDropStart) {
        BootCube leader;
        leader.model = leaderModel(t, alternate);
        const float glow = t >= kSlamTime ? 0.12f + 0.35f * std::exp(-static_cast<float>(t - kSlamTime) * 3.0f) : 0.06f;
        leader.color = alternate ? hueColor(static_cast<float>(t) * 0.6f) : kLeaderColor;
        leader.emissive = glow;
        frame.cubes.push_back(leader);
    }

    // Camera: high front view while tracing, then an isometric swing.
    const float settle = easeInOutCubic(phase(t, kSlamTime, kSettleLength));
    const float drift = static_cast<float>(std::max(0.0, t - kSlamTime - kSettleLength)) * 0.05f;
    const float pitch = lerp(62.0f, 36.0f, settle) * kPi / 180.0f;
    const float yaw = (lerp(0.0f, 28.0f, settle) * kPi / 180.0f) + drift;
    const float approach = phase(t, kRollStart, kHopStart - kRollStart);
    const float distance = lerp(12.5f - 0.8f * approach, 15.0f, settle);
    // Lowering the target lifts the emblem on screen, above the wordmark.
    const Vec3 target{0.0f, lerp(0.3f, -0.75f, settle), 0.0f};
    frame.eye = target + Vec3{distance * std::cos(pitch) * std::sin(yaw), distance * std::sin(pitch),
                              distance * std::cos(pitch) * std::cos(yaw)};
    frame.viewProjection = Mat4::perspective(30.0f * kPi / 180.0f, aspect, 0.1f, 60.0f) *
                           Mat4::lookAt(frame.eye, target, {0.0f, 1.0f, 0.0f});
    return frame;
}

std::vector<BootCue> BootAnimation::cues(bool alternate) {
    std::vector<BootCue> out;
    out.push_back({kDropStart + 0.32, Sound::Land, 0.9f});
    out.push_back({kDropStart + 0.50, Sound::Roll, 0.45f});
    out.push_back({kDropStart + 0.60, Sound::Roll, 0.3f});
    for (int k = 1; k <= kRolls; ++k)
        out.push_back({kRollStart + k * kRollStep, Sound::Roll, 0.6f});
    out.push_back({kSlamTime, alternate ? Sound::ChimeAlt : Sound::Chime, 1.0f});
    return out;
}

void BootAnimation::start(double now) {
    m_start = now;
    m_lastT = -1.0;
    m_alternate = false;
}

void BootAnimation::skip(double now) {
    if (elapsed(now) < kSkipTo) {
        m_start = now - kSkipTo;
        m_lastT = kSkipTo;
    }
}

void BootAnimation::update(double now, bool alternateHeld, Platform& platform) {
    const double t = elapsed(now);
    if (alternateHeld && t < kSlamTime)
        m_alternate = true;
    for (const BootCue& cue : cues(m_alternate)) {
        if (cue.time > m_lastT && cue.time <= t)
            platform.playSound(cue.sound, cue.gain);
    }
    m_lastT = t;
}

void BootAnimation::draw(Renderer& renderer, double now, const BootBranding& branding) const {
    const double t = elapsed(now);
    const BootFrame frame = evaluate(t, m_alternate, Renderer::kWidth / Renderer::kHeight);
    constexpr float W = Renderer::kWidth;
    constexpr float H = Renderer::kHeight;

    renderer.rect(0, 0, W, H, rgb(0x0E0A2C));
    renderer.softRect(W * 0.5f - 420.0f, H * 0.42f - 300.0f, 840.0f, 600.0f, 300.0f, 260.0f,
                      withAlpha(m_alternate ? rgb(0x7A3BC4) : rgb(0x4B3BC4), 0.75f));
    renderer.softRect(W * 0.5f - 200.0f, H * 0.40f - 140.0f, 400.0f, 280.0f, 140.0f, 160.0f,
                      withAlpha(rgb(0x8D7DFF), 0.25f + 0.4f * frame.flash));

    renderer.begin3D(0, 0, W, H);
    for (const BootCube& cube : frame.cubes)
        renderer.cube(frame.viewProjection, cube.model, frame.eye, cube.color, cube.emissive);
    renderer.end3D();

    if (frame.logoAlpha > 0.0f) {
        const float rise = (1.0f - frame.logoAlpha) * 14.0f;
        const Color soft = withAlpha(rgb(0xB9B0FF), frame.logoAlpha);
        renderer.text(FontWeight::Display, 20.0f, W * 0.5f, 452.0f + rise, branding.presents, soft, Align::Center, 1.0f);
        if (branding.logo && *branding.logo) {
            const float lw = 560.0f;
            const float lh = lw * static_cast<float>(branding.logo->height) / static_cast<float>(branding.logo->width);
            renderer.image(*branding.logo, W * 0.5f - lw * 0.5f, 484.0f + rise, lw, lh,
                           withAlpha(rgb(0xFFFFFF), frame.logoAlpha));
        } else {
            renderer.text(FontWeight::Display, 66.0f, W * 0.5f, 482.0f + rise, "PartyBoard",
                          withAlpha(rgb(0xFFFFFF), frame.logoAlpha), Align::Center, 1.5f);
        }
        renderer.text(FontWeight::Display, 22.0f, W * 0.5f, 596.0f + rise, "GAMECUBE", soft, Align::Center, 9.0f);
    }

    if (frame.flash > 0.0f) {
        // A burst of light from the core rather than a flat wash.
        const float r = 180.0f + 520.0f * (1.0f - frame.flash / 0.55f);
        renderer.softRect(W * 0.5f - r, H * 0.42f - r, r * 2.0f, r * 2.0f, r, r * 0.8f,
                          withAlpha(rgb(0xF1EDFF), frame.flash * 0.9f));
        renderer.rect(0, 0, W, H, withAlpha(rgb(0xFFFFFF), frame.flash * 0.08f));
    }
    if (frame.fadeIn < 1.0f)
        renderer.rect(0, 0, W, H, withAlpha(rgb(0x000000), 1.0f - frame.fadeIn));
    if (frame.blackout > 0.0f)
        renderer.rect(0, 0, W, H, withAlpha(rgb(0x000000), frame.blackout));
}

} // namespace partyboard::launcher
