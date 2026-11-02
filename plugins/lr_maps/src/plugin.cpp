// lr_maps — Plugify C++ port of the Pisex LR module "[LR] ExStats Maps"
// (the original's lr_maps.cpp; the Metamod module ships as addons/lr_modules/lr_maps with
// lr_maps.vdf).
//
// What it does (identical to the original): keeps one row per (steam, map) in the
// `<lr_table>_maps` table — map plays, kills/deaths, round counters, bomb/hostage counters and the
// playtime on the map — counts the events of the map, saves on disconnect and (when
// lr_db_savedata_player_mode is set) after every round, zeroes the row on !reset.
//
// This plugin calls s2sdk directly - no pisexcompat, no guard library (docs/PLUGIN_STANDARD.md §7).
// The safety rules of handoff/weaponskins/PITFALLS.md therefore live right here:
//   * B1/B2 map gate: the OnMapStart/OnMapEnd listeners are registered in OnPluginStart and the
//     gate g_mapReady is opened by the first OnPluginUpdate after OnMapStart (or by the first
//     update when the plugin was loaded into a running map) and dropped by OnMapEnd. The map name
//     is captured only once the gate is open, so neither a client nor a map query can reach the
//     torn-down state between OnMapEnd and the next OnMapStart.
//   * C2 client checks: 0 <= slot < 64, IsClientConnected, IsClientInGame, !IsClientSourceTV,
//     !IsFakeClient, IsClientAuthorized, a valid controller handle (PlayerSlotToEntHandle +
//     IsValidEntHandle; a *negative* handle is valid, only 0/-1 are not) and, for round_end, a
//     pawn and a positive SteamID64. Every s2sdk binding slot is checked for null as well, because
//     the generated wrappers call theirs unchecked.
//   * C12/C13: the six events are hooked with s2sdk::HookEvent(..., HookMode::Pre) in
//     OnPluginStart and removed with s2sdk::UnhookEvent in OnPluginEnd. The fields are read while
//     the event is alive, and every callback answers Continue on every path: a Pre hook result
//     >= Handled would free the event for every other plugin. Player-typed fields ("attacker" and
//     every "userid") are read with EventPlayerSlot(): s2sdk::GetEventPlayerSlot by default, and a
//     map-gated GetEventPlayerPawn + EntPointerToPlayerSlot fallback for a player_pawn field when
//     that binding is missing or did not resolve; numeric fields ("winner") use
//     s2sdk::GetEventInt. GetEventInt on a player field returns the engine user id, which is not
//     a slot (the original indexed its arrays with that id).
//   * A7: OnPluginEnd saves the rows of the current map, removes every hook/listener and closes the
//     database, so `plg reload` leaves nothing that points into the unloaded library.
//
// Port notes (the rest is parity):
//   * The original hooked IServerGameDLL::GameFrame for the per-second playtime tick and
//     IServerGameClients::ClientDisconnect; the port ticks from OnPluginUpdate (the game thread)
//     and uses s2sdk's OnClientDisconnect, like the levels_ranks core does (s2sdk fires it before
//     the client data is reset, the same window as the original hook).
//   * StartupServer() (the map name) becomes the s2sdk map lifecycle: OnMapEnd saves the old map's
//     rows while the cached SteamIDs and the map name are still available, and the first update
//     after OnMapStart captures the new name and loads every cached player for it. changelevel
//     keeps the clients and their slots, so without this the new map would never get a row.
//     A plugin loaded into a running map adopts the slots the levels_ranks core already loaded
//     (its reconcile does not announce them again).
//   * The disconnect save drops the original's "the player must still have a pawn and a SteamID"
//     test: the counters (plus the id/steam cached at OnPlayerLoaded) are what has to reach the
//     database, and the cached id keeps the save working even if the engine values are already
//     gone. The database sees the same UPDATE.
//   * The database is its own plgdb connection with the core's settings
//     (GetDatabaseConfig(), "lr.sqlite3" for the SQLite fallback); a Plugify plugin cannot share
//     another plugin's connection object. Layout and statements are the original's.
//   * Errors go through plg::Log (with the null check every pure-s2sdk plugin uses), never
//     pisex::ErrorLog.

#include <plg/plugin.hpp>
#include <plg/string.hpp>

#include <s2sdk/s2sdk.hpp>

#include <lr_api.hpp>

#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "maps_core.hpp"

namespace {

// GetLogTag() of the original.
constexpr char kLogTag[] = "[LR-Maps]";

// The number of s2sdk player slots (CS2's MAXPLAYERS); the core's arrays have the same size.
constexpr int kMaxPlayers = lr_maps::kMaxSlots;

// A failed row load is retried after this delay; a failed CREATE TABLE (the database may simply
// not be up yet at plugin start) after the longer one. Save/load errors are logged at most once
// per interval so a database outage does not write one line per event.
constexpr int64_t kLoadRetrySeconds = 5;
constexpr int64_t kDdlRetrySeconds = 30;
constexpr int64_t kErrorLogSeconds = 10;

// Hooks() switch of the original: the six events the module counted.
constexpr std::array<const char*, 6> kEventNames{
	"player_death", "round_end", "bomb_planted", "bomb_defused", "hostage_killed", "hostage_rescued"};

int64_t NowSeconds() {
	return static_cast<int64_t>(std::time(nullptr));
}

// The original's g_pLRCore.
levels_ranks::LrApi* g_pLRCore = &levels_ranks::LrApi::Get();

using namespace lr_maps;

// --- logging (pisex::ErrorLog replacement) -------------------------------------------------------

void LogMessage(std::string_view message, plg::Severity severity) {
	// plg::Log is filled by the plugify module right before OnPluginStart; the null check keeps the
	// plugin silent (not crashing) in an unusual host that never provides it.
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) {
	LogMessage(message, plg::Severity::Info);
}

// The printf-style convenience of the original's pisex::ErrorLog, on top of plg::Log; the format
// attribute keeps -Werror format checking working for every call site.
#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
void LogError(const char* format, ...) {
	if (plg::Log == nullptr) {
		return;
	}
	va_list args;
	va_start(args, format);
	va_list sizeArgs;
	va_copy(sizeArgs, args);
	const int size = std::vsnprintf(nullptr, 0, format, sizeArgs);
	va_end(sizeArgs);
	std::string message;
	if (size > 0) {
		message.resize(static_cast<std::size_t>(size) + 1);
		std::vsnprintf(message.data(), message.size(), format, args);
		message.resize(static_cast<std::size_t>(size));
	} else {
		message = format; // vsnprintf failed: still report something useful
	}
	va_end(args);
	LogMessage(message, plg::Severity::Error);
}

std::string ToStd(const plg::string& text) {
	return std::string(text.data(), text.size());
}

// --- map gate (PITFALLS B1/B2) -------------------------------------------------------------------

bool g_mapReady = false;                // a map is up: s2sdk client calls are allowed
bool g_mapStartSeen = false;            // an OnMapStart arrived since OnPluginStart / the last OnMapEnd
bool g_mapEndSeen = false;              // an OnMapEnd arrived since OnPluginStart
bool g_mapListenersRegistered = false;  // OnMapStart/OnMapEnd are registered with s2sdk
// The next ProcessMapTransition() has to read s2sdk::GetCurrentMap(). It is armed at start (a
// plugin loaded into a running map) and by every OnMapStart, disarmed when the name was captured -
// the per-frame update path then costs no map query at all.
bool g_mapTransitionPending = true;

class LrMapsPlugin;

// s2sdk listeners: plain function pointers, called on the game thread. Nothing here may throw.
void OnMapStartListener() {
	// The engine calls this once the new map exists. Readiness itself is granted by the first
	// OnPluginUpdate after this point (PITFALLS B1): no s2sdk query runs inside the listener.
	g_mapStartSeen = true;
	g_mapEndSeen = false;
	g_mapTransitionPending = true;
}

void OnMapEndListener();
void RegisterMapListeners();
void UnregisterMapListeners();
void OnClientDisconnectListener(int32_t slot, s2sdk::NetworkDisconnectionReason reason);
s2sdk::ResultType OnGameEvent(const plg::string& name, void* event, bool dontBroadcast);

void PollMapGate() {
	if (!g_mapListenersRegistered || g_mapReady) {
		return;
	}
	// An OnMapStart is the engine's proof that a map exists: readiness is granted on the first
	// update after it (PITFALLS B1), never inside the listener.
	if (g_mapStartSeen) {
		g_mapReady = true;
		return;
	}
	if (g_mapEndSeen) {
		return; // only a new OnMapStart re-opens the gate after a map end
	}
	// A plugin loaded into a running map never saw an OnMapStart: the first OnPluginUpdate after
	// OnPluginStart is pumped from the server frame. A boot-time update before the first map must
	// not open the gate, so the null-safe map name is the proof of the live map (GetCurrentMap
	// checks gpGlobals itself and answers an empty string before it exists).
	if (__s2sdk_GetCurrentMap == nullptr) {
		return; // OnPluginStart already reported the missing binding; stay fail-safe/closed
	}
	try {
		if (!s2sdk::GetCurrentMap().empty()) {
			g_mapReady = true;
		}
	} catch (...) {
		// stay closed; the next update tries again
	}
}

// --- client gates (PITFALLS C1/C2) ---------------------------------------------------------------

// Entity HANDLES are int32 views of the engine's uint32 CEntityHandle (index | serial << 15):
// valid <=> h != INVALID_EHANDLE_INDEX (-1) && h != 0 (index 0 is the world). The sign means
// nothing - once the engine's serial counter passes 65535 bit 31 is set and a valid handle is a
// negative int; a `h > 0` test silently drops those entities.
bool IsValidHandle(int32_t handle) {
	return handle != -1 && handle != 0;
}

// pisex::g_pPlayers->IsAuthenticated() of the old port: the slot has an engine-authenticated human
// client. Every check is short-circuited in the order of the PITFALLS rule, and the map gate comes
// first - a closed gate means no s2sdk client call may run at all.
bool IsAuthenticated(int slot) {
	if (!g_mapReady || slot < 0 || slot >= kMaxPlayers) {
		return false;
	}
	if (__s2sdk_IsClientConnected == nullptr || __s2sdk_IsClientInGame == nullptr ||
		__s2sdk_IsClientSourceTV == nullptr || __s2sdk_IsFakeClient == nullptr ||
		__s2sdk_IsClientAuthorized == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(slot) && s2sdk::IsClientInGame(slot) &&
			   !s2sdk::IsClientSourceTV(slot) && !s2sdk::IsFakeClient(slot) &&
			   s2sdk::IsClientAuthorized(slot);
	} catch (...) {
		return false;
	}
}

// The original's `CCSPlayerController::FromSlot(slot)` null check plus the PITFALLS C2 handle
// rule: a controller pointer alone is not enough, its handle has to be valid as well.
bool HasController(int slot) {
	if (!g_mapReady || slot < 0 || slot >= kMaxPlayers) {
		return false;
	}
	if (__s2sdk_PlayerSlotToEntPointer == nullptr || __s2sdk_PlayerSlotToEntHandle == nullptr ||
		__s2sdk_IsValidEntPointer == nullptr || __s2sdk_IsValidEntHandle == nullptr) {
		return false;
	}
	try {
		void* controller = s2sdk::PlayerSlotToEntPointer(slot);
		if (controller == nullptr || !s2sdk::IsValidEntPointer(controller)) {
			return false;
		}
		const int32_t handle = s2sdk::PlayerSlotToEntHandle(slot);
		return IsValidHandle(handle) && s2sdk::IsValidEntHandle(handle);
	} catch (...) {
		return false;
	}
}

// The condition the original's round_end loop used: a fully authenticated client with a controller,
// a pawn and a positive SteamID64.
bool PlayerUsable(int slot) {
	if (!IsAuthenticated(slot) || !HasController(slot)) {
		return false;
	}
	if (__s2sdk_GetClientPawn == nullptr || __s2sdk_GetClientSteamID64 == nullptr) {
		return false;
	}
	try {
		return s2sdk::GetClientPawn(slot) != nullptr && s2sdk::GetClientSteamID64(slot) > 0;
	} catch (...) {
		return false;
	}
}

// engine->GetPlayerNetworkIDString(slot) of the original, before ConvertSteamID(): the engine's
// authentication string. A bot, SourceTV or a client that is still connecting has none.
std::string ClientNetworkId(int slot) {
	if (!g_mapReady || slot < 0 || slot >= kMaxPlayers) {
		return {};
	}
	if (__s2sdk_IsClientConnected == nullptr || __s2sdk_IsClientSourceTV == nullptr ||
		__s2sdk_IsFakeClient == nullptr || __s2sdk_GetClientAuthId == nullptr) {
		return {};
	}
	try {
		if (!s2sdk::IsClientConnected(slot) || s2sdk::IsClientSourceTV(slot) ||
			s2sdk::IsFakeClient(slot)) {
			return {};
		}
		return ToStd(s2sdk::GetClientAuthId(slot));
	} catch (...) {
		return {};
	}
}

int ClientTeam(int slot) {
	if (!g_mapReady || slot < 0 || slot >= kMaxPlayers || __s2sdk_GetClientTeam == nullptr) {
		return 0;
	}
	try {
		return static_cast<int>(s2sdk::GetClientTeam(slot));
	} catch (...) {
		return 0;
	}
}

// The slot behind a player-typed event field ("attacker", "userid", ...). s2sdk's
// GetEventPlayerSlot is the primary accessor and handles both player_controller_and_pawn and
// player_pawn fields through IGameEvent::GetPlayerSlot. When that binding is missing, or it did
// not resolve a slot, the field is a player_pawn one on an older s2sdk: resolve the pawn with
// GetEventPlayerPawn and ask the engine for its slot. The fallback is map-gated and validates the
// pawn pointer first; it deliberately never falls back to GetEventInt, whose value is the engine
// user id and not a slot.
int EventPlayerSlot(void* event, const char* key) {
	if (event == nullptr || !g_mapReady) {
		return -1;
	}
	const plg::string field(key);
	if (__s2sdk_GetEventPlayerSlot != nullptr) {
		try {
			const int slot = static_cast<int>(s2sdk::GetEventPlayerSlot(event, field));
			if (slot >= 0 && slot < kMaxPlayers) {
				return slot;
			}
		} catch (...) {
			// try the pawn fallback below
		}
	}
	if (__s2sdk_GetEventPlayerPawn == nullptr || __s2sdk_EntPointerToPlayerSlot == nullptr ||
		__s2sdk_IsValidEntPointer == nullptr) {
		return -1;
	}
	try {
		void* pawn = s2sdk::GetEventPlayerPawn(event, field);
		if (pawn == nullptr || !s2sdk::IsValidEntPointer(pawn)) {
			return -1;
		}
		const int slot = static_cast<int>(s2sdk::EntPointerToPlayerSlot(pawn));
		return slot >= 0 && slot < kMaxPlayers ? slot : -1;
	} catch (...) {
		return -1;
	}
}

bool HasPlayerSlotAccessor() {
	return __s2sdk_GetEventPlayerSlot != nullptr ||
		   (__s2sdk_GetEventPlayerPawn != nullptr && __s2sdk_EntPointerToPlayerSlot != nullptr &&
			__s2sdk_IsValidEntPointer != nullptr);
}

// engine->GetPlayerNetworkIDString() + ConvertSteamID() of the original: CS2's "[U:1:accountid]"
// becomes "STEAM_1:x:y" (the core's own conversion, so both write the same `steam` value); a value
// that already is a STEAM_ id is passed through, anything unusable becomes "".
std::string ConvertSteamId(std::string_view network_id) {
	const std::string text(network_id);
	if (text.empty()) {
		return {};
	}
	if (text.rfind("STEAM_", 0) == 0) {
		return text;
	}
	std::string stripped;
	stripped.reserve(text.size());
	for (const char c : text) {
		if (c != '[' && c != ']') {
			stripped.push_back(c);
		}
	}
	std::string parts[3];
	int part = 0;
	for (const char c : stripped) {
		if (c == ':') {
			if (part < 2) {
				++part;
			}
			continue;
		}
		parts[part].push_back(c);
	}
	if (parts[2].empty()) {
		return stripped; // the original crashed on such input (usteamid_split[2]); the port keeps it
	}
	int64_t account = 0;
	try {
		account = std::stoll(parts[2]); // the original's std::stoi: a 64-bit account id no longer throws
	} catch (const std::exception&) {
		return stripped;
	}
	return "STEAM_1:" + std::string(account % 2 == 0 ? "0:" : "1:") + std::to_string(account / 2);
}

// --- the plugin ----------------------------------------------------------------------------------

class LrMapsPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// OnMapEnd of the map lifecycle: save the counters of the map that is going away with the
	// cached ids and the captured name (PITFALLS B2: no client query may run here).
	void HandleMapEnd();
	// lr_maps::OnClientDisconnect of the original (s2sdk OnClientDisconnect context: still called
	// before the client data is reset, in the same slot-preserving window as the original hook).
	void OnClientDisconnect(int32_t slot);
	// Hooks() switch of the original; the event name selects the counter.
	void DispatchEvent(const plg::string& name, void* event);

private:
	void OnCoreIsReady();
	// The first update after OnMapStart (or after a plugin loaded into a running map): capture the
	// map name, save the previous map and load/reload the rows of the new one.
	void ProcessMapTransition();
	// Adopt the slots the levels_ranks core already loaded (a plugin reload gets no OnPlayerLoaded
	// for them) and keep the cache in sync as a safety net for a missed callback.
	void SeedFromCore();
	// Forgets the per-slot state of the map that ended / is replaced; the cached steam ids survive
	// a changelevel because the clients and their slots do.
	void ResetSlots(bool keep_ids);

	void OnPlayerLoaded(int slot, const char* steamid);
	void OnResetPlayerStats(int slot, const char* steamid);
	// The event handlers of the original's Hooks() switch.
	void OnPlayerDeathEvent(void* event);
	void OnRoundEndEvent(void* event);
	void OnBombPlantedEvent(void* event);
	void OnBombDefusedEvent(void* event);
	void OnHostageKilledEvent(void* event);
	void OnHostageRescuedEvent(void* event);

	// SaveDataPlayer() of the original; queues nothing without a steam id or map name.
	void SaveData(int slot);
	void SaveDataWithId(int slot, const std::string& id);
	// The rows of every loaded slot of the current map (map change, unload).
	void SaveAllForCurrentMap();
	// engine->GetPlayerNetworkIDString(slot) + ConvertSteamID, with the cached id as the fallback.
	std::string SteamIdOf(int slot) const;
	// s2sdk::GetCurrentMap() under the map gate; answers "" and logs once when there is none.
	std::string CurrentMapName();
	void LogMissingMapOnce();
	// The load of |id| for the current map; a query error leaves the slot inactive and schedules a
	// retry.
	void StartLoad(int slot, const std::string& id);
	void UpdateRetries();
	void IssueCreateTable();
	void Tick();
	void LogLoadError(const std::string& error);
	void LogSaveError(const std::string& error);

	State m_state{};
	std::array<std::string, lr_maps::kMaxSlots> m_steamids{};
	// The slot's (steam, map) row is in memory: only then are its counters counted and saved. A
	// failed SELECT used to look like "no row" and the next round-end save replaced the player's
	// whole per-map row with the zeros of the session.
	std::array<bool, lr_maps::kMaxSlots> m_loaded{};
	std::array<bool, lr_maps::kMaxSlots> m_load_in_flight{};
	std::array<int64_t, lr_maps::kMaxSlots> m_load_retry_at{};
	std::array<uint64_t, lr_maps::kMaxSlots> m_load_seq{};
	std::unique_ptr<plgdb::Database> m_db;
	std::string m_table; // <lr_table>_maps
	std::string m_map;
	std::time_t m_last_tick = 0;
	bool m_disconnect_registered = false;
	std::array<bool, kEventNames.size()> m_event_hooked{};
	bool m_logged_missing_map = false;
	// The table must exist before any load/save is issued; a failed CREATE TABLE is retried so a
	// database that comes up after the core does not leave the module without a table.
	bool m_table_ready = false;
	int64_t m_ddl_requested_at = 0;
	int64_t m_last_load_error_log = 0;
	int64_t m_last_save_error_log = 0;
	int64_t m_last_seed_check = 0;
};

LrMapsPlugin g_plugin;

void OnMapEndListener() {
	// PITFALLS B2: from here until the next OnMapStart s2sdk's client list is torn down and any
	// client query crashes. The plugin saves with its cached ids/name and only drops state.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = true;
	g_mapTransitionPending = false;
	g_plugin.HandleMapEnd();
}

void OnClientDisconnectListener(int32_t slot, s2sdk::NetworkDisconnectionReason /*reason*/) {
	try {
		g_plugin.OnClientDisconnect(slot);
	} catch (const std::exception& ex) {
		LogError("%s OnClientDisconnect failed: %s", kLogTag, ex.what());
	} catch (...) {
		LogError("%s OnClientDisconnect failed with an unknown exception", kLogTag);
	}
}

// The single game-event hook. A Pre hook must answer Continue on every path: a result >= Handled
// frees the event for every other plugin on the server (the original's listener returned false for
// the same reason).
s2sdk::ResultType OnGameEvent(const plg::string& name, void* event, bool /*dontBroadcast*/) {
	try {
		g_plugin.DispatchEvent(name, event);
	} catch (const std::exception& ex) {
		LogError("%s a game event failed: %s", kLogTag, ex.what());
	} catch (...) {
		LogError("%s a game event failed with an unknown exception", kLogTag);
	}
	return s2sdk::ResultType::Continue;
}

void RegisterMapListeners() {
	if (g_mapListenersRegistered) {
		return;
	}
	if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapEnd_Register == nullptr) {
		// Without the map-end listener nothing could take readiness away again, so the gate stays
		// closed for good (PITFALLS A1: log and stay inactive, never return Failed).
		LogError("%s the s2sdk map listeners are unavailable - the map statistics stay off", kLogTag);
		return;
	}
	s2sdk::OnMapStart_Register(&OnMapStartListener);
	s2sdk::OnMapEnd_Register(&OnMapEndListener);
	g_mapListenersRegistered = true;
}

void UnregisterMapListeners() {
	if (!g_mapListenersRegistered) {
		return;
	}
	g_mapListenersRegistered = false;
	if (__s2sdk_OnMapStart_Unregister != nullptr) {
		s2sdk::OnMapStart_Unregister(&OnMapStartListener);
	}
	if (__s2sdk_OnMapEnd_Unregister != nullptr) {
		s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
	}
}

// --- database ------------------------------------------------------------------------------------

void LrMapsPlugin::OnCoreIsReady() {
	// OnCoreIsReadyHook() of the original: the connection first, then the table and the hooks.
	if (m_db != nullptr) {
		return;
	}
	m_table = TableName(g_pLRCore->GetTableName());
	m_db = plgdb::Database::Create(
		levels_ranks::MakeDatabaseConfig(g_pLRCore->GetDatabaseConfig(), "lr.sqlite3"));
	if (m_db == nullptr) {
		LogError("%s the database configuration is invalid - the map statistics are off", kLogTag);
		return;
	}
	// The DDL is queued (FIFO before every later statement), like the original's async Query; its
	// result is checked and a failure is retried (the database may not be up yet).
	m_table_ready = false;
	IssueCreateTable();

	g_pLRCore->HookOnPlayerLoaded([this](int slot, const char* steamid) { OnPlayerLoaded(slot, steamid); });
	g_pLRCore->HookOnResetPlayerStats(
		[this](int slot, const char* steamid) { OnResetPlayerStats(slot, steamid); });
}

void LrMapsPlugin::IssueCreateTable() {
	if (m_db == nullptr) {
		return;
	}
	m_ddl_requested_at = NowSeconds();
	plgdb::Statement ddl{CreateTableSql(m_table, m_db->GetDriver()), {}};
	m_db->Query(std::move(ddl.sql), std::move(ddl.params), [this](const plgdb::Result& result) {
		if (result.Ok()) {
			m_table_ready = true;
			return;
		}
		LogError("%s creating the %s table failed: %s", kLogTag, m_table.c_str(), result.Error().c_str());
	});
}

void LrMapsPlugin::LogLoadError(const std::string& error) {
	const int64_t now = NowSeconds();
	if (now - m_last_load_error_log < kErrorLogSeconds) {
		return;
	}
	m_last_load_error_log = now;
	LogError("%s loading the map statistics failed: %s", kLogTag, error.c_str());
}

void LrMapsPlugin::LogSaveError(const std::string& error) {
	const int64_t now = NowSeconds();
	if (now - m_last_save_error_log < kErrorLogSeconds) {
		return;
	}
	m_last_save_error_log = now;
	LogError("%s saving the map statistics failed: %s", kLogTag, error.c_str());
}

void LrMapsPlugin::StartLoad(int slot, const std::string& id) {
	const std::size_t index = static_cast<std::size_t>(slot);
	const uint64_t seq = ++m_load_seq[index];
	m_load_in_flight[index] = true;
	m_loaded[index] = false;
	m_load_retry_at[index] = 0;
	Statement load = LoadStatement(m_table, id, m_map);
	m_db->Query(std::move(load.sql), std::move(load.params),
				[this, slot, id, seq](const plgdb::Result& result) {
					const std::size_t index = static_cast<std::size_t>(slot);
					if (m_load_seq[index] != seq || m_steamids[index] != id) {
						// The slot was taken by another player while the query was in flight (a fast
						// reconnect): this row is not theirs (the core drops stale loads the same way).
						return;
					}
					m_load_in_flight[index] = false;
					if (!result.Ok()) {
						// An error is not "no row": keep the slot inactive and retry; the counters
						// of the map are not loaded yet.
						LogLoadError(result.Error());
						m_load_retry_at[index] = NowSeconds() + kLoadRetrySeconds;
						return;
					}
					SlotState& state = m_state[index];
					if (ApplyLoaded(state, result)) {
						m_loaded[index] = true;
						return;
					}
					// No row for this (steam, map): the original defaulted the counters and inserted
					// the row.
					ResetSlot(state);
					m_loaded[index] = true;
					Statement insert = InsertStatement(m_table, id, m_map);
					m_db->Query(std::move(insert.sql), std::move(insert.params),
								[this, slot, id, seq](const plgdb::Result& insert_result) {
									const std::size_t index = static_cast<std::size_t>(slot);
									if (m_load_seq[index] != seq || m_steamids[index] != id) {
										return;
									}
									if (insert_result.Ok()) {
										return;
									}
									// The UPDATEs of SaveData would match no row: go back to inactive
									// and let the retry (which re-selects and re-inserts) fix it.
									LogLoadError(insert_result.Error());
									m_loaded[index] = false;
									m_state[index].active = false;
									m_load_retry_at[index] = NowSeconds() + kLoadRetrySeconds;
								});
				});
}

void LrMapsPlugin::UpdateRetries() {
	if (m_db == nullptr) {
		return;
	}
	const int64_t now = NowSeconds();
	if (!m_table_ready) {
		if (now - m_ddl_requested_at >= kDdlRetrySeconds) {
			IssueCreateTable();
		}
		return; // no load can succeed before the table exists
	}
	if (!g_mapReady || m_map.empty()) {
		return; // the next map's rows are loaded by ProcessMapTransition()
	}
	if (now != m_last_seed_check) {
		m_last_seed_check = now;
		SeedFromCore();
	}
	for (int slot = 0; slot < kMaxPlayers; ++slot) {
		const std::size_t index = static_cast<std::size_t>(slot);
		if (m_loaded[index] || m_load_in_flight[index] || m_steamids[index].empty()) {
			continue;
		}
		if (now < m_load_retry_at[index]) {
			continue;
		}
		StartLoad(slot, m_steamids[index]);
	}
}

void LrMapsPlugin::SeedFromCore() {
	if (!g_mapReady || m_db == nullptr || m_table.empty()) {
		return;
	}
	for (int slot = 0; slot < kMaxPlayers; ++slot) {
		const std::size_t index = static_cast<std::size_t>(slot);
		if (!m_steamids[index].empty()) {
			continue; // known already (loaded at connect, or by an earlier seed)
		}
		if (!g_pLRCore->GetClientStatus(slot)) {
			continue; // the core has not loaded this slot; its OnPlayerLoaded hook will announce it
		}
		const std::string id = SteamIdOf(slot);
		if (id.empty()) {
			continue;
		}
		OnPlayerLoaded(slot, id.c_str());
	}
}

void LrMapsPlugin::ResetSlots(bool keep_ids) {
	for (int slot = 0; slot < kMaxPlayers; ++slot) {
		const std::size_t index = static_cast<std::size_t>(slot);
		// A load of the previous map's (or occupant's) row must not land in this state.
		++m_load_seq[index];
		m_load_in_flight[index] = false;
		m_loaded[index] = false;
		m_load_retry_at[index] = 0;
		m_state[index] = SlotState{};
		if (!keep_ids) {
			m_steamids[index].clear();
		}
	}
}

void LrMapsPlugin::ProcessMapTransition() {
	if (!g_mapReady) {
		return;
	}
	if (!g_mapTransitionPending && !m_map.empty()) {
		return;
	}
	const std::string current = CurrentMapName();
	if (current.empty()) {
		return; // stays pending; the next update asks again
	}
	g_mapTransitionPending = false;
	if (current == m_map) {
		return;
	}
	// Defensive: a map that ends without OnMapEnd (a listener registered late) still has its rows
	// saved before the name is replaced.
	SaveAllForCurrentMap();
	ResetSlots(/*keep_ids=*/true);
	m_map = current;
	m_last_tick = 0;
	m_logged_missing_map = false;
	m_last_seed_check = 0;
	// changelevel keeps the clients and their slots: load the new map's rows right away. A plugin
	// loaded into a running map has no cached ids yet - SeedFromCore() adopts the core's slots.
	SeedFromCore();
}

void LrMapsPlugin::HandleMapEnd() {
	// The cached ids and m_map are still the ones of the map that is going away; no s2sdk client
	// call is made here (PITFALLS B2). The cached ids survive because a changelevel keeps the
	// clients, which is what ProcessMapTransition() loads the next map's rows for.
	SaveAllForCurrentMap();
	m_map.clear();
	ResetSlots(/*keep_ids=*/true);
	m_last_tick = 0;
	m_logged_missing_map = false;
	m_last_seed_check = 0;
}

std::string LrMapsPlugin::SteamIdOf(int slot) const {
	std::string id = ConvertSteamId(ClientNetworkId(slot));
	if (id.empty() && slot >= 0 && slot < kMaxPlayers) {
		// The id the LR core loaded into the slot: still usable while the map gate is closed, which
		// is exactly the window a changelevel's disconnects run in.
		id = m_steamids[static_cast<std::size_t>(slot)];
	}
	return id;
}

void LrMapsPlugin::SaveData(int slot) {
	if (slot < 0 || slot >= kMaxPlayers || m_db == nullptr) {
		return;
	}
	const std::string id = SteamIdOf(slot);
	if (id.empty()) {
		LogError("%s no steam id for slot %d - the map statistics were not saved", kLogTag, slot);
		return;
	}
	SaveDataWithId(slot, id);
}

void LrMapsPlugin::SaveDataWithId(int slot, const std::string& id) {
	if (slot < 0 || slot >= kMaxPlayers || m_db == nullptr || !m_table_ready) {
		return;
	}
	const std::size_t index = static_cast<std::size_t>(slot);
	if (!m_loaded[index]) {
		return; // no row in memory (or a failed load): an UPDATE would write the session's zeros
	}
	if (m_map.empty()) {
		return; // the UPDATE could not match a row without the key's second half; already logged
	}
	Statement statement = SaveStatement(m_table, m_state[index], id, m_map);
	m_db->Query(std::move(statement.sql), std::move(statement.params), [this](const plgdb::Result& result) {
		if (!result.Ok()) {
			LogSaveError(result.Error());
		}
	});
}

void LrMapsPlugin::SaveAllForCurrentMap() {
	if (m_db == nullptr || !m_table_ready || m_map.empty()) {
		return;
	}
	for (int slot = 0; slot < kMaxPlayers; ++slot) {
		const std::size_t index = static_cast<std::size_t>(slot);
		if (m_loaded[index] && !m_steamids[index].empty()) {
			SaveDataWithId(slot, m_steamids[index]);
		}
	}
}

std::string LrMapsPlugin::CurrentMapName() {
	if (__s2sdk_GetCurrentMap == nullptr) {
		LogMissingMapOnce();
		return {};
	}
	try {
		const std::string map = ToStd(s2sdk::GetCurrentMap());
		if (map.empty()) {
			LogMissingMapOnce();
		}
		return map;
	} catch (...) {
		LogMissingMapOnce();
		return {};
	}
}

void LrMapsPlugin::LogMissingMapOnce() {
	if (m_logged_missing_map) {
		return;
	}
	m_logged_missing_map = true;
	LogError("%s no map name yet - the map statistics are not loaded or saved", kLogTag);
}

void LrMapsPlugin::Tick() {
	// lr_maps::GameFrame of the original: a wall-clock second between the playtime ticks.
	const std::time_t now = std::time(nullptr);
	if (m_last_tick == 0) {
		m_last_tick = now;
		return;
	}
	if (now - m_last_tick < 1) {
		return;
	}
	m_last_tick = now;
	if (g_pLRCore->CheckCountPlayers()) {
		TickSecond(m_state);
	}
}

void LrMapsPlugin::OnPlayerLoaded(int slot, const char* steamid) {
	if (slot < 0 || slot >= kMaxPlayers || m_db == nullptr) {
		return;
	}
	const std::size_t index = static_cast<std::size_t>(slot);
	const std::string id = steamid != nullptr ? steamid : "";
	m_steamids[index] = id;
	++m_load_seq[index]; // a load of the previous occupant (or map) must not land in this slot
	m_load_in_flight[index] = false;
	m_loaded[index] = false;
	m_load_retry_at[index] = 0;
	if (m_map.empty() || id.empty()) {
		// The map name is captured by the first update after OnMapStart; UpdateRetries() starts the
		// load once both halves of the key exist.
		return;
	}
	// The original's player-active flag is cleared here (it only becomes true with the row loaded).
	m_state[index].active = false;
	if (m_table_ready) {
		StartLoad(slot, id);
	}
	// Otherwise UpdateRetries() starts the load once the CREATE TABLE succeeded.
}

void LrMapsPlugin::OnResetPlayerStats(int slot, const char* steamid) {
	if (slot < 0 || slot >= kMaxPlayers || m_db == nullptr) {
		return;
	}
	const std::size_t index = static_cast<std::size_t>(slot);
	const std::string id = steamid != nullptr ? steamid : "";
	m_steamids[index] = id;
	// An answer of a load that is still in flight would put the pre-reset row back into the slot.
	++m_load_seq[index];
	m_load_in_flight[index] = false;
	const bool was_loaded = m_loaded[index];
	m_load_retry_at[index] = 0;
	// OnResetLoadedHook() of the original: the UPDATE (countplays included) and the local reset.
	if (m_table_ready) {
		const uint64_t seq = m_load_seq[index];
		Statement reset = ResetStatement(m_table, id);
		m_db->Query(std::move(reset.sql), std::move(reset.params),
					[this, index, id, seq, was_loaded](const plgdb::Result& result) {
						if (m_load_seq[index] != seq || m_steamids[index] != id) {
							return;
						}
						if (!result.Ok()) {
							// The row was not zeroed; the local zero still makes the next save
							// write the zero-based counters, so the reset is not undone.
							LogSaveError(result.Error());
							m_loaded[index] = was_loaded;
							return;
						}
						// A row that was loaded is known to exist and is zeroed now: the local reset
						// is the same state, so there is nothing to re-read. A slot without a row
						// (or whose row was never loaded) has to SELECT/INSERT once the statement
						// finished.
						m_loaded[index] = was_loaded;
					});
		ResetSlot(m_state[index]);
	} else if (was_loaded) {
		// The row cannot be zeroed in the database yet, but it exists: the local reset makes the
		// next save write the zero-based counters, so the old values do not come back.
		ResetSlot(m_state[index]);
		m_loaded[index] = true;
	} else {
		// No row loaded yet: the retry loads and, when this map's row is missing, inserts it with
		// countplays = 1 once the table exists.
		m_state[index] = SlotState{};
		m_loaded[index] = false;
	}
}

// --- game events ---------------------------------------------------------------------------------

void LrMapsPlugin::DispatchEvent(const plg::string& name, void* event) {
	if (event == nullptr || !g_mapReady || !g_pLRCore->CheckCountPlayers()) {
		return;
	}
	const std::string_view event_name(name.data(), name.size());
	if (event_name == "player_death") {
		OnPlayerDeathEvent(event);
	} else if (event_name == "round_end") {
		OnRoundEndEvent(event);
	} else if (event_name == "bomb_planted") {
		OnBombPlantedEvent(event);
	} else if (event_name == "bomb_defused") {
		OnBombDefusedEvent(event);
	} else if (event_name == "hostage_killed") {
		OnHostageKilledEvent(event);
	} else if (event_name == "hostage_rescued") {
		OnHostageRescuedEvent(event);
	}
}

void LrMapsPlugin::OnPlayerDeathEvent(void* event) {
	// "attacker" and "userid" are player fields: EventPlayerSlot resolves the 0-based slot and
	// answers -1 when no player is behind them. GetEventInt on the same field answers the engine
	// user id, which is not a slot (the original indexed by it).
	const int attacker = EventPlayerSlot(event, "attacker");
	const int victim = EventPlayerSlot(event, "userid");
	lr_maps::OnPlayerDeath(m_state, attacker, victim);
}

void LrMapsPlugin::OnRoundEndEvent(void* event) {
	if (__s2sdk_GetEventInt == nullptr || __s2sdk_GetClientTeam == nullptr) {
		return;
	}
	// "winner" is a numeric field: the same GetInt the original used.
	const int winner = static_cast<int>(s2sdk::GetEventInt(event, "winner"));
	const bool save_after_round = g_pLRCore->GetSettingsValue(levels_ranks::LR_DB_SaveDataPlayer_Mode) != 0;
	for (int slot = 0; slot < kMaxPlayers; ++slot) {
		const std::size_t index = static_cast<std::size_t>(slot);
		SlotState& state = m_state[index];
		if (!state.active || !m_loaded[index]) {
			continue;
		}
		// The original required the controller, a pawn and a positive steam id (with the s2sdk
		// equivalent of its IsClientFullyAuthenticated).
		if (!PlayerUsable(slot)) {
			continue;
		}
		CountRound(state, ClientTeam(slot), winner);
		if (save_after_round) {
			SaveData(slot);
		}
	}
}

void LrMapsPlugin::OnBombPlantedEvent(void* event) {
	lr_maps::OnBombPlanted(m_state, EventPlayerSlot(event, "userid"));
}

void LrMapsPlugin::OnBombDefusedEvent(void* event) {
	lr_maps::OnBombDefused(m_state, EventPlayerSlot(event, "userid"));
}

void LrMapsPlugin::OnHostageKilledEvent(void* event) {
	lr_maps::OnHostageKilled(m_state, EventPlayerSlot(event, "userid"));
}

void LrMapsPlugin::OnHostageRescuedEvent(void* event) {
	lr_maps::OnHostageRescued(m_state, EventPlayerSlot(event, "userid"));
}

void LrMapsPlugin::OnClientDisconnect(int32_t slot) {
	// lr_maps::OnClientDisconnect of the original.
	if (slot < 0 || slot >= kMaxPlayers) {
		return;
	}
	SaveData(static_cast<int>(slot));
	// SaveData above ran while the client data was still valid; drop the cached id now - s2sdk
	// resets the client once the disconnect callbacks return, and this is still the same slot.
	const std::size_t index = static_cast<std::size_t>(slot);
	++m_load_seq[index]; // an in-flight load of this occupant must not land in the reused slot
	m_load_in_flight[index] = false;
	m_loaded[index] = false;
	m_load_retry_at[index] = 0;
	m_steamids[index].clear();
	m_state[index] = SlotState{};
}

// --- lifecycle ------------------------------------------------------------------------------------

plg::PluginResult LrMapsPlugin::OnPluginStart() {
	try {
		g_mapReady = false;
		g_mapStartSeen = false;
		g_mapEndSeen = false;
		g_mapTransitionPending = true;

		// PITFALLS B1: registering the map listeners touches no client, so it is safe here. They
		// are the only source of the readiness every s2sdk client/map call of this plugin is gated
		// on.
		RegisterMapListeners();

		// The ClientDisconnect hook of the original: it saves the row while the client data is
		// still valid, so it is not optional (the statistics would only be saved on a map change).
		if (__s2sdk_OnClientDisconnect_Register != nullptr) {
			s2sdk::OnClientDisconnect_Register(&OnClientDisconnectListener);
			m_disconnect_registered = true;
		} else {
			LogError("%s the s2sdk OnClientDisconnect listener is unavailable - the statistics are only saved on a map change",
					 kLogTag);
		}

		// PITFALLS C12: the six Pre hooks of the original's Hooks() switch. The handlers need the
		// player-slot and the numeric event accessors, so a build whose s2sdk lacks one reports it
		// here instead of failing silently on every event; DispatchEvent() guards them again,
		// because the generated wrappers call their slots unchecked.
		if (__s2sdk_HookEvent == nullptr) {
			LogError("%s the s2sdk event bindings are unavailable - the map statistics stay off", kLogTag);
		} else {
			for (std::size_t i = 0; i < kEventNames.size(); ++i) {
				try {
					if (s2sdk::HookEvent(plg::string(kEventNames[i]), &OnGameEvent,
										 s2sdk::HookMode::Pre) == s2sdk::EventHookError::Okay) {
						m_event_hooked[i] = true;
					} else {
						LogError("%s hooking '%s' was rejected - that counter stays off", kLogTag,
								 kEventNames[i]);
					}
				} catch (const std::exception& ex) {
					LogError("%s hooking '%s' failed: %s", kLogTag, kEventNames[i], ex.what());
				} catch (...) {
					LogError("%s hooking '%s' failed with an unknown exception", kLogTag, kEventNames[i]);
				}
			}
		}
		if (!HasPlayerSlotAccessor()) {
			LogError("%s the s2sdk player-field accessors are unavailable - the player statistics stay off", kLogTag);
		}
		if (__s2sdk_GetEventInt == nullptr) {
			LogError("%s the s2sdk numeric event accessor is unavailable - the round statistics stay off", kLogTag);
		}
		if (__s2sdk_GetCurrentMap == nullptr) {
			LogError("%s the s2sdk map-name accessor is unavailable - the map statistics stay off", kLogTag);
		}

		// AllPluginsLoaded() of the original: the event hooks first, then the LR core lookup.
		if (g_pLRCore->CoreIsLoaded()) {
			OnCoreIsReady();
		} else {
			g_pLRCore->HookOnCoreIsReady([this] { OnCoreIsReady(); });
		}
		LogInfo(std::string(kLogTag) + " ready");
		return plg::PluginResult{};
	} catch (const std::exception& ex) {
		LogError("%s OnPluginStart failed: %s", kLogTag, ex.what());
	} catch (...) {
		LogError("%s OnPluginStart failed with an unknown exception", kLogTag);
	}
	return plg::PluginResult{}; // never Failed: that stops every plugify plugin
}

plg::PluginResult LrMapsPlugin::OnPluginUpdate(std::chrono::milliseconds /*dt*/) {
	try {
		// PITFALLS B1/B2: the host pumps this from the server frame, so the first update proves a
		// live map for a plugin loaded into a running map.
		PollMapGate();
		ProcessMapTransition();
		if (m_db != nullptr) {
			m_db->Poll();
		}
		UpdateRetries();
		Tick();
	} catch (const std::exception& ex) {
		LogError("%s OnPluginUpdate failed: %s", kLogTag, ex.what());
	} catch (...) {
		LogError("%s OnPluginUpdate failed with an unknown exception", kLogTag);
	}
	return plg::PluginResult{};
}

plg::PluginResult LrMapsPlugin::OnPluginEnd() {
	try {
		// The current map's counters have to reach the database before the connection dies; a
		// changelevel/unload does not deliver OnClientDisconnect for the players it keeps.
		SaveAllForCurrentMap();
		// PITFALLS A7/C12: every registration of this library is removed again, so `plg reload`
		// leaves nothing that points into the unloaded .so.
		for (std::size_t i = 0; i < kEventNames.size(); ++i) {
			if (!m_event_hooked[i]) {
				continue;
			}
			m_event_hooked[i] = false;
			if (__s2sdk_UnhookEvent != nullptr) {
				try {
					s2sdk::UnhookEvent(plg::string(kEventNames[i]), &OnGameEvent, s2sdk::HookMode::Pre);
				} catch (...) {
					// nothing left to do at unload
				}
			}
		}
		if (m_disconnect_registered) {
			m_disconnect_registered = false;
			if (__s2sdk_OnClientDisconnect_Unregister != nullptr) {
				s2sdk::OnClientDisconnect_Unregister(&OnClientDisconnectListener);
			}
		}
		// The Unhook* are no-ops when levels_ranks is not loaded (lr_api.hpp checks the binding
		// slot); the Available() guard only keeps that intent visible here.
		if (g_pLRCore->Available()) {
			g_pLRCore->UnhookOnPlayerLoaded();
			g_pLRCore->UnhookOnResetPlayerStats();
			g_pLRCore->UnhookOnCoreIsReady();
		}
		// The gate closes last: a listener the engine may still hold for a moment must not reach a
		// client, and nothing may call into this library after it is unloaded.
		UnregisterMapListeners();
		g_mapReady = false;
		g_mapStartSeen = false;
		g_mapEndSeen = false;
		g_mapTransitionPending = false;
		m_db.reset();
	} catch (...) {
		// nothing left to do at unload
	}
	return plg::PluginResult{};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
