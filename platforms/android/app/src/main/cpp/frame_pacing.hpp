#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <utility>
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
// CLOCK_MONOTONIC) nanoseconds, but display times, OpenXR's (only their
// distance from the steady clock's is used).
class FramePacer {
public:
  static constexpr int64_t kMarginNs = 1'500'000;
  // No look for this long: the game keeps its own clock (flat screen, pause).
  static constexpr int64_t kStaleNs = 100'000'000;
  // A start missed by less than this is taken at once, not a period later.
  static constexpr int64_t kLateNs = 1'000'000;

  // XR thread, each look for a finished image, for the display frame shown
  // at `displayNs` (OpenXR's predicted display time). The looks themselves
  // are not on a grid: with the board on the Quest 3 they came from 6.8 ms
  // before to 6.9 ms after where a grid locked on them put them, and 29 of
  // 30 missed images a second had finished before their due look, which had
  // come early, or late enough to be taken for the next one (2026-10-04,
  // 72 Hz). The display times are exact: an image is due for a display
  // frame, and must be finished by the earliest its look comes, `lead`
  // before that frame's display time (the looks' 98th percentile).
  void look(int64_t atNs, int64_t displayNs, int64_t periodNs) {
    if (periodNs <= 0) return;
    const int64_t lead = displayNs - atNs;
    if (periodNs != mPeriodNs || mGridNs == 0 || atNs - mLastLookNs > kStaleNs) {
      mLeadNs = lead;
      mLeads.clear();
    }
    if (mLeads.size() < 512) mLeads.push_back(lead);
    mJitterNs = mLeadNs - lead;
    mGridNs = displayNs - mLeadNs;
    mPeriodNs = periodNs;
    mLastLookNs = atNs;
  }

  // An image shown: from its frame's start to the end of its GPU work.
  // Over the plan's budget twice within kRecent images (the scene got
  // heavier), the plan grows at once to the longer of the two instead of at
  // the next update(): from a light view to the whole board, 34 images
  // missed their display frame in 2 s before the plan caught up (Toad board,
  // build 194, 2026-10-04). It shrinks back only at update().
  static constexpr size_t kRecent = 8;
  void finished(int64_t workNs) {
    if (!(workNs > 0 && workNs < 200'000'000)) return;
    if (mWork.size() < 512) mWork.push_back(workNs);
    if (mLooks <= 0 || mPeriodNs <= 0) return;
    const int64_t budget = mLooks * mPeriodNs - mPhaseNs - kMarginNs;
    mRecentOver[mRecentIndex] = workNs > budget ? workNs : 0;
    mRecentIndex = (mRecentIndex + 1) % kRecent;
    int64_t over = 0, longest = 0;
    for (const int64_t w : mRecentOver) {
      over += w != 0;
      longest = std::max(longest, w);
    }
    if (over >= 2) {
      plan(longest);
      mRecentOver.fill(0);
      ++mReplans;
    }
  }
  // Plans grown at once since the last call (the "Pacing" log).
  uint32_t take_replans() { return std::exchange(mReplans, 0u); }

  // Once a second: the start's place from the images finished since.
  void update() {
    if (mPeriodNs > 0 && mWork.size() >= 20) {
      const size_t index = mWork.size() * 98 / 100;
      std::nth_element(mWork.begin(), mWork.begin() + static_cast<std::ptrdiff_t>(index), mWork.end());
      plan(mWork[index]);
    }
    mWork.clear();
    if (mLeads.size() >= 20) {
      const size_t index = mLeads.size() * 98 / 100;
      std::nth_element(mLeads.begin(), mLeads.begin() + static_cast<std::ptrdiff_t>(index), mLeads.end());
      if (std::llabs(mLeads[index] - mLeadNs) > 750'000) mLeadNs = mLeads[index];
    }
    mLeads.clear();
  }

  // Game thread, once its frame is done: when the next one starts, for
  // `targetHz` images a second, and the display frame its image is for
  // (`dueNs`, a display time, 0 before the GPU's time is known). 0: not paced (no recent look, or a rate
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
    if (dueNs != nullptr && mLooks > 0) *dueNs = scheduled - mPhaseNs + mLooks * mPeriodNs + mLeadNs;
    return start;
  }

  // XR thread: whether an image due for the display frame at `dueNs` may be
  // shown in the one at `displayNs`. Earlier, it waits (it finished early:
  // shown now, the one before it would stay a display frame only); a due
  // time far off is not trusted.
  bool is_due(int64_t dueNs, int64_t displayNs) const {
    return dueNs == 0 || mPeriodNs <= 0 || displayNs >= dueNs - mPeriodNs / 4 || dueNs - displayNs > 4 * mPeriodNs;
  }

  int64_t work_ns() const { return mWorkNs; }
  int64_t phase_ns() const { return mPhaseNs; }
  int64_t looks() const { return mLooks; }
  // The last look's distance after the earliest planned for (negative: earlier).
  int64_t jitter_ns() const { return mJitterNs; }
  // From the earliest look to its display frame's display time.
  int64_t lead_ns() const { return mLeadNs; }

private:
  // The start's place for `work` from start to GPU finish.
  void plan(int64_t work) {
    const int64_t looks = ceil_div(work + kMarginNs, mPeriodNs);
    const int64_t phase = looks * mPeriodNs - work - kMarginNs;
    // Each move shifts one start: only for a clear change.
    if (looks != mLooks || std::llabs(phase - mPhaseNs) > 750'000) {
      mPhaseNs = phase;
      mLooks = looks;
    }
    mWorkNs = work;
  }
  static int64_t floor_div(int64_t a, int64_t b) {
    const int64_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
  }
  static int64_t ceil_div(int64_t a, int64_t b) { return -floor_div(-a, b); }
  static int64_t round_div(int64_t a, int64_t b) { return floor_div(a + b / 2, b); }

  int64_t mPeriodNs = 0;
  int64_t mGridNs = 0;     // the latest display frame's earliest look
  int64_t mLeadNs = 0;
  int64_t mJitterNs = 0;
  std::vector<int64_t> mLeads; // this second's, from each look to its display time
  std::array<int64_t, kRecent> mRecentOver{}; // the last images' work when over budget, else 0
  size_t mRecentIndex = 0;
  uint32_t mReplans = 0;
  int64_t mLastLookNs = 0;
  int64_t mLastStartNs = 0;
  int64_t mPhaseNs = 0;    // start after a look
  int64_t mLooks = 0;      // periods from the start to the look that finds the image
  int64_t mWorkNs = 0;     // the last 95th percentile
  std::vector<int64_t> mWork;
};

} // namespace quest
