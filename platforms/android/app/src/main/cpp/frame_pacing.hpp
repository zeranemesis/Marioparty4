#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace quest {

// Starts the game's frames on the display's schedule (stereo_view.cpp). On
// its own 60 Hz clock the game drifts against the headset's 120 Hz: an image
// finishing near the moment the XR thread looks for a finished one is shown
// for one display frame, the next for three, and the eye poses, predicted for
// the average delay, are a display frame off for both. The board judders,
// most when the head moves. Here each frame starts a fixed time after one of
// those looks, chosen so that nearly every image (98%) finishes kMarginNs
// before the look k periods later, its due look, and no image is shown
// before it: every image is shown for the same number of display frames,
// with the shortest delay that allows. Paced but shown as soon as finished,
// 11% of the board's images stayed one or three display frames instead of
// two: the GPU's time varies more than a period (Quest 3, build 112).
//
// Not synchronized: the caller holds its lock. Times are steady-clock (Android:
// CLOCK_MONOTONIC) nanoseconds.
class FramePacer {
public:
  static constexpr int64_t kMarginNs = 1'500'000;
  // No look for this long: the game keeps its own clock (flat screen, pause).
  static constexpr int64_t kStaleNs = 100'000'000;
  // A start missed by less than this is taken at once, not a period later.
  static constexpr int64_t kLateNs = 1'000'000;

  // XR thread, each look for a finished image.
  void look(int64_t atNs, int64_t periodNs) {
    if (periodNs <= 0) return;
    if (periodNs != mPeriodNs || mGridNs == 0 || atNs - mLastLookNs > kStaleNs) {
      mGridNs = atNs;
    } else {
      // Locked on the looks, an eighth of each one's scheduling jitter.
      const int64_t predicted = mGridNs + round_div(atNs - mGridNs, periodNs) * periodNs;
      mGridNs = predicted + (atNs - predicted) / 8;
    }
    mPeriodNs = periodNs;
    mLastLookNs = atNs;
  }

  // An image shown: from its frame's start to the end of its GPU work.
  void finished(int64_t workNs) {
    if (workNs > 0 && workNs < 200'000'000 && mWork.size() < 512) mWork.push_back(workNs);
  }

  // Once a second: the start's place from the images finished since.
  void update() {
    if (mPeriodNs > 0 && mWork.size() >= 20) {
      const size_t index = mWork.size() * 98 / 100;
      std::nth_element(mWork.begin(), mWork.begin() + static_cast<std::ptrdiff_t>(index), mWork.end());
      const int64_t work = mWork[index];
      const int64_t looks = ceil_div(work + kMarginNs, mPeriodNs);
      const int64_t phase = looks * mPeriodNs - work - kMarginNs;
      // Each move shifts one start: only for a clear change.
      if (looks != mLooks || std::llabs(phase - mPhaseNs) > 750'000) {
        mPhaseNs = phase;
        mLooks = looks;
      }
      mWorkNs = work;
    }
    mWork.clear();
  }

  // Game thread, once its frame is done: when the next one starts, for
  // `targetHz` images a second, and the look its image is for (`dueNs`, 0
  // before the GPU's time is known). 0: not paced (no recent look, or a rate
  // that is not the display's divided by a whole number).
  int64_t next_start(int64_t nowNs, float targetHz, int64_t* dueNs = nullptr) {
    if (dueNs != nullptr) *dueNs = 0;
    if (mPeriodNs <= 0 || mGridNs == 0 || nowNs - mLastLookNs > kStaleNs || !(targetHz > 0.0f)) return 0;
    const double displayHz = 1e9 / static_cast<double>(mPeriodNs);
    const int64_t every = std::max<int64_t>(1, std::llround(displayHz / targetHz));
    if (std::abs(displayHz / static_cast<double>(every) - targetHz) > 1.5) return 0;
    int64_t earliest = nowNs - kLateNs;
    // One frame every `every` periods, not two in a row after a late one.
    if (mLastStartNs > 0 && nowNs - mLastStartNs < 4 * every * mPeriodNs) {
      earliest = std::max(earliest, mLastStartNs + every * mPeriodNs - mPeriodNs / 2);
    }
    const int64_t base = mGridNs + mPhaseNs;
    const int64_t scheduled = base + ceil_div(earliest - base, mPeriodNs) * mPeriodNs;
    const int64_t start = std::max(nowNs, scheduled);
    mLastStartNs = start;
    if (dueNs != nullptr && mLooks > 0) *dueNs = scheduled - mPhaseNs + mLooks * mPeriodNs;
    return start;
  }

  // XR thread: whether an image due at `dueNs` may be shown at this look.
  // Earlier, it waits (it finished early: shown now, the one before it would
  // stay a display frame only); a due time far off is not trusted.
  bool is_due(int64_t dueNs, int64_t lookNs) const {
    return dueNs == 0 || mPeriodNs <= 0 || lookNs >= dueNs - mPeriodNs / 4 || dueNs - lookNs > 4 * mPeriodNs;
  }

  int64_t work_ns() const { return mWorkNs; }
  int64_t phase_ns() const { return mPhaseNs; }
  int64_t looks() const { return mLooks; }

private:
  static int64_t floor_div(int64_t a, int64_t b) {
    const int64_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
  }
  static int64_t ceil_div(int64_t a, int64_t b) { return -floor_div(-a, b); }
  static int64_t round_div(int64_t a, int64_t b) { return floor_div(a + b / 2, b); }

  int64_t mPeriodNs = 0;
  int64_t mGridNs = 0;     // a look, smoothed
  int64_t mLastLookNs = 0;
  int64_t mLastStartNs = 0;
  int64_t mPhaseNs = 0;    // start after a look
  int64_t mLooks = 0;      // periods from the start to the look that finds the image
  int64_t mWorkNs = 0;     // the last 95th percentile
  std::vector<int64_t> mWork;
};

} // namespace quest
