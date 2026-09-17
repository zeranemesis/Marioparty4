#pragma once
#include "port/netplay_state.hpp"
#include "port/netplay_state.h"
extern "C" {
#include "game/gamework.h"
#include "game/gamework_data.h"
#include "game/objsub.h"
#include "game/pad.h"
extern u32 boardRandSeed;
void PartyBoard_RollbackSysFlagsSave(void *);
}
namespace partyboard::netplay {
// Version 2 groups every field under a subsystem so a divergence report can
// name the area that changed. Coverage is deliberately explicit: only values
// whose meaning is identical on two machines are hashed. Pointers, addresses,
// handles, padding, presentation buffers and physical audio state are never
// exported; structures contribute named fields, never their object layout.
inline CanonicalState captureCanonical(const StateDigest &stamp) {
    CanonicalState out;
    out.names.reserve(4096);
    out.values.reserve(4096);
#define WORD(value) out.add(#value, static_cast<std::uint32_t>(value))

    out.begin(Subsystem::Meta);
    WORD(stamp.version); WORD(stamp.frame); WORD(stamp.context);

    out.begin(Subsystem::Rng);
    WORD(stamp.frand); WORD(stamp.rand8); WORD(boardRandSeed);
    // Consumption counters localise an extra draw to a generator and an index.
    WORD(PartyBoard_NetplayFrandCalls());
    WORD(PartyBoard_NetplayRand8Calls());
    WORD(PartyBoard_NetplayBoardRandCalls());

    out.begin(Subsystem::Timers);
    WORD(stamp.counter);
    PartyBoard_NetplayTimerState(CanonicalState::sink, &out);

    out.begin(Subsystem::Gamework);
    WORD(GWSystem.team);
    WORD(GWSystem.party);
    WORD(GWSystem.diff_story);
    WORD(GWSystem.save_mode);
    WORD(GWSystem.mess_speed);
    WORD(GWSystem.mg_list);
    WORD(GWSystem.show_com_mg);
    WORD(GWSystem.explain_mg);
    WORD(GWSystem.bonus_star);
    WORD(GWSystem.turn);
    WORD(GWSystem.max_turn);
    WORD(GWSystem.star_flag);
    WORD(GWSystem.star_total);
    WORD(GWSystem.board);
    WORD(GWSystem.star_pos);
    WORD(GWSystem.last5_effect);
    WORD(GWSystem.player_curr);
    WORD(GWSystem.storyCharBit);
    WORD(GWSystem.storyChar);
    WORD(GWSystem.block_pos);
    WORD(GWSystem.mess_delay);
    WORD(GWSystem.bowser_event);
    WORD(GWSystem.bowser_loss);
    WORD(GWSystem.lucky_value);
    WORD(GWSystem.mg_next);
    WORD(GWSystem.mg_type);
    for (const auto v : GWSystem.board_data) out.add("GWSystem.board_data", v);
    for (const auto &row : GWSystem.flag)
        for (const auto v : row) out.add("GWSystem.flag", v);
    u8 sysFlags[16] {};
    PartyBoard_RollbackSysFlagsSave(sysFlags);
    for (const auto v : sysFlags) out.add("system_flags", v);
    WORD(GWGameStat.language); WORD(GwLanguage); WORD(GwLanguageSave);
    WORD(GWGameStat.total_stars); WORD(GWGameStat.customPackEnable);
    WORD(GWGameStat.veryHardUnlock); WORD(GWGameStat.open_w06);
    WORD(GWGameStat.party_continue); WORD(GWGameStat.story_continue);
    // Save dates, audio preferences, padding and unknown unused fields omitted.

    out.begin(Subsystem::Input);
    for (unsigned player = 0; player < 4; ++player) {
        WORD(player);
        WORD(HuPadBtn[player]); WORD(HuPadBtnDown[player]); WORD(HuPadBtnRep[player]);
        WORD(HuPadStkX[player]); WORD(HuPadStkY[player]); WORD(HuPadSubStkX[player]); WORD(HuPadSubStkY[player]);
        WORD(HuPadTrigL[player]); WORD(HuPadTrigR[player]); WORD(HuPadDStk[player]); WORD(HuPadDStkRep[player]);
        WORD(HuPadErr[player]);
    }

    out.begin(Subsystem::Players);
    for (unsigned player = 0; player < 4; ++player) {
        WORD(player);
        const auto &p = GWPlayer[player];
        const auto &c = GWPlayerCfg[player];
        WORD(c.character); WORD(c.pad_idx); WORD(c.diff); WORD(c.group); WORD(c.iscom);
        WORD(p.ticket_player);
        WORD(p.draw_ticket);
        WORD(p.auto_size);
        WORD(p.character);
        WORD(p.com);
        WORD(p.diff);
        WORD(p.player_idx);
        WORD(p.spark);
        WORD(p.team);
        WORD(p.handicap);
        WORD(p.port);
        WORD(p.team_backup);
        WORD(p.bowser_suit);
        WORD(p.rank);
        WORD(p.num_dice);
        WORD(p.size);
        WORD(p.show_next);
        WORD(p.jump);
        WORD(p.moving);
        WORD(p.color);
        WORD(p.roll);
        WORD(p.space_curr);
        WORD(p.space_prev);
        WORD(p.space_next);
        WORD(p.space_shock);
        WORD(p.blue_count);
        WORD(p.red_count);
        WORD(p.question_count);
        WORD(p.fortune_count);
        WORD(p.bowser_count);
        WORD(p.battle_count);
        WORD(p.mushroom_count);
        WORD(p.warp_count);
        WORD(p.coins);
        WORD(p.coins_mg);
        WORD(p.coins_total);
        WORD(p.coins_max);
        WORD(p.coins_battle);
        WORD(p.coin_collect);
        WORD(p.coin_win);
        WORD(p.stars);
        WORD(p.stars_max);
        for (const auto v : p.items) out.add("p.items", static_cast<std::uint32_t>(v));
    }

    out.begin(Subsystem::Minigame);
    for (const auto v : GWGameStat.mg_custom) out.add("mg_custom", v);
    for (unsigned i = 0; i < 64; ++i) out.add("mg_available", GWMGAvailGet(401 + i) != 0);
    for (const auto v : GWGameStat.mg_record) out.add("mg_record", v);
    WORD(mgTypeCurr); WORD(mgBattleStarMax); WORD(mgRecordExtra); WORD(mgQuitExtraF);
    WORD(mgPracticeEnableF); WORD(mgInstExitEnableF); WORD(mgBoardHostEnableF);
    for (const auto v : mgBattleStar) out.add("mgBattleStar", static_cast<std::uint32_t>(v));
    for (const auto &row : mgTicTacToeGrid)
        for (const auto v : row) out.add("mgTicTacToeGrid", static_cast<std::uint32_t>(v));

    out.begin(Subsystem::Sequence);
    PartyBoard_NetplaySequenceState(CanonicalState::sink, &out);

    out.begin(Subsystem::Board);
    PartyBoard_NetplayDiceState(CanonicalState::sink, &out);

    out.begin(Subsystem::Overlay);
    PartyBoard_NetplayOverlayState(CanonicalState::sink, &out);

    out.begin(Subsystem::Processes);
    PartyBoard_NetplayProcessState(CanonicalState::sink, &out);

    out.begin(Subsystem::Objects);
    PartyBoard_NetplayObjectState(CanonicalState::sink, &out);

    out.begin(Subsystem::Scene);
    PartyBoard_NetplaySceneState(CanonicalState::sink, &out);

    out.begin(Subsystem::Heaps);
    PartyBoard_NetplayHeapState(CanonicalState::sink, &out);

    out.begin(Subsystem::Audio);
    PartyBoard_NetplayAudioState(CanonicalState::sink, &out);

    out.begin(Subsystem::Animation);
    PartyBoard_NetplayAnimationState(CanonicalState::sink, &out);

    // Sprite logical time is still not covered.
#undef WORD
    return out;
}
} // namespace partyboard::netplay
