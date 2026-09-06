#include "game/gamework.h"
#include "game/flag.h"
#include "game/gamework_data.h"
#include "port/rollback_game.h"
#include <string.h>
#include "version.h"

#ifndef __MWERKS__
#include "game/pad.h"
#include "port/settings.h"
#endif

#ifdef TARGET_PC
#include <stdlib.h>
#endif

SHARED_SYM s16 GwLanguage = 1;
SHARED_SYM s16 GwLanguageSave = -1;

SHARED_SYM GameStat GWGameStatDefault;
SHARED_SYM GameStat GWGameStat;
SHARED_SYM SystemState GWSystem;
SHARED_SYM PlayerState GWPlayer[4];
SHARED_SYM PlayerConfig GWPlayerCfg[4];

#ifdef TARGET_PC
#define ROLLBACK_GAME_MAGIC 0x47575331u /* "GWS1" */
#define ROLLBACK_GAME_VERSION 1u

typedef struct RollbackGameHeader {
    u32 magic;
    u32 version;
    u32 total_size;
    u32 game_stat_size;
    u32 system_state_size;
    u32 player_state_size;
    u32 player_config_size;
    u32 player_count;
    u32 system_flags_size;
    u32 reserved;
} RollbackGameHeader;

extern size_t PartyBoard_RollbackSysFlagsSizeGet(void);
extern void PartyBoard_RollbackSysFlagsSave(void *destination);
extern void PartyBoard_RollbackSysFlagsLoad(const void *source);

size_t PartyBoard_RollbackGameSize(void)
{
    return sizeof(RollbackGameHeader)
        + sizeof(GWGameStatDefault) + sizeof(GWGameStat) + sizeof(GWSystem)
        + sizeof(GWPlayer) + sizeof(GWPlayerCfg)
        + sizeof(GwLanguage) + sizeof(GwLanguageSave)
        + PartyBoard_RollbackSysFlagsSizeGet();
}

BOOL PartyBoard_RollbackGameSave(void *destination, size_t capacity)
{
    const size_t required = PartyBoard_RollbackGameSize();
    RollbackGameHeader header = {
        ROLLBACK_GAME_MAGIC, ROLLBACK_GAME_VERSION, (u32)required,
        sizeof(GameStat), sizeof(SystemState), sizeof(PlayerState),
        sizeof(PlayerConfig), 4, (u32)PartyBoard_RollbackSysFlagsSizeGet(), 0
    };
    u8 *cursor = (u8 *)destination;
    if (destination == NULL || required > UINT32_MAX || capacity != required) {
        return FALSE;
    }
#define ROLLBACK_GAME_WRITE(value) do { \
    memcpy(cursor, &(value), sizeof(value)); cursor += sizeof(value); \
} while (0)
    ROLLBACK_GAME_WRITE(header);
    ROLLBACK_GAME_WRITE(GWGameStatDefault);
    ROLLBACK_GAME_WRITE(GWGameStat);
    ROLLBACK_GAME_WRITE(GWSystem);
    ROLLBACK_GAME_WRITE(GWPlayer);
    ROLLBACK_GAME_WRITE(GWPlayerCfg);
    ROLLBACK_GAME_WRITE(GwLanguage);
    ROLLBACK_GAME_WRITE(GwLanguageSave);
#undef ROLLBACK_GAME_WRITE
    PartyBoard_RollbackSysFlagsSave(cursor);
    return TRUE;
}

BOOL PartyBoard_RollbackGameLoad(const void *source, size_t size)
{
    RollbackGameHeader header;
    GameStat gameStatDefault, gameStat;
    SystemState system;
    PlayerState players[4];
    PlayerConfig playerConfigs[4];
    s16 language, languageSave;
    u8 systemFlags[16];
    const u8 *cursor = (const u8 *)source;
    const u8 *end;
    const size_t required = PartyBoard_RollbackGameSize();

    if (source == NULL || size != required || size < sizeof(header)) {
        return FALSE;
    }
    memcpy(&header, cursor, sizeof(header));
    if (header.magic != ROLLBACK_GAME_MAGIC
        || header.version != ROLLBACK_GAME_VERSION
        || header.total_size != required
        || header.game_stat_size != sizeof(GameStat)
        || header.system_state_size != sizeof(SystemState)
        || header.player_state_size != sizeof(PlayerState)
        || header.player_config_size != sizeof(PlayerConfig)
        || header.player_count != 4
        || header.system_flags_size != sizeof(systemFlags)
        || header.system_flags_size != PartyBoard_RollbackSysFlagsSizeGet()
        || header.reserved != 0) {
        return FALSE;
    }
    cursor += sizeof(header);
    end = (const u8 *)source + size;
#define ROLLBACK_GAME_READ(value) do { \
    if ((size_t)(end - cursor) < sizeof(value)) return FALSE; \
    memcpy(&(value), cursor, sizeof(value)); cursor += sizeof(value); \
} while (0)
    ROLLBACK_GAME_READ(gameStatDefault);
    ROLLBACK_GAME_READ(gameStat);
    ROLLBACK_GAME_READ(system);
    ROLLBACK_GAME_READ(players);
    ROLLBACK_GAME_READ(playerConfigs);
    ROLLBACK_GAME_READ(language);
    ROLLBACK_GAME_READ(languageSave);
    ROLLBACK_GAME_READ(systemFlags);
#undef ROLLBACK_GAME_READ
    if (cursor != end) {
        return FALSE;
    }

    /* All bytes have been validated and copied away from the caller's buffer.
     * From this point the commit cannot fail or partially reject a snapshot. */
    GWGameStatDefault = gameStatDefault;
    GWGameStat = gameStat;
    GWSystem = system;
    memcpy(GWPlayer, players, sizeof(GWPlayer));
    memcpy(GWPlayerCfg, playerConfigs, sizeof(GWPlayerCfg));
    GwLanguage = language;
    GwLanguageSave = languageSave;
    PartyBoard_RollbackSysFlagsLoad(systemFlags);
    return TRUE;
}

BOOL PartyBoard_RollbackGameSelfTest(void)
{
    const size_t size = PartyBoard_RollbackGameSize();
    u8 *original = (u8 *)malloc(size);
    u8 *changed = (u8 *)malloc(size);
    u8 *observed = (u8 *)malloc(size);
    u8 *cursor;
    BOOL ok = original != NULL && changed != NULL && observed != NULL;
    BOOL originalSaved = FALSE;
    BOOL restored = FALSE;

    if (ok) {
        originalSaved = PartyBoard_RollbackGameSave(original, size);
        ok = originalSaved;
    }
    if (ok) {
        memcpy(changed, original, size);
        cursor = changed + sizeof(RollbackGameHeader);
#define ROLLBACK_GAME_MUTATE(type) do { *cursor ^= 0x5Au; cursor += sizeof(type); } while (0)
        ROLLBACK_GAME_MUTATE(GameStat); /* GWGameStatDefault */
        ROLLBACK_GAME_MUTATE(GameStat); /* GWGameStat */
        ROLLBACK_GAME_MUTATE(SystemState);
        *cursor ^= 0x5Au; cursor += sizeof(GWPlayer);
        *cursor ^= 0x5Au; cursor += sizeof(GWPlayerCfg);
        ROLLBACK_GAME_MUTATE(s16); /* GwLanguage */
        ROLLBACK_GAME_MUTATE(s16); /* GwLanguageSave */
#undef ROLLBACK_GAME_MUTATE
        *cursor ^= 0x5Au; /* _Sys_Flag */
        ok = PartyBoard_RollbackGameLoad(changed, size)
            && PartyBoard_RollbackGameSave(observed, size)
            && memcmp(observed, changed, size) == 0;
    }
    if (ok) {
        original[0] ^= 1;
        ok = !PartyBoard_RollbackGameLoad(original, size)
            && PartyBoard_RollbackGameSave(observed, size)
            && memcmp(observed, changed, size) == 0;
        original[0] ^= 1;
    }
    if (ok) {
        ok = !PartyBoard_RollbackGameLoad(original, size - 1)
            && PartyBoard_RollbackGameSave(observed, size)
            && memcmp(observed, changed, size) == 0;
    }
    if (originalSaved) {
        restored = PartyBoard_RollbackGameLoad(original, size);
        if (restored && observed != NULL) {
            restored = PartyBoard_RollbackGameSave(observed, size)
                && memcmp(observed, original, size) == 0;
        }
    }
    free(original);
    free(changed);
    free(observed);
    return ok && restored;
}
#endif

static inline void GWErase(void)
{
    memset(GWPlayerCfg, 0, sizeof(GWPlayerCfg));
    memset(GWPlayer, 0, sizeof(GWPlayer));
    memset(&GWSystem, 0, sizeof(GWSystem));
}

static inline void InitPlayerConfig(void)
{
    PlayerConfig *config;
    s32 i;
    for (i = 0; i < 4; i++) {
        config = &GWPlayerCfg[i];
        config->character = i;
        config->pad_idx = i;
        config->diff = 0;
        config->group = i;
        if (!HuPadStatGet(i)) {
            config->iscom = 0;
        }
        else {
            config->iscom = 1;
        }
    }
}

// TODO: get these properly declared somewhere
extern void GWRumbleSet(s32 value);
extern void GWMGExplainSet(s32 value);
extern void GWMGShowComSet(s32 value);
extern void GWMessSpeedSet(s32 value);
extern void GWSaveModeSet(s32 value);

static inline void ResetBoardSettings(void)
{
    GWRumbleSet(1);
    GWMGExplainSet(1);
    GWMGShowComSet(1);
    GWMessSpeedSet(1);
    GWSaveModeSet(0);
}

void GWInit(void)
{
    GWGameStatReset();
    _InitFlag();
    GWErase();
    InitPlayerConfig();
#if VERSION_JP
    GWGameStat.language = 0;
#elif VERSION_ENG && !defined(TARGET_PC)
    GWGameStat.language = 1;
#else
    GWLanguageSet(GwLanguage);
#endif
    ResetBoardSettings();
}

static inline void ResetMGRecord(GameStat *game_stat)
{
    game_stat->mg_record[0] = 300 * REFRESH_RATE;
    game_stat->mg_record[1] = 80;
    game_stat->mg_record[2] = 60 * REFRESH_RATE;
    game_stat->mg_record[3] = 120 * REFRESH_RATE;
    game_stat->mg_record[4] = 0;
    game_stat->mg_record[5] = 60 * REFRESH_RATE;
    game_stat->mg_record[6] = 300 * REFRESH_RATE;
    game_stat->mg_record[7] = 300 * REFRESH_RATE;
    game_stat->mg_record[8] = 300 * REFRESH_RATE;
    game_stat->mg_record[9] = 0;
    game_stat->mg_record[10] = 5 * REFRESH_RATE;
    game_stat->mg_record[11] = 0;
    game_stat->mg_record[12] = 0;
    game_stat->mg_record[13] = 0;
    game_stat->mg_record[14] = 0;
}

static inline void ResetBoardRecord(GameStat *game_stat)
{
    s32 i;
    s32 j;

    for (i = 0; i < 9; i++) {
        for (j = 0; j < 8; j++) {
            game_stat->board_win_count[i][j] = 0;
        }
        game_stat->board_play_count[i] = 0;
        game_stat->board_max_stars[i] = 0;
        game_stat->board_max_coins[i] = 0;
    }
}

static inline void ResetPresent(GameStat *game_stat)
{
    s32 i;
    for (i = 0; i < 60; i++) {
        game_stat->present[i] = 0;
    }
    (void)i; // HACK to match GWResetGameStat
}

static inline void ResetFlag(GameStat *game_stat)
{
    game_stat->story_continue = 0;
    game_stat->party_continue = 0;
    game_stat->open_w06 = 0;
    game_stat->veryHardUnlock = 0;
    game_stat->customPackEnable = 0;
    game_stat->musicAllF = 0;
}

static inline void ResetPauseConfig(GameStat *game_stat)
{
    game_stat->story_pause.explain_mg = game_stat->party_pause.explain_mg = 1;
    game_stat->story_pause.show_com_mg = game_stat->party_pause.show_com_mg = 1;
    game_stat->story_pause.mg_list = game_stat->party_pause.mg_list = 0;
    game_stat->story_pause.mess_speed = game_stat->party_pause.mess_speed = 1;
    game_stat->story_pause.save_mode = game_stat->party_pause.save_mode = 0;
}

void GWGameStatReset(void)
{
    GameStat *game_stat = &GWGameStatDefault;
    memset(game_stat, 0, sizeof(GameStat));
    game_stat->unk_00 = 0;
#if VERSION_JP
    game_stat->language = 0;
#elif VERSION_ENG && !defined(TARGET_PC)
    game_stat->language = 1;
#else
    game_stat->language = GwLanguage;
#endif
    game_stat->sound_mode = 1;
    game_stat->rumble = 1;
    game_stat->total_stars = 0;
    game_stat->create_time = 0;
    game_stat->mg_custom[0] = 0;
    game_stat->mg_custom[1] = 0;
    game_stat->mg_avail[0] = 0;
    game_stat->mg_avail[1] = 0;
    ResetMGRecord(game_stat);
    ResetBoardRecord(game_stat);
    ResetPresent(game_stat);
    ResetFlag(game_stat);
    ResetPauseConfig(game_stat);
    memcpy(&GWGameStat, &GWGameStatDefault, sizeof(GameStat));
    ResetBoardSettings();
}

s32 GWMessDelayGet(void)
{
#if VERSION_NTSC
    if (GWSystem.mess_delay > 48) {
        GWSystem.mess_speed = 1;
        GWSystem.mess_delay = 32;
    }
#else
    if (GWSystem.mess_delay > 64) {
        GWSystem.mess_speed = 1;
        GWSystem.mess_delay = 48;
    }
#endif
    return GWSystem.mess_delay;
}

void GWMGRecordSet(s32 index, u32 value)
{
    if (!_CheckFlag(FLAG_ID_MAKE(1, 12))) {
        GWGameStat.mg_record[index] = value;
    }
}

u32 GWMGRecordGet(s32 index)
{
    return GWGameStat.mg_record[index];
}

void GWCharColorGet(s32 character, GXColor *color)
{
    GXColor char_color[] = { { 227, 67, 67, 255 }, { 68, 67, 227, 255 }, { 241, 158, 220, 255 }, { 67, 228, 68, 255 }, { 138, 60, 180, 255 },
        { 146, 85, 55, 255 }, { 227, 228, 68, 255 }, { 40, 40, 40, 255 } };
    *color = char_color[character];
}

void GWBoardPlayCountSet(s32 board, u8 value)
{
    if (value > 99) {
        value = 99;
    }
    GWGameStat.board_play_count[board] = value;
}

void GWBoardPlayCountAdd(s32 board, u8 value)
{
    value += GWGameStat.board_play_count[board];
    if (value > 99) {
        value = 99;
    }
    GWGameStat.board_play_count[board] = value;
}

u8 GWBoardPlayCountGet(s32 board)
{
    return GWGameStat.board_play_count[board];
}

void GWBoardMaxStarsSet(s32 board, s16 value)
{
    GWGameStat.board_max_stars[board] = value;
}

s32 GWBoardMaxStarsGet(s32 board)
{
    return GWGameStat.board_max_stars[board];
}

void GWBoardMaxCoinsSet(s32 board, s32 value)
{
    GWGameStat.board_max_coins[board] = value;
}

s32 GWBoardMaxCoinsGet(s32 board)
{
    return GWGameStat.board_max_coins[board];
}

s32 GWBoardWinCountInc(s32 character, s32 board)
{
    s32 win_count = GWGameStat.board_win_count[board][character] + 1;
    if (win_count > 99) {
        win_count = 99;
    }
    GWGameStat.board_win_count[board][character] = win_count;
    return win_count;
}

s32 GWBoardWinCountGet(s32 character, s32 board)
{
    return GWGameStat.board_win_count[board][character];
}

void GWBoardWinCountSet(s32 character, s32 board, s32 value)
{
    GWGameStat.board_win_count[board][character] = value;
}

s32 GWMGAvailGet(s32 id)
{
    s32 word;
    s32 bit;
    id -= 401;
#ifdef TARGET_PC
    if ((u32)id < 64U && partyboard_settings_unlock_all_minigames()) {
        return 1;
    }
#endif
    word = id >> 5;
    bit = id % 32;
    if (GWGameStat.mg_avail[word] & (1 << bit)) {
        return 1;
    }
    else {
        return 0;
    }
}

void GWMGAvailSet(s32 id)
{
    s32 word;
    s32 bit;
    id -= 401;
    word = id >> 5;
    bit = id % 32;
    GWGameStat.mg_avail[word] |= (1 << bit);
}

s32 GWMGCustomGet(s32 id)
{
    s32 word;
    s32 bit;
    id -= 401;
    word = id >> 5;
    bit = id % 32;
    if (GWGameStat.mg_custom[word] & (1 << bit)) {
        return 1;
    }
    else {
        return 0;
    }
}

void GWMGCustomSet(s32 id)
{
    s32 word;
    s32 bit;
    id -= 401;
    word = id >> 5;
    bit = id % 32;
    GWGameStat.mg_custom[word] |= (1 << bit);
}

void GWMGCustomReset(s32 id)
{
    s32 word;
    s32 bit;
    id -= 401;
    word = id >> 5;
    bit = id % 32;
    GWGameStat.mg_custom[word] &= ~(1 << bit);
}

s16 GWCoinsGet(s32 player)
{
    return GWPlayer[player].coins;
}

void GWCoinsSet(s32 player, s16 value)
{
    if (!_CheckFlag(FLAG_ID_MAKE(1, 12))) {
        if (value < 0) {
            value = 0;
        }
        if (value > 999) {
            value = 999;
        }
        if (value > GWPlayer[player].coins_max) {
            GWPlayer[player].coins_max = value;
        }
        GWPlayer[player].coins = value;
    }
}

void GWCoinsAdd(s32 player, s16 amount)
{
    GWCoinsSet(player, GWPlayer[player].coins + amount);
}

void GWStarsSet(s32 player, s16 value)
{
    if (value < 0) {
        value = 0;
    }
    if (value > 999) {
        value = 999;
    }
    if (value > GWPlayer[player].stars_max) {
        GWPlayer[player].stars_max = value;
    }
    GWPlayer[player].stars = value;
}

void GWStarsAdd(s32 player, s16 amount)
{
    GWStarsSet(player, GWPlayer[player].stars + amount);
}

s32 GWStarsGet(s32 player)
{
    return GWPlayer[player].stars;
}

void GWTotalStarsSet(s16 value)
{
    if (value < 0) {
        value = 0;
    }
    if (value > 10000) {
        value = 10000;
    }
    GWGameStat.total_stars = value;
}

void GWTotalStarsAdd(s16 amount)
{
    GWTotalStarsSet(GWGameStat.total_stars + amount);
}

u16 GWTotalStarsGet(void)
{
    return GWGameStat.total_stars;
}

#ifdef TARGET_PC
#include "port/rollback_scene.h"
bool PartyBoard_RollbackGameRegions(PartyBoardRollbackRegionSink sink, void *context)
{
    if (!sink) return false;
#define REGION(value) if (!sink(context, &(value), sizeof(value))) return false;
    REGION(GWGameStatDefault) REGION(GWGameStat) REGION(GWSystem)
    REGION(GWPlayer) REGION(GWPlayerCfg) REGION(GwLanguage) REGION(GwLanguageSave)
#undef REGION
    return PartyBoard_RollbackFlagRegions(sink, context);
}
#endif
