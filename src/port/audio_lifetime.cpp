#include "port/audio_lifetime.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "port/crash_report.h"

// Sample-bank lifetime instrumentation. See include/port/audio_lifetime.h for
// the question this answers and why it is asked this way.
//
// What the addresses are. On PC a group push copies the sample blob once
// (sndPushGroup), and then hwSaveSample copies EVERY SAMPLE AGAIN into an
// emulated ARAM block of its own and rewrites sdir->addr to point at that
// second copy. dataGetSample hands that pointer straight to a voice, so
// vp->smp_info.addr is exactly one of those per-sample allocations, never an
// interior pointer into the staging blob. Matching a voice against a bank
// therefore has to be done per sample, which is what the table below holds. A
// first attempt matched voices against the staging blob instead and classified
// 7724 voices out of 7724 as belonging to no bank at all, which is how the
// mistake was found.
//
// Threading. The game thread stores and frees samples; the audio thread starts,
// renders and stops voices. The table is written only by the game thread and
// read by both, so its fields are atomics and the audio thread never takes a
// lock to consult it. The event log is written under its own mutex, which is
// always taken last: the audio thread already holds the MusyX global mutex when
// it logs, and the game thread takes that same mutex (inside
// PartyBoard_AudioWalkVoices) before logging at a free. The order is global
// mutex -> log mutex on both sides, and never the reverse.

namespace {

constexpr u32 kMaxVoices = 64;       // SYNTH_MAX_VOICES
constexpr u32 kSampleSlots = 16384;  // power of two; open addressing on the address
constexpr u32 kMaxLoggedEvents = 2000000;

enum SlotState : int { kSlotEmpty = 0, kSlotLive = 1, kSlotRetired = 2 };

// One emulated-ARAM sample allocation. Retired entries keep their address so a
// read after the free can still be named; the slot is only reused when the same
// address comes back from the allocator, which is exactly a new generation.
struct SampleRecord {
    std::atomic<uintptr_t> addr { 0 };
    std::atomic<size_t> length { 0 };
    std::atomic<int> state { kSlotEmpty };
    std::atomic<u32> generation { 0 };
    std::atomic<u32> bankId { 0 };
    std::atomic<u32> sampleId { 0 };
    std::atomic<s32> groupId { -1 };
    std::atomic<u32> storeFrame { 0 };
    std::atomic<u32> freeFrame { 0 };
    std::atomic<u64> freeIrq { 0 };
};

SampleRecord gSamples[kSampleSlots];
std::atomic<u32> gSamplesLive { 0 };
std::atomic<bool> gTableFullReported { false };

// Bumped whenever a sample is retired, so a voice's cached verdict expires
// without anyone having to walk the caches.
std::atomic<u32> gRetireEpoch { 0 };

struct VoiceCache {
    std::atomic<uintptr_t> addr { 0 };
    std::atomic<u32> epoch { 0xFFFFFFFFu };
};
VoiceCache gVoiceCache[kMaxVoices];

struct VoiceRecord {
    std::atomic<uintptr_t> addr { 0 };
    std::atomic<u32> bankId { 0 };
    std::atomic<u32> generation { 0 };
    std::atomic<u32> startFrame { 0 };
    std::atomic<int> live { 0 };
};
VoiceRecord gVoices[kMaxVoices];

// The group currently being pushed. Every sample stored while this is open is
// attributed to it, which is how a per-sample allocation gets a bank name.
struct PushContext {
    std::atomic<int> open { 0 };
    std::atomic<u32> bankId { 0 };
    std::atomic<u32> generation { 0 };
    std::atomic<s32> groupId { -1 };
    std::atomic<u32> stored { 0 };
};
PushContext gPush;

// Bank identity by sdir, so release and free can name the bank the push opened.
constexpr u32 kMaxBanks = 128;
struct BankRecord {
    std::atomic<uintptr_t> sdir { 0 };
    std::atomic<uintptr_t> base { 0 };
    std::atomic<size_t> size { 0 };
    std::atomic<u32> bankId { 0 };
    std::atomic<u32> generation { 0 };
    std::atomic<s32> groupId { -1 };
    std::atomic<u32> loadFrame { 0 };
    std::atomic<u32> releaseFrame { 0 };
    std::atomic<u32> samples { 0 };
    std::atomic<int> state { kSlotEmpty };
};
BankRecord gBanks[kMaxBanks];

std::atomic<u64> gIrqCounter { 0 };
std::atomic<u32> gNextBankId { 1 };
std::atomic<u32> gNextGeneration { 1 };

std::atomic<u32> gStatBanksLoaded { 0 };
std::atomic<u32> gStatBanksFreed { 0 };
std::atomic<u32> gStatStaleAtFree { 0 };
std::atomic<u32> gStatStaleReads { 0 };
std::atomic<u32> gStatVoicesStarted { 0 };
std::atomic<u32> gStatUnknownVoices { 0 };
std::atomic<u32> gStatSamplesStored { 0 };
std::atomic<u32> gStatSamplesFreed { 0 };
std::atomic<u32> gStatVoicesDetached { 0 };
std::atomic<u32> gEventsWritten { 0 };
std::atomic<u32> gEventsDropped { 0 };
std::atomic<bool> gViolation { false };
std::atomic<bool> gQuiet { false };

std::mutex gLogMutex;
FILE *gLog = nullptr;
bool gLogOpened = false;

// -1 not yet resolved, 0 off, 1 on, 2 on and logging every voice event.
// Resolved once from the environment; the self-test sets it directly so the
// detector can be exercised in a build whose environment has diagnostics off.
std::atomic<int> gEnabled { -1 };

int enabledImpl()
{
    const char *value = std::getenv("PARTYBOARD_AUDIO_DIAGNOSTICS");
    if (value == nullptr || value[1] != 0) return 0;
    if (value[0] == '1') return 1;
    if (value[0] == '2') return 2;
    return 0;
}

// Replaced by the self-test so detector 1 can be given voices chosen in advance.
PartyBoardAudioVoiceWalk gVoiceWalkOverride = nullptr;

int level()
{
    int state = gEnabled.load(std::memory_order_acquire);
    if (state < 0) {
        state = enabledImpl();
        int expected = -1;
        gEnabled.compare_exchange_strong(expected, state, std::memory_order_acq_rel);
        state = gEnabled.load(std::memory_order_acquire);
    }
    return state;
}

std::string logPath()
{
    std::string directory;
    if (const char *dir = std::getenv("PARTYBOARD_CRASH_DIR")) {
        directory = dir;
    } else if (const char *diagnostic = std::getenv("PARTYBOARD_NET_DIAGNOSTIC")) {
        directory = diagnostic;
        const size_t cut = directory.find_last_of("/\\");
        directory = cut == std::string::npos ? std::string(".") : directory.substr(0, cut);
    } else {
        directory = ".";
    }
    std::string peer = "x";
    if (const char *value = std::getenv("PARTYBOARD_CRASH_PEER")) {
        if (value[0] != 0) peer = value;
    }
    return directory + "/audio-lifetime-peer-" + peer + ".txt";
}

void writeEvent(const char *event, const char *format, ...)
{
    std::lock_guard<std::mutex> guard(gLogMutex);
    if (gQuiet.load(std::memory_order_relaxed)) return;
    if (gEventsWritten.load(std::memory_order_relaxed) >= kMaxLoggedEvents) {
        gEventsDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!gLogOpened) {
        gLogOpened = true;
        const std::string path = logPath();
        gLog = std::fopen(path.c_str(), "w");
        if (gLog) {
            std::fprintf(gLog, "%s",
                "# PartyBoard audio bank/voice lifetime trace\n"
                "# every line: <event> frame=<simulation frame> overlay=<game context>"
                " irq=<audio callback counter> <event fields>\n");
        }
    }
    if (!gLog) return;

    std::fprintf(gLog, "%s frame=%u overlay=%d irq=%llu ", event,
        PartyBoard_CrashCurrentFrame(), PartyBoard_CrashCurrentOverlay(),
        (unsigned long long)gIrqCounter.load(std::memory_order_relaxed));
    va_list args;
    va_start(args, format);
    std::vfprintf(gLog, format, args);
    va_end(args);
    std::fputc('\n', gLog);
    gEventsWritten.fetch_add(1, std::memory_order_relaxed);
    std::fflush(gLog);
}

u32 slotFor(uintptr_t address)
{
    // Sample copies come from malloc, so the low bits carry little information.
    u64 mixed = (u64)(address >> 4) * 0x9E3779B97F4A7C15ull;
    return (u32)(mixed >> 40) & (kSampleSlots - 1);
}

// Finds the slot holding `address`, or the first free slot it could be put in.
// Returns null only when the table is full and the address is absent.
SampleRecord *findSlot(uintptr_t address, bool forInsert)
{
    const u32 start = slotFor(address);
    SampleRecord *firstEmpty = nullptr;
    for (u32 probe = 0; probe < kSampleSlots; ++probe) {
        SampleRecord &record = gSamples[(start + probe) & (kSampleSlots - 1)];
        const int state = record.state.load(std::memory_order_acquire);
        if (state == kSlotEmpty) {
            if (!forInsert) return nullptr;
            return firstEmpty != nullptr ? firstEmpty : &record;
        }
        if (record.addr.load(std::memory_order_relaxed) == address) return &record;
    }
    return nullptr;
}

BankRecord *findBank(uintptr_t sdir)
{
    for (u32 i = 0; i < kMaxBanks; ++i) {
        if (gBanks[i].state.load(std::memory_order_acquire) == kSlotLive &&
            gBanks[i].sdir.load(std::memory_order_relaxed) == sdir) {
            return &gBanks[i];
        }
    }
    return nullptr;
}

// Context carried through the voice walk at a sample free.
struct FreeScan {
    uintptr_t addr;
    size_t length;
    u32 bankId;
    u32 generation;
    u32 sampleId;
    s32 groupId;
    u32 referencing;
    u32 liveVoices;
};

void freeScanSink(void *context, u32 voice, u32 state, const void *addr, u32 smpId, u8 compType,
    u32 length, u32 posHi)
{
    FreeScan *scan = (FreeScan *)context;
    if (state == 0) return;
    ++scan->liveVoices;
    const uintptr_t address = (uintptr_t)addr;
    if (address == 0 || address < scan->addr || address >= scan->addr + scan->length) return;

    ++scan->referencing;
    gStatStaleAtFree.fetch_add(1, std::memory_order_relaxed);
    gViolation.store(true, std::memory_order_release);
    writeEvent("SAMPLE_STALE_REFERENCE_AT_FREE",
        "bank=%u generation=%u group=%d sample=%u voice=%u voice_state=%u voice_smp_id=%u "
        "comp=%u sample_length=%zu voice_length=%u pos=%u addr=%p offset=%zu "
        "voice_started_frame=%u",
        scan->bankId, scan->generation, scan->groupId, scan->sampleId, voice, state, smpId,
        (unsigned)compType, scan->length, length, posHi, addr, (size_t)(address - scan->addr),
        gVoices[voice < kMaxVoices ? voice : 0].startFrame.load(std::memory_order_relaxed));

    PartyBoard_CrashBreadcrumb(PARTYBOARD_CRASH_CAT_AUDIO,
        "sample %u of bank %u gen %u freed while voice %u still reads it", scan->sampleId,
        scan->bankId, scan->generation, voice);
}

void walkVoices(PartyBoardAudioVoiceSink sink, void *context)
{
    if (gVoiceWalkOverride != nullptr) {
        gVoiceWalkOverride(sink, context);
    } else {
        PartyBoard_AudioWalkVoices(sink, context);
    }
}

} // namespace

extern "C" bool PartyBoard_AudioLifetimeEnabled(void) { return level() > 0; }

extern "C" void PartyBoard_AudioLifetimeSetQuiet(bool quiet)
{
    gQuiet.store(quiet, std::memory_order_release);
}

extern "C" void PartyBoard_AudioIrqTick(void)
{
    if (level() == 0) return;
    gIrqCounter.fetch_add(1, std::memory_order_relaxed);
}

// ---- banks ----------------------------------------------------------------

extern "C" void PartyBoard_AudioBankLoad(const void *sdir, const void *base, size_t size,
    s32 groupId)
{
    if (level() == 0 || sdir == nullptr) return;

    const u32 bankId = gNextBankId.fetch_add(1, std::memory_order_relaxed);
    const u32 generation = gNextGeneration.fetch_add(1, std::memory_order_relaxed);

    BankRecord *slot = nullptr;
    for (u32 i = 0; i < kMaxBanks; ++i) {
        if (gBanks[i].state.load(std::memory_order_acquire) != kSlotLive) {
            slot = &gBanks[i];
            break;
        }
    }
    if (slot != nullptr) {
        slot->sdir.store((uintptr_t)sdir, std::memory_order_relaxed);
        slot->base.store((uintptr_t)base, std::memory_order_relaxed);
        slot->size.store(size, std::memory_order_relaxed);
        slot->bankId.store(bankId, std::memory_order_relaxed);
        slot->generation.store(generation, std::memory_order_relaxed);
        slot->groupId.store(groupId, std::memory_order_relaxed);
        slot->loadFrame.store(PartyBoard_CrashCurrentFrame(), std::memory_order_relaxed);
        slot->releaseFrame.store(0, std::memory_order_relaxed);
        slot->samples.store(0, std::memory_order_relaxed);
        slot->state.store(kSlotLive, std::memory_order_release);
    }

    gPush.bankId.store(bankId, std::memory_order_relaxed);
    gPush.generation.store(generation, std::memory_order_relaxed);
    gPush.groupId.store(groupId, std::memory_order_relaxed);
    gPush.stored.store(0, std::memory_order_relaxed);
    gPush.open.store(1, std::memory_order_release);

    gStatBanksLoaded.fetch_add(1, std::memory_order_relaxed);
    writeEvent("BANK_LOAD", "bank=%u generation=%u group=%d sdir=%p staging=%p staging_size=%zu",
        bankId, generation, groupId, sdir, base, size);
}

extern "C" void PartyBoard_AudioBankVisible(const void *sdir)
{
    if (level() == 0) return;
    const u32 stored = gPush.stored.load(std::memory_order_relaxed);
    const u32 bankId = gPush.bankId.load(std::memory_order_relaxed);
    const u32 generation = gPush.generation.load(std::memory_order_relaxed);
    gPush.open.store(0, std::memory_order_release);

    BankRecord *bank = findBank((uintptr_t)sdir);
    if (bank != nullptr) bank->samples.store(stored, std::memory_order_relaxed);

    writeEvent("BANK_VISIBLE", "bank=%u generation=%u samples=%u", bankId, generation, stored);
}

extern "C" void PartyBoard_AudioBankReleaseRequest(const void *sdir, s32 groupId)
{
    if (level() == 0) return;
    BankRecord *bank = findBank((uintptr_t)sdir);
    if (bank == nullptr) {
        writeEvent("BANK_RELEASE_REQUEST", "bank=? generation=? group=%d sdir=%p unregistered=1",
            groupId, sdir);
        return;
    }
    bank->releaseFrame.store(PartyBoard_CrashCurrentFrame(), std::memory_order_relaxed);
    writeEvent("BANK_RELEASE_REQUEST", "bank=%u generation=%u group=%d loaded_frame=%u samples=%u",
        bank->bankId.load(std::memory_order_relaxed),
        bank->generation.load(std::memory_order_relaxed), groupId,
        bank->loadFrame.load(std::memory_order_relaxed),
        bank->samples.load(std::memory_order_relaxed));
}

extern "C" void PartyBoard_AudioDrainBegin(s32 steps)
{
    if (level() == 0) return;
    writeEvent("BANK_AUDIO_DRAIN_BEGIN", "steps=%d", steps);
}

extern "C" void PartyBoard_AudioDrainEnd(void)
{
    if (level() == 0) return;
    writeEvent("BANK_AUDIO_DRAIN_END", "irq_total=%llu",
        (unsigned long long)gIrqCounter.load(std::memory_order_relaxed));
}

extern "C" void PartyBoard_AudioBankFree(const void *sdir, const void *base)
{
    if (level() == 0) return;
    BankRecord *bank = findBank((uintptr_t)sdir);
    if (bank == nullptr) {
        writeEvent("BANK_FREE", "bank=? generation=? sdir=%p staging=%p unregistered=1", sdir,
            base);
        return;
    }
    writeEvent("BANK_FREE", "bank=%u generation=%u group=%d samples=%u loaded_frame=%u "
        "release_frame=%u",
        bank->bankId.load(std::memory_order_relaxed),
        bank->generation.load(std::memory_order_relaxed),
        bank->groupId.load(std::memory_order_relaxed),
        bank->samples.load(std::memory_order_relaxed),
        bank->loadFrame.load(std::memory_order_relaxed),
        bank->releaseFrame.load(std::memory_order_relaxed));
    bank->state.store(kSlotEmpty, std::memory_order_release);
    gStatBanksFreed.fetch_add(1, std::memory_order_relaxed);
}

// ---- samples --------------------------------------------------------------

extern "C" void PartyBoard_AudioSampleStore(const void *addr, size_t length)
{
    if (level() == 0 || addr == nullptr || length == 0) return;

    SampleRecord *record = findSlot((uintptr_t)addr, true);
    if (record == nullptr) {
        bool expected = false;
        if (gTableFullReported.compare_exchange_strong(expected, true)) {
            writeEvent("SAMPLE_TABLE_FULL", "live=%u slots=%u",
                gSamplesLive.load(std::memory_order_relaxed), kSampleSlots);
        }
        return;
    }

    if (record->state.load(std::memory_order_relaxed) != kSlotLive) {
        gSamplesLive.fetch_add(1, std::memory_order_relaxed);
    }
    record->addr.store((uintptr_t)addr, std::memory_order_relaxed);
    record->length.store(length, std::memory_order_relaxed);
    record->generation.store(gNextGeneration.fetch_add(1, std::memory_order_relaxed),
        std::memory_order_relaxed);
    record->bankId.store(gPush.open.load(std::memory_order_acquire)
            ? gPush.bankId.load(std::memory_order_relaxed)
            : 0,
        std::memory_order_relaxed);
    record->groupId.store(gPush.open.load(std::memory_order_acquire)
            ? gPush.groupId.load(std::memory_order_relaxed)
            : -1,
        std::memory_order_relaxed);
    record->sampleId.store(0, std::memory_order_relaxed);
    record->storeFrame.store(PartyBoard_CrashCurrentFrame(), std::memory_order_relaxed);
    record->freeFrame.store(0, std::memory_order_relaxed);
    record->state.store(kSlotLive, std::memory_order_release);

    gPush.stored.fetch_add(1, std::memory_order_relaxed);
    gStatSamplesStored.fetch_add(1, std::memory_order_relaxed);

    // A retired slot becoming live again means its address has been handed out
    // afresh, so every cached verdict about it is stale.
    gRetireEpoch.fetch_add(1, std::memory_order_acq_rel);
}

extern "C" void PartyBoard_AudioSampleIdentify(const void *addr, u32 sampleId)
{
    if (level() == 0 || addr == nullptr) return;
    SampleRecord *record = findSlot((uintptr_t)addr, false);
    if (record != nullptr) record->sampleId.store(sampleId, std::memory_order_relaxed);
}

extern "C" void PartyBoard_AudioSampleFree(const void *addr)
{
    if (level() == 0 || addr == nullptr) return;

    SampleRecord *record = findSlot((uintptr_t)addr, false);
    if (record == nullptr || record->state.load(std::memory_order_acquire) != kSlotLive) {
        writeEvent("SAMPLE_FREE_UNREGISTERED", "addr=%p", addr);
        return;
    }

    FreeScan scan;
    scan.addr = record->addr.load(std::memory_order_relaxed);
    scan.length = record->length.load(std::memory_order_relaxed);
    scan.bankId = record->bankId.load(std::memory_order_relaxed);
    scan.generation = record->generation.load(std::memory_order_relaxed);
    scan.sampleId = record->sampleId.load(std::memory_order_relaxed);
    scan.groupId = record->groupId.load(std::memory_order_relaxed);
    scan.referencing = 0;
    scan.liveVoices = 0;

    // Detector 1. Walking under the audio backend's own lock means the voices
    // cannot change state underneath the scan, so a report is a fact rather than
    // a torn read.
    walkVoices(freeScanSink, &scan);

    record->freeFrame.store(PartyBoard_CrashCurrentFrame(), std::memory_order_relaxed);
    record->freeIrq.store(gIrqCounter.load(std::memory_order_relaxed), std::memory_order_relaxed);
    record->state.store(kSlotRetired, std::memory_order_release);
    gSamplesLive.fetch_sub(1, std::memory_order_relaxed);
    gStatSamplesFreed.fetch_add(1, std::memory_order_relaxed);
    gRetireEpoch.fetch_add(1, std::memory_order_acq_rel);

    if (scan.referencing == 0 && level() >= 2) {
        writeEvent("SAMPLE_RELEASE_SAFE", "bank=%u generation=%u sample=%u live_voices=%u",
            scan.bankId, scan.generation, scan.sampleId, scan.liveVoices);
    }
}

// ---- voices ---------------------------------------------------------------

extern "C" void PartyBoard_AudioVoiceStart(u32 voice, const void *addr, u32 smpId, u8 compType,
    u32 length)
{
    const int on = level();
    if (on == 0 || voice >= kMaxVoices) return;

    SampleRecord *record = findSlot((uintptr_t)addr, false);
    const bool known = record != nullptr && record->state.load(std::memory_order_acquire) != kSlotEmpty;
    const bool retired = known && record->state.load(std::memory_order_acquire) == kSlotRetired;

    gVoices[voice].addr.store((uintptr_t)addr, std::memory_order_relaxed);
    gVoices[voice].bankId.store(known ? record->bankId.load(std::memory_order_relaxed) : 0,
        std::memory_order_relaxed);
    gVoices[voice].generation.store(
        known ? record->generation.load(std::memory_order_relaxed) : 0, std::memory_order_relaxed);
    gVoices[voice].startFrame.store(PartyBoard_CrashCurrentFrame(), std::memory_order_relaxed);
    gVoices[voice].live.store(1, std::memory_order_release);
    gStatVoicesStarted.fetch_add(1, std::memory_order_relaxed);
    if (!known) gStatUnknownVoices.fetch_add(1, std::memory_order_relaxed);

    if (on >= 2) {
        writeEvent("VOICE_START", "voice=%u smp_id=%u comp=%u length=%u addr=%p", voice, smpId,
            (unsigned)compType, length, addr);
        if (known) {
            writeEvent("VOICE_BANK_REFERENCE", "voice=%u bank=%u generation=%u sample=%u retired=%d",
                voice, record->bankId.load(std::memory_order_relaxed),
                record->generation.load(std::memory_order_relaxed),
                record->sampleId.load(std::memory_order_relaxed), retired ? 1 : 0);
        } else {
            writeEvent("VOICE_BANK_REFERENCE", "voice=%u bank=unknown addr=%p", voice, addr);
        }
    }

    if (retired) {
        gViolation.store(true, std::memory_order_release);
        writeEvent("VOICE_STARTED_ON_RETIRED_SAMPLE",
            "voice=%u bank=%u generation=%u sample=%u freed_frame=%u addr=%p", voice,
            record->bankId.load(std::memory_order_relaxed),
            record->generation.load(std::memory_order_relaxed),
            record->sampleId.load(std::memory_order_relaxed),
            record->freeFrame.load(std::memory_order_relaxed), addr);
    }
}

extern "C" void PartyBoard_AudioVoiceStopRequest(u32 voice)
{
    if (level() < 2 || voice >= kMaxVoices) return;
    writeEvent("VOICE_STOP_REQUEST", "voice=%u bank=%u generation=%u still_live=%d", voice,
        gVoices[voice].bankId.load(std::memory_order_relaxed),
        gVoices[voice].generation.load(std::memory_order_relaxed),
        gVoices[voice].live.load(std::memory_order_relaxed));
}

extern "C" void PartyBoard_AudioVoiceStopped(u32 voice, const char *why)
{
    const int on = level();
    if (on == 0 || voice >= kMaxVoices) return;
    const u32 bank = gVoices[voice].bankId.load(std::memory_order_relaxed);
    const u32 generation = gVoices[voice].generation.load(std::memory_order_relaxed);
    gVoices[voice].live.store(0, std::memory_order_release);
    gVoices[voice].addr.store(0, std::memory_order_relaxed);
    if (on >= 2) {
        writeEvent("VOICE_STOPPED", "voice=%u bank=%u generation=%u why=%s", voice, bank,
            generation, why ? why : "?");
        writeEvent("VOICE_RELEASE_REFERENCE", "voice=%u bank=%u generation=%u", voice, bank,
            generation);
    }
}

extern "C" void PartyBoard_AudioNoteSampleRead(u32 voice, const void *addr)
{
    if (level() == 0 || voice >= kMaxVoices) return;

    const uintptr_t address = (uintptr_t)addr;
    const u32 epoch = gRetireEpoch.load(std::memory_order_acquire);
    if (gVoiceCache[voice].addr.load(std::memory_order_relaxed) == address &&
        gVoiceCache[voice].epoch.load(std::memory_order_relaxed) == epoch) {
        return;
    }

    SampleRecord *record = findSlot(address, false);

    gVoiceCache[voice].addr.store(address, std::memory_order_relaxed);
    gVoiceCache[voice].epoch.store(epoch, std::memory_order_relaxed);

    if (record != nullptr && record->state.load(std::memory_order_acquire) == kSlotRetired) {
        gStatStaleReads.fetch_add(1, std::memory_order_relaxed);
        gViolation.store(true, std::memory_order_release);
        writeEvent("AUDIO_STALE_SAMPLE_READ",
            "voice=%u bank=%u generation=%u group=%d sample=%u addr=%p length=%zu "
            "freed_frame=%u freed_irq=%llu voice_generation=%u",
            voice, record->bankId.load(std::memory_order_relaxed),
            record->generation.load(std::memory_order_relaxed),
            record->groupId.load(std::memory_order_relaxed),
            record->sampleId.load(std::memory_order_relaxed), addr,
            record->length.load(std::memory_order_relaxed),
            record->freeFrame.load(std::memory_order_relaxed),
            (unsigned long long)record->freeIrq.load(std::memory_order_relaxed),
            gVoices[voice].generation.load(std::memory_order_relaxed));
        PartyBoard_CrashBreadcrumb(PARTYBOARD_CRASH_CAT_AUDIO,
            "voice %u reading sample %u of bank %u freed at frame %u", voice,
            record->sampleId.load(std::memory_order_relaxed),
            record->bankId.load(std::memory_order_relaxed),
            record->freeFrame.load(std::memory_order_relaxed));
    }
}

extern "C" void PartyBoard_AudioVoiceDetachedForFree(u32 voice, const void *addr)
{
    if (level() == 0) return;
    gStatVoicesDetached.fetch_add(1, std::memory_order_relaxed);
    SampleRecord *record = findSlot((uintptr_t)addr, false);
    writeEvent("VOICE_DETACHED_FOR_FREE", "voice=%u bank=%u generation=%u sample=%u addr=%p",
        voice, record ? record->bankId.load(std::memory_order_relaxed) : 0,
        record ? record->generation.load(std::memory_order_relaxed) : 0,
        record ? record->sampleId.load(std::memory_order_relaxed) : 0, addr);
}

extern "C" u32 PartyBoard_AudioVoicesDetached(void)
{
    return gStatVoicesDetached.load(std::memory_order_relaxed);
}

extern "C" bool PartyBoard_AudioLifetimeViolationDetected(void)
{
    return gViolation.load(std::memory_order_acquire);
}

extern "C" void PartyBoard_AudioLifetimeStats(u32 *banksLoaded, u32 *banksFreed, u32 *staleAtFree,
    u32 *staleReads, u32 *voicesStarted)
{
    if (banksLoaded) *banksLoaded = gStatBanksLoaded.load(std::memory_order_relaxed);
    if (banksFreed) *banksFreed = gStatBanksFreed.load(std::memory_order_relaxed);
    if (staleAtFree) *staleAtFree = gStatStaleAtFree.load(std::memory_order_relaxed);
    if (staleReads) *staleReads = gStatStaleReads.load(std::memory_order_relaxed);
    if (voicesStarted) *voicesStarted = gStatVoicesStarted.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Self-test
//
// A detector is worth nothing until it has been handed corruptions chosen in
// advance and has named them. These cases give it stale references it is
// supposed to catch, and near misses it is supposed to ignore: a detector that
// fires on everything proves as little as one that fires on nothing.
//
// No real memory is freed here. The sample ranges are plain address arithmetic
// over a local buffer, because what is under test is the bookkeeping.
// ---------------------------------------------------------------------------

namespace {

struct FakeVoice {
    u32 index;
    u32 state;
    const void *addr;
};

FakeVoice gFakeVoices[8];
u32 gFakeVoiceCount = 0;

void fakeVoiceWalk(PartyBoardAudioVoiceSink sink, void *context)
{
    for (u32 i = 0; i < gFakeVoiceCount; ++i) {
        sink(context, gFakeVoices[i].index, gFakeVoices[i].state, gFakeVoices[i].addr, 100 + i, 0,
            4096, 0);
    }
}

void resetForSelfTest()
{
    for (u32 i = 0; i < kSampleSlots; ++i) {
        gSamples[i].state.store(kSlotEmpty, std::memory_order_relaxed);
        gSamples[i].addr.store(0, std::memory_order_relaxed);
        gSamples[i].length.store(0, std::memory_order_relaxed);
    }
    for (u32 i = 0; i < kMaxBanks; ++i) {
        gBanks[i].state.store(kSlotEmpty, std::memory_order_relaxed);
    }
    for (u32 i = 0; i < kMaxVoices; ++i) {
        gVoiceCache[i].addr.store(0, std::memory_order_relaxed);
        gVoiceCache[i].epoch.store(0xFFFFFFFFu, std::memory_order_relaxed);
        gVoices[i].live.store(0, std::memory_order_relaxed);
        gVoices[i].addr.store(0, std::memory_order_relaxed);
        gVoices[i].bankId.store(0, std::memory_order_relaxed);
        gVoices[i].generation.store(0, std::memory_order_relaxed);
    }
    gPush.open.store(0, std::memory_order_relaxed);
    gSamplesLive.store(0, std::memory_order_relaxed);
    gTableFullReported.store(false, std::memory_order_relaxed);
    gStatStaleAtFree.store(0, std::memory_order_relaxed);
    gStatStaleReads.store(0, std::memory_order_relaxed);
    gStatUnknownVoices.store(0, std::memory_order_relaxed);
    gStatVoicesDetached.store(0, std::memory_order_relaxed);
    gViolation.store(false, std::memory_order_relaxed);
    gFakeVoiceCount = 0;
}

u32 staleAtFreeCount() { return gStatStaleAtFree.load(std::memory_order_relaxed); }
u32 staleReadCount() { return gStatStaleReads.load(std::memory_order_relaxed); }
u32 unknownVoiceCount() { return gStatUnknownVoices.load(std::memory_order_relaxed); }

bool expect(bool condition, const char *what)
{
    if (!condition) {
        std::fprintf(stderr, "audio lifetime self-test FAILED: %s\n", what);
    }
    return condition;
}

} // namespace

extern "C" bool PartyBoard_AudioLifetimeRunSelfTest(void)
{
    // Address space standing in for one emulated-ARAM sample copy. Never read.
    static unsigned char sampleStorage[10240];
    unsigned char *const addr = sampleStorage;
    const size_t length = sizeof(sampleStorage);

    const int savedEnabled = gEnabled.load(std::memory_order_acquire);
    PartyBoardAudioVoiceWalk savedWalk = gVoiceWalkOverride;
    gEnabled.store(2, std::memory_order_release);
    gVoiceWalkOverride = fakeVoiceWalk;
    PartyBoard_AudioLifetimeSetQuiet(true);

    bool ok = true;
    const void *const sdir = (const void *)0x1000;

    // 1. A sample freed with no live voice at all is clean.
    resetForSelfTest();
    PartyBoard_AudioBankLoad(sdir, addr, length, 7);
    PartyBoard_AudioSampleStore(addr, length);
    PartyBoard_AudioBankVisible(sdir);
    PartyBoard_AudioSampleFree(addr);
    ok &= expect(staleAtFreeCount() == 0, "clean free reported a stale reference");
    ok &= expect(!PartyBoard_AudioLifetimeViolationDetected(), "clean free raised a violation");

    // 2. A live voice carrying the sample's own address must be caught. This is
    //    the shape a real voice has: smp_info.addr IS the allocation base.
    resetForSelfTest();
    PartyBoard_AudioSampleStore(addr, length);
    gFakeVoices[0] = { 3, 2, addr };
    gFakeVoiceCount = 1;
    PartyBoard_AudioSampleFree(addr);
    ok &= expect(staleAtFreeCount() == 1, "a voice at the sample base was not caught");
    ok &= expect(PartyBoard_AudioLifetimeViolationDetected(), "a stale reference raised nothing");

    // 3. The last byte of the sample is still inside it.
    resetForSelfTest();
    PartyBoard_AudioSampleStore(addr, length);
    gFakeVoices[0] = { 3, 2, addr + length - 1 };
    gFakeVoiceCount = 1;
    PartyBoard_AudioSampleFree(addr);
    ok &= expect(staleAtFreeCount() == 1, "a voice at the last byte was not caught");

    // 4. One byte past the end belongs to someone else.
    resetForSelfTest();
    PartyBoard_AudioSampleStore(addr, length);
    gFakeVoices[0] = { 3, 2, addr + length };
    gFakeVoiceCount = 1;
    PartyBoard_AudioSampleFree(addr);
    ok &= expect(staleAtFreeCount() == 0, "a voice one byte past the end was miscounted");

    // 5. One byte before the start belongs to someone else.
    resetForSelfTest();
    PartyBoard_AudioSampleStore(addr, length);
    gFakeVoices[0] = { 3, 2, addr - 1 };
    gFakeVoiceCount = 1;
    PartyBoard_AudioSampleFree(addr);
    ok &= expect(staleAtFreeCount() == 0, "a voice one byte before the base was miscounted");

    // 6. A stopped voice inside the range is not a reference. This is the state
    //    the fix has to produce, so a detector that cannot tell it apart from a
    //    live one would make the fix unverifiable.
    resetForSelfTest();
    PartyBoard_AudioSampleStore(addr, length);
    gFakeVoices[0] = { 3, 0, addr + 64 };
    gFakeVoiceCount = 1;
    PartyBoard_AudioSampleFree(addr);
    ok &= expect(staleAtFreeCount() == 0, "a stopped voice was counted as a reference");
    ok &= expect(!PartyBoard_AudioLifetimeViolationDetected(), "a stopped voice raised a violation");

    // 7. Several voices, only some of them inside the sample.
    resetForSelfTest();
    PartyBoard_AudioSampleStore(addr, length);
    gFakeVoices[0] = { 1, 2, addr + 8 };
    gFakeVoices[1] = { 2, 2, addr - 4096 };
    gFakeVoices[2] = { 3, 2, addr + length - 8 };
    gFakeVoices[3] = { 4, 0, addr + 16 };
    gFakeVoiceCount = 4;
    PartyBoard_AudioSampleFree(addr);
    ok &= expect(staleAtFreeCount() == 2, "the mixed voice set was not counted exactly");

    // 8. Detector 2: reading a live sample is silent, reading it after the free
    //    is not, and the per-voice cache must not hide the transition.
    resetForSelfTest();
    PartyBoard_AudioSampleStore(addr, length);
    PartyBoard_AudioNoteSampleRead(5, addr);
    ok &= expect(staleReadCount() == 0, "reading a live sample was reported as stale");
    PartyBoard_AudioNoteSampleRead(5, addr);
    PartyBoard_AudioSampleFree(addr);
    PartyBoard_AudioNoteSampleRead(5, addr);
    ok &= expect(staleReadCount() == 1, "reading a freed sample was not reported");
    ok &= expect(PartyBoard_AudioLifetimeViolationDetected(), "a stale read raised no violation");

    // 9. An address in no sample at all is not a stale read. Stream buffers and
    //    base groups take other paths, and calling those a defect would drown
    //    the real one.
    resetForSelfTest();
    PartyBoard_AudioSampleStore(addr, length);
    PartyBoard_AudioNoteSampleRead(6, (const void *)0x40);
    ok &= expect(staleReadCount() == 0, "an unregistered address was reported as stale");
    ok &= expect(!PartyBoard_AudioLifetimeViolationDetected(),
        "an unregistered address raised a violation");

    // 10. The allocator handing the same address back is a new generation, and a
    //     read is then legitimate again.
    resetForSelfTest();
    PartyBoard_AudioSampleStore(addr, length);
    PartyBoard_AudioSampleFree(addr);
    PartyBoard_AudioSampleStore(addr, length);
    PartyBoard_AudioNoteSampleRead(7, addr);
    ok &= expect(staleReadCount() == 0, "a reused address was still reported as retired");
    ok &= expect(!PartyBoard_AudioLifetimeViolationDetected(),
        "a reused address raised a violation");

    // 11. A voice starting on a sample that has already been freed is itself a
    //     violation, and one that does not need the audio thread to be running.
    resetForSelfTest();
    PartyBoard_AudioSampleStore(addr, length);
    PartyBoard_AudioSampleFree(addr);
    PartyBoard_AudioVoiceStart(8, addr, 42, 0, 1024);
    ok &= expect(PartyBoard_AudioLifetimeViolationDetected(),
        "a voice starting on a freed sample was not reported");

    // 12. A voice on an address the table never saw is counted, not reported:
    //     this is the number that told us the first detector was looking at the
    //     wrong memory entirely.
    resetForSelfTest();
    PartyBoard_AudioVoiceStart(9, (const void *)0x8000, 1, 0, 16);
    ok &= expect(unknownVoiceCount() == 1, "an unattributed voice was not counted");
    ok &= expect(!PartyBoard_AudioLifetimeViolationDetected(),
        "an unattributed voice raised a violation");

    // 13. Many distinct samples must all stay findable: the table is open
    //     addressed, so a probing mistake would silently lose entries.
    resetForSelfTest();
    for (u32 i = 0; i < 4096; ++i) {
        PartyBoard_AudioSampleStore(sampleStorage + (size_t)i * 2, 2);
    }
    u32 found = 0;
    for (u32 i = 0; i < 4096; ++i) {
        if (findSlot((uintptr_t)(sampleStorage + (size_t)i * 2), false) != nullptr) ++found;
    }
    ok &= expect(found == 4096, "the sample table lost entries under collision");

    resetForSelfTest();
    PartyBoard_AudioLifetimeSetQuiet(false);
    gVoiceWalkOverride = savedWalk;
    gEnabled.store(savedEnabled, std::memory_order_release);
    if (ok) {
        std::printf("Audio bank lifetime: PASS (13 cases: stale reference at a free, both range "
                    "boundaries, both near misses, a stopped voice, a mixed voice set, a stale "
                    "read after the free, an unregistered address, a reused address, a voice "
                    "starting on a freed sample, an unattributed voice, 4096 colliding entries). "
                    "Bookkeeping only; no real sample was freed.\n");
    }
    return ok;
}
