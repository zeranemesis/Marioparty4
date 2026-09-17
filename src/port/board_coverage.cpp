/* Which board mechanics a run actually exercised. See include/port/board_coverage.h. */

#include "port/board_coverage.h"
#include "port/crash_report.h"

#include <cstdio>
#include <cstring>
#include <mutex>

namespace
{

// Small and fixed. The list of board mechanics is the contents of
// src/game/board/, which changes about once a year; a growable structure here
// would be machinery in exchange for nothing.
constexpr unsigned kMaxSubsystems = 32;
constexpr unsigned kNameLength = 32;

struct Seen
{
    char name[kNameLength];
    unsigned firstFrame;
    unsigned hits;
};

std::mutex gMutex;
Seen gSeen[kMaxSubsystems];
unsigned gCount = 0;

} // namespace

extern "C" void PartyBoard_BoardCoverageMark(const char *subsystem)
{
    if (!subsystem || !subsystem[0]) return;

    const unsigned frame = PartyBoard_CrashSimulationFrame();

    std::lock_guard<std::mutex> guard(gMutex);
    for (unsigned i = 0; i < gCount; ++i) {
        if (std::strncmp(gSeen[i].name, subsystem, kNameLength) == 0) {
            ++gSeen[i].hits;
            return;
        }
    }
    if (gCount >= kMaxSubsystems) return;

    Seen &entry = gSeen[gCount++];
    std::snprintf(entry.name, sizeof(entry.name), "%s", subsystem);
    entry.firstFrame = frame;
    entry.hits = 1;

    // Both destinations on purpose. stdout is what the campaign and the session
    // recorder already capture and parse; the breadcrumb is what a crash report
    // carries, so a session that dies still says which mechanics it had reached
    // by then - which is exactly the question asked of a crash on turn 14.
    std::printf("COVERAGE> %s first reached at frame %u\n", subsystem, frame);
    std::fflush(stdout);
    PartyBoard_CrashBreadcrumb(PARTYBOARD_CRASH_CAT_BOARD, "coverage %s first reached", subsystem);
}

extern "C" int PartyBoard_BoardCoverageSummary(char *out, unsigned capacity)
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (out && capacity > 0) {
        out[0] = '\0';
        unsigned used = 0;
        for (unsigned i = 0; i < gCount; ++i) {
            const int written = std::snprintf(out + used, capacity - used, "%s%s@%u",
                used ? " " : "", gSeen[i].name, gSeen[i].firstFrame);
            if (written < 0 || static_cast<unsigned>(written) >= capacity - used) break;
            used += static_cast<unsigned>(written);
        }
    }
    return static_cast<int>(gCount);
}
