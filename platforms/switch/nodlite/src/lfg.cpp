#include "lfg.hpp"

#include <algorithm>
#include <cstring>

namespace nodlite {

namespace {

// The output is the buffer's memory read as bytes once every word has been
// stored big-endian, so XOR steps can work on whole words either way.
uint32_t toBigEndianStorage(uint32_t v) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return v;
#else
    return __builtin_bswap32(v);
#endif
}

void generateSeed(uint32_t out[LaggedFibonacci::kSeedWords], const uint8_t id[4], uint8_t discNumber,
                  uint32_t sector) {
    const uint32_t seed = ((uint32_t(id[2]) << 24) | (uint32_t(id[1]) << 16) |
                           (uint32_t(uint8_t(id[3] + id[2])) << 8) | uint32_t(uint8_t(id[0] + id[1]))) ^
                          discNumber;
    uint32_t n = (seed * 0x260BCD5u) ^ (sector * 0x1EF29123u);
    for (size_t i = 0; i < LaggedFibonacci::kSeedWords; ++i) {
        uint32_t v = 0;
        for (size_t k = 0; k < LaggedFibonacci::kJ; ++k) {
            n = n * 0x5D588B65u + 1u;
            v = (v >> 1) | (n & 0x80000000u);
        }
        out[i] = v;
    }
    out[16] ^= (out[0] >> 9) ^ (out[16] << 23);
}

} // namespace

void LaggedFibonacci::init() {
    for (size_t i = kSeedWords; i < kK; ++i)
        m_buffer[i] = (m_buffer[i - kSeedWords] << 23) ^ (m_buffer[i - kSeedWords + 1] >> 9) ^ m_buffer[i - 1];
    // Dolphin shifts by 18 instead of 16 when emitting one byte; doing it here
    // keeps the output a plain byte copy.
    for (uint32_t& x : m_buffer)
        x = toBigEndianStorage((x & 0xFF00FFFFu) | ((x >> 2) & 0x00FF0000u));
    for (int i = 0; i < 4; ++i)
        forward();
}

void LaggedFibonacci::forward() {
    for (size_t i = 0; i < kJ; ++i)
        m_buffer[i] ^= m_buffer[i + kK - kJ];
    for (size_t i = kJ; i < kK; ++i)
        m_buffer[i] ^= m_buffer[i - kJ];
}

void LaggedFibonacci::initWithSeedBytes(const uint8_t* seed) {
    for (size_t i = 0; i < kSeedWords; ++i) {
        const uint8_t* p = seed + i * 4;
        m_buffer[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }
    m_position = 0;
    init();
}

void LaggedFibonacci::initWithDisc(const uint8_t discId[4], uint8_t discNumber, uint64_t offset) {
    generateSeed(m_buffer, discId, discNumber, static_cast<uint32_t>(offset / kSectorSize));
    m_position = 0;
    init();
    skip(static_cast<size_t>(offset % kSectorSize));
}

void LaggedFibonacci::skip(size_t bytes) {
    m_position += bytes;
    while (m_position >= kKBytes) {
        forward();
        m_position -= kKBytes;
    }
}

void LaggedFibonacci::fill(uint8_t* out, size_t size) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(m_buffer);
    while (size > 0) {
        while (m_position >= kKBytes) {
            forward();
            m_position -= kKBytes;
        }
        const size_t n = std::min(size, kKBytes - m_position);
        std::memcpy(out, bytes + m_position, n);
        m_position += n;
        out += n;
        size -= n;
    }
}

void LaggedFibonacci::fillSectorChunked(uint8_t* out, size_t size, const uint8_t discId[4], uint8_t discNumber,
                                        uint64_t offset) {
    LaggedFibonacci lfg;
    while (size > 0) {
        lfg.initWithDisc(discId, discNumber, offset);
        const size_t n = std::min<size_t>(size, kSectorSize - offset % kSectorSize);
        lfg.fill(out, n);
        out += n;
        size -= n;
        offset += n;
    }
}

} // namespace nodlite
