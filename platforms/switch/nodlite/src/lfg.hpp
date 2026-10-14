#pragma once

// Lagged Fibonacci generator for GameCube junk (padding) data, as used by
// RVZ to store junk as a 68-byte seed instead of the bytes themselves.
//
// Port of nod's util/lfg.rs (MIT OR Apache-2.0), itself based on Dolphin's
// LaggedFibonacciGenerator.cpp and docs/WiaAndRvz.md (CC0-1.0).

#include <cstddef>
#include <cstdint>

namespace nodlite {

class LaggedFibonacci {
public:
    static constexpr size_t kK = 521;
    static constexpr size_t kJ = 32;
    static constexpr size_t kSeedWords = 17;
    static constexpr size_t kSeedBytes = kSeedWords * 4;
    static constexpr size_t kKBytes = kK * 4;
    static constexpr uint64_t kSectorSize = 0x8000;

    // Seed stored in an RVZ group: 17 big-endian words.
    void initWithSeedBytes(const uint8_t* seed);
    // The disc's own junk at a disc offset (what mastering wrote).
    void initWithDisc(const uint8_t discId[4], uint8_t discNumber, uint64_t offset);

    void skip(size_t bytes);
    void fill(uint8_t* out, size_t size);

    // Junk restarts every 32 KiB sector: reseed at each boundary.
    static void fillSectorChunked(uint8_t* out, size_t size, const uint8_t discId[4], uint8_t discNumber,
                                  uint64_t offset);

private:
    void init();
    void forward();

    uint32_t m_buffer[kK] = {};
    size_t m_position = 0;
};

} // namespace nodlite
