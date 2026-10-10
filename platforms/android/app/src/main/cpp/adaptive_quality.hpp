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
  // CalmAtCap: calm long enough to raise, but at the ceiling already (a
  // foveation level may be given back instead, stereo_view.cpp).
  enum class Decision { Kept, Lowered, Raised, Restored, CalmAtCap };
  // A lowering that did not bring more images is undone, and pixels are not
  // lowered again for this many fresh windows (one a second).
  static constexpr unsigned kHoldWindows = 30;
  // After a lowering, pixels are not raised again for this many fresh
  // windows. Five calm seconds were enough before: on the Toad board the
  // eyes went 90, 95, 100, 95, 90% within a minute (2026-10-04), each raise
  // making new images (the ring drains, a hitch) and bringing back the late
  // images the lowering had removed.
  static constexpr unsigned kRaiseHoldWindows = 20;
  // The highest scale of the recommended eye size: a minigame, lighter than
  // a board, starts there (StereoView::set_board_mode).
  static constexpr float kMaxCap = 1.3f;

  void set_cap(float cap) {
    mCap = std::clamp(cap, 0.65f, kMaxCap);
    mScale = std::min(mScale, mCap);
    mCalm = 0;
    mTrial = {};
  }
  float scale() const { return mScale; }
  // How far one lowering or raising goes. A minigame seen from its camera's
  // place is limited by its pixels: 5% less never brought Manta Rings' 15 ms
  // eyes' pass under its 13.9 ms, so each trial was undone and the pixels held
  // for 30 s at 120% while it ran at 48 to 59 images a second (2026-10-10).
  void set_step(float step) { mStep = std::clamp(step, 0.01f, 0.25f); }
  void set_hitches_need_gpu(bool need) { mHitchesNeedGpu = need; }
  // The lowest scale a lowering may reach (0.65 by default, readable). On a
  // board the eyes' pass took 8.7 ms at 65% as at 90%, the images made
  // smaller too (2026-10-04, build 191): its time is in the draws and their
  // vertices, not the pixels, and lowering only blurred the board.
  void set_floor(float floor) {
    mFloor = std::clamp(floor, 0.65f, mCap);
    mScale = std::max(mScale, mFloor);
  }
  // Start again from `scale` (a scene known to need it), no trial pending.
  void start_at(float scale) {
    mScale = std::clamp(scale, mFloor, mCap);
    mTrial = {};
    mCalm = 0;
    mHold = 0;
    mRaiseHold = 0;
  }
  float cap() const { return mCap; }
  Decision last_decision() const { return mDecision; }
  bool holding() const { return mHold != 0; }

  // `requests`: images the game asked for in the window; `busy`: those
  // refused because every eye image was in use; `slow`: the game's images
  // that came late, a visible hitch of the board (stereo_view.cpp);
  // `missed`: finished images that missed the display frame they were paced
  // for, shown a frame late or replaced by the next one. The game's requests
  // stay regular then (they are paced), so `slow` cannot see them: on the
  // Toad board 11 of 72 images a second missed while the eyes stayed at 90%
  // (2026-10-04).
  float update(const QualitySample& sample, unsigned requests, unsigned busy, unsigned slow = 0,
               unsigned missed = 0) {
    mDecision = Decision::Kept;
    if (sample.sequence == 0 || sample.sequence == mSequence) return mScale;
    mSequence = sample.sequence;
    if (requests < 30 || !std::isfinite(sample.refreshHz) || sample.refreshHz < 60) {
      mCalm = 0;
      return mScale;
    }
    const unsigned delivered = requests - std::min(busy, requests);
    const unsigned previousSlow = mLastSlow;
    const unsigned previousMissed = mLastMissed;
    const bool hadWindow = mHadWindow;
    mLastSlow = slow;
    mLastMissed = missed;
    mHadWindow = true;
    if (mHold != 0) --mHold;
    if (mRaiseHold != 0) --mRaiseHold;
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
    // Hitches: more than 2% of the game's images late. At 60 images a second
    // on 120 Hz the counters cannot see them and more images cannot be
    // delivered, so this is the pressure that matters: at 100% the board
    // hitched twice a second, at 80% not at all (Quest 3, 2026-09-28).
    const bool hitching = slow * 50ull > requests;
    // More than 5% of the images missing their display frame: the pacer
    // plans for 2% (frame_pacing.hpp), the rest is the GPU's queue.
    const bool missing = missed * 20ull > requests;
    // In a minigame (set_hitches_need_gpu), hitches and missed frames only
    // count while the GPU is busy: in Booksquirm one or two images a second
    // came late (the game's own spikes, up to 52 ms) with the GPU at 66% and
    // the eyes' pass at 7 ms of 13.9; each lowering was undone only in part
    // and the eyes sank to 70 to 80%, blurred for nothing (2026-10-10). A
    // board keeps them: there fewer pixels did end its hitches.
    // 90%, not 75%: the GPU then ran at its lowest clock (456 of 640 MHz) and
    // the runtime raises it only for a GPU that is really loaded; lowered at
    // 75%, Toad's Quick Draw and Paratrooper Plunge sat at 80%, blurred,
    // while the GPU had a third of its clock to spare (2026-10-10).
    const bool gpuBusy = !mHitchesNeedGpu || !gpuKnown || gpu >= 0.9f * budget;
    const bool pressure = ((hitching || missing) && gpuBusy) || (gpuKnown ? (gpu >= 0.9f * budget ||
        (sample.latePercent > 1 && gpu >= 0.85f * budget) || (stalled && !cpuBound)) : (congested && !cpuBound));
    // Fewer pixels only help a frame limited by its pixels. The headset's
    // counters cannot tell (they miss the game's Vulkan work): on the Toad
    // board, 414 draws per eye kept 43-45 images/s from 80% down to 70%.
    // So a lowering is a trial: the window right after it drains the ring
    // for the new images and is skipped; the two next ones must bring, on
    // average, clearly more images than the two before it, or the pixels
    // come back and stay for kHoldWindows. One window each way was too
    // noisy: while the board's intro camera moved, three lowerings in 16 s
    // passed and the eyes sat at 65% for the rest of the game (120 Hz,
    // 2026-09-28).
    const unsigned previousDelivered = mLastDelivered;
    mLastDelivered = delivered;
    if (mTrial.active) {
      ++mTrial.windows;
      if (mTrial.windows < 2) return mScale;
      mTrial.deliveredAfter += delivered;
      mTrial.slowAfter += slow;
      mTrial.missedAfter += missed;
      if (mTrial.windows < 3) return mScale;
      const float after = static_cast<float>(mTrial.deliveredAfter) / 2.0f;
      // More images, or (at a capped rate) half the hitches or missed
      // display frames, or fewer.
      const bool helped = after >= 1.08f * mTrial.deliveredBefore + 1.0f ||
          (mTrial.slowBefore >= 1.0f && mTrial.slowAfter / 2.0f <= 0.5f * mTrial.slowBefore) ||
          (mTrial.missedBefore >= 2.0f && mTrial.missedAfter / 2.0f <= 0.5f * mTrial.missedBefore);
      mTrial.active = false;
      if (pressure && !helped) {
        mScale = mTrial.scaleBefore;
        mHold = kHoldWindows;
        mCalm = 0;
        mDecision = Decision::Restored;
        return mScale;
      }
    }
    if (pressure && mHold == 0 && mScale > mFloor) {
      const float before = previousDelivered != 0 ? (delivered + previousDelivered) / 2.0f
                                                  : static_cast<float>(delivered);
      const float slowBefore = hadWindow ? (slow + previousSlow) / 2.0f : static_cast<float>(slow);
      const float missedBefore = hadWindow ? (missed + previousMissed) / 2.0f : static_cast<float>(missed);
      mTrial = {true, mScale, before, 0, 0, slowBefore, 0, missedBefore, 0};
      mScale = std::max(mFloor, mScale - mStep);
      mCalm = 0;
      mRaiseHold = kRaiseHoldWindows;
      mDecision = Decision::Lowered;
    } else if (pressure) {
      mCalm = 0;
    } else if (mRaiseHold != 0) {
      mCalm = 0;
    } else if (!cpuBound && busy == 0 && slow == 0 && missed * 50ull <= requests && sample.latePercent < 1 &&
               (!gpuKnown || gpu < 0.85f * budget) &&
               (!cpuKnown || sample.cpuMs < 0.85f * budget)) {
      if (++mCalm >= 5) {
        const float before = mScale;
        mScale = std::min(mCap, mScale + mStep);
        mCalm = 0;
        mDecision = mScale != before ? Decision::Raised : Decision::CalmAtCap;
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
    float deliveredBefore = 0;  // average of the two windows before lowering
    unsigned windows = 0;
    unsigned deliveredAfter = 0; // sum of the two judged windows
    float slowBefore = 0;        // hitches, average of the two windows before
    unsigned slowAfter = 0;      // hitches, sum of the two judged windows
    float missedBefore = 0;      // missed display frames, average of the two windows before
    unsigned missedAfter = 0;    // missed display frames, sum of the two judged windows
  };
  float mScale = 1.0f, mCap = 1.25f, mFloor = 0.65f;
  float mStep = 0.05f;
  bool mHitchesNeedGpu = false;
  uint64_t mSequence = 0;
  unsigned mCalm = 0;
  unsigned mHold = 0;
  unsigned mRaiseHold = 0;
  unsigned mLastDelivered = 0;
  unsigned mLastSlow = 0;
  unsigned mLastMissed = 0;
  bool mHadWindow = false;
  Trial mTrial;
  Decision mDecision = Decision::Kept;
};

// Whether the ring's images must be made again for the eyes' drawn size
// (stereo_view.cpp). New images empty the ring first: the headset shows the
// last image again for a few frames, a hitch of the board, and the game makes
// its render targets again. A smaller size is drawn in a part of the images
// at once (Aurora draws each eye at the left of its half, centered
// vertically), so a lowering the controller undoes costs no new images. The
// images shrink once more than 15% of them has stayed undrawn for
// kShrinkSeconds, whatever small steps the drawn size took meanwhile: the
// GPU clears, resolves and writes out whole images (4x multisampled), and
// with the controller stepping every few seconds, a 20 s steady size was
// seldom reached: the images stayed at 4208x2200 while 70 to 95% of them
// was drawn (2026-10-03). A larger size needs new images at once.
class ImageSizePolicy {
public:
  static constexpr unsigned kShrinkSeconds = 8;

  // Once a second. `changed`: the drawn size changed since the last call.
  bool update(uint32_t drawWidth, uint32_t drawHeight, uint32_t imageWidth, uint32_t imageHeight, bool changed) {
    if (drawWidth > imageWidth || drawHeight > imageHeight) {
      mSmallerSeconds = 0;
      return true;
    }
    const uint64_t drawn = static_cast<uint64_t>(drawWidth) * drawHeight;
    const uint64_t image = static_cast<uint64_t>(imageWidth) * imageHeight;
    (void)changed;
    if (drawn * 100 >= image * 85) {
      mSmallerSeconds = 0;
      return false;
    }
    if (++mSmallerSeconds < kShrinkSeconds) {
      return false;
    }
    mSmallerSeconds = 0;
    return true;
  }
  void reset() { mSmallerSeconds = 0; }

private:
  unsigned mSmallerSeconds = 0;
};

inline float quality_cap(int screenHeight) {
  return screenHeight >= 2160 ? 1.25f : screenHeight >= 1440 ? 1.0f : 0.8f;
}

} // namespace quest
