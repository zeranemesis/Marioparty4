// Translation of GameCube RAM addresses to the port's variables.
//
// A RetroAchievements set reads the console's memory: "the byte at 0x8018FC3B"
// rather than "player 1's character". The port keeps those same variables, but
// wherever the host placed them and in the host's byte order. This file answers
// the set's reads by rebuilding, for each variable the set uses, the bytes the
// console held at its address (config/GMPE01_00/symbols.txt), big-endian.
//
// The structures involved need no field-by-field layout work: under TARGET_PC
// the game declares their bitfields in reverse order precisely so that each
// container's integer value matches the console (include/game/gamework_data.h),
// and they hold no pointers. Copying one and byte-swapping its multi-byte
// fields -- what the save-data code already does -- gives the console image.
// The static_asserts below keep that true: if a structure's size stops matching
// the console, this fails to build instead of serving shifted data.
//
// Any address outside these regions is reported as unreadable, and rcheevos
// disables the achievements that use it rather than evaluating them against
// made-up data. Those addresses are logged, so the log lists what is left.

#include "port/retroachievements_memory.hpp"

extern "C" {
#include "game/gamework_data.h"
#include "game/hu3d.h"
#include "game/object.h"
#include "port/byteswap.h"

const omOvlHisData* PartyBoard_RAOverlayHistory(int* count);
s32 PartyBoard_RAReadStatDirId(s32 slot);
}

#include <aurora/lib/logging.hpp>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace partyboard::ra {

namespace {

aurora::Module Log("partyboard::ra");

// Console sizes, from config/GMPE01_00/symbols.txt. The set is registered for
// the USA Rev 1 disc (GMPE01_01), where every address used below is the same
// as in Rev 0.
static_assert(sizeof(PlayerConfig) * 4 == 0x28, "GWPlayerCfg no longer matches the console layout");
static_assert(sizeof(PlayerState) * 4 == 0xC0, "GWPlayer no longer matches the console layout");
static_assert(sizeof(SystemState) == 0xDC, "GWSystem no longer matches the console layout");
static_assert(sizeof(GameStat) == 0x118, "GWGameStat no longer matches the console layout");
static_assert(sizeof(omOvlHisData) == 0xC, "omovlhis no longer matches the console layout");
// Hu3DData is served only up to linkMdlId, the last field before the first
// member whose size differs on a 64-bit host (mallocNo).
static_assert(offsetof(HU3DMODEL, layerNo) == 0x06 && offsetof(HU3DMODEL, motIdSrc) == 0x20 && offsetof(HU3DMODEL, cameraBit) == 0x22
        && offsetof(HU3DMODEL, linkMdlId) == 0x24,
    "HU3DMODEL's leading fields no longer match the console layout");

constexpr uint32_t kRamBase = 0x80000000u;
constexpr uint32_t kHu3DModelConsoleSize = 0x124;
constexpr uint32_t kHu3DServedPrefix = 0x26;

uint16_t be16(uint16_t v) { return static_cast<uint16_t>((v << 8) | (v >> 8)); }
uint32_t be32(uint32_t v) {
    return (v << 24) | ((v << 8) & 0x00FF0000u) | ((v >> 8) & 0x0000FF00u) | (v >> 24);
}

// One variable, served as the console laid it out. The image is rebuilt at most
// once per game tick; rcheevos reads a few hundred bytes of it each tick.
struct Region {
    uint32_t start;  // console address
    uint32_t size;
    void (*build)(std::vector<uint8_t>& out);
    std::vector<uint8_t> image;
    uint64_t builtAt = ~0ull;
};

void build_player_cfg(std::vector<uint8_t>& out) {
    PlayerConfig copy[4];
    std::memcpy(copy, GWPlayerCfg, sizeof(copy));
    for (auto& cfg : copy) {
        byteswap_s16(&cfg.character);
        byteswap_s16(&cfg.pad_idx);
        byteswap_s16(&cfg.diff);
        byteswap_s16(&cfg.group);
        byteswap_s16(&cfg.iscom);
    }
    std::memcpy(out.data(), copy, sizeof(copy));
}

void build_player(std::vector<uint8_t>& out) {
    PlayerState copy[4];
    std::memcpy(copy, GWPlayer, sizeof(copy));
    for (auto& player : copy) {
        byteswap_playerstate(&player);
    }
    std::memcpy(out.data(), copy, sizeof(copy));
}

void build_system(std::vector<uint8_t>& out) {
    SystemState copy;
    std::memcpy(&copy, &GWSystem, sizeof(copy));
    byteswap_systemstate(&copy);
    std::memcpy(out.data(), &copy, sizeof(copy));
}

void build_game_stat(std::vector<uint8_t>& out) {
    GameStat copy;
    std::memcpy(&copy, &GWGameStat, sizeof(copy));
    byteswap_gamestat(&copy);
    std::memcpy(out.data(), &copy, sizeof(copy));
}

void build_overlay_history(std::vector<uint8_t>& out) {
    int count = 0;
    const omOvlHisData* history = PartyBoard_RAOverlayHistory(&count);
    for (int i = 0; i < count && (i + 1) * 12 <= static_cast<int>(out.size()); ++i) {
        const uint32_t fields[3] = {be32(static_cast<uint32_t>(history[i].overlay)),
            be32(static_cast<uint32_t>(history[i].event)), be32(static_cast<uint32_t>(history[i].stat))};
        std::memcpy(out.data() + i * 12, fields, sizeof(fields));
    }
}

void build_read_stat(std::vector<uint8_t>& out) {
    const uint32_t dirId = be32(static_cast<uint32_t>(PartyBoard_RAReadStatDirId(0)));
    std::memcpy(out.data(), &dirId, sizeof(dirId));
}

Region s_regions[] = {
    // ReadDataStat[0].dirId only: the rest of the entry holds pointers.
    {0x80142840u, 4, build_read_stat},
    {0x8018FB38u, 0xC0, build_overlay_history},
    {0x8018FC10u, 0x28, build_player_cfg},
    {0x8018FC38u, 0xC0, build_player},
    {0x8018FCF8u, 0xDC, build_system},
    {0x8018FDD8u, 0x118, build_game_stat},
};

// Hu3DData is 512 models of 0x124 bytes on the console; only the leading fields
// of each are served, built on demand for the one model being read.
constexpr uint32_t kHu3DDataStart = 0x801677C0u;

bool read_hu3d_byte(uint32_t address, uint8_t& out) {
    const uint32_t offset = address - kHu3DDataStart;
    const uint32_t index = offset / kHu3DModelConsoleSize;
    const uint32_t within = offset % kHu3DModelConsoleSize;
    if (index >= HU3D_MODEL_MAX || within >= kHu3DServedPrefix) {
        return false;
    }
    const HU3DMODEL& model = Hu3DData[index];
    // Five single bytes, one byte of padding, then 16-bit fields up to 0x26.
    if (within < 5) {
        out = reinterpret_cast<const uint8_t*>(&model)[within];
        return true;
    }
    if (within == 5) {
        out = 0;
        return true;
    }
    uint16_t field;
    std::memcpy(&field, reinterpret_cast<const uint8_t*>(&model) + (within & ~1u), sizeof(field));
    field = be16(field);
    out = reinterpret_cast<const uint8_t*>(&field)[within & 1u];
    return true;
}

std::mutex s_mutex;
uint64_t s_tick = 0;
std::set<uint32_t> s_unmapped;

bool read_byte(uint32_t address, uint8_t& out) {
    for (Region& region : s_regions) {
        if (address >= region.start && address < region.start + region.size) {
            if (region.builtAt != s_tick) {
                region.image.assign(region.size, 0);
                region.build(region.image);
                region.builtAt = s_tick;
            }
            out = region.image[address - region.start];
            return true;
        }
    }
    if (address >= kHu3DDataStart && address < kHu3DDataStart + HU3D_MODEL_MAX * kHu3DModelConsoleSize) {
        return read_hu3d_byte(address, out);
    }
    return false;
}

} // namespace

void beginTick() {
    std::lock_guard lock(s_mutex);
    ++s_tick;
}

uint32_t readMemory(uint32_t address, uint8_t* buffer, uint32_t numBytes) {
    std::lock_guard lock(s_mutex);
    for (uint32_t i = 0; i < numBytes; ++i) {
        if (!read_byte(kRamBase + address + i, buffer[i])) {
            s_unmapped.insert(address + i);
            return i;
        }
    }
    return numBytes;
}

void logUnmappedAddresses() {
    std::lock_guard lock(s_mutex);
    if (s_unmapped.empty()) {
        Log.info("Every GameCube address the set reads is translated");
        return;
    }
    // Mario Party 4's set leaves two here, 0x80425523 and 0x80425527: the beach
    // volleyball scores, read from that minigame's dynamically loaded memory
    // and used only in the rich presence text. No achievement or leaderboard
    // depends on them.
    Log.warn("{} GameCube address(es) read by the set are not translated; achievements and "
             "leaderboards using them are disabled, rich presence shows them as 0:",
        s_unmapped.size());
    std::string line;
    for (const uint32_t address : s_unmapped) {
        char text[16];
        std::snprintf(text, sizeof(text), " %08X", kRamBase + address);
        line += text;
        if (line.size() > 100) {
            Log.warn("{}", line);
            line.clear();
        }
    }
    if (!line.empty()) {
        Log.warn("{}", line);
    }
}

} // namespace partyboard::ra
