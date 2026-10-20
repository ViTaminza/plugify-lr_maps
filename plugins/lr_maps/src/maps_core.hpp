#pragma once
// Engine-free core of the lr_maps port (the Pisex LR module "[LR] ExStats Maps", the original's
// lr_maps.cpp): the per-map counters of OnPlayerLoadedHook and the game events, and the
// `<lr_table>_maps` table (steam + name_map key, playtime/kills/deaths/round/bomb/hostage counters).
//
// The engine-side half (the game events, the disconnect listener, the per-second tick, the LR core
// hooks, plgdb) lives in src/plugin.cpp; everything here is testable against SQLite and MySQL.

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include <plgdb/db.hpp>

namespace lr_maps {

inline constexpr int kMaxSlots = 64;

// The original's per-slot counters (g_iMapCount_* + g_bPlayerActive), one row of the `_maps` table
// per (steam, name_map).
struct SlotState {
	bool active = false;       // g_bPlayerActive (the row was loaded for this map)
	int play = 0;              // countplays
	int kills = 0;
	int deaths = 0;
	int rounds_overall = 0;
	int rounds_ct = 0;         // g_iMapCount_Round[slot][0]
	int rounds_t = 0;          // g_iMapCount_Round[slot][1]
	int bomb_planted = 0;
	int bomb_defused = 0;
	int hostage_rescued = 0;
	int hostage_killed = 0;
	int time = 0;              // playtime (seconds of this session on the map)
};

using State = std::array<SlotState, kMaxSlots>;

// --- the game-event counters (the Hooks() switch of the original) --------------------------------
// Every function takes the slot values as the events carry them; out-of-range indexes are dropped
// (the original's guards allowed the one-past-the-end index 64).

// player_death: the killer's kill and the victim's death (skipped for a suicide).
void OnPlayerDeath(State& state, int attacker, int victim);
// round_end for one (loaded, authenticated) player of |player_team|: the round counters of the
// original, including the missing `break` after the CT case (a CT win counted the round for both
// sides).
void CountRound(SlotState& slot, int player_team, int winner_team);
void OnBombPlanted(State& state, int client);
void OnBombDefused(State& state, int client);
void OnHostageKilled(State& state, int client);
void OnHostageRescued(State& state, int client);
// The per-second GameFrame tick (the caller checks CheckCountPlayers first): every active slot's
// playtime on the map.
void TickSecond(State& state);

// The OnResetLoadedHook() local state: countplays = 1, the counters zeroed, the slot active.
void ResetSlot(SlotState& slot);

// --- SQL -----------------------------------------------------------------------------------------
// Statements are built here and executed by the plugin (async, on plgdb's worker) or by the tests
// (QuerySync). Values are always bound parameters, never interpolated.

// `<base>_maps` (the original's g_sTableName + "_maps").
std::string TableName(std::string_view base);

std::string CreateTableSql(std::string_view table, plgdb::Driver driver);

struct Statement {
	std::string sql;
	plgdb::Params params;
};

// OnPlayerLoadedHook(): SELECT of the eleven counters for (steam, map).
Statement LoadStatement(std::string_view table, std::string_view steamid, std::string_view map);
// The original's INSERT of a player without a row for this map: countplays = 1, everything else 0.
Statement InsertStatement(std::string_view table, std::string_view steamid, std::string_view map);

// The callback of OnPlayerLoadedHook: an existing row counts the play (countplays + 1) and copies
// the counters; the slot becomes active. Returns false when the result has no row (the caller then
// ResetSlot()s and inserts).
bool ApplyLoaded(SlotState& slot, const plgdb::Result& result);

// SaveDataPlayer(): UPDATE of the eleven counters WHERE steam AND name_map.
Statement SaveStatement(std::string_view table, const SlotState& slot, std::string_view steamid,
						std::string_view map);

// OnResetLoadedHook(): UPDATE of every counter to 0 WHERE steam (the whole player, all maps - the
// original had no name_map in its WHERE clause).
Statement ResetStatement(std::string_view table, std::string_view steamid);

} // namespace lr_maps
