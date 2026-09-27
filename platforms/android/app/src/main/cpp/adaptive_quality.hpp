#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace quest {

// One fresh measurement window. Unknown counters stay negative, never zero.
struct QualitySample {
  uint64_t sequence = 0;
  float refreshHz = 0, gpuMs = -1, cpuMs = -1, compositorGpuMs = -1, latePercent = 0;
};

class AdaptiveQuality {
public:
  void set_cap(float cap) {
    mCap = std::clamp(cap, 0.65f, 1.25f);
    mScale = std::min(mScale, mCap);
    mCalm = 0;
  }
  float scale() const { return mScale; }
  float cap() const { return mCap; }

  float update(const QualitySample& sample, unsigned requests, unsigned busy) {
    if (sample.sequence == 0 || sample.sequence == mSequence) return mScale;
    mSequence = sample.sequence;
    if (requests < 30 || !std::isfinite(sample.refreshHz) || sample.refreshHz < 60) {
      mCalm = 0;
      return mScale;
    }
    const float budget = 1000.0f / sample.refreshHz;
    const bool gpuKnown = std::isfinite(sample.gpuMs) && sample.gpuMs >= 0;
    const bool cpuKnown = std::isfinite(sample.cpuMs) && sample.cpuMs >= 0;
    const float gpu = sample.gpuMs +
        ((std::isfinite(sample.compositorGpuMs) && sample.compositorGpuMs >= 0) ? sample.compositorGpuMs : 0);
    const bool cpuBound = cpuKnown && sample.cpuMs >= 0.9f * budget;
    const bool congested = busy * 20ull > requests;
    // Frames really missed while every eye image is busy: the GPU is behind,
    // whatever its counter says (it may only see the headset's own GL work,
    // not the game's Vulkan rendering).
    const bool stalled = congested && sample.latePercent > 1;
    const bool pressure = gpuKnown ? (gpu >= 0.9f * budget ||
        (sample.latePercent > 1 && gpu >= 0.85f * budget) || (stalled && !cpuBound)) : (congested && !cpuBound);
    if (pressure) {
      mScale = std::max(0.65f, mScale - 0.05f);
      mCalm = 0;
    } else if (!cpuBound && busy == 0 && sample.latePercent < 1 &&
               (!gpuKnown || gpu < 0.85f * budget) &&
               (!cpuKnown || sample.cpuMs < 0.85f * budget)) {
      if (++mCalm >= 5) {
        mScale = std::min(mCap, mScale + 0.05f);
        mCalm = 0;
      }
    } else {
      mCalm = 0;
    }
    return mScale;
  }

private:
  float mScale = 1.0f, mCap = 1.25f;
  uint64_t mSequence = 0;
  unsigned mCalm = 0;
};

inline float quality_cap(int screenHeight) {
  return screenHeight >= 2160 ? 1.25f : screenHeight >= 1440 ? 1.0f : 0.8f;
}

} // namespace quest
