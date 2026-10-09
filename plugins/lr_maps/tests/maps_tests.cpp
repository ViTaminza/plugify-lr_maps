// Tests for plugins/lr_maps/src/maps_core.hpp (engine-free core of the ported
// "[LR] ExStats Maps" module).
//
//   * the event counters of the original's Hooks() switch (player_death, round_end with the
//     missing-break quirk, bomb/hostage, the per-second playtime tick),
//   * the OnPlayerLoadedHook load semantics (countplays + 1, a missing row defaulting to 1) and
//     OnResetLoadedHook,
//   * the SQL layer against SQLite ":memory:" always and against MySQL when MYSQL_HOST is set
//     (test.sh provides MYSQL_HOST/MYSQL_PORT/MYSQL_USER/MYSQL_PASSWORD/MYSQL_DATABASE). Rows
//     created in MySQL are deleted at the end.

#include <plg/plugin.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

#include "maps_core.hpp"

// A real plugin gets these from PLUGIFY_PLUGIN; the test leaves them null.
namespace plg {
GetBaseDirFn GetBaseDir{nullptr};
GetExtensionsDirFn GetExtensionsDir{nullptr};
GetConfigsDirFn GetConfigsDir{nullptr};
GetDataDirFn GetDataDir{nullptr};
GetLogsDirFn GetLogsDir{nullptr};
GetCacheDirFn GetCacheDir{nullptr};
IsLoadedFn IsLoaded{nullptr};
LogFn Log{nullptr};
BeginZoneFn BeginZone{nullptr};
EndZoneFn EndZone{nullptr};
} // namespace plg

namespace {

int g_checks = 0;
int g_failures = 0;
std::string_view g_section = "?";

void Section(std::string_view name) {
	g_section = name;
	std::printf("-- %.*s\n", static_cast<int>(name.size()), name.data());
}

#define CHECK(expr)                                                                            \
	do {                                                                                       \
		++g_checks;                                                                            \
		if (!(expr)) {                                                                         \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: %s\n", static_cast<int>(g_section.size()),         \
						g_section.data(), __FILE__, __LINE__, #expr);                          \
		}                                                                                      \
	} while (false)

#define CHECK_EQ_INT(actual, expected)                                                         \
	do {                                                                                       \
		++g_checks;                                                                            \
		const long long a_ = static_cast<long long>(actual);                                   \
		const long long e_ = static_cast<long long>(expected);                                 \
		if (a_ != e_) {                                                                        \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: got %lld, expected %lld\n",                        \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,       \
						__LINE__, a_, e_);                                                     \
		}                                                                                      \
	} while (false)

#define CHECK_EQ_STR(actual, expected)                                                         \
	do {                                                                                       \
		++g_checks;                                                                            \
		const std::string a_{std::string_view(actual)};                                        \
		const std::string e_{std::string_view(expected)};                                      \
		if (a_ != e_) {                                                                        \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: got \"%s\", expected \"%s\"\n",                    \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,       \
						__LINE__, a_.c_str(), e_.c_str());                                     \
		}                                                                                      \
	} while (false)

using namespace lr_maps;

// --- the event counters ---------------------------------------------------------------------------

void TestEvents() {
	Section("event counters");
	State state{};
	state[3].active = true;

	// player_death: killer gets a kill, victim a death; suicides and out-of-range indexes dropped.
	OnPlayerDeath(state, 3, 7);
	CHECK_EQ_INT(state[3].kills, 1);
	CHECK_EQ_INT(state[7].deaths, 1);
	OnPlayerDeath(state, 5, 5); // suicide: skipped
	CHECK_EQ_INT(state[5].kills, 0);
	OnPlayerDeath(state, -1, 7); // world: skipped
	OnPlayerDeath(state, 64, 7); // the original's <= 64 guard would have written out of bounds
	CHECK_EQ_INT(state[7].deaths, 1);

	// round_end: the overall counter for the player, the team column only on a matching winner.
	CountRound(state[3], /*player_team=*/2, /*winner_team=*/2);
	CHECK_EQ_INT(state[3].rounds_overall, 1);
	CHECK_EQ_INT(state[3].rounds_t, 1);
	CHECK_EQ_INT(state[3].rounds_ct, 0);
	// A CT win counts for both columns (the original's missing break).
	CountRound(state[3], /*player_team=*/3, /*winner_team=*/3);
	CHECK_EQ_INT(state[3].rounds_overall, 2);
	CHECK_EQ_INT(state[3].rounds_ct, 1);
	CHECK_EQ_INT(state[3].rounds_t, 2);
	// A loss only counts the overall round.
	CountRound(state[3], /*player_team=*/2, /*winner_team=*/3);
	CHECK_EQ_INT(state[3].rounds_overall, 3);
	CHECK_EQ_INT(state[3].rounds_ct, 1);
	CHECK_EQ_INT(state[3].rounds_t, 2);

	// bomb / hostages (userid indexes).
	OnBombPlanted(state, 3);
	OnBombDefused(state, 4);
	OnHostageKilled(state, 3);
	OnHostageRescued(state, 3);
	CHECK_EQ_INT(state[3].bomb_planted, 1);
	CHECK_EQ_INT(state[4].bomb_defused, 1);
	CHECK_EQ_INT(state[3].hostage_killed, 1);
	CHECK_EQ_INT(state[3].hostage_rescued, 1);
	OnBombPlanted(state, 64); // out of range: dropped (the original's <= 64 would be OOB)
	CHECK_EQ_INT(state[3].bomb_planted, 1);

	// The per-second tick counts only active slots.
	TickSecond(state);
	TickSecond(state);
	CHECK_EQ_INT(state[3].time, 2);
	CHECK_EQ_INT(state[4].time, 0);
}

void TestResetSlot() {
	Section("ResetSlot (OnResetLoadedHook)");
	SlotState slot;
	slot.time = 400;
	slot.kills = 12;
	slot.active = false;
	ResetSlot(slot);
	CHECK_EQ_INT(slot.time, 0);
	CHECK_EQ_INT(slot.kills, 0);
	CHECK_EQ_INT(slot.play, 1); // countplays = 1 of the local reset
	CHECK(slot.active);
	CHECK_EQ_STR(TableName("lvl_base"), "lvl_base_maps");
}

// --- SQL -----------------------------------------------------------------------------------------

plgdb::Config SqliteConfig() {
	plgdb::Config config;
	config.driver = plgdb::Driver::SQLite;
	config.sqlitePath = ":memory:";
	return config;
}

bool MySqlConfig(plgdb::Config& config, std::string& error) {
	const char* host = std::getenv("MYSQL_HOST");
	if (host == nullptr || *host == '\0') {
		return false;
	}
	config.driver = plgdb::Driver::MySQL;
	config.host = host;
	config.user = std::getenv("MYSQL_USER") != nullptr ? std::getenv("MYSQL_USER") : "";
	config.password = std::getenv("MYSQL_PASSWORD") != nullptr ? std::getenv("MYSQL_PASSWORD") : "";
	config.database = std::getenv("MYSQL_DATABASE") != nullptr ? std::getenv("MYSQL_DATABASE") : "";
	const char* port = std::getenv("MYSQL_PORT");
	if (port != nullptr && *port != '\0') {
		try {
			const int value = std::stoi(port);
			if (value < 1 || value > 65535) {
				error = "MYSQL_PORT out of range";
				return false;
			}
			config.port = static_cast<uint16_t>(value);
		} catch (const std::exception&) {
			error = "MYSQL_PORT is not a number";
			return false;
		}
	}
	return true;
}

void TestDatabase(const plgdb::Config& config, std::string_view label, const std::string& table,
				  bool cleanup) {
	Section(std::string("database ") + std::string(label));
	std::unique_ptr<plgdb::Database> db = plgdb::Database::Create(config);
	CHECK(db != nullptr);
	if (db == nullptr) {
		return;
	}
	CHECK(db->QuerySync(CreateTableSql(table, db->GetDriver()), {}).Ok());

	const std::string steam = "STEAM_1:1:90000001";
	const std::string map = "de_dust2";
	State state{};
	SlotState& slot = state[3];

	// A fresh player: the SELECT finds no row; the original defaults the counters (countplays = 1)
	// and inserts the row.
	{
		Statement load = LoadStatement(table, steam, map);
		const plgdb::Result result = db->QuerySync(load.sql, load.params);
		CHECK(result.Ok());
		CHECK_EQ_INT(result.RowCount(), 0);
		CHECK(!ApplyLoaded(slot, result));
		ResetSlot(slot);
		CHECK_EQ_INT(slot.play, 1);
		Statement insert = InsertStatement(table, steam, map);
		CHECK(db->QuerySync(insert.sql, insert.params).Ok());
	}

	// A session: a kill, a death of another player, a T round win, a bomb plant, 30 seconds.
	OnPlayerDeath(state, 3, 7);
	CountRound(slot, /*player_team=*/2, /*winner_team=*/2);
	OnBombPlanted(state, 3);
	for (int i = 0; i < 30; ++i) {
		TickSecond(state);
	}
	{
		Statement save = SaveStatement(table, slot, steam, map);
		CHECK(db->QuerySync(save.sql, save.params).Ok());
	}

	// The player reconnects on the same map: the row loads, countplays gets +1 for the load itself.
	{
		Statement load = LoadStatement(table, steam, map);
		const plgdb::Result result = db->QuerySync(load.sql, load.params);
		CHECK_EQ_INT(result.RowCount(), 1);
		SlotState reloaded{};
		CHECK(ApplyLoaded(reloaded, result));
		CHECK_EQ_INT(reloaded.play, 2); // 1 stored + 1 for this load
		CHECK_EQ_INT(reloaded.kills, 1);
		CHECK_EQ_INT(reloaded.rounds_overall, 1);
		CHECK_EQ_INT(reloaded.rounds_t, 1);
		CHECK_EQ_INT(reloaded.rounds_ct, 0);
		CHECK_EQ_INT(reloaded.bomb_planted, 1);
		CHECK_EQ_INT(reloaded.time, 30);
		CHECK(reloaded.active);
	}

	// A different map is a different row.
	{
		Statement load = LoadStatement(table, steam, "de_inferno");
		const plgdb::Result result = db->QuerySync(load.sql, load.params);
		CHECK_EQ_INT(result.RowCount(), 0);
	}

	// OnResetLoadedHook(): every counter zeroed for the whole SteamID (both maps would be hit; the
	// original's WHERE had no name_map).
	{
		Statement reset = ResetStatement(table, steam);
		CHECK(db->QuerySync(reset.sql, reset.params).Ok());
		Statement load = LoadStatement(table, steam, map);
		const plgdb::Result result = db->QuerySync(load.sql, load.params);
		CHECK_EQ_INT(result.RowCount(), 1);
		for (std::size_t column = 0; column < 11; ++column) {
			CHECK_EQ_INT(result.GetInt(0, column, -1), 0);
		}
	}

	if (cleanup) {
		db->QuerySync("DROP TABLE `" + table + "`;", {});
	}
}

} // namespace

int main() {
	std::printf("lr_maps_tests\n");

	std::random_device device;
	std::mt19937_64 rng(device());
	const std::string tag = std::to_string(rng() % 1000000ULL);

	TestEvents();
	TestResetSlot();

	TestDatabase(SqliteConfig(), "sqlite (:memory:)", "lvl_base_maps_sqlite", /*cleanup=*/false);

	plgdb::Config mysql;
	std::string error;
	if (MySqlConfig(mysql, error)) {
		std::printf("== mysql: %s:%u/%s\n", mysql.host.c_str(), mysql.port, mysql.database.c_str());
		TestDatabase(mysql, "mysql", "lvl_maps_test_" + tag, /*cleanup=*/true);
	} else if (!error.empty()) {
		std::printf("FATAL: %s\n", error.c_str());
		++g_failures;
	}

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
