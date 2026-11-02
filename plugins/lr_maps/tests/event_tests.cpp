// Engine test for the *real* map-statistics path of plugins/lr_maps/src/plugin.cpp.
//
// src/plugin.cpp is compiled into this executable together with the generated binding objects
// (s2sdk_bindings and lr_bindings). Their __s2sdk_* / __levels_ranks_* globals are plain variables
// that the plugify module normally fills; here the test fills them with fakes, drives the plugin
// through its own exported entry points (Plugify_PluginStart / _Update / _End) and fires the
// callbacks the plugin registered. The plugin's own SQL goes to a real, throw-away SQLite file, so
// the asynchronous load / insert / save path is exercised end to end.
//
// What it proves:
//   * "attacker" and every player-typed "userid" are read with s2sdk::GetEventPlayerSlot, not with
//     s2sdk::GetEventInt. The fake event answers deliberately different values (the slot 3 while
//     GetEventInt would answer 13), and the counters must follow the slot. A player_pawn field whose
//     primary accessor is absent is resolved by the map-gated GetEventPlayerPawn +
//     EntPointerToPlayerSlot fallback, never by GetEventInt. "winner" is a numeric field and is
//     read with GetEventInt.
//   * B1/B2: no event field or client is touched before the first map, none in the OnMapEnd window
//     (even while the host keeps pumping updates), and the next OnMapStart opens the gate again.
//   * the map name is captured only after the gate opens; OnMapEnd saves the rows of the map that
//     is going away with the cached ids and the captured name (changelevel keeps the clients, so
//     the next map's rows are loaded for the same slots);
//   * the disconnect save writes the counters of the slot before the cached id is dropped;
//   * a load that is still in flight when the slot is disconnected and reused (a fast reconnect)
//     cannot land in the new occupant's row - the next player's SELECT/INSERT wins;
//   * OnResetPlayerStats zeroes the database row and the local counters, and the next save writes
//     the local reset (countplays = 1) rather than the old values;
//   * OnPluginEnd removes every event hook, the map listeners and the disconnect listener, and
//     unregisters the levels_ranks callbacks (PITFALLS A7).
//
// Nothing here talks to a game server or a production database: every s2sdk/levels_ranks binding
// is a fake in this file and the only database is a temporary SQLite file. Run by ../test.sh
// (build/tests/lr_maps_event_tests).

#include <plg/plugin.hpp>
#include <plg/string.hpp>

#include <s2sdk/s2sdk.hpp>

#include <levels_ranks/levels_ranks.hpp>

#include <lr_api.hpp>

#include <plgdb/db.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

// The entry points PLUGIFY_PLUGIN (compiled into this executable from src/plugin.cpp) exports.
extern "C" plg::PluginResult Plugify_PluginStart();
extern "C" plg::PluginResult Plugify_PluginUpdate(std::chrono::milliseconds dt);
extern "C" plg::PluginResult Plugify_PluginEnd();

namespace {

int g_checks = 0;
int g_failures = 0;

void CheckImpl(bool condition, std::string_view what, int line) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL %s:%d: %.*s\n", __FILE__, line, static_cast<int>(what.size()), what.data());
	}
}

void CheckEqImpl(long long actual, long long expected, std::string_view what, int line) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL %s:%d: %.*s (got %lld, expected %lld)\n", __FILE__, line,
					static_cast<int>(what.size()), what.data(), actual, expected);
	}
}

#define CHECK(condition, message) CheckImpl((condition), (message), __LINE__)
#define CHECK_EQ(actual, expected, message) CheckEqImpl((actual), (expected), (message), __LINE__)

std::string ToStd(const plg::string& text) {
	return std::string(text.data(), text.size());
}

template <typename Container>
bool Contains(const Container& values, const std::string& wanted) {
	for (const auto& value : values) {
		if (value == wanted) {
			return true;
		}
	}
	return false;
}

// --- fakes: clients ------------------------------------------------------------------------------

constexpr int kSlots = 64;

struct ClientState {
	bool connected = false;
	bool in_game = false;
	bool source_tv = false;
	bool fake = false;
	bool authorized = false;
	bool controller = false;
	bool controller_pointer_valid = true;
	bool pawn = false;
	int32_t controller_handle = 0;
	uint64_t steamid64 = 0;
	std::string auth_id;
	int32_t team = 0;
};

std::array<ClientState, kSlots> g_clients{};
int g_client_calls = 0;

bool InRange(int32_t slot) {
	return slot >= 0 && slot < kSlots;
}

// A usable human client: every PITFALLS C2 layer passes. The controller handle is a *negative*
// valid handle on purpose (PITFALLS C1: the sign of a handle means nothing).
void SetupClient(int slot, const std::string& auth_id, int32_t team) {
	if (!InRange(slot)) {
		return;
	}
	ClientState& client = g_clients[static_cast<std::size_t>(slot)];
	client = ClientState{};
	client.connected = true;
	client.in_game = true;
	client.authorized = true;
	client.controller = true;
	client.controller_pointer_valid = true;
	client.pawn = true;
	client.controller_handle = -2 - slot;
	client.steamid64 = 76561197960287930ull + static_cast<uint64_t>(slot);
	client.auth_id = auth_id;
	client.team = team;
}

bool FakeIsClientConnected(int32_t slot) {
	++g_client_calls;
	return InRange(slot) && g_clients[static_cast<std::size_t>(slot)].connected;
}

bool FakeIsClientInGame(int32_t slot) {
	++g_client_calls;
	return InRange(slot) && g_clients[static_cast<std::size_t>(slot)].in_game;
}

bool FakeIsClientSourceTV(int32_t slot) {
	++g_client_calls;
	return InRange(slot) && g_clients[static_cast<std::size_t>(slot)].source_tv;
}

bool FakeIsFakeClient(int32_t slot) {
	++g_client_calls;
	return InRange(slot) && g_clients[static_cast<std::size_t>(slot)].fake;
}

bool FakeIsClientAuthorized(int32_t slot) {
	++g_client_calls;
	return InRange(slot) && g_clients[static_cast<std::size_t>(slot)].authorized;
}

void* FakePlayerSlotToEntPointer(int32_t slot) {
	++g_client_calls;
	if (!InRange(slot) || !g_clients[static_cast<std::size_t>(slot)].controller ||
		!g_clients[static_cast<std::size_t>(slot)].controller_pointer_valid) {
		return nullptr;
	}
	return reinterpret_cast<void*>(static_cast<intptr_t>(0x1000 + slot));
}

int32_t FakePlayerSlotToEntHandle(int32_t slot) {
	++g_client_calls;
	if (!InRange(slot) || !g_clients[static_cast<std::size_t>(slot)].controller) {
		return -1;
	}
	return g_clients[static_cast<std::size_t>(slot)].controller_handle;
}

bool FakeIsValidEntPointer(void* pointer) {
	++g_client_calls;
	return pointer != nullptr;
}

bool FakeIsValidEntHandle(int32_t handle) {
	++g_client_calls;
	return handle != 0 && handle != -1;
}

void* FakeGetClientPawn(int32_t slot) {
	++g_client_calls;
	if (!InRange(slot) || !g_clients[static_cast<std::size_t>(slot)].pawn) {
		return nullptr;
	}
	return reinterpret_cast<void*>(static_cast<intptr_t>(0x2000 + slot));
}

uint64_t FakeGetClientSteamID64(int32_t slot) {
	++g_client_calls;
	return InRange(slot) ? g_clients[static_cast<std::size_t>(slot)].steamid64 : 0;
}

plg::string FakeGetClientAuthId(int32_t slot) {
	++g_client_calls;
	return InRange(slot) ? plg::string(g_clients[static_cast<std::size_t>(slot)].auth_id) : plg::string();
}

s2sdk::CSTeam FakeGetClientTeam(int32_t slot) {
	++g_client_calls;
	const int32_t team = InRange(slot) ? g_clients[static_cast<std::size_t>(slot)].team : 0;
	return static_cast<s2sdk::CSTeam>(team);
}

// --- fakes: map lifecycle ------------------------------------------------------------------------

std::string g_current_map = "de_dust2";
int g_current_map_calls = 0;

plg::string FakeGetCurrentMap() {
	++g_current_map_calls;
	return plg::string(g_current_map);
}

s2sdk::OnMapStartCallback g_on_map_start = nullptr;
s2sdk::OnMapEndCallback g_on_map_end = nullptr;
bool g_map_start_registered = false;
bool g_map_end_registered = false;
bool g_map_start_unregistered = false;
bool g_map_end_unregistered = false;

void FakeOnMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_on_map_start = callback;
	g_map_start_registered = callback != nullptr;
}

void FakeOnMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_on_map_end = callback;
	g_map_end_registered = callback != nullptr;
}

void FakeOnMapStartUnregister(s2sdk::OnMapStartCallback /*callback*/) {
	g_map_start_unregistered = true;
	g_on_map_start = nullptr;
}

void FakeOnMapEndUnregister(s2sdk::OnMapEndCallback /*callback*/) {
	g_map_end_unregistered = true;
	g_on_map_end = nullptr;
}

s2sdk::OnClientDisconnectCallback g_disconnect = nullptr;
bool g_disconnect_registered = false;
bool g_disconnect_unregistered = false;

void FakeOnClientDisconnectRegister(s2sdk::OnClientDisconnectCallback callback) {
	g_disconnect = callback;
	g_disconnect_registered = callback != nullptr;
}

void FakeOnClientDisconnectUnregister(s2sdk::OnClientDisconnectCallback /*callback*/) {
	g_disconnect_unregistered = true;
	g_disconnect = nullptr;
}

// --- fakes: game events --------------------------------------------------------------------------

struct FakeEvent {
	std::map<std::string, int32_t> slots;  // GetEventPlayerSlot answers
	std::map<std::string, int32_t> ints;   // GetEventInt answers
	std::map<std::string, void*> pawns;    // GetEventPlayerPawn answers (player_pawn fallback)
	std::vector<std::string> slot_keys;
	std::vector<std::string> int_keys;
	std::vector<std::string> pawn_keys;
};

FakeEvent g_event;
int g_event_storage = 0;
std::map<void*, int32_t> g_pawn_slots;
int g_ent_pointer_to_slot_calls = 0;

int32_t FakeGetEventPlayerSlot(void* event, const plg::string& key) {
	if (event != &g_event_storage) {
		return -1;
	}
	const std::string name = ToStd(key);
	g_event.slot_keys.push_back(name);
	const auto it = g_event.slots.find(name);
	return it == g_event.slots.end() ? -1 : it->second;
}

int32_t FakeGetEventInt(void* event, const plg::string& key) {
	if (event != &g_event_storage) {
		return -1;
	}
	const std::string name = ToStd(key);
	g_event.int_keys.push_back(name);
	const auto it = g_event.ints.find(name);
	return it == g_event.ints.end() ? -1 : it->second;
}

void* FakeGetEventPlayerPawn(void* event, const plg::string& key) {
	if (event != &g_event_storage) {
		return nullptr;
	}
	const std::string name = ToStd(key);
	g_event.pawn_keys.push_back(name);
	const auto it = g_event.pawns.find(name);
	return it == g_event.pawns.end() ? nullptr : it->second;
}

int32_t FakeEntPointerToPlayerSlot(void* pawn) {
	++g_ent_pointer_to_slot_calls;
	const auto it = g_pawn_slots.find(pawn);
	return it == g_pawn_slots.end() ? -1 : it->second;
}

std::map<std::string, s2sdk::EventCallback> g_hooks;
std::map<std::string, s2sdk::HookMode> g_hook_modes;
std::vector<std::string> g_unhooked;
std::vector<s2sdk::HookMode> g_unhook_modes;
std::vector<s2sdk::EventCallback> g_unhook_callbacks;

s2sdk::EventHookError FakeHookEvent(const plg::string& name, s2sdk::EventCallback callback,
									s2sdk::HookMode mode) {
	const std::string key = ToStd(name);
	g_hooks[key] = callback;
	g_hook_modes[key] = mode;
	return s2sdk::EventHookError::Okay;
}

s2sdk::EventHookError FakeUnhookEvent(const plg::string& name, s2sdk::EventCallback callback,
									  s2sdk::HookMode mode) {
	const std::string key = ToStd(name);
	g_unhooked.push_back(key);
	g_unhook_modes.push_back(mode);
	g_unhook_callbacks.push_back(callback);
	g_hooks.erase(key);
	return s2sdk::EventHookError::Okay;
}

void ResetEvent() {
	g_event = FakeEvent{};
	g_pawn_slots.clear();
	g_ent_pointer_to_slot_calls = 0;
}

s2sdk::ResultType FireEvent(const std::string& name) {
	const auto it = g_hooks.find(name);
	if (it == g_hooks.end() || it->second == nullptr) {
		CHECK(false, ("the plugin registered a hook for " + name).c_str());
		return s2sdk::ResultType::Stop;
	}
	return it->second(plg::string(name), &g_event_storage, false);
}

void FireMapStart() {
	if (g_on_map_start != nullptr) {
		g_on_map_start();
	}
}

void FireMapEnd() {
	if (g_on_map_end != nullptr) {
		g_on_map_end();
	}
}

// --- fakes: levels_ranks -------------------------------------------------------------------------

bool g_core_loaded = true;
bool g_core_status[kSlots] = {};
int32_t g_save_mode = 0;
std::string g_table_name = "lvl_base";
std::string g_db_path;

levels_ranks::PlayerLoadedCallback g_player_loaded = nullptr;
levels_ranks::ResetPlayerStatsCallback g_reset_player_stats = nullptr;
bool g_player_loaded_unregistered = false;
bool g_reset_unregistered = false;
bool g_core_ready_unregistered = false;

bool FakeIsCoreLoaded() {
	return g_core_loaded;
}

plg::string FakeGetTableName() {
	return plg::string(g_table_name);
}

plg::vector<plg::string> FakeGetDatabaseConfig() {
	return plg::vector<plg::string>{plg::string("sqlite"), plg::string(g_db_path), plg::string(),
									plg::string(), plg::string(), plg::string()};
}

bool FakeCheckCountPlayers() {
	return true;
}

int32_t FakeGetSettingsValue(int32_t setting) {
	return setting == levels_ranks::LR_DB_SaveDataPlayer_Mode ? g_save_mode : 0;
}

bool FakeGetClientStatus(int32_t slot) {
	return InRange(slot) && g_core_status[static_cast<std::size_t>(slot)];
}

void FakeOnCoreIsReadyRegister(levels_ranks::CoreReadyCallback /*callback*/) {}
void FakeOnCoreIsReadyUnregister(levels_ranks::CoreReadyCallback /*callback*/) {
	g_core_ready_unregistered = true;
}

void FakeOnPlayerLoadedRegister(levels_ranks::PlayerLoadedCallback callback) {
	g_player_loaded = callback;
}

void FakeOnPlayerLoadedUnregister(levels_ranks::PlayerLoadedCallback /*callback*/) {
	g_player_loaded = nullptr;
	g_player_loaded_unregistered = true;
}

void FakeOnResetPlayerStatsRegister(levels_ranks::ResetPlayerStatsCallback callback) {
	g_reset_player_stats = callback;
}

void FakeOnResetPlayerStatsUnregister(levels_ranks::ResetPlayerStatsCallback /*callback*/) {
	g_reset_player_stats = nullptr;
	g_reset_unregistered = true;
}

// --- fake logger ---------------------------------------------------------------------------------

std::vector<std::string> g_errors;

void FakeLog(std::string_view message, plg::Severity severity, const plg::source_location& /*location*/) {
	if (severity == plg::Severity::Error) {
		g_errors.emplace_back(message);
	}
}

// --- the temporary SQLite database ---------------------------------------------------------------

std::unique_ptr<plgdb::Database> g_test_db;

plgdb::Params Strings(std::initializer_list<std::string> values) {
	plgdb::Params params;
	params.reserve(values.size());
	for (const std::string& value : values) {
		params.emplace_back(value);
	}
	return params;
}

int64_t QueryInt(const std::string& sql, plgdb::Params params = {}) {
	if (g_test_db == nullptr) {
		return -1;
	}
	const plgdb::Result result = g_test_db->QuerySync(sql, std::move(params));
	if (!result.Ok() || result.RowCount() == 0) {
		return -1;
	}
	return result.GetInt(0, 0, -1);
}

bool TableExists() {
	return QueryInt("SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name = 'lvl_base_maps';") == 1;
}

bool RowExists(const std::string& steam, const std::string& map) {
	return QueryInt("SELECT count(*) FROM `lvl_base_maps` WHERE `steam` = ? AND `name_map` = ?;",
					Strings({steam, map})) == 1;
}

int64_t Field(const std::string& steam, const std::string& map, const std::string& column) {
	const plgdb::Result result = g_test_db->QuerySync(
		"SELECT `" + column + "` FROM `lvl_base_maps` WHERE `steam` = ? AND `name_map` = ?;",
		Strings({steam, map}));
	if (!result.Ok() || result.RowCount() == 0) {
		return -1;
	}
	const std::optional<std::size_t> index = result.ColumnIndex(column);
	return index.has_value() ? result.GetInt(0, *index, -1) : -1;
}

// --- fixtures ------------------------------------------------------------------------------------

void InstallFakes() {
	plg::Log = &FakeLog;

	__s2sdk_HookEvent = &FakeHookEvent;
	__s2sdk_UnhookEvent = &FakeUnhookEvent;
	__s2sdk_GetEventPlayerSlot = &FakeGetEventPlayerSlot;
	__s2sdk_GetEventPlayerPawn = &FakeGetEventPlayerPawn;
	__s2sdk_EntPointerToPlayerSlot = &FakeEntPointerToPlayerSlot;
	__s2sdk_GetEventInt = &FakeGetEventInt;
	__s2sdk_GetCurrentMap = &FakeGetCurrentMap;

	__s2sdk_OnMapStart_Register = &FakeOnMapStartRegister;
	__s2sdk_OnMapEnd_Register = &FakeOnMapEndRegister;
	__s2sdk_OnMapStart_Unregister = &FakeOnMapStartUnregister;
	__s2sdk_OnMapEnd_Unregister = &FakeOnMapEndUnregister;
	__s2sdk_OnClientDisconnect_Register = &FakeOnClientDisconnectRegister;
	__s2sdk_OnClientDisconnect_Unregister = &FakeOnClientDisconnectUnregister;

	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_IsClientAuthorized = &FakeIsClientAuthorized;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_GetClientPawn = &FakeGetClientPawn;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_GetClientAuthId = &FakeGetClientAuthId;
	__s2sdk_GetClientTeam = &FakeGetClientTeam;

	__levels_ranks_IsCoreLoaded = &FakeIsCoreLoaded;
	__levels_ranks_GetTableName = &FakeGetTableName;
	__levels_ranks_GetDatabaseConfig = &FakeGetDatabaseConfig;
	__levels_ranks_CheckCountPlayers = &FakeCheckCountPlayers;
	__levels_ranks_GetSettingsValue = &FakeGetSettingsValue;
	__levels_ranks_GetClientStatus = &FakeGetClientStatus;
	__levels_ranks_OnCoreIsReady_Register = &FakeOnCoreIsReadyRegister;
	__levels_ranks_OnCoreIsReady_Unregister = &FakeOnCoreIsReadyUnregister;
	__levels_ranks_OnPlayerLoaded_Register = &FakeOnPlayerLoadedRegister;
	__levels_ranks_OnPlayerLoaded_Unregister = &FakeOnPlayerLoadedUnregister;
	__levels_ranks_OnResetPlayerStats_Register = &FakeOnResetPlayerStatsRegister;
	__levels_ranks_OnResetPlayerStats_Unregister = &FakeOnResetPlayerStatsUnregister;
}

bool PumpUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	for (;;) {
		Plugify_PluginUpdate(std::chrono::milliseconds(0));
		if (predicate()) {
			return true;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
}

void PumpOnce() {
	Plugify_PluginUpdate(std::chrono::milliseconds(0));
}

void FirePlayerLoaded(int slot, const std::string& steam) {
	if (g_player_loaded == nullptr) {
		CHECK(false, "the levels_ranks OnPlayerLoaded callback is registered");
		return;
	}
	g_player_loaded(slot, plg::string(steam));
}

void FireResetPlayerStats(int slot, const std::string& steam) {
	if (g_reset_player_stats == nullptr) {
		CHECK(false, "the levels_ranks OnResetPlayerStats callback is registered");
		return;
	}
	g_reset_player_stats(slot, plg::string(steam));
}

void FireDisconnect(int slot) {
	if (g_disconnect == nullptr) {
		CHECK(false, "the s2sdk OnClientDisconnect callback is registered");
		return;
	}
	g_disconnect(slot, s2sdk::NetworkDisconnectionReason::DisconnectByUser);
}

// --- cases ---------------------------------------------------------------------------------------

constexpr const char* kSteamA = "STEAM_1:1:500";   // [U:1:1001] in the fake client
constexpr const char* kSteamB = "STEAM_1:0:1001";  // [U:1:2002]
constexpr const char* kSteamC = "STEAM_1:1:1501";  // [U:1:3003]
constexpr const char* kSteamD = "STEAM_1:0:2002";  // [U:1:4004]

void TestStart() {
	std::printf("-- start / registrations\n");
	const plg::PluginResult result = Plugify_PluginStart();
	CHECK(result.GetCode() == plg::PluginCode::Ok, "OnPluginStart answers Ok (never Failed, PITFALLS A1)");
	CHECK(g_map_start_registered && g_map_end_registered, "the plugin registers both map listeners");
	CHECK(g_disconnect_registered, "the plugin registers the disconnect listener");
	CHECK_EQ(static_cast<int>(g_hooks.size()), 6, "the plugin hooks all six events");
	for (const char* name : {"player_death", "round_end", "bomb_planted", "bomb_defused",
							 "hostage_killed", "hostage_rescued"}) {
		CHECK(g_hooks.count(name) == 1, (std::string("the hook for ") + name + " is registered").c_str());
		CHECK(g_hook_modes[name] == s2sdk::HookMode::Pre,
			  (std::string("the ") + name + " hook is a Pre hook").c_str());
	}
	CHECK(g_player_loaded != nullptr, "the levels_ranks OnPlayerLoaded callback is registered");
	CHECK(g_reset_player_stats != nullptr, "the levels_ranks OnResetPlayerStats callback is registered");
}

void TestNoMapYet() {
	std::printf("-- the B1 gate: no map, no event field\n");
	SetupClient(3, "[U:1:1001]", 2);
	SetupClient(5, "[U:1:2002]", 3);
	ResetEvent();
	g_event.slots["attacker"] = 3;
	g_event.slots["userid"] = 5;
	const int client_calls = g_client_calls;
	const int map_calls = g_current_map_calls;
	CHECK(FireEvent("player_death") == s2sdk::ResultType::Continue,
		  "a closed gate still answers Continue");
	CHECK(g_event.slot_keys.empty(), "no event field is read before the first map");
	CHECK_EQ(g_client_calls, client_calls, "no client call before the first map");
	CHECK_EQ(g_current_map_calls, map_calls, "no map query before the first map");
}

void TestBootUpdateWithoutMap() {
	std::printf("-- a boot-time update before the first map does not open the gate\n");
	g_current_map.clear();
	PumpOnce();
	ResetEvent();
	g_event.slots["attacker"] = 3;
	g_event.slots["userid"] = 5;
	const int client_calls = g_client_calls;
	CHECK(FireEvent("player_death") == s2sdk::ResultType::Continue,
		  "an event before a map exists answers Continue");
	CHECK(g_event.slot_keys.empty(), "no event field is read while GetCurrentMap() is empty");
	CHECK_EQ(g_client_calls, client_calls, "no client call while GetCurrentMap() is empty");
	g_current_map = "de_dust2";
}

void TestFirstUpdateOpensGate() {
	std::printf("-- the first update captures the map and opens the table\n");
	CHECK(PumpUntil(TableExists), "the asynchronous CREATE TABLE reached SQLite");
	CHECK(g_current_map_calls > 0, "the first update captured s2sdk::GetCurrentMap()");
	CHECK_EQ(static_cast<int>(g_errors.size()), 0, "no error is logged on the normal path");
}

void TestLoadRows() {
	std::printf("-- OnPlayerLoaded loads the existing rows / inserts new ones\n");
	SetupClient(3, "[U:1:1001]", 2);
	SetupClient(5, "[U:1:2002]", 3);
	g_core_status[3] = true;
	g_core_status[5] = true;
	FirePlayerLoaded(3, kSteamA);
	FirePlayerLoaded(5, kSteamB);
	CHECK(PumpUntil([&] { return RowExists(kSteamA, "de_dust2") && RowExists(kSteamB, "de_dust2"); }),
		  "both rows exist for the first map");
	CHECK_EQ(Field(kSteamA, "de_dust2", "countplays"), 1, "a new row starts with countplays = 1");
	CHECK_EQ(Field(kSteamA, "de_dust2", "kills"), 0, "a new row starts with zero kills");
	CHECK_EQ(Field(kSteamB, "de_dust2", "countplays"), 1, "the second new row starts with countplays = 1");
}

void TestPlayerSlotAccessor() {
	std::printf("-- player-typed fields use GetEventPlayerSlot\n");
	ResetEvent();
	// What GetEventInt would answer for the same fields: deliberately different from the slots.
	g_event.ints["attacker"] = 13;
	g_event.ints["userid"] = 14;
	g_event.slots["attacker"] = 3;
	g_event.slots["userid"] = 5;
	CHECK(FireEvent("player_death") == s2sdk::ResultType::Continue, "the callback answers Continue");
	CHECK_EQ(static_cast<int>(g_event.slot_keys.size()), 2, "both player fields are resolved");
	CHECK(Contains(g_event.slot_keys, "attacker"), "the attacker field is resolved as a player");
	CHECK(Contains(g_event.slot_keys, "userid"), "the victim field is resolved as a player");
	CHECK(g_event.int_keys.empty(), "GetEventInt is never asked for a player field");
}

void TestMissingAccessors() {
	std::printf("-- a missing event accessor fails soft\n");
	__s2sdk_GetEventPlayerSlot = nullptr;
	__s2sdk_GetEventPlayerPawn = nullptr; // no primary and no player_pawn fallback either
	ResetEvent();
	g_event.slots["attacker"] = 3;
	g_event.slots["userid"] = 5;
	g_event.ints["attacker"] = 13;
	g_event.ints["userid"] = 14;
	CHECK(FireEvent("player_death") == s2sdk::ResultType::Continue,
		  "a missing player accessor still answers Continue");
	CHECK(g_event.slot_keys.empty(), "no player field is read without a player accessor");
	CHECK(g_event.pawn_keys.empty(), "the pawn fallback is not attempted without its bindings");
	CHECK(g_event.int_keys.empty(), "and no GetEventInt fallback is attempted");
	__s2sdk_GetEventPlayerSlot = &FakeGetEventPlayerSlot;
	__s2sdk_GetEventPlayerPawn = &FakeGetEventPlayerPawn;

	__s2sdk_GetEventInt = nullptr;
	ResetEvent();
	g_event.ints["winner"] = 2;
	CHECK(FireEvent("round_end") == s2sdk::ResultType::Continue,
		  "a missing GetEventInt still answers Continue");
	CHECK(g_event.int_keys.empty(), "round_end reads no numeric field without the int binding");
	__s2sdk_GetEventInt = &FakeGetEventInt;
}

// The parent finding: a player_pawn field (bomb_pickup.userid and friends) may not be resolved by
// the primary accessor on an older s2sdk. The map-gated GetEventPlayerPawn +
// EntPointerToPlayerSlot fallback must resolve it, and it may never fall back to GetEventInt.
void TestPlayerPawnFallback() {
	std::printf("-- the player_pawn fallback (no GetEventInt)\n");
	__s2sdk_GetEventPlayerSlot = nullptr; // force the fallback path
	ResetEvent();
	void* attacker_pawn = reinterpret_cast<void*>(static_cast<intptr_t>(0x3003));
	void* victim_pawn = reinterpret_cast<void*>(static_cast<intptr_t>(0x3005));
	g_event.pawns["attacker"] = attacker_pawn;
	g_event.pawns["userid"] = victim_pawn;
	g_pawn_slots[attacker_pawn] = 3;
	g_pawn_slots[victim_pawn] = 5;
	g_event.ints["attacker"] = 13; // the user id, which must not be used
	g_event.ints["userid"] = 14;
	CHECK(FireEvent("player_death") == s2sdk::ResultType::Continue,
		  "the fallback path answers Continue");
	CHECK(Contains(g_event.pawn_keys, "attacker"), "the attacker pawn is resolved");
	CHECK(Contains(g_event.pawn_keys, "userid"), "the victim pawn is resolved");
	CHECK_EQ(g_ent_pointer_to_slot_calls, 2, "both pawns are converted through EntPointerToPlayerSlot");
	CHECK(g_event.slot_keys.empty(), "the missing primary accessor was not called");
	CHECK(g_event.int_keys.empty(), "GetEventInt is never used as a player-slot fallback");
	__s2sdk_GetEventPlayerSlot = &FakeGetEventPlayerSlot;
}

void TestRoundEndAndRoundSave() {
	std::printf("-- round_end: numeric winner + per-round save\n");
	g_save_mode = 1; // lr_db_savedata_player_mode
	ResetEvent();
	g_event.ints["winner"] = 2;
	CHECK(FireEvent("round_end") == s2sdk::ResultType::Continue, "the round_end callback answers Continue");
	CHECK(g_event.slot_keys.empty(), "round_end reads no player field");
	CHECK(g_event.pawn_keys.empty(), "round_end reads no pawn field");
	CHECK_EQ(static_cast<int>(g_event.int_keys.size()), 1, "round_end reads exactly one numeric field");
	CHECK(Contains(g_event.int_keys, "winner"), "the numeric field is the winner");
	CHECK(PumpUntil([&] {
			  return Field(kSteamA, "de_dust2", "rounds_overall") == 1 &&
					 Field(kSteamB, "de_dust2", "rounds_overall") == 1;
		  }),
		  "both players' round counters are saved by lr_db_savedata_player_mode");
	CHECK_EQ(Field(kSteamA, "de_dust2", "kills"), 2, "the killer's kill was counted from slot 3 twice");
	CHECK_EQ(Field(kSteamA, "de_dust2", "deaths"), 0, "the killer has no death");
	CHECK_EQ(Field(kSteamB, "de_dust2", "kills"), 0, "the victim has no kill");
	CHECK_EQ(Field(kSteamB, "de_dust2", "deaths"), 2, "the victim's death was counted from slot 5 twice");
	CHECK_EQ(Field(kSteamA, "de_dust2", "rounds_t"), 1, "a T player gets the T column and the overall round");
	CHECK_EQ(Field(kSteamA, "de_dust2", "rounds_ct"), 0, "a T player gets no CT column");
	CHECK_EQ(Field(kSteamB, "de_dust2", "rounds_t"), 0, "a CT player gets no T column");
	CHECK_EQ(Field(kSteamB, "de_dust2", "rounds_ct"), 0, "a lost CT round only counts the overall round");
}

void TestBombAndHostageAccessors() {
	std::printf("-- bomb/hostage userid is a player field\n");
	g_save_mode = 0; // these saves are checked at map end
	ResetEvent();
	g_event.ints["userid"] = 13;
	g_event.slots["userid"] = 3;
	CHECK(FireEvent("bomb_planted") == s2sdk::ResultType::Continue, "bomb_planted answers Continue");
	CHECK(g_event.int_keys.empty(), "bomb_planted never reads userid through GetEventInt");
	CHECK(Contains(g_event.slot_keys, "userid"), "bomb_planted reads userid as a player slot");

	ResetEvent();
	g_event.ints["userid"] = 14;
	g_event.slots["userid"] = 5;
	CHECK(FireEvent("bomb_defused") == s2sdk::ResultType::Continue, "bomb_defused answers Continue");
	CHECK(g_event.int_keys.empty(), "bomb_defused never reads userid through GetEventInt");

	ResetEvent();
	g_event.ints["userid"] = 13;
	g_event.slots["userid"] = 3;
	CHECK(FireEvent("hostage_killed") == s2sdk::ResultType::Continue, "hostage_killed answers Continue");
	CHECK(g_event.int_keys.empty(), "hostage_killed never reads userid through GetEventInt");

	ResetEvent();
	g_event.ints["userid"] = 14;
	g_event.slots["userid"] = 5;
	CHECK(FireEvent("hostage_rescued") == s2sdk::ResultType::Continue, "hostage_rescued answers Continue");
	CHECK(g_event.int_keys.empty(), "hostage_rescued never reads userid through GetEventInt");
}

void TestPlaytimeTick() {
	std::printf("-- the per-second playtime tick\n");
	PumpOnce(); // arms the wall clock
	std::this_thread::sleep_for(std::chrono::milliseconds(1100));
	PumpOnce();
}

void TestMapEndSaves() {
	std::printf("-- OnMapEnd saves with the cached ids and the captured map name\n");
	const int client_calls = g_client_calls;
	const int map_calls = g_current_map_calls;
	FireMapEnd();
	CHECK_EQ(g_client_calls, client_calls, "OnMapEnd makes no client call (PITFALLS B2)");
	CHECK_EQ(g_current_map_calls, map_calls, "OnMapEnd does not query the map name again");

	// The map-end window must ignore every event, even while the host pumps updates.
	ResetEvent();
	g_event.slots["attacker"] = 3;
	g_event.slots["userid"] = 5;
	PumpOnce();
	const int window_client_calls = g_client_calls;
	CHECK(FireEvent("player_death") == s2sdk::ResultType::Continue,
		  "an event in the map-end window answers Continue");
	CHECK(g_event.slot_keys.empty(), "no event field is read in the map-end window");
	CHECK_EQ(g_client_calls, window_client_calls, "an event in the map-end window makes no client call");
	CHECK(g_current_map_calls == map_calls,
		  "an update in the map-end window does not re-open the gate or query the map");

	CHECK(PumpUntil([&] {
			  return Field(kSteamA, "de_dust2", "bomb_planted") == 1 &&
					 Field(kSteamA, "de_dust2", "hostage_killed") == 1 &&
					 Field(kSteamB, "de_dust2", "bomb_defused") == 1 &&
					 Field(kSteamB, "de_dust2", "hostage_rescued") == 1;
		  }),
		  "the map-end save wrote the bomb/hostage counters");
	CHECK_EQ(Field(kSteamA, "de_dust2", "kills"), 2, "the map-end save kept the kills");
	CHECK_EQ(Field(kSteamB, "de_dust2", "deaths"), 2, "the map-end save kept the deaths");
	CHECK(Field(kSteamA, "de_dust2", "playtime") >= 1, "the playtime tick was saved");
	CHECK_EQ(Field(kSteamA, "de_dust2", "countplays"), 1, "countplays is not incremented by a save");
}

void TestMapChangeLoadsNewRows() {
	std::printf("-- OnMapStart + first update load the next map for the same slots\n");
	g_current_map = "de_inferno";
	const int client_calls = g_client_calls;
	const int map_calls = g_current_map_calls;
	FireMapStart();
	CHECK_EQ(g_client_calls, client_calls, "OnMapStart makes no client call before the first update");
	CHECK_EQ(g_current_map_calls, map_calls, "OnMapStart does not query the map name");
	PumpOnce(); // the first update after OnMapStart
	CHECK(g_current_map_calls > map_calls, "the first update after OnMapStart reads the new map name");
	CHECK(PumpUntil([&] { return RowExists(kSteamA, "de_inferno") && RowExists(kSteamB, "de_inferno"); }),
		  "the new map's rows exist for both cached SteamIDs");
	CHECK_EQ(Field(kSteamA, "de_inferno", "countplays"), 1, "the new map starts with a fresh row");
	CHECK_EQ(Field(kSteamA, "de_inferno", "kills"), 0, "the new map's counters start at zero");
	CHECK_EQ(Field(kSteamA, "de_inferno", "rounds_overall"), 0, "the old map's rounds are not copied");
	CHECK_EQ(Field(kSteamA, "de_dust2", "kills"), 2, "the old map's row is untouched by the change");
	CHECK_EQ(Field(kSteamA, "de_dust2", "countplays"), 1, "the old map's row keeps its play count");
}

void TestDisconnectSave() {
	std::printf("-- disconnect saves before the cached id is dropped\n");
	ResetEvent();
	g_event.slots["attacker"] = 3;
	g_event.slots["userid"] = 5;
	CHECK(FireEvent("player_death") == s2sdk::ResultType::Continue, "a kill on the new map is counted");
	FireDisconnect(3);
	g_core_status[3] = false;
	CHECK(PumpUntil([&] { return Field(kSteamA, "de_inferno", "kills") == 1; }),
		  "the disconnect save wrote the new map's kill");
	CHECK_EQ(Field(kSteamA, "de_inferno", "deaths"), 0, "the disconnect save kept the counters separate");
	CHECK_EQ(Field(kSteamB, "de_inferno", "kills"), 0, "the other slot's row is unaffected");
}

void TestSlotReuseDropsStaleLoad() {
	std::printf("-- a stale load of a reused slot is dropped\n");
	// C connects and its SELECT is issued, but the answer is not polled yet.
	SetupClient(3, "[U:1:3003]", 2);
	g_core_status[3] = true;
	FirePlayerLoaded(3, kSteamC);
	// C leaves immediately and D takes the same slot before the SELECT callback runs.
	FireDisconnect(3);
	g_core_status[3] = false;
	SetupClient(3, "[U:1:4004]", 2);
	g_core_status[3] = true;
	FirePlayerLoaded(3, kSteamD);
	CHECK(PumpUntil([&] { return RowExists(kSteamD, "de_inferno"); }),
		  "the new occupant's row was inserted");
	PumpOnce();
	CHECK(!RowExists(kSteamC, "de_inferno"), "the stale SELECT did not insert the old occupant's row");
	CHECK_EQ(Field(kSteamD, "de_inferno", "countplays"), 1, "the new occupant starts with countplays = 1");
}

void TestReset() {
	std::printf("-- OnResetPlayerStats zeroes the row and the local counters\n");
	ResetEvent();
	g_event.slots["attacker"] = 3;
	g_event.slots["userid"] = 5;
	CHECK(FireEvent("player_death") == s2sdk::ResultType::Continue, "a kill before the reset is counted");
	FireResetPlayerStats(3, kSteamD);
	CHECK(PumpUntil([&] {
			  return Field(kSteamD, "de_inferno", "kills") == 0 &&
					 Field(kSteamD, "de_inferno", "countplays") == 0;
		  }),
		  "the reset zeroed the database row (countplays included, the original's UPDATE)");
	// The local reset (countplays = 1) reaches the database on the next save; the disconnect is
	// that save and also proves the slot is still loaded after the reset.
	FireDisconnect(3);
	g_core_status[3] = false;
	CHECK(PumpUntil([&] { return Field(kSteamD, "de_inferno", "countplays") == 1; }),
		  "the save after the reset wrote the local countplays = 1");
	CHECK_EQ(Field(kSteamD, "de_inferno", "kills"), 0, "the save after the reset wrote the zero counters");
}

void TestEnd() {
	std::printf("-- OnPluginEnd removes every registration\n");
	const plg::PluginResult result = Plugify_PluginEnd();
	CHECK(result.GetCode() == plg::PluginCode::Ok, "OnPluginEnd answers Ok");
	CHECK_EQ(static_cast<int>(g_hooks.size()), 0, "every event hook is removed again");
	CHECK_EQ(static_cast<int>(g_unhooked.size()), 6, "six UnhookEvent calls were made");
	for (const std::string& name : g_unhooked) {
		CHECK(g_hook_modes.count(name) == 1, "the unhooked name was one of the registered events");
	}
	CHECK_EQ(static_cast<int>(g_unhook_modes.size()), 6, "all unhooks used a mode");
	for (const s2sdk::HookMode mode : g_unhook_modes) {
		CHECK(mode == s2sdk::HookMode::Pre, "every unhook used the same Pre mode");
	}
	CHECK_EQ(static_cast<int>(g_unhook_callbacks.size()), 6, "every unhook carried a callback");
	for (const s2sdk::EventCallback callback : g_unhook_callbacks) {
		CHECK(callback != nullptr, "the removed event hook carried the registered callback");
	}
	CHECK(g_map_start_unregistered && g_map_end_unregistered, "both map listeners are removed");
	CHECK(g_disconnect_unregistered, "the disconnect listener is removed");
	CHECK(g_on_map_start == nullptr && g_on_map_end == nullptr && g_disconnect == nullptr,
		  "no callback of the unloaded library stays registered");
	CHECK(g_player_loaded_unregistered && g_reset_unregistered, "the levels_ranks callbacks are unregistered");
	CHECK(g_core_ready_unregistered, "the levels_ranks OnCoreIsReady callback is unregistered");
}

} // namespace

int main() {
	std::printf("lr_maps_event_tests\n");

	// A throw-away SQLite file (SQLite itself keeps it private to this test run).
	char file[] = "/tmp/lr_maps_event_XXXXXX";
	const int descriptor = mkstemp(file);
	if (descriptor < 0) {
		std::printf("FATAL: mkstemp failed\n");
		return 1;
	}
	close(descriptor);
	g_db_path = file;

	plgdb::Config config;
	config.driver = plgdb::Driver::SQLite;
	config.sqlitePath = g_db_path;
	g_test_db = plgdb::Database::Create(config);
	if (g_test_db == nullptr) {
		std::printf("FATAL: cannot open the temporary SQLite database\n");
		return 1;
	}

	InstallFakes();
	TestStart();
	TestNoMapYet();
	TestBootUpdateWithoutMap();
	TestFirstUpdateOpensGate();
	TestLoadRows();
	TestPlayerSlotAccessor();
	TestMissingAccessors();
	TestPlayerPawnFallback();
	TestRoundEndAndRoundSave();
	TestBombAndHostageAccessors();
	TestPlaytimeTick();
	TestMapEndSaves();
	TestMapChangeLoadsNewRows();
	TestDisconnectSave();
	TestSlotReuseDropsStaleLoad();
	TestReset();
	TestEnd();

	// The final unload save runs through the plugin's connection; after OnPluginEnd the test
	// connection can read the result.
	std::printf("-- final rows\n");
	std::printf("  %s de_dust2: kills=%lld deaths=%lld rounds=%lld playtime=%lld\n", kSteamA,
				static_cast<long long>(Field(kSteamA, "de_dust2", "kills")),
				static_cast<long long>(Field(kSteamA, "de_dust2", "deaths")),
				static_cast<long long>(Field(kSteamA, "de_dust2", "rounds_overall")),
				static_cast<long long>(Field(kSteamA, "de_dust2", "playtime")));

	g_test_db.reset();
	std::remove(g_db_path.c_str());
	std::remove((g_db_path + "-journal").c_str());
	std::remove((g_db_path + "-wal").c_str());
	std::remove((g_db_path + "-shm").c_str());

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
