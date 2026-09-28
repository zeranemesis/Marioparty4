#include "../../platforms/android/app/src/main/cpp/adaptive_quality.hpp"
#include "../../extern/aurora/lib/gfx/rgba_mips.hpp"
#include "rgba_mips_reference.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>

int main() {
  quest::AdaptiveQuality gpu;
  quest::QualitySample sample{1, 120, 8, 2, 1, 4};
  assert(std::abs(gpu.update(sample, 120, 20) - 0.95f) < 0.0001f);
  assert(std::abs(gpu.update(sample, 120, 20) - 0.95f) < 0.0001f); // stale sample
  quest::AdaptiveQuality cpu;
  sample = {1, 120, 2, 12, 1, 20};
  assert(cpu.update(sample, 120, 80) == 1); // lowering pixels cannot fix CPU load
  sample = {2, 72, 8, 2, 1, 0};
  assert(cpu.update(sample, 72, 0) == 1); // same work fits the slower display
  for (unsigned i = 3; i < 7; ++i) { sample.sequence = i; cpu.update(sample, 72, 0); }
  assert(std::abs(cpu.scale() - 1.05f) < 0.0001f); // five fresh calm windows
  cpu.set_cap(0.8f);
  assert(cpu.scale() == 0.8f);
  for (unsigned i = 7; i < 40; ++i) { sample.sequence = i; cpu.update(sample, 72, 0); }
  assert(cpu.scale() == 0.8f); // profile ceiling
  sample = {40, 120, 20, 2, 1, 50};
  for (unsigned i = 40; i < 60; ++i) { sample.sequence = i; cpu.update(sample, 120, 80); }
  assert(cpu.scale() == 0.65f); // readable floor
  quest::AdaptiveQuality blind;
  sample = {1, 120, 2, 2, 1, 5};
  assert(blind.update(sample, 120, 20) < 1); // late frames, full ring: lower despite a low GPU counter
  sample = {2, 120, 2, 2, 1, 0};
  const float held = blind.update(sample, 120, 20);
  assert(held == blind.scale() && held < 1); // a full ring alone is not enough to lower again
  quest::AdaptiveQuality missing;
  sample = {1, 120, -1, 12, -1, 10};
  assert(missing.update(sample, 120, 80) == 1); // missing GPU, known CPU pressure
  sample = {2, 120, -1, -1, -1, 10};
  assert(missing.update(sample, 120, 80) < 1); // bounded fallback
  sample = {3, std::numeric_limits<float>::quiet_NaN(), 20, 2, 1, 20};
  const float previous = missing.scale();
  assert(missing.update(sample, 120, 80) == previous);
  sample = {4, 120, 20, 2, 1, 20};
  assert(missing.update(sample, 0, 0) == previous); // menus/inactive world
  assert(quest::quality_cap(1080) == 0.8f && quest::quality_cap(1440) == 1 && quest::quality_cap(2160) == 1.25f);

  const uint8_t contrast[]{0,0,0,255, 255,255,255,255};
  const auto mips = aurora::gfx::rgba_mip_chain(contrast, 2, 1);
  assert(mips.levels == 2 && mips.bytes.size() == 12);
  for (unsigned i = 0; i < 8; ++i) assert(mips.bytes[i] == contrast[i]);
  assert(mips.bytes[8] >= 187 && mips.bytes[8] <= 189 && mips.bytes[11] == 255); // linear light
  const uint8_t fringe[]{255,0,0,255, 0,0,255,0};
  const auto alpha = aurora::gfx::rgba_mip_chain(fringe, 2, 1);
  assert(alpha.bytes[8] == 255 && alpha.bytes[9] == 0 && alpha.bytes[10] == 0); // no blue fringe
  const uint8_t odd[]{0,0,0,255, 0,0,0,255, 255,255,255,255};
  const auto npot = aurora::gfx::rgba_mip_chain(odd, 3, 1);
  assert(npot.levels == 2 && npot.bytes[12] >= 155 && npot.bytes[12] <= 157); // last texel included
  const uint8_t smooth[]{255,255,255,64, 255,255,255,128};
  const auto translucent = aurora::gfx::rgba_mip_chain(smooth, 2, 1);
  assert(translucent.bytes[11] == 96); // no cutout rescale on smooth alpha
  assert(aurora::gfx::rgba_mip_chain(nullptr, 0, 0).bytes.empty());

  // The lookup encoder gives pow's byte for every float near each rounding
  // step (where a difference could hide) and on a stride through [0, 2].
  namespace detail = aurora::gfx::rgba_mips_detail;
  const auto& tables = detail::srgb_tables();
  const auto as_float = [](uint32_t bits) { float f; std::memcpy(&f, &bits, sizeof f); return f; };
  const auto same_encoding = [&](uint32_t bits) {
    const float v = as_float(bits);
    return detail::encode_srgb_table(tables, v) == detail::encode_srgb(v);
  };
  for (unsigned k = 1; k < 256; ++k) {
    uint32_t bits;
    std::memcpy(&bits, &tables.threshold[k], sizeof bits);
    for (uint32_t b = bits - std::min(bits, 4096u); b <= bits + 4096u; ++b) assert(same_encoding(b));
  }
  for (uint32_t b = 0; b <= 0x40000000u; b += 997) assert(same_encoding(b));

  // Same bytes as the pow-per-texel version, on textures of every kind.
  std::mt19937 random{4};
  const auto texture = [&](uint32_t w, uint32_t h, int kind) {
    std::vector<uint8_t> t(size_t(w) * h * 4);
    for (size_t i = 0; i < t.size(); ++i) {
      const uint8_t v = uint8_t(random());
      t[i] = i % 4 != 3 ? v : kind == 0 ? 255 : kind == 1 ? (v < 100 ? 0 : 255) : v;
    }
    return t;
  };
  const uint32_t sizes[][2]{{16, 16}, {64, 32}, {33, 17}, {256, 256}, {7, 1}, {1, 9}, {100, 60}, {8, 1}, {1, 8}, {2, 2}, {48, 7}};
  for (const auto& size : sizes)
    for (int kind = 0; kind < 3; ++kind) {
      const auto t = texture(size[0], size[1], kind);
      const auto a = aurora::gfx::rgba_mip_chain(t.data(), size[0], size[1]);
      const auto b = reference::rgba_mip_chain(t.data(), size[0], size[1]);
      assert(a.levels == b.levels && a.bytes == b.bytes);
    }

  // How long a 512x512 world texture takes on this PC, before and after.
  const auto big = texture(512, 512, 2);
  const auto time = [&](auto&& build) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 5; ++i) build();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 5;
  };
  const double before = time([&] { (void)reference::rgba_mip_chain(big.data(), 512, 512); });
  const double after = time([&] { (void)aurora::gfx::rgba_mip_chain(big.data(), 512, 512); });
  std::printf("512x512 mip chain: %.2f ms with pow per texel, %.2f ms with the lookup\n", before, after);
  std::puts("PASS: GPU/CPU decisions, freshness, recovery, profile bounds, gamma, alpha and NPOT mipmaps");
}
