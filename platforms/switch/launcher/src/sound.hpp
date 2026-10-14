#pragma once

// Procedurally synthesised launcher sounds and a tiny lock-free mixer.
//
// Every sound is generated at start-up from oscillators: the launcher ships
// no recorded audio, and none of it is taken from a Nintendo console.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace partyboard::launcher {

enum class Sound : unsigned char {
    Move,
    Select,
    Back,
    Open,
    Error,
    Roll,
    Land,
    Chime,
    ChimeAlt,
    Count,
};

constexpr int kSampleRate = 48000;

// Stereo interleaved signed 16-bit PCM at kSampleRate.
struct SoundBank {
    std::array<std::vector<int16_t>, static_cast<size_t>(Sound::Count)> clips;

    void generate();
    const std::vector<int16_t>& clip(Sound sound) const { return clips[static_cast<size_t>(sound)]; }
};

// trigger() is called from the UI thread, mix() from the audio thread. The
// only shared state is a single-producer/single-consumer command ring.
class Mixer {
public:
    explicit Mixer(const SoundBank& bank) : m_bank(bank) {}

    void trigger(Sound sound, float gain = 1.0f);
    void mix(int16_t* out, size_t frames);

private:
    struct Command {
        Sound sound;
        float gain;
    };
    struct Voice {
        const std::vector<int16_t>* clip = nullptr;
        size_t position = 0;
        float gain = 0.0f;
        uint32_t age = 0;
    };

    const SoundBank& m_bank;
    std::array<Command, 64> m_commands{};
    std::atomic<uint32_t> m_write{0};
    std::atomic<uint32_t> m_read{0};
    std::array<Voice, 12> m_voices{};
    uint32_t m_clock = 0;
};

} // namespace partyboard::launcher
