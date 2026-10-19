#ifndef _GAMEWORK_DATA_H
#define _GAMEWORK_DATA_H

#include "dolphin.h"
#include "game/flag.h"
#include "version.h"

// HACK: to prevent prototype errors
extern void HuPadRumbleAllStop(void);

typedef struct player_config {
    s16 character;
    s16 pad_idx;
    s16 diff;
    s16 group;
    s16 iscom;
} PlayerConfig;

typedef struct system_state {
    /* 0x00 */ struct {
#ifdef TARGET_PC
        u8 PC_PADDING_00: 5;
        /* PartyBoard 8-player party (4 teams of 2, or Battle Royale for 8).
           Lives in what was padding, so older saves read it as 0 (off). */
        u8 eight_players : 1;
        u8 team : 1;
        u8 party : 1;
#else
        u8 party : 1;
        u8 team : 1;
#endif
    };
    /* 0x01 */ u8 diff_story;
    /* 0x02 */ union {
        struct {
#ifdef TARGET_PC
            u16 PC_PADDING_02: 7;
            u16 save_mode : 2;
            u16 mess_speed : 2;
            u16 mg_list : 2;
            u16 show_com_mg : 1;
            u16 explain_mg : 1;
            u16 bonus_star : 1;
#else
            u16 bonus_star : 1;
            u16 explain_mg : 1;
            u16 show_com_mg : 1;
            u16 mg_list : 2;
            u16 mess_speed : 2;
            u16 save_mode : 2;
#endif
        };
        u16 bitfield2;
    };
    /* 0x04 */ u8 turn;
    /* 0x05 */ u8 max_turn;
    /* 0x06 */ u8 star_flag;
    /* 0x07 */ u8 star_total;
    /* 0x08 */ struct {
#ifdef TARGET_PC
        u8 board : 5;
        u8 star_pos : 3;
#else
        u8 star_pos : 3;
        u8 board : 5;
#endif
    };
    /* 0x09 */ s8 last5_effect;
    /* 0x0A */ s8 player_curr;
    /* 0x0B */ u8 storyCharBit;
    /* 0x0C */ s8 storyChar;
    /* 0x0E */ s16 block_pos;
    /* 0x10 */ u8 ATTRIBUTE_ALIGN(4) board_data[32];
    /* 0x30 */ u8 mess_delay;
    /* 0x31 */ struct {
#ifdef TARGET_PC
        u8 bowser_event : 4;
        u8 bowser_loss : 4;
#else
        u8 bowser_loss : 4;
        u8 bowser_event : 4;
#endif
    };
    /* 0x32 */ s8 lucky_value;
    /* 0x34 */ u16 mg_next;
    /* 0x36 */ s16 mg_type;
    /* 0x38 */ u16 unk_38;
    /* 0x3A */ u8 flag[3][16];
    /* 0x6A */ u8 unk_6A[0x72];
} SystemState; // 8018fcf8, sizeof 0xDC

typedef struct player_state {
    /* 0x00 */ union {
        struct {
#ifdef TARGET_PC
            u16 ticket_player : 6;
            u16 draw_ticket : 1;
            u16 auto_size : 2;
            u16 character : 4;
            u16 com : 1;
            u16 diff : 2;
#else
            u16 diff : 2;
            u16 com : 1;
            u16 character : 4;
            u16 auto_size : 2;
            u16 draw_ticket : 1;
            u16 ticket_player : 6;
#endif
        };
        u16 bitfield1;
    };
    /* 0x02 */ struct {
#ifdef TARGET_PC
        /* PartyBoard 8-player party: the high bits of player_idx (8 players) and
           team (4 teams) live in what was padding, below the original fields, so
           the original bits keep their positions and older saves read them as 0. */
        u8 PC_PADDING_02: 2;
        u8 team_hi : 1;
        u8 player_idx_hi : 1;
        u8 player_idx : 2;
        u8 spark : 1;
        u8 team : 1;
#else
        u8 team : 1;
        u8 spark : 1;
        u8 player_idx : 2;
#endif
    };
    /* 0x03 */ s8 handicap;
    /* 0x04 */ s8 port;
    /* 0x05 */ s8 items[3];
    /* 0x08 */ union {
        struct {
#ifdef TARGET_PC
            u16 PC_PADDING_08: 1;
            /* PartyBoard 8-player party: high bits, as for player_idx/team above. */
            u16 team_backup_hi : 1;
            u16 rank_hi : 1;
            u16 team_backup : 1;
            u16 bowser_suit : 1;
            u16 rank : 2;
            u16 num_dice : 2;
            u16 size : 2;
            u16 show_next : 1;
            u16 jump : 1;
            u16 moving : 1;
            u16 color : 2;
#else
            u16 color : 2;
            u16 moving : 1;
            u16 jump : 1;
            u16 show_next : 1;
            u16 size : 2;
            u16 num_dice : 2;
            u16 rank : 2;
            u16 bowser_suit : 1;
            u16 team_backup : 1;
#endif
        };
        u16 bitfield3;
    };
    /* 0x0A */ s8 roll;
    /* 0x0C */ s16 space_curr;
    /* 0x0E */ s16 space_prev;
    /* 0x10 */ s16 space_next;
    /* 0x12 */ s16 space_shock;
    /* 0x14 */ s8 blue_count;
    /* 0x15 */ s8 red_count;
    /* 0x16 */ s8 question_count;
    /* 0x17 */ s8 fortune_count;
    /* 0x18 */ s8 bowser_count;
    /* 0x19 */ s8 battle_count;
    /* 0x1A */ s8 mushroom_count;
    /* 0x1B */ s8 warp_count;
    /* 0x1C */ s16 coins;
    /* 0x1E */ s16 coins_mg;
    /* 0x20 */ s16 coins_total;
    /* 0x22 */ s16 coins_max;
    /* 0x24 */ s16 coins_battle;
    /* 0x26 */ s16 coin_collect;
    /* 0x28 */ s16 coin_win;
    /* 0x2A */ s16 stars;
    /* 0x2C */ s16 stars_max;
#ifdef TARGET_PC
    /* PartyBoard 8-player party: the player's place in the turn order, plus one
       (0 = not set), fixed when the turn order is rolled. It travels with the
       player's record while the minigame groups are moved into slots 0-3, so the
       turn order can be restored and the cards keep their places. Unused (0) in
       4-player games. Was unused padding. */
    /* 0x2E */ u8 turn_order;
    /* 0x2F PartyBoard 8-player party: the player's seat (0-7, the slot they had
       when the game started, i.e. "Player N" in the menu) and own controller
       port (0-7), recorded when the game starts: bit 7 set, seat in bits 3-5,
       port in bits 0-2 (0 = not set). It travels with the record like
       turn_order, so a player keeps their controller while the minigame groups
       move them between slots (see GWPlayerOwnPortGet). Unused (0) in 4-player
       games. Was unused padding. */
    /* 0x2F */ u8 pad_info;
#else
    /* 0x2E */ char unk_2E[2];
#endif
} PlayerState; // size of 0x30

typedef struct pause_backup_config {
#ifdef TARGET_PC
    u8 save_mode : 2;
    u8 mess_speed : 2;
    u8 mg_list : 2;
    u8 show_com_mg : 1;
    u8 explain_mg : 1;
#else
    u8 explain_mg : 1;
    u8 show_com_mg : 1;
    u8 mg_list : 2;
    u8 mess_speed : 2;
    u8 save_mode : 2;
#endif
} PauseBackupConfig;

typedef struct game_stat {
    /* 0x0 */ s16 unk_00;
    /* 0x2 */ u8 language;
    /* 0x3 */ u8 sound_mode;
    /* 0x4 */ s8 rumble;
    /* 0x6 */ u16 total_stars;
    /* 0x8 */ OSTime create_time;
    /* 0x10 */ u32 mg_custom[2];
    /* 0x18 */ u32 mg_avail[2];
    /* 0x20 */ u32 mg_record[15];
    /* 0x5C */ u8 board_win_count[9][8];
    /* 0xA4 */ u8 board_play_count[9];
    /* 0xAE */ u16 board_max_stars[9];
    /* 0xC0 */ u16 board_max_coins[9];
    /* 0xD2 */ u8 present[60];
    /* 0x10E */ struct {
#ifdef TARGET_PC
        u8 PC_PADDING_10E: 2;
        u8 musicAllF : 1;
        u8 customPackEnable : 1;
        u8 veryHardUnlock : 1;
        u8 open_w06 : 1;
        u8 party_continue : 1;
        u8 story_continue : 1;
#else
        u8 story_continue : 1;
        u8 party_continue : 1;
        u8 open_w06 : 1;
        u8 veryHardUnlock : 1;
        u8 customPackEnable : 1;
        u8 musicAllF : 1;
#endif
    };
    /* 0x10F */ PauseBackupConfig story_pause;
    /* 0x110 */ PauseBackupConfig party_pause;
} GameStat;

SHARED_SYM extern s16 GwLanguage;
SHARED_SYM extern s16 GwLanguageSave;

/* Room for the PartyBoard 8-player party. Everything outside it still uses
   players 0-3 only; GWPlayerCount() says how many are in the game. */
#define GW_PARTY_PLAYER_MAX 8

SHARED_SYM extern PlayerConfig GWPlayerCfg[GW_PARTY_PLAYER_MAX];
SHARED_SYM extern PlayerState GWPlayer[GW_PARTY_PLAYER_MAX];
SHARED_SYM extern SystemState GWSystem;
SHARED_SYM extern GameStat GWGameStat;

static inline s32 GWPlayerCfgGroupGet(s32 player)
{
    return GWPlayerCfg[player].group;
}

/* Players in the current game: 8 in the PartyBoard 8-player party, else 4. */
static inline s32 GWPlayerCount(void)
{
#ifdef TARGET_PC
    return GWSystem.eight_players ? 8 : 4;
#else
    return 4;
#endif
}

static inline s32 GWTeamGet(void)
{
    return GWSystem.team;
}

static inline s32 GWMGTypeGet(void)
{
    return GWSystem.mg_type;
}

static inline void GWMGTypeSet(s32 type)
{
    GWSystem.mg_type = type;
}

static inline s32 GWPartyGet(void)
{
    return GWSystem.party;
}

static inline void GWLanguageSet(s16 language)
{
    GWGameStat.language = language;
}

static inline s32 GWLanguageGet(void)
{
    return GWGameStat.language;
}

static inline s32 GWRumbleGet(void)
{
    return GWGameStat.rumble;
}

static inline void GWRumbleSet(s32 value)
{
    GWGameStat.rumble = value;
    if (value == 0) {
        HuPadRumbleAllStop();
    }
}

static inline s32 GWBonusStarGet(void)
{
    return GWSystem.bonus_star;
}

static inline s32 GWMGExplainGet(void)
{
    return GWSystem.explain_mg;
}

static inline void GWMGExplainSet(s32 value)
{
    GWSystem.explain_mg = value;
}

static inline s32 GWMGShowComGet(void)
{
    return GWSystem.show_com_mg;
}

static inline void GWMGShowComSet(s32 value)
{
    GWSystem.show_com_mg = value;
}

static inline s32 GWMGListGet(void)
{
    if (GWSystem.mg_list == 3) {
        GWSystem.mg_list = 0;
    }
    return GWSystem.mg_list;
}

static inline void GWMGListSet(s32 value)
{
    GWSystem.mg_list = value;
}

static inline s32 GWMessSpeedGet(void)
{
    if (GWSystem.mess_speed == 3) {
        GWSystem.mess_speed = 1;
    }
    return GWSystem.mess_speed;
}

static inline void GWMessSpeedSet(s32 value)
{
#if VERSION_NTSC
    GWSystem.mess_speed = value;
    switch (value) {
        case 0:
            GWSystem.mess_delay = 16;
            break;

        case 2:
            GWSystem.mess_delay = 48;
            break;

        default:
            GWSystem.mess_delay = 32;
            break;
    }
#else
    GWSystem.mess_speed = value;
    switch (value) {
        case 0:
            GWSystem.mess_delay = 32;
            break;

        case 2:
            GWSystem.mess_delay = 64;
            break;

        case 1:
            GWSystem.mess_delay = 48;
            break;

        default:
            GWSystem.mess_delay = 120;
            break;
    }
#endif
}

static inline void GWSaveModeSet(s32 value)
{
    GWSystem.save_mode = value;
}

static inline s32 GWSaveModeGet(void)
{
    if (GWSystem.save_mode == 3) {
        GWSaveModeSet(1);
    }
    return GWSystem.save_mode;
}

static inline s32 GWBoardGet(void)
{
    return GWSystem.board;
}

static inline s32 GWPlayerCurrGet(void)
{
    return GWSystem.player_curr;
}

static inline s32 GWStoryCharGet(void)
{
    return GWSystem.storyChar;
}

static inline void GWStoryCharSet(s32 storyChar)
{
    GWSystem.storyChar = storyChar;
}

/* Full team number: 0-1 in the original game, 0-3 in the 8-player party's
   Tag Battle (4 teams of 2). Use these instead of the .team bit directly. */
static inline s32 GWPlayerTeamGet(s32 player)
{
#ifdef TARGET_PC
    return GWPlayer[player].team | (GWPlayer[player].team_hi << 1);
#else
    return GWPlayer[player].team;
#endif
}

static inline void GWPlayerTeamSet(s32 player, s32 team)
{
    GWPlayer[player].team = team & 1;
#ifdef TARGET_PC
    GWPlayer[player].team_hi = (team >> 1) & 1;
#endif
}

#ifdef TARGET_PC
/* 8-player Tag Battle: minigames know two sides (0 = the red team, 1 = the
   blue team, as the original's teams 1 and 2). Slots 0-3 hold two teams of
   the four: team 1 (red cards) always plays as side 0 and team 2 (blue cards)
   as side 1, so their colours match the board; teams 3 and 4 take the side
   that is left (both: the lower team number is side 0). */
static inline s32 GWEightMGSide(s32 player)
{
    s32 team = GWPlayerTeamGet(player);
    s32 other = -1;
    s32 i;
    for (i = 0; i < 4; i++) {
        if (GWPlayerTeamGet(i) != team) {
            other = GWPlayerTeamGet(i);
            break;
        }
    }
    if (team < 2) {
        return team;
    }
    if (other < 0) {
        return 0;
    }
    if (other < 2) {
        return 1 - other;
    }
    return team < other ? 0 : 1;
}
#endif

static inline s32 GWPlayerTeamBackupGet(s32 player)
{
#ifdef TARGET_PC
    return GWPlayer[player].team_backup | (GWPlayer[player].team_backup_hi << 1);
#else
    return GWPlayer[player].team_backup;
#endif
}

static inline void GWPlayerTeamBackupSet(s32 player, s32 team)
{
    GWPlayer[player].team_backup = team & 1;
#ifdef TARGET_PC
    GWPlayer[player].team_backup_hi = (team >> 1) & 1;
#endif
}

/* Ranking 0 (1st) to 3 in the original game, to 7 in the 8-player party. */
static inline s32 GWPlayerRankGet(s32 player)
{
#ifdef TARGET_PC
    return GWPlayer[player].rank | (GWPlayer[player].rank_hi << 2);
#else
    return GWPlayer[player].rank;
#endif
}

static inline void GWPlayerRankSet(s32 player, s32 rank)
{
    GWPlayer[player].rank = rank & 3;
#ifdef TARGET_PC
    GWPlayer[player].rank_hi = (rank >> 2) & 1;
#endif
}

/* The ranking folded onto the game's own 4-place tables (prizes, item odds...):
   in the 8-player party, 1st-2nd count as 1st, 3rd-4th as 2nd, and so on. */
static inline s32 GWPlayerRank4Get(s32 player)
{
    return GWPlayerCount() == 8 ? GWPlayerRankGet(player) / 2 : GWPlayerRankGet(player);
}

/* Index of a PlayerState in GWPlayer (0-7). */
static inline s32 GWPlayerStateIdxGet(const PlayerState *state)
{
#ifdef TARGET_PC
    return state->player_idx | (state->player_idx_hi << 2);
#else
    return state->player_idx;
#endif
}

static inline void GWPlayerStateIdxSet(PlayerState *state, s32 idx)
{
    state->player_idx = idx & 3;
#ifdef TARGET_PC
    state->player_idx_hi = (idx >> 2) & 1;
#endif
}

/* 8-player party: the player's place in the turn order (0-7), or -1 when not set. */
static inline s32 GWPlayerTurnOrderGet(const PlayerState *state)
{
#ifdef TARGET_PC
    return (s32)state->turn_order - 1;
#else
    return -1;
#endif
}

static inline void GWPlayerTurnOrderSet(PlayerState *state, s32 order)
{
#ifdef TARGET_PC
    state->turn_order = (u8)(order + 1);
#endif
}

/* 8-player party: the player's own controller port (0-7, the device that drives
   them), or -1 when not recorded. On the board GWPlayer[i].port is this port;
   while a minigame group is wired (board/main.c) port is the slot instead and
   this keeps the real one. */
static inline s32 GWPlayerOwnPortGet(const PlayerState *state)
{
#ifdef TARGET_PC
    return (state->pad_info & 0x80) ? (s32)(state->pad_info & 7) : -1;
#else
    return -1;
#endif
}

/* 8-player party: the player's seat (0-7, "Player N" at the start), or -1. */
static inline s32 GWPlayerSeatGet(const PlayerState *state)
{
#ifdef TARGET_PC
    return (state->pad_info & 0x80) ? (s32)((state->pad_info >> 3) & 7) : -1;
#else
    return -1;
#endif
}

static inline void GWPlayerPadInfoSet(PlayerState *state, s32 seat, s32 port)
{
#ifdef TARGET_PC
    state->pad_info = (u8)(0x80 | ((seat & 7) << 3) | (port & 7));
#endif
}

static inline s32 GWPlayerHandicapGet(s32 player)
{
    return GWPlayer[player].handicap;
}

static inline s32 GWLuckyValueGet(void)
{
    return GWSystem.lucky_value;
}

static inline void GWLuckyValueSet(s32 value)
{
    GWSystem.lucky_value = value;
}

static inline s16 GWPlayerCoinBattleGet(s32 player)
{
    return GWPlayer[player].coins_battle;
}

static inline s16 GWPlayerCoinCollectGet(s32 player)
{
    return GWPlayer[player].coin_collect;
}

static inline void GWPlayerCoinCollectSet(s32 player, s16 value)
{
    GWPlayer[player].coin_collect = value;
}

static inline s16 GWPlayerCoinWinGet(s32 player)
{
    return GWPlayer[player].coin_win;
}

static inline void GWPlayerCoinWinSet(s32 player, s16 value)
{
    if (_CheckFlag(FLAG_ID_MAKE(1, 12)) == 0) {
        GWPlayer[player].coin_win = value;
    }
}

#define GWPlayerCoinWinAdd(player, value) GWPlayerCoinWinSet((player), GWPlayerCoinWinGet((player)) + (value))
#define GWPlayerCoinCollectAdd(player, value) GWPlayerCoinCollectSet((player), (s32)GWPlayerCoinCollectGet((player)) + (value))

#endif
