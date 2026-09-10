
static bool runCanonicalStateSelfTest()
{
    using namespace partyboard::netplay;
    const auto player = GWPlayer[0];
    const auto system = GWSystem;
    const auto stat = GWGameStat;
    const auto savedFrand = frand_state_get();
    const auto savedRand8 = rand8_state_get();
    const auto savedBoard = boardRandSeed;
    const auto enabled = gRuntime.enabled;
    const StateDigest stamp {42, 7, 0x12345678, 0x90123456, 41};
    const auto base = captureCanonical(stamp).hash;
    bool passed = PartyBoard_NetplayFloatWord(0.0f) == PartyBoard_NetplayFloatWord(-0.0f)
        && PartyBoard_NetplayFloatWord(1.0f) == 0x3f800000u;
    ++GWPlayer[0].coins;
    passed &= captureCanonical(stamp).hash != base;
    GWPlayer[0] = player;
    ++GWPlayer[0].roll;
    passed &= captureCanonical(stamp).hash != base;
    GWPlayer[0] = player;
    GWSystem.board_data[3] ^= 1;
    passed &= captureCanonical(stamp).hash != base;
    GWSystem = system;
    ++GWSystem.turn;
    passed &= captureCanonical(stamp).hash != base;
    GWSystem = system;
    boardRandSeed ^= 1;
    passed &= captureCanonical(stamp).hash != base;
    boardRandSeed = savedBoard;
    ++GWGameStat.create_time;
    GWGameStat.sound_mode ^= 1;
    GWSystem.PC_PADDING_00 ^= 1;
    GWPlayer[0].PC_PADDING_02 ^= 1;
    passed &= captureCanonical(stamp).hash == base;
    GWPlayer[0] = player; GWSystem = system; GWGameStat = stat;

    // Zero frand seed and board initialization must ignore different wall times
    // online. Both real RNG implementations are used; all state is restored.
    gRuntime.enabled = true;
    frand_state_set(0); rand8_state_set(17);
    const auto first = frand();
    const auto firstRand8 = rand8_state_get();
    BoardRandInit(); const auto firstBoard = BoardRand();
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    frand_state_set(0); rand8_state_set(17);
    passed &= frand() == first && rand8_state_get() == firstRand8;
    BoardRandInit(); passed &= BoardRand() == firstBoard;
    // Offline keeps the original OS time seed.
    gRuntime.enabled = false;
    const auto before = static_cast<u32>(OSGetTime());
    BoardRandInit();
    const auto after = static_cast<u32>(OSGetTime());
    passed &= boardRandSeed - before <= after - before;
    gRuntime.enabled = enabled;
    frand_state_set(savedFrand); rand8_state_set(savedRand8); boardRandSeed = savedBoard;
    std::printf("Canonical gameplay state: %s (coins/dice/turn/board RNG mutations, padding/date/audio excluded, online zero-seed repeatability, offline board seed).\n", passed ? "PASS" : "FAIL");
    return passed;
}
