// The mip chain as shipped before the lookup-table encoder, kept to prove the
// current one gives the same bytes (tools/tests/quest_quality_test.cpp).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace reference {
struct RgbaMipChain {
  std::vector<uint8_t> bytes;
  uint32_t levels = 1;
};

// Keep level zero exact. Average sRGB colors in linear light with alpha
// weighting so transparent texels cannot introduce colored fringes.
inline RgbaMipChain rgba_mip_chain(const uint8_t* pixels, uint32_t width, uint32_t height) {
  RgbaMipChain out;
  if (!pixels || !width || !height) return out;
  out.bytes.assign(pixels, pixels + size_t(width) * height * 4);
  float linear[256];
  for (unsigned i = 0; i < 256; ++i) {
    const float c = i / 255.0f;
    linear[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
  }
  size_t offset = 0;
  bool cutout = true;
  size_t covered = 0;
  for (size_t i = 3; i < out.bytes.size(); i += 4) {
    cutout &= out.bytes[i] == 0 || out.bytes[i] == 255;
    covered += out.bytes[i] >= 128;
  }
  const float coverage = float(covered) / (size_t(width) * height);
  cutout &= coverage > 0 && coverage < 1;
  while (width > 1 || height > 1) {
    const uint32_t nextW = std::max(1u, width / 2), nextH = std::max(1u, height / 2);
    std::vector<uint8_t> next(size_t(nextW) * nextH * 4);
    for (uint32_t y = 0; y < nextH; ++y) for (uint32_t x = 0; x < nextW; ++x) {
      const float x0 = float(x) * width / nextW, x1 = float(x + 1) * width / nextW;
      const float y0 = float(y) * height / nextH, y1 = float(y + 1) * height / nextH;
      float rgb[3]{}, alpha = 0, weight = 0;
      for (uint32_t sy = uint32_t(y0); sy < std::min(height, uint32_t(std::ceil(y1))); ++sy)
        for (uint32_t sx = uint32_t(x0); sx < std::min(width, uint32_t(std::ceil(x1))); ++sx) {
          const float w = (std::min(x1, float(sx + 1)) - std::max(x0, float(sx))) *
                          (std::min(y1, float(sy + 1)) - std::max(y0, float(sy)));
          const auto* p = out.bytes.data() + offset + (size_t(sy) * width + sx) * 4;
          const float a = p[3] / 255.0f;
          for (unsigned c = 0; c < 3; ++c) rgb[c] += linear[p[c]] * a * w;
          alpha += a * w;
          weight += w;
        }
      auto* p = next.data() + (size_t(y) * nextW + x) * 4;
      for (unsigned c = 0; c < 3; ++c) {
        const float v = alpha > 0 ? rgb[c] / alpha : 0;
        const float encoded = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        p[c] = uint8_t(std::clamp(std::lround(encoded * 255), 0l, 255l));
      }
      p[3] = uint8_t(std::clamp(std::lround(alpha / weight * 255), 0l, 255l));
    }
    // Preserve the closest representable cutout coverage at the common 0.5
    // threshold. Smooth alpha textures are left untouched.
    if (cutout) {
      float lo = 0, hi = 8, best = 1, error = 2;
      for (unsigned attempt = 0; attempt < 16; ++attempt) {
        const float scale = (lo + hi) * 0.5f;
        size_t visible = 0;
        for (size_t i = 3; i < next.size(); i += 4) visible += next[i] * scale >= 127.5f;
        const float fraction = float(visible) / (size_t(nextW) * nextH);
        if (std::abs(fraction - coverage) < error) { error = std::abs(fraction - coverage); best = scale; }
        if (fraction < coverage) lo = scale; else hi = scale;
      }
      for (size_t i = 3; i < next.size(); i += 4)
        next[i] = uint8_t(std::clamp(std::lround(next[i] * best), 0l, 255l));
    }
    offset = out.bytes.size();
    out.bytes.insert(out.bytes.end(), next.begin(), next.end());
    width = nextW;
    height = nextH;
    ++out.levels;
  }
  return out;
}
} // namespace reference
