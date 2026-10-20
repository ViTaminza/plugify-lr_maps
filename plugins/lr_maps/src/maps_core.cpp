#include "maps_core.hpp"

#include <optional>
#include <utility>

namespace lr_maps {
namespace {

bool InRange(int slot) {
	return slot >= 0 && slot < kMaxSlots;
}

} // namespace

void OnPlayerDeath(State& state, int attacker, int victim) {
	// The original's range test (0..64) plus the attacker != victim guard.
	if (!InRange(attacker) || !InRange(victim) || attacker == victim) {
		return;
	}
	state[static_cast<std::size_t>(attacker)].kills++;
	state[static_cast<std::size_t>(victim)].deaths++;
}

void CountRound(SlotState& slot, int player_team, int winner_team) {
	slot.rounds_overall++;
	if (player_team != winner_team) {
		return;
	}
	switch (winner_team) {
	case 3:
		// The original's CT case falls through into the T one (no break): a CT round counts for
		// both columns.
		slot.rounds_ct++;
		[[fallthrough]];
	case 2:
		slot.rounds_t++;
		break;
	default:
		break;
	}
}

void OnBombPlanted(State& state, int client) {
	if (InRange(client)) {
		state[static_cast<std::size_t>(client)].bomb_planted++;
	}
}

void OnBombDefused(State& state, int client) {
	if (InRange(client)) {
		state[static_cast<std::size_t>(client)].bomb_defused++;
	}
}

void OnHostageKilled(State& state, int client) {
	if (InRange(client)) {
		state[static_cast<std::size_t>(client)].hostage_killed++;
	}
}

void OnHostageRescued(State& state, int client) {
	if (InRange(client)) {
		state[static_cast<std::size_t>(client)].hostage_rescued++;
	}
}

void TickSecond(State& state) {
	for (SlotState& slot : state) {
		if (slot.active) {
			slot.time++; // g_iMapCount_Time[slot] of the original
		}
	}
}

void ResetSlot(SlotState& slot) {
	slot.play = 1;
	slot.kills = 0;
	slot.deaths = 0;
	slot.rounds_overall = 0;
	slot.rounds_ct = 0;
	slot.rounds_t = 0;
	slot.time = 0;
	slot.bomb_planted = 0;
	slot.bomb_defused = 0;
	slot.hostage_rescued = 0;
	slot.hostage_killed = 0;
	slot.active = true;
}

std::string TableName(std::string_view base) {
	return std::string(base) + "_maps";
}

std::string CreateTableSql(std::string_view table, plgdb::Driver driver) {
	const std::string name(table);
	if (driver == plgdb::Driver::MySQL) {
		// The CREATE TABLE of the original, byte for byte.
		return "CREATE TABLE IF NOT EXISTS `" + name + "` ("
			   "`steam` varchar(32) NOT NULL default '', "
			   "`name_map` varchar(128) NOT NULL default '', "
			   "`countplays` int NOT NULL DEFAULT 0, "
			   "`kills` int NOT NULL DEFAULT 0, "
			   "`deaths` int NOT NULL DEFAULT 0, "
			   "`rounds_overall` int NOT NULL DEFAULT 0, "
			   "`rounds_ct` int NOT NULL DEFAULT 0, "
			   "`rounds_t` int NOT NULL DEFAULT 0, "
			   "`bomb_planted` int NOT NULL DEFAULT 0, "
			   "`bomb_defused` int NOT NULL DEFAULT 0, "
			   "`hostage_rescued` int NOT NULL DEFAULT 0, "
			   "`hostage_killed` int NOT NULL DEFAULT 0, "
			   "`playtime` int NOT NULL DEFAULT 0, "
			   "PRIMARY KEY (`steam`, `name_map`)"
			   ") CHARSET = utf8 COLLATE utf8_general_ci;";
	}
	// SQLite has no CHARSET/COLLATE clauses; the columns and the primary key are the same.
	return "CREATE TABLE IF NOT EXISTS `" + name + "` ("
		   "`steam` TEXT NOT NULL DEFAULT '', "
		   "`name_map` TEXT NOT NULL DEFAULT '', "
		   "`countplays` INTEGER NOT NULL DEFAULT 0, "
		   "`kills` INTEGER NOT NULL DEFAULT 0, "
		   "`deaths` INTEGER NOT NULL DEFAULT 0, "
		   "`rounds_overall` INTEGER NOT NULL DEFAULT 0, "
		   "`rounds_ct` INTEGER NOT NULL DEFAULT 0, "
		   "`rounds_t` INTEGER NOT NULL DEFAULT 0, "
		   "`bomb_planted` INTEGER NOT NULL DEFAULT 0, "
		   "`bomb_defused` INTEGER NOT NULL DEFAULT 0, "
		   "`hostage_rescued` INTEGER NOT NULL DEFAULT 0, "
		   "`hostage_killed` INTEGER NOT NULL DEFAULT 0, "
		   "`playtime` INTEGER NOT NULL DEFAULT 0, "
		   "PRIMARY KEY (`steam`, `name_map`)"
		   ");";
}

Statement LoadStatement(std::string_view table, std::string_view steamid, std::string_view map) {
	Statement statement;
	statement.sql = "SELECT `countplays`, `kills`, `deaths`, `rounds_overall`, `rounds_ct`, "
					"`rounds_t`, `bomb_planted`, `bomb_defused`, `hostage_rescued`, `hostage_killed`, "
					"`playtime` FROM `" + std::string(table) +
					"` WHERE `steam` = ? AND `name_map` = ?;";
	statement.params.emplace_back(std::string(steamid));
	statement.params.emplace_back(std::string(map));
	return statement;
}

Statement InsertStatement(std::string_view table, std::string_view steamid, std::string_view map) {
	// The original inserted the freshly defaulted counters (countplays = 1, the rest 0).
	Statement statement;
	statement.sql = "INSERT INTO `" + std::string(table) +
					"` (`steam`, `name_map`, `countplays`, `kills`, `deaths`, `rounds_overall`, "
					"`rounds_ct`, `rounds_t`, `bomb_planted`, `bomb_defused`, `hostage_rescued`, "
					"`hostage_killed`, `playtime`) VALUES (?, ?, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);";
	statement.params.emplace_back(std::string(steamid));
	statement.params.emplace_back(std::string(map));
	return statement;
}

bool ApplyLoaded(SlotState& slot, const plgdb::Result& result) {
	const std::optional<std::size_t> play_column = result.ColumnIndex("countplays");
	if (!result.Ok() || result.RowCount() == 0 || !play_column.has_value()) {
		return false;
	}
	const std::size_t base = *play_column;
	const std::size_t row = 0;
	const auto read = [&](std::size_t index) {
		return static_cast<int>(result.GetInt(row, base + index, 0));
	};
	slot.play = read(0) + 1; // the original counts the load itself
	slot.kills = read(1);
	slot.deaths = read(2);
	slot.rounds_overall = read(3);
	slot.rounds_ct = read(4);
	slot.rounds_t = read(5);
	slot.bomb_planted = read(6);
	slot.bomb_defused = read(7);
	slot.hostage_rescued = read(8);
	slot.hostage_killed = read(9);
	slot.time = read(10);
	slot.active = true;
	return true;
}

Statement SaveStatement(std::string_view table, const SlotState& slot, std::string_view steamid,
						std::string_view map) {
	Statement statement;
	statement.sql = "UPDATE `" + std::string(table) +
					"` SET `countplays` = ?, `kills` = ?, `deaths` = ?, `rounds_overall` = ?, "
					"`rounds_ct` = ?, `rounds_t` = ?, `bomb_planted` = ?, `bomb_defused` = ?, "
					"`hostage_rescued` = ?, `hostage_killed` = ?, `playtime` = ? "
					"WHERE `steam` = ? AND `name_map` = ?;";
	statement.params.emplace_back(static_cast<int64_t>(slot.play));
	statement.params.emplace_back(static_cast<int64_t>(slot.kills));
	statement.params.emplace_back(static_cast<int64_t>(slot.deaths));
	statement.params.emplace_back(static_cast<int64_t>(slot.rounds_overall));
	statement.params.emplace_back(static_cast<int64_t>(slot.rounds_ct));
	statement.params.emplace_back(static_cast<int64_t>(slot.rounds_t));
	statement.params.emplace_back(static_cast<int64_t>(slot.bomb_planted));
	statement.params.emplace_back(static_cast<int64_t>(slot.bomb_defused));
	statement.params.emplace_back(static_cast<int64_t>(slot.hostage_rescued));
	statement.params.emplace_back(static_cast<int64_t>(slot.hostage_killed));
	statement.params.emplace_back(static_cast<int64_t>(slot.time));
	statement.params.emplace_back(std::string(steamid));
	statement.params.emplace_back(std::string(map));
	return statement;
}

Statement ResetStatement(std::string_view table, std::string_view steamid) {
	Statement statement;
	statement.sql =
		"UPDATE `" + std::string(table) +
		"` SET `countplays` = 0, `kills` = 0, `deaths` = 0, `rounds_overall` = 0, "
		"`rounds_ct` = 0, `rounds_t` = 0, `bomb_planted` = 0, `bomb_defused` = 0, "
		"`hostage_rescued` = 0, `hostage_killed` = 0, `playtime` = 0 WHERE `steam` = ?;";
	statement.params.emplace_back(std::string(steamid));
	return statement;
}

} // namespace lr_maps
