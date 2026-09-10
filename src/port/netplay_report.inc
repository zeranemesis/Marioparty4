// Per-peer desync report. Included by netplay_runtime.cpp inside the anonymous
// namespace, after gRuntime and the canonical history are defined.
//
// The format is line-oriented and stable so tools/netplay_compare.py can diff
// two peers mechanically. Every value is canonical: no addresses, no handles,
// no host paths. Both peers write the same sections in the same order, so the
// first differing FIELD line is the smallest divergent gameplay value.

constexpr unsigned kReportFramesBefore = 120;
constexpr unsigned kReportFramesAfter = 30;
constexpr unsigned kReportVersion = 1;

std::string reportTimestamp()
{
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm parts {};
#ifdef _WIN32
    localtime_s(&parts, &now);
#else
    localtime_r(&now, &parts);
#endif
    char text[32];
    std::snprintf(text, sizeof(text), "%04d-%02d-%02d_%02d%02d%02d", parts.tm_year + 1900,
        parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
    return text;
}

std::filesystem::path reportDirectory()
{
    std::error_code ec;
    if (!PartyBoard_ConfigPath.empty()) {
        auto directory = PartyBoard_ConfigPath / "netplay";
        std::filesystem::create_directories(directory, ec);
        if (!ec) return directory;
    }
    return std::filesystem::current_path(ec);
}

void writeDigestLine(std::FILE *file, const char *role, const StateDigest &digest)
{
    std::fprintf(file, "DIGEST %s frame=%u context=%u hash=%016llx frand=%08x rand8=%08x counter=%u",
        role, digest.frame, digest.context, static_cast<unsigned long long>(digest.hash),
        digest.frand, digest.rand8, digest.counter);
    for (std::size_t index = 0; index < kSubsystemCount; ++index)
        std::fprintf(file, " %s=%08x", subsystemName(index), digest.parts[index]);
    std::fputc('\n', file);
}

// `frame` is the first divergent checkpoint; `failure` explains why the stream
// stopped. Returns the report path, or an empty path when nothing was written.
std::string writeDesyncReport(std::uint32_t frame, StateFailure failure)
{
    const auto *local = gRuntime.states.getLocal(frame);
    const auto *remote = gRuntime.states.getRemote(frame);
    const auto directory = reportDirectory();
    char name[128];
    std::snprintf(name, sizeof(name), "netplay_desync_%s_player%u.log",
        reportTimestamp().c_str(), static_cast<unsigned>(gRuntime.localPlayer + 1));
    const auto path = directory / name;
    std::FILE *file = nullptr;
#ifdef _WIN32
    if (_wfopen_s(&file, path.wstring().c_str(), L"wb") != 0) file = nullptr;
#else
    file = std::fopen(path.c_str(), "wb");
#endif
    if (!file) return {};

    const auto &disc = partyboard::version::getDiskID();
    std::fprintf(file, "PARTYBOARD_NETPLAY_DESYNC_REPORT version=%u\n", kReportVersion);
    std::fprintf(file, "build_describe=%s\n", PARTY_BOARD_WC_DESCRIBE);
    std::fprintf(file, "build_revision=%s\n", PARTY_BOARD_WC_REVISION);
    std::fprintf(file, "build_branch=%s\n", PARTY_BOARD_WC_BRANCH);
    std::fprintf(file, "build_type=%s\n", PARTY_BOARD_BUILD_TYPE);
    std::fprintf(file, "protocol=%u hash_version=%u packet_bytes=%zu\n",
        static_cast<unsigned>(kProtocolVersion), kStateHashVersion, kNetplayPacketSize);
    std::fprintf(file, "disc=%.4s%.2s disc_version=%u disc_number=%u\n", disc.gameName,
        disc.company, static_cast<unsigned>(disc.gameVersion),
        static_cast<unsigned>(disc.diskNumber));
    std::fprintf(file, "session=%08x player=%u input_delay=%u full_game=%d rollback=%d\n",
        kSessionId, static_cast<unsigned>(gRuntime.localPlayer),
        static_cast<unsigned>(gRuntime.inputDelay), gRuntime.fullGame, gRuntime.rollbackRequested);
    std::fprintf(file,
        "net received=%u rejected=%u repaired=%u send_errors=%u tx_sequence=%u stalled=%u longest_stall=%u\n",
        gRuntime.receivedPackets, gRuntime.rejectedPackets, gRuntime.repairPackets,
        gRuntime.sendFailures, gRuntime.sequence, gRuntime.stalledTicks,
        gRuntime.maximumStalledTicks);
    std::fprintf(file, "failure=%u first_desync_frame=%u last_good_frame=%d current_frame=%u\n",
        static_cast<unsigned>(failure), frame,
        frame ? static_cast<int>(frame) - 1 : -1, gRuntime.frame);
    std::fprintf(file, "context=%d overlay=%d minigame=%d\n", gRuntime.observedContext,
        PartyBoard_NetplayContextId(), PartyBoard_NetplayMinigameId());
    std::fprintf(file, "rng frand=%08x rand8=%08x boardrand=%08x frand_calls=%u rand8_calls=%u boardrand_calls=%u\n",
        frand_state_get(), static_cast<u32>(rand8_state_get()), boardRandSeed,
        PartyBoard_NetplayFrandCalls(), PartyBoard_NetplayRand8Calls(),
        PartyBoard_NetplayBoardRandCalls());

    // Values the MusyX audio thread drives and that are NOT hashed, because a
    // difference in them is not by itself a gameplay divergence. They are
    // recorded because gameplay does block on some of them, and when it does
    // the resulting desync surfaces elsewhere: see C5/C6 in
    // docs/NETPLAY_DETERMINISM_AUDIT.md. If these differ between two peers,
    // suspect them before anything the verdict table points at.
    // Only msmMusGetStatus is used here: the play counters exist in the MusyX
    // backend alone, and this file is compiled for both.
    std::fprintf(file, "audio_thread_context");
    for (int channel = 0; channel < 4; ++channel)
        std::fprintf(file, " mus_status[%d]=%d", channel,
            static_cast<int>(msmMusGetStatus(channel)));
    std::fputc('\n', file);

    // Subsystem verdict table: the answer to "what diverged" in one glance.
    if (local && remote) {
        std::fprintf(file, "category=%s\n",
            subsystemName(local->firstDifferentPart(*remote)));
        for (std::size_t index = 0; index < kSubsystemCount; ++index) {
            std::fprintf(file, "SUBSYSTEM %-10s local=%08x remote=%08x %s\n",
                subsystemName(index), local->parts[index], remote->parts[index],
                local->parts[index] == remote->parts[index] ? "OK" : "DIFFERENT");
        }
    } else {
        std::fprintf(file, "category=UNKNOWN local_digest=%d remote_digest=%d\n",
            local != nullptr, remote != nullptr);
    }

    // Field-level evidence for the failing frame. Both peers emit the same
    // index sequence whenever the state matches, so netplay_compare can report
    // the first differing field by index without needing the peer's memory.
    const auto &detail = gRuntime.detailHistory[frame % kDetailHistorySize];
    const auto detailFrame = gRuntime.detailFrames[frame % kDetailHistorySize];
    if (detailFrame == frame) {
        std::fprintf(file, "FIELDS frame=%u count=%zu\n", frame, detail.size());
        for (std::size_t index = 0; index < detail.size(); ++index) {
            std::fprintf(file, "FIELD %zu %s %s %08x\n", index,
                subsystemName(detail.subsystemOf(index)), detail.names[index],
                detail.values[index]);
        }
    } else {
        std::fprintf(file, "FIELDS frame=%u count=0 retained_frame=%u\n", frame, detailFrame);
    }

    // Digest stream around the divergence, local and remote.
    const std::uint32_t first = frame > kReportFramesBefore ? frame - kReportFramesBefore : 0;
    const std::uint32_t last = frame + kReportFramesAfter;
    for (std::uint32_t index = first; index <= last; ++index) {
        if (const auto *value = gRuntime.states.getLocal(index)) writeDigestLine(file, "local", *value);
        if (const auto *value = gRuntime.states.getRemote(index)) writeDigestLine(file, "remote", *value);
    }

    // Input timeline around the divergence. An input applied to the wrong frame
    // shows up here even when the canonical state still matches.
    for (std::uint32_t index = first; index <= last; ++index) {
        const auto *localInput = findInput(gRuntime.localHistory, index);
        const auto *remoteInput = findInput(gRuntime.remoteHistory, index);
        if (!localInput && !remoteInput) continue;
        std::fprintf(file, "INPUT frame=%u", index);
        const auto emit = [file](const char *role, const InputSlot *slot) {
            if (!slot) { std::fprintf(file, " %s=absent", role); return; }
            std::fprintf(file, " %s=%04x/%d/%d/%d/%d/%u/%u@ctx%u", role, slot->input.buttons,
                slot->input.stickX, slot->input.stickY, slot->input.substickX,
                slot->input.substickY, slot->input.triggerLeft, slot->input.triggerRight,
                slot->captureContext);
        };
        emit("local", localInput);
        emit("remote", remoteInput);
        std::fputc('\n', file);
    }
    std::fprintf(file, "END\n");
    std::fclose(file);
    return path.string();
}
