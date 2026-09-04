#include "port/imgui.h"
#include "port/frame_interpolation.h"
#include "port/settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <aurora/gfx.h>
#include <chrono>
#include <cmath>
#include <fmt/format.h>
#include <imgui.h>
#include <numeric>
#include <thread>
#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>

#if _WIN32
#include "Windows.h"
#endif

static bool m_frameRate = true;
static bool m_pipelineInfo = false;
static bool m_graphicsBackend = true;
static int m_debugOverlayCorner = 0; // top-left

using namespace std::string_literals;
using namespace std::string_view_literals;

static void SetOverlayWindowLocation(int corner)
{
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImVec2 workPos = viewport->WorkPos; // Use work area to avoid menu-bar/task-bar, if any!
    ImVec2 workSize = viewport->WorkSize;
    ImVec2 windowPos;
    ImVec2 windowPosPivot;
    constexpr float padding = 10.0f;
    windowPos.x = (corner & 1) != 0 ? (workPos.x + workSize.x - padding) : (workPos.x + padding);
    windowPos.y = (corner & 2) != 0 ? (workPos.y + workSize.y - padding) : (workPos.y + padding);
    windowPosPivot.x = (corner & 1) != 0 ? 1.0f : 0.0f;
    windowPosPivot.y = (corner & 2) != 0 ? 1.0f : 0.0f;
    ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always, windowPosPivot);
}

static void ImGuiStringViewText(std::string_view text)
{
    // begin()/end() do not work on MSVC
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

static std::string BytesToString(size_t bytes)
{
    constexpr std::array suffixes{"B"sv, "KB"sv, "MB"sv, "GB"sv, "TB"sv, "PB"sv, "EB"sv};
    uint32_t s = 0;
    auto count = static_cast<double>(bytes);
    while (count >= 1024.0 && s < 7)
    {
        s++;
        count /= 1024.0;
    }
    if (count - floor(count) == 0.0)
    {
        return fmt::format(FMT_STRING("{}{}"), static_cast<size_t>(count), suffixes[s]);
    }
    return fmt::format(FMT_STRING("{:.1f}{}"), count, suffixes[s]);
}

void imgui_main(const AuroraInfo *info)
{

    ImGuiIO &io = ImGui::GetIO();
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration |
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (m_debugOverlayCorner != -1)
    {
        SetOverlayWindowLocation(m_debugOverlayCorner);
        windowFlags |= ImGuiWindowFlags_NoMove;
    }
    ImGui::SetNextWindowBgAlpha(0.65f);
    if (ImGui::Begin("Debug Overlay", nullptr, windowFlags))
    {
        bool hasPrevious = false;
        if (m_frameRate)
        {
            if (hasPrevious)
            {
                ImGui::Separator();
            }
            hasPrevious = true;

            ImGuiStringViewText(fmt::format(FMT_STRING("FPS: {:.1f}\n"), io.Framerate));
        }
        if (m_graphicsBackend)
        {
            if (hasPrevious)
            {
                ImGui::Separator();
            }
            hasPrevious = true;

            std::string_view backendString = "Unknown"sv;
            switch (info->backend)
            {
            case BACKEND_D3D12:
                backendString = "D3D12"sv;
                break;
            case BACKEND_METAL:
                backendString = "Metal"sv;
                break;
            case BACKEND_VULKAN:
                backendString = "Vulkan"sv;
                break;
            case BACKEND_OPENGL:
                backendString = "OpenGL"sv;
                break;
            case BACKEND_OPENGLES:
                backendString = "OpenGL ES"sv;
                break;
            case BACKEND_WEBGPU:
                backendString = "WebGPU"sv;
                break;
            case BACKEND_NULL:
                backendString = "Null"sv;
                break;
            }
            ImGuiStringViewText(fmt::format(FMT_STRING("Backend: {}\n"), backendString));
        }
        if (m_pipelineInfo)
        {
            if (hasPrevious)
            {
                ImGui::Separator();
            }
            hasPrevious = true;
            auto stats = aurora_get_stats();

            ImGuiStringViewText(
                fmt::format(FMT_STRING("Queued pipelines:  {}\n"), stats->queuedPipelines));
            ImGuiStringViewText(
                fmt::format(FMT_STRING("Created pipelines:    {}\n"), stats->createdPipelines));
            ImGuiStringViewText(
                fmt::format(FMT_STRING("Draw call count:   {}\n"), stats->drawCallCount));
            ImGuiStringViewText(fmt::format(FMT_STRING("Merged draw calls: {}\n"),
                                            stats->mergedDrawCallCount));
            ImGuiStringViewText(fmt::format(FMT_STRING("Vertex size:       {}\n"),
                                            BytesToString(stats->lastVertSize)));
            ImGuiStringViewText(fmt::format(FMT_STRING("Uniform size:      {}\n"),
                                            BytesToString(stats->lastUniformSize)));
            ImGuiStringViewText(fmt::format(FMT_STRING("Index size:        {}\n"),
                                            BytesToString(stats->lastIndexSize)));
            ImGuiStringViewText(fmt::format(FMT_STRING("Storage size:      {}\n"),
                                            BytesToString(stats->lastStorageSize)));
            ImGuiStringViewText(fmt::format(
                FMT_STRING("Total:             {}\n"),
                BytesToString(stats->lastVertSize + stats->lastUniformSize +
                              stats->lastIndexSize + stats->lastStorageSize)));
        }
    }
    ImGui::End();
}

class Limiter
{
    using delta_clock = std::chrono::steady_clock;
    using duration_t = std::chrono::nanoseconds;

  public:
    void Reset()
    {
        m_oldTime = delta_clock::now();
    }

    void Sleep(duration_t targetFrameTime)
    {
        if (targetFrameTime.count() == 0)
        {
            return;
        }

        auto start = delta_clock::now();
        duration_t adjustedSleepTime = SleepTime(targetFrameTime);
        if (adjustedSleepTime.count() > 0)
        {
            NanoSleep(adjustedSleepTime);
            duration_t overslept = TimeSince(start) - adjustedSleepTime;
            if (overslept < duration_t{targetFrameTime})
            {
                m_overheadTimes[m_overheadTimeIdx] = overslept;
                m_overheadTimeIdx = (m_overheadTimeIdx + 1) % m_overheadTimes.size();
            }
        }
        Reset();
    }

    duration_t SleepTime(duration_t targetFrameTime)
    {
        const auto sleepTime = duration_t{targetFrameTime} - TimeSince(m_oldTime);
        m_overhead = std::accumulate(m_overheadTimes.begin(), m_overheadTimes.end(), duration_t{}) /
                     m_overheadTimes.size();
        if (sleepTime > m_overhead)
        {
            return sleepTime - m_overhead;
        }
        return duration_t{0};
    }

  private:
    delta_clock::time_point m_oldTime;
    std::array<duration_t, 4> m_overheadTimes{};
    size_t m_overheadTimeIdx = 0;
    duration_t m_overhead = duration_t{0};

    duration_t TimeSince(delta_clock::time_point start)
    {
        return std::chrono::duration_cast<duration_t>(delta_clock::now() - start);
    }

#if _WIN32
    bool m_initialized = false;
    double m_countPerNs = 0.0;
    size_t m_sleepCount = 0;

    void NanoSleep(const duration_t duration)
    {
        if (!m_initialized || (++m_sleepCount % 1000) == 0)
        {
            LARGE_INTEGER freq;
            if (QueryPerformanceFrequency(&freq) == 0)
            {
                return;
            }
            m_countPerNs = static_cast<double>(freq.QuadPart) / 1000000000.0;
            m_initialized = true;
        }

        DWORD ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        auto tickCount =
            static_cast<LONGLONG>(static_cast<double>(duration.count()) * m_countPerNs);
        LARGE_INTEGER count;
        QueryPerformanceCounter(&count);
        if (ms > 1)
        {
            /* Keep the render and audio workers schedulable, then busy-wait
             * only for the final millisecond. */
            ::Sleep(ms - 1);
        }
        auto end = count.QuadPart + tickCount;
        do
        {
            YieldProcessor();
            QueryPerformanceCounter(&count);
        } while (count.QuadPart < end);
    }
#else
    void NanoSleep(const duration_t duration)
    {
        std::this_thread::sleep_for(duration);
    }
#endif
};

static Limiter g_frameLimiter;

namespace {
constexpr int kOriginalSimulationRate = 60;
constexpr int kMaxSimulationTicksPerFrame = 16;
using FramePacerClock = std::chrono::steady_clock;

FramePacerClock::time_point g_previousFrameSample;
FramePacerClock::time_point g_currentSnapshotTime;
bool g_framePacerInitialized = false;
int g_lastTargetFrameRate = kOriginalSimulationRate;
bool g_interpolationActive = false;

constexpr auto kSimulationPeriod = std::chrono::duration_cast<FramePacerClock::duration>(
    std::chrono::duration<double>(1.0 / static_cast<double>(kOriginalSimulationRate)));
constexpr auto kAbnormalGapResetThreshold = std::chrono::milliseconds(250);

int target_frame_rate()
{
    return std::clamp(partyboard::getSettings().video.targetFrameRate.getValue(),
                      kOriginalSimulationRate, 240);
}
}

void frame_pacer_reset()
{
    const auto now = FramePacerClock::now();
    g_previousFrameSample = now;
    /* Present one fixed step behind real time so previous and current are
     * both known. This is the stable interpolation model used by Dusklight. */
    g_currentSnapshotTime = now - kSimulationPeriod;
    g_framePacerInitialized = true;
    PartyBoard_FrameInterpolationReset();
}

int frame_pacer_simulation_tick()
{
    const int targetFrameRate = target_frame_rate();
    const auto now = FramePacerClock::now();
    if (targetFrameRate <= kOriginalSimulationRate)
    {
        g_previousFrameSample = now;
        g_currentSnapshotTime = now;
        g_framePacerInitialized = true;
        g_lastTargetFrameRate = targetFrameRate;
        g_interpolationActive = false;
        return 1;
    }

    if (!g_framePacerInitialized)
    {
        g_framePacerInitialized = true;
        g_previousFrameSample = now;
        g_currentSnapshotTime = now;
    }
    if (targetFrameRate != g_lastTargetFrameRate)
    {
        frame_pacer_reset();
        g_lastTargetFrameRate = targetFrameRate;
        g_interpolationActive = true;
        return 0;
    }
    g_lastTargetFrameRate = targetFrameRate;
    g_interpolationActive = true;

    const auto frameGap = now - g_previousFrameSample;
    g_previousFrameSample = now;
    if (frameGap > kAbnormalGapResetThreshold)
    {
        // Do not fast-forward after a breakpoint, blocked window, or asset
        // load. Re-seed both transform snapshots before presenting again.
        g_currentSnapshotTime = now - kSimulationPeriod;
        PartyBoard_FrameInterpolationReset();
        return 0;
    }

    int simulationTicks = 0;
    auto projectedSnapshotTime = g_currentSnapshotTime;
    const auto renderTime = now - kSimulationPeriod;
    while (simulationTicks < kMaxSimulationTicksPerFrame &&
           projectedSnapshotTime < renderTime)
    {
        projectedSnapshotTime += kSimulationPeriod;
        ++simulationTicks;
    }
    return simulationTicks;
}

void frame_pacer_commit_simulation_tick()
{
    if (g_interpolationActive)
    {
        g_currentSnapshotTime += kSimulationPeriod;
    }
}

float frame_pacer_interpolation_step()
{
    if (!g_interpolationActive)
    {
        return 1.0f;
    }
    /* Sample immediately before presentation. At 240 FPS, measuring before
     * simulation can make a few milliseconds of CPU work equal most of a
     * display frame and produces visible phase jitter. Dusklight likewise
     * samples the presentation clock after fixed-step simulation. */
    const auto elapsed = FramePacerClock::now() - g_currentSnapshotTime;
    const float interpolationPhase = std::chrono::duration<float>(elapsed).count() /
        std::chrono::duration<float>(kSimulationPeriod).count();
    return std::clamp(interpolationPhase, 0.0f, 1.0f);
}

bool frame_pacer_interpolation_enabled()
{
    return target_frame_rate() > kOriginalSimulationRate;
}

void frame_limiter()
{
    g_frameLimiter.Sleep(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds{1}) /
        target_frame_rate());
}
