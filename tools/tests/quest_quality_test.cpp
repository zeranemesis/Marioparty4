#include "../../platforms/android/app/src/main/cpp/adaptive_quality.hpp"
#include "../../extern/aurora/lib/gfx/rgba_mips.hpp"
#include <cassert>
#include <cstdio>
#include <limits>

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
  std::puts("PASS: GPU/CPU decisions, freshness, recovery, profile bounds, gamma, alpha and NPOT mipmaps");
}
