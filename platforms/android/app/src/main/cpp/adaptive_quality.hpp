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
  // What the last update did, for the log (stereo_view.cpp).
  enum class Decision { Kept, Lowered, Raised, Restored };
  // A lowering that did not bring more images is undone, and pixels are not
  // lowered again for this many fresh windows (one a second).
  static constexpr unsigned kHoldWindows = 30;

  void set_cap(float cap) {
    mCap = std::clamp(cap, 0.65f, 1.25f);
    mScale = std::min(mScale, mCap);
    mCalm = 0;
    mTrial = {};
  }
  float scale() const { return mScale; }
  float cap() const { return mCap; }
  Decision last_decision() const { return mDecision; }
  bool holding() const { return mHold != 0; }

  // `requests`: images the game asked for in the window; `busy`: those
  // refused because every eye image was in use.
  float update(const QualitySample& sample, unsigned requests, unsigned busy) {
    mDecision = Decision::Kept;
    if (sample.sequence == 0 || sample.sequence == mSequence) return mScale;
    mSequence = sample.sequence;
    if (requests < 30 || !std::isfinite(sample.refreshHz) || sample.refreshHz < 60) {
      mCalm = 0;
      return mScale;
    }
    const unsigned delivered = requests - std::min(busy, requests);
    if (mHold != 0) --mHold;
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
    // Fewer pixels only help a frame limited by its pixels. The headset's
    // counters cannot tell (they miss the game's Vulkan work): on the Toad
    // board, 414 draws per eye kept 43-45 images/s from 80% down to 70%.
    // So a lowering is a trial: the window right after it drains the ring
    // for the new images and is skipped; the next one must bring clearly
    // more images, or the pixels come back and stay for kHoldWindows.
    if (mTrial.active) {
      if (++mTrial.windows < 2) return mScale;
      const bool helped = static_cast<float>(delivered) >= 1.05f * static_cast<float>(mTrial.deliveredBefore) + 1.0f;
      mTrial.active = false;
      if (pressure && !helped) {
        mScale = mTrial.scaleBefore;
        mHold = kHoldWindows;
        mCalm = 0;
        mDecision = Decision::Restored;
        return mScale;
      }
    }
    if (pressure && mHold == 0 && mScale > 0.65f) {
      mTrial = {true, mScale, delivered, 0};
      mScale = std::max(0.65f, mScale - 0.05f);
      mCalm = 0;
      mDecision = Decision::Lowered;
    } else if (pressure) {
      mCalm = 0;
    } else if (!cpuBound && busy == 0 && sample.latePercent < 1 &&
               (!gpuKnown || gpu < 0.85f * budget) &&
               (!cpuKnown || sample.cpuMs < 0.85f * budget)) {
      if (++mCalm >= 5) {
        const float before = mScale;
        mScale = std::min(mCap, mScale + 0.05f);
        mCalm = 0;
        if (mScale != before) mDecision = Decision::Raised;
      }
    } else {
      mCalm = 0;
    }
    return mScale;
  }

private:
  struct Trial {
    bool active = false;
    float scaleBefore = 1.0f;
    unsigned deliveredBefore = 0;
    unsigned windows = 0;
  };
  float mScale = 1.0f, mCap = 1.25f;
  uint64_t mSequence = 0;
  unsigned mCalm = 0;
  unsigned mHold = 0;
  Trial mTrial;
  Decision mDecision = Decision::Kept;
};

inline float quality_cap(int screenHeight) {
  return screenHeight >= 2160 ? 1.25f : screenHeight >= 1440 ? 1.0f : 0.8f;
}

} // namespace quest
