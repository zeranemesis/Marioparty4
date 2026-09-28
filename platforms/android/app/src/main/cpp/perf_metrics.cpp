#include "perf_metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace quest {

namespace {

constexpr auto kSampleEvery = std::chrono::seconds(1);
constexpr auto kReportEvery = std::chrono::seconds(5);
constexpr const char* kPrefix = "/perfmetrics_meta/";

const char* unit_suffix(XrPerformanceMetricsCounterUnitMETA unit) {
  switch (unit) {
  case XR_PERFORMANCE_METRICS_COUNTER_UNIT_PERCENTAGE_META:
    return "%";
  case XR_PERFORMANCE_METRICS_COUNTER_UNIT_MILLISECONDS_META:
    return "ms";
  case XR_PERFORMANCE_METRICS_COUNTER_UNIT_BYTES_META:
    return "B";
  case XR_PERFORMANCE_METRICS_COUNTER_UNIT_HERTZ_META:
    return "Hz";
  default:
    return "";
  }
}

// The game's resident memory and what the system still has, with the report:
// a climb across scenes, or a transition's peak, shows here before the
// low-memory killer acts. statm and meminfo are cheap to read, unlike smaps
// (a walk of every mapping), so the XR thread can afford them.
void log_memory() {
  long residentPages = -1;
  if (FILE* file = std::fopen("/proc/self/statm", "r")) {
    long sizePages = 0;
    if (std::fscanf(file, "%ld %ld", &sizePages, &residentPages) != 2) residentPages = -1;
    std::fclose(file);
  }
  long availableKb = -1;
  if (FILE* file = std::fopen("/proc/meminfo", "r")) {
    char line[128];
    while (std::fgets(line, sizeof(line), file)) {
      if (std::sscanf(line, "MemAvailable: %ld kB", &availableKb) == 1) break;
    }
    std::fclose(file);
  }
  const long pageKb = sysconf(_SC_PAGESIZE) / 1024;
  LOGI("Memory: rss=%ldMB available=%ldMB", residentPages >= 0 ? residentPages * pageKb / 1024 : -1,
       availableKb >= 0 ? availableKb / 1024 : -1);
}

} // namespace

void PerfMetrics::init(XrInstance instance, XrSession session, bool available) {
  mCounters.clear();
  mQuery = nullptr;
  mFrames = mLateFrames = mWorstSkip = mQualityFrames = mQualityLate = 0;
  mTotalFrames = mTotalLate = 0;
  mLastDisplayTime = 0;
  mThreadSumMs = mThreadMaxMs = mRefreshRate = 0;
  {
    std::lock_guard lock{mNumbersMutex};
    mQuality = {};
    mNumbers.fill(-1);
  }
  mInstance = instance;
  mSession = session;
  mWindowStart = mSampledAt = std::chrono::steady_clock::now();
  if (!available) {
    LOGI("Perf: headset counters unavailable, frame pacing only");
    return;
  }
  auto enumerate = proc<PFN_xrEnumeratePerformanceMetricsCounterPathsMETA>(
      instance, "xrEnumeratePerformanceMetricsCounterPathsMETA");
  auto setState = proc<PFN_xrSetPerformanceMetricsStateMETA>(instance, "xrSetPerformanceMetricsStateMETA");
  mQuery = proc<PFN_xrQueryPerformanceMetricsCounterMETA>(instance, "xrQueryPerformanceMetricsCounterMETA");
  uint32_t count = 0;
  if (enumerate == nullptr || setState == nullptr || mQuery == nullptr ||
      !check(instance, enumerate(instance, 0, &count, nullptr), "Perf counter paths")) {
    mQuery = nullptr;
    return;
  }
  std::vector<XrPath> paths(count);
  enumerate(instance, count, &count, paths.data());
  XrPerformanceMetricsStateMETA state{XR_TYPE_PERFORMANCE_METRICS_STATE_META};
  state.enabled = XR_TRUE;
  if (!check(instance, setState(session, &state), "Perf counters")) {
    mQuery = nullptr;
    return;
  }
  std::string names;
  for (XrPath path : paths) {
    char text[XR_MAX_PATH_LENGTH] = {};
    uint32_t length = 0;
    if (XR_FAILED(xrPathToString(instance, path, sizeof(text), &length, text))) {
      continue;
    }
    std::string name{text};
    if (name.rfind(kPrefix, 0) == 0) {
      name.erase(0, std::strlen(kPrefix));
    }
    names += (names.empty() ? "" : ", ") + name;
    mCounters.push_back({.path = path, .name = std::move(name)});
  }
  LOGI("Perf: %zu headset counters: %s", mCounters.size(), names.c_str());
}

void PerfMetrics::sample_counters() {
  std::lock_guard lock{mNumbersMutex};
  mQuality.gpuMs = mQuality.cpuMs = mQuality.compositorGpuMs = -1;
  if (mQuery == nullptr) {
    return;
  }
  for (auto& counter : mCounters) {
    XrPerformanceMetricsCounterMETA value{XR_TYPE_PERFORMANCE_METRICS_COUNTER_META};
    if (XR_FAILED(mQuery(mSession, counter.path, &value)) ||
        !(value.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_ANY_VALUE_VALID_BIT_META)) {
      continue;
    }
    const double number = (value.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META)
                              ? static_cast<double>(value.floatValue)
                              : static_cast<double>(value.uintValue);
    counter.unit = value.counterUnit;
    counter.sum += number;
    counter.max = counter.samples == 0 ? number : std::max(counter.max, number);
    ++counter.samples;
    if (value.counterUnit == XR_PERFORMANCE_METRICS_COUNTER_UNIT_MILLISECONDS_META && std::isfinite(number)) {
      if (counter.name == "app/gpu_frametime") mQuality.gpuMs = static_cast<float>(number);
      if (counter.name == "app/cpu_frametime") mQuality.cpuMs = static_cast<float>(number);
      if (counter.name == "compositor/gpu_frametime") mQuality.compositorGpuMs = static_cast<float>(number);
    }
  }
}

const PerfMetrics::Counter* PerfMetrics::find(const char* name) const {
  const auto it = std::ranges::find_if(mCounters, [name](const Counter& c) { return c.name == name; });
  return it != mCounters.end() && it->samples != 0 ? &*it : nullptr;
}

void PerfMetrics::frame(const XrFrameState& state, std::chrono::nanoseconds threadTime, float resolutionPercent,
                        const std::array<float, 3>& renderInfo) {
  mRenderInfo = renderInfo;
  const auto now = std::chrono::steady_clock::now();
  // Display times the loop skipped: its frame came after the next vsync.
  if (mLastDisplayTime != 0 && state.predictedDisplayPeriod > 0) {
    const XrTime delta = state.predictedDisplayTime - mLastDisplayTime;
    if (delta > 0 && delta < 1'000'000'000) { // longer: paused (menu, headset off)
      const auto skipped = static_cast<uint32_t>(
          std::max<long long>(0, std::llround(static_cast<double>(delta) / state.predictedDisplayPeriod) - 1));
      mLateFrames += skipped;
      mQualityLate += skipped;
      mWorstSkip = std::max(mWorstSkip, skipped);
    }
  }
  if (state.predictedDisplayPeriod > 0) {
    mRefreshRate = 1e9 / static_cast<double>(state.predictedDisplayPeriod);
  }
  mLastDisplayTime = state.predictedDisplayTime;
  ++mFrames;
  ++mQualityFrames;
  const double threadMs = std::chrono::duration<double, std::milli>(threadTime).count();
  mThreadSumMs += threadMs;
  mThreadMaxMs = std::max(mThreadMaxMs, threadMs);

  if (now - mSampledAt >= kSampleEvery) {
    mSampledAt = now;
    sample_counters();
    std::lock_guard lock{mNumbersMutex};
    mQuality.refreshHz = static_cast<float>(mRefreshRate);
    const uint32_t attempts = mQualityFrames + mQualityLate;
    mQuality.latePercent = attempts ? 100.0f * mQualityLate / attempts : 0.0f;
    ++mQuality.sequence;
    mQualityFrames = mQualityLate = 0;
  }
  const double seconds = std::chrono::duration<double>(now - mWindowStart).count();
  if (now - mWindowStart >= kReportEvery) {
    report(seconds, resolutionPercent);
    mWindowStart = now;
  }
}

void PerfMetrics::report(double seconds, float resolutionPercent) {
  const uint32_t shown = mFrames + mLateFrames;
  const double latePercent = shown != 0 ? 100.0 * mLateFrames / shown : 0.0;
  mTotalFrames += shown;
  mTotalLate += mLateFrames;
  LOGI("Perf: %.0fHz display, xr=%.1fHz late=%u (%.1f%%, worst skip %u, session %.2f%%) xrThread avg=%.2fms "
       "max=%.2fms res=%.0f%%",
       mRefreshRate, mFrames / seconds, mLateFrames, latePercent, mWorstSkip,
       mTotalFrames != 0 ? 100.0 * mTotalLate / mTotalFrames : 0.0, mFrames != 0 ? mThreadSumMs / mFrames : 0.0,
       mThreadMaxMs, resolutionPercent);
  log_memory();
  if (!mCounters.empty()) {
    std::string line;
    char item[160];
    for (const auto& counter : mCounters) {
      if (counter.samples == 0) {
        continue;
      }
      std::snprintf(item, sizeof(item), "%s%s=%.4g%s(max %.4g)", line.empty() ? "" : " ", counter.name.c_str(),
                    counter.average(), unit_suffix(counter.unit), counter.max);
      line += item;
    }
    LOGI("Perf counters: %s", line.c_str());
  }

  std::array<float, Count> numbers{};
  const auto average = [this](const char* name) {
    const Counter* counter = find(name);
    return counter != nullptr ? static_cast<float>(counter->average()) : -1.0f;
  };
  numbers[RefreshRate] = static_cast<float>(mRefreshRate);
  numbers[Resolution] = resolutionPercent;
  numbers[GpuUtilization] = average("device/gpu_utilization");
  numbers[CpuUtilization] = average("device/cpu_utilization_average");
  numbers[LateFrames] = static_cast<float>(latePercent);
  numbers[AppGpuMs] = average("app/gpu_frametime");
  numbers[AppCpuMs] = average("app/cpu_frametime");
  numbers[EyeWidth] = mRenderInfo[0];
  numbers[EyeHeight] = mRenderInfo[1];
  numbers[WorldRate] = mRenderInfo[2];
  {
    std::lock_guard lock{mNumbersMutex};
    mNumbers = numbers;
  }

  for (auto& counter : mCounters) {
    counter.sum = counter.max = 0.0;
    counter.samples = 0;
  }
  mFrames = mLateFrames = mWorstSkip = 0;
  mThreadSumMs = mThreadMaxMs = 0.0;
}

std::array<float, PerfMetrics::Count> PerfMetrics::numbers() const {
  std::lock_guard lock{mNumbersMutex};
  return mNumbers;
}

QualitySample PerfMetrics::quality_sample() const {
  std::lock_guard lock{mNumbersMutex};
  return mQuality;
}

} // namespace quest
