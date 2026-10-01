#include "sound.hpp"

#include <algorithm>
#include <cmath>

namespace partyboard::launcher {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

// Stereo float buffer that clips are composed in before quantisation.
struct Track {
    std::vector<float> left;
    std::vector<float> right;

    explicit Track(float seconds)
        : left(static_cast<size_t>(seconds * kSampleRate), 0.0f),
          right(static_cast<size_t>(seconds * kSampleRate), 0.0f) {}

    void add(size_t frame, float value, float pan) {
        if (frame >= left.size())
            return;
        // Constant-power pan, pan in [-1, 1].
        const float angle = (pan + 1.0f) * 0.25f * 3.14159265f;
        left[frame] += value * std::cos(angle);
        right[frame] += value * std::sin(angle);
    }
};

float envelope(float t, float attack, float decay) {
    if (t < 0.0f)
        return 0.0f;
    if (t < attack)
        return t / attack;
    return std::exp(-(t - attack) / decay);
}

// Two-operator FM bell: bright attack that mellows as the index decays.
void bell(Track& track, float start, float freq, float length, float gain, float pan, float ratio = 3.5f) {
    const size_t first = static_cast<size_t>(start * kSampleRate);
    const size_t count = static_cast<size_t>(length * kSampleRate);
    for (size_t i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / kSampleRate;
        const float index = 2.4f * std::exp(-t / 0.18f);
        const float mod = std::sin(kTwoPi * freq * ratio * t) * index;
        const float carrier = std::sin(kTwoPi * freq * t + mod);
        const float shimmer = 0.18f * std::sin(kTwoPi * freq * 2.0f * t) * std::exp(-t / 0.25f);
        track.add(first + i, (carrier + shimmer) * envelope(t, 0.004f, length * 0.32f) * gain, pan);
    }
}

void tone(Track& track, float start, float freqFrom, float freqTo, float length, float gain, float pan,
          float attack = 0.003f, float decay = 0.05f, float harmonics = 0.0f) {
    const size_t first = static_cast<size_t>(start * kSampleRate);
    const size_t count = static_cast<size_t>(length * kSampleRate);
    float phase = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / kSampleRate;
        const float freq = freqFrom + (freqTo - freqFrom) * (t / length);
        phase += kTwoPi * freq / kSampleRate;
        float v = std::sin(phase);
        if (harmonics > 0.0f)
            v += harmonics * (std::sin(phase * 3.0f) / 3.0f + std::sin(phase * 5.0f) / 5.0f);
        track.add(first + i, v * envelope(t, attack, decay) * gain, pan);
    }
}

// Deterministic noise so every build sounds identical.
struct Noise {
    uint32_t state = 0x9E3779B9u;
    float next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(state & 0xFFFF) / 32767.5f - 1.0f;
    }
};

void click(Track& track, float start, float freq, float length, float gain, float pan) {
    Noise noise;
    float low = 0.0f;
    const size_t first = static_cast<size_t>(start * kSampleRate);
    const size_t count = static_cast<size_t>(length * kSampleRate);
    for (size_t i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / kSampleRate;
        low += 0.25f * (noise.next() - low);
        const float body = std::sin(kTwoPi * freq * t * (1.0f - 0.35f * t / length));
        const float v = body * envelope(t, 0.001f, length * 0.25f) + low * envelope(t, 0.0005f, 0.006f);
        track.add(first + i, v * gain, pan);
    }
}

void pad(Track& track, float start, float freq, float length, float gain) {
    const size_t first = static_cast<size_t>(start * kSampleRate);
    const size_t count = static_cast<size_t>(length * kSampleRate);
    for (size_t i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / kSampleRate;
        const float swell = std::sin(3.14159265f * std::min(1.0f, t / length));
        const float v = std::sin(kTwoPi * freq * t) + 0.5f * std::sin(kTwoPi * freq * 2.003f * t) +
                        0.25f * std::sin(kTwoPi * freq * 2.997f * t);
        track.add(first + i, v * swell * swell * gain, -0.3f);
        track.add(first + i, std::sin(kTwoPi * freq * 1.004f * t) * swell * swell * gain * 0.6f, 0.3f);
    }
}

std::vector<int16_t> quantise(const Track& track) {
    std::vector<int16_t> out(track.left.size() * 2);
    for (size_t i = 0; i < track.left.size(); ++i) {
        // Gentle tanh limiter: chords may sum above 1.0.
        out[i * 2] = static_cast<int16_t>(std::tanh(track.left[i]) * 30000.0f);
        out[i * 2 + 1] = static_cast<int16_t>(std::tanh(track.right[i]) * 30000.0f);
    }
    return out;
}

// Note frequencies (equal temperament, A4 = 440 Hz).
constexpr float C5 = 523.25f, E5 = 659.26f, G5 = 783.99f, A5 = 880.00f, B5 = 987.77f;
constexpr float C6 = 1046.50f, E6 = 1318.51f, G6 = 1567.98f, C7 = 2093.00f;
constexpr float C3 = 130.81f, G3 = 196.00f;

std::vector<int16_t> makeChime() {
    // An original rising major arpeggio that resolves on a sustained chord,
    // timed so the last bell lands as the logo locks in.
    Track track(3.2f);
    pad(track, 0.0f, C3, 2.8f, 0.10f);
    pad(track, 0.15f, G3, 2.6f, 0.06f);
    bell(track, 0.00f, G5, 1.0f, 0.26f, -0.5f);
    bell(track, 0.11f, C6, 1.0f, 0.26f, -0.2f);
    bell(track, 0.22f, E6, 1.0f, 0.24f, 0.2f);
    bell(track, 0.33f, G6, 1.2f, 0.22f, 0.5f);
    bell(track, 0.62f, C6, 2.4f, 0.22f, -0.3f, 2.0f);
    bell(track, 0.62f, E6, 2.4f, 0.18f, 0.0f, 2.0f);
    bell(track, 0.62f, G6, 2.4f, 0.16f, 0.3f, 2.0f);
    bell(track, 0.62f, C7, 2.5f, 0.10f, 0.0f, 1.5f);
    return quantise(track);
}

std::vector<int16_t> makeChimeAlt() {
    // The playful variant: bouncing notes and a slide-whistle finish.
    Track track(3.2f);
    pad(track, 0.0f, C3, 2.6f, 0.08f);
    const float notes[] = {C6, G5, E6, C6, G6, E6, C7};
    for (int i = 0; i < 7; ++i)
        bell(track, 0.09f * static_cast<float>(i), notes[i], 0.6f, 0.2f, (i % 2) ? 0.5f : -0.5f, 4.0f);
    tone(track, 0.70f, A5, C7, 0.45f, 0.18f, 0.0f, 0.02f, 0.30f);
    bell(track, 1.15f, C6, 2.0f, 0.22f, -0.2f, 2.0f);
    bell(track, 1.15f, E6, 2.0f, 0.18f, 0.2f, 2.0f);
    bell(track, 1.15f, G6, 2.0f, 0.15f, 0.0f, 2.0f);
    return quantise(track);
}

} // namespace

void SoundBank::generate() {
    auto set = [this](Sound sound, std::vector<int16_t> pcm) { clips[static_cast<size_t>(sound)] = std::move(pcm); };

    {
        Track t(0.08f);
        tone(t, 0.0f, 1320.0f, 1320.0f, 0.08f, 0.22f, 0.0f, 0.002f, 0.022f, 0.2f);
        set(Sound::Move, quantise(t));
    }
    {
        Track t(0.26f);
        bell(t, 0.0f, A5, 0.14f, 0.22f, -0.1f, 2.0f);
        bell(t, 0.06f, E6, 0.2f, 0.22f, 0.1f, 2.0f);
        set(Sound::Select, quantise(t));
    }
    {
        Track t(0.22f);
        bell(t, 0.0f, B5, 0.12f, 0.2f, 0.1f, 2.0f);
        bell(t, 0.06f, E5, 0.16f, 0.2f, -0.1f, 2.0f);
        set(Sound::Back, quantise(t));
    }
    {
        Track t(0.2f);
        tone(t, 0.0f, 420.0f, 980.0f, 0.18f, 0.18f, 0.0f, 0.02f, 0.08f);
        set(Sound::Open, quantise(t));
    }
    {
        Track t(0.42f);
        tone(t, 0.0f, 233.0f, 220.0f, 0.18f, 0.22f, 0.0f, 0.004f, 0.12f, 0.8f);
        tone(t, 0.2f, 207.0f, 196.0f, 0.22f, 0.22f, 0.0f, 0.004f, 0.14f, 0.8f);
        set(Sound::Error, quantise(t));
    }
    {
        Track t(0.09f);
        click(t, 0.0f, 760.0f, 0.09f, 0.32f, 0.0f);
        set(Sound::Roll, quantise(t));
    }
    {
        Track t(0.22f);
        click(t, 0.0f, 180.0f, 0.22f, 0.55f, 0.0f);
        bell(t, 0.0f, C5, 0.2f, 0.08f, 0.0f, 1.0f);
        set(Sound::Land, quantise(t));
    }
    set(Sound::Chime, makeChime());
    set(Sound::ChimeAlt, makeChimeAlt());
}

void Mixer::trigger(Sound sound, float gain) {
    const uint32_t write = m_write.load(std::memory_order_relaxed);
    const uint32_t read = m_read.load(std::memory_order_acquire);
    if (write - read >= m_commands.size())
        return; // full: dropping a UI blip beats blocking the frame
    m_commands[write % m_commands.size()] = {sound, gain};
    m_write.store(write + 1, std::memory_order_release);
}

void Mixer::mix(int16_t* out, size_t frames) {
    uint32_t read = m_read.load(std::memory_order_relaxed);
    const uint32_t write = m_write.load(std::memory_order_acquire);
    for (; read != write; ++read) {
        const Command command = m_commands[read % m_commands.size()];
        Voice* slot = &m_voices[0];
        for (Voice& voice : m_voices) {
            if (!voice.clip) {
                slot = &voice;
                break;
            }
            if (voice.age < slot->age)
                slot = &voice; // steal the oldest
        }
        slot->clip = &m_bank.clip(command.sound);
        slot->position = 0;
        slot->gain = command.gain;
        slot->age = ++m_clock;
    }
    m_read.store(read, std::memory_order_release);

    for (size_t i = 0; i < frames * 2; ++i) {
        int32_t sum = 0;
        for (Voice& voice : m_voices) {
            if (!voice.clip)
                continue;
            if (voice.position >= voice.clip->size()) {
                voice.clip = nullptr;
                continue;
            }
            sum += static_cast<int32_t>(static_cast<float>((*voice.clip)[voice.position++]) * voice.gain);
        }
        out[i] = static_cast<int16_t>(std::clamp<int32_t>(sum, -32768, 32767));
    }
}

} // namespace partyboard::launcher
