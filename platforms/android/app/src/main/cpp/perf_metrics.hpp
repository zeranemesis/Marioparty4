#pragma once

// Meta Quest: how well the game keeps up with the headset, for the log file
// (QuestLog.java keeps it in Download/PartyBoard) and the placement panel.
//
// Two sources: the XR frame loop's own pacing (display times the loop skipped
// because a frame came late), and the headset's counters
// (XR_META_performance_metrics: the app's CPU and GPU frame times, the
// compositor's dropped frames, CPU and GPU utilization). Every counter the
// runtime lists is logged, so a later build can steer by whichever proves useful.

#include "xr_util.hpp"
#include "adaptive_quality.hpp"

#include <array>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace quest {

class PerfMetrics {
public:
  static constexpr const char* kExtension = XR_META_PERFORMANCE_METRICS_EXTENSION_NAME;

  // The numbers the placement panel shows (QuestVr.nativePerfNumbers); < 0: unknown.
  enum Number { RefreshRate, Resolution, GpuUtilization, CpuUtilization, LateFrames, AppGpuMs, AppCpuMs,
                EyeWidth, EyeHeight, WorldRate, Count };

  // XR thread, once the session exists; `available`: the extension is enabled.
  void init(XrInstance instance, XrSession session, bool available);
  // XR thread, once per frame after xrEndFrame: the frame's timing, how long
  // the XR thread spent on it, and the eyes' resolution (% of recommended).
  void frame(const XrFrameState& state, std::chrono::nanoseconds threadTime, float resolutionPercent,
             const std::array<float, 3>& renderInfo);
  // Any thread: the last window's summary.
  std::array<float, Count> numbers() const;
  QualitySample quality_sample() const;

private:
  struct Counter {
    XrPath path = XR_NULL_PATH;
    std::string name; // without the "/perfmetrics_meta/" prefix
    XrPerformanceMetricsCounterUnitMETA unit = XR_PERFORMANCE_METRICS_COUNTER_UNIT_GENERIC_META;
    double sum = 0.0, max = 0.0;
    uint32_t samples = 0;
    double average() const { return samples != 0 ? sum / samples : -1.0; }
  };

  void sample_counters();
  void report(double seconds, float resolutionPercent);
  const Counter* find(const char* name) const;

  XrInstance mInstance = XR_NULL_HANDLE;
  XrSession mSession = XR_NULL_HANDLE;
  PFN_xrQueryPerformanceMetricsCounterMETA mQuery = nullptr;
  std::vector<Counter> mCounters;

  std::chrono::steady_clock::time_point mWindowStart{}, mSampledAt{};
  XrTime mLastDisplayTime = 0;
  uint32_t mFrames = 0, mLateFrames = 0, mWorstSkip = 0;
  double mThreadSumMs = 0.0, mThreadMaxMs = 0.0;
  double mRefreshRate = 0.0;
  uint64_t mTotalFrames = 0, mTotalLate = 0;

  mutable std::mutex mNumbersMutex;
  std::array<float, Count> mNumbers{-1, -1, -1, -1, -1, -1, -1};
  QualitySample mQuality;
  uint32_t mQualityFrames = 0, mQualityLate = 0;
  std::array<float, 3> mRenderInfo{};
};

} // namespace quest
