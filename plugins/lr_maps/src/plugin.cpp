// lr_maps — Plugify C++ port of the Pisex LR module "[LR] ExStats Maps"
// (ref/src/cs2-lvl_ranks_modules/LR_ExStatsMaps/lr_maps.cpp, 365 lines; the production plugin
// addons/lr_modules/lr_maps, vdf lr_maps.vdf).
//
// What it does (identical to the original): keeps one row per (steam, map) in the
// `<lr_table>_maps` table — map plays, kills/deaths, round counters, bomb/hostage counters and the
// playtime on the map — counts the events of the map, saves on disconnect and (when
// lr_db_savedata_player_mode is set) after every round, zeroes the row on !reset.
//
// Port notes (the rest is parity):
//   * The original hooked IServerGameDLL::GameFrame for the per-second playtime tick and
//     IServerGameClients::ClientDisconnect; the port ticks from OnPluginUpdate (the game thread)
//     and uses s2sdk's OnClientDisconnect_Post, like the levels_ranks core does.
//   * StartupServer() (the map name) becomes pisex::MapStartHook; s2sdk's GetCurrentMap() is the
//     same gpGlobals->mapname the original read, so the `name_map` values match the production
//     rows.
//   * The disconnect save drops the original's "the player must still have a pawn and a SteamID"
//     test: at post-disconnect time the engine may already have dropped them, and the counters
//     (plus the id/steam cached at OnPlayerLoaded) are what has to reach the database. The
//     database sees the same UPDATE.
//   * The database is its own plgdb connection with the core's settings
//     (GetDatabaseConfig(), "lr.sqlite3" for the SQLite fallback); a Plugify plugin cannot share
//     another plugin's connection object. Layout and statements are the original's.

#include <pisex/pisex.hpp>

#include <plg/plugin.hpp>

#include <s2sdk/s2sdk.hpp>

#include <lr_api.hpp>

#include <array>
#include <chrono>
#include <cstdint>
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

// The original's g_pLRCore.
levels_ranks::LrApi* g_pLRCore = &levels_ranks::LrApi::Get();

using namespace lr_maps;

void OnClientDisconnectListener(int32_t slot, s2sdk::NetworkDisconnectionReason reason);

class LrMapsPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// lr_maps::OnClientDisconnect of the original (s2sdk OnClientDisconnect_Post context).
	void OnClientDisconnect(int32_t slot);
	// The per-second GameFrame tick of the original (from OnPluginUpdate).
	void Tick();

	// The event handlers of the original's Hooks() switch (pisexcompat pre hooks).
	void OnPlayerDeath(pisex::IGameEvent* event);
	void OnRoundEnd(pisex::IGameEvent* event);
	void OnBombPlanted(pisex::IGameEvent* event);
	void OnBombDefused(pisex::IGameEvent* event);
	void OnHostageKilled(pisex::IGameEvent* event);
	void OnHostageRescued(pisex::IGameEvent* event);

private:
	void OnCoreIsReady();
	void OnPlayerLoaded(int slot, const char* steamid);
	void OnResetPlayerStats(int slot, const char* steamid);
	// SaveDataPlayer() of the original; queues nothing without a steam id or map name.
	void SaveData(int slot);
	// engine->GetPlayerNetworkIDString(slot) + ConvertSteamID, with the cached id as the fallback.
	std::string SteamIdOf(int slot) const;
	// The map name the row is keyed by: the captured one, CurrentMap() when the map hook has not
	// fired (a plugin loaded before the first MapStartHook).
	std::string MapName();

	State m_state{};
	std::array<std::string, kMaxSlots> m_steamids{};
	std::unique_ptr<plgdb::Database> m_db;
	std::string m_table; // <lr_table>_maps
	std::string m_map;
	std::time_t m_last_tick = 0;
	bool m_disconnect_registered = false;
	bool m_logged_missing_map = false;
};

LrMapsPlugin g_plugin;

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

void OnClientDisconnectListener(int32_t slot, s2sdk::NetworkDisconnectionReason /*reason*/) {
	try {
		g_plugin.OnClientDisconnect(slot);
	} catch (const std::exception& ex) {
		pisex::ErrorLog("%s OnClientDisconnect failed: %s", kLogTag, ex.what());
	} catch (...) {
		pisex::ErrorLog("%s OnClientDisconnect failed with an unknown exception", kLogTag);
	}
}

// --- database ------------------------------------------------------------------------------------

std::string LrMapsPlugin::MapName() {
	if (!m_map.empty()) {
		return m_map;
	}
	const std::string_view current = pisex::CurrentMap();
	if (!current.empty()) {
		return std::string(current);
	}
	if (!m_logged_missing_map) {
		m_logged_missing_map = true;
		pisex::ErrorLog("%s no map name yet - the map statistics are not loaded or saved", kLogTag);
	}
	return {};
}

void LrMapsPlugin::OnCoreIsReady() {
	// OnCoreIsReadyHook() of the original: the connection first, then the table and the hooks.
	m_table = TableName(g_pLRCore->GetTableName());
	m_db = plgdb::Database::Create(
		levels_ranks::MakeDatabaseConfig(g_pLRCore->GetDatabaseConfig(), "lr.sqlite3"));
	if (m_db == nullptr) {
		pisex::ErrorLog("%s the database configuration is invalid - the map statistics are off", kLogTag);
		return;
	}
	// The DDL is queued (FIFO before every later statement), like the original's async Query.
	m_db->Query(CreateTableSql(m_table, m_db->GetDriver()), {});

	g_pLRCore->HookOnPlayerLoaded([this](int slot, const char* steamid) { OnPlayerLoaded(slot, steamid); });
	g_pLRCore->HookOnResetPlayerStats(
		[this](int slot, const char* steamid) { OnResetPlayerStats(slot, steamid); });
}

void LrMapsPlugin::OnPlayerLoaded(int slot, const char* steamid) {
	if (slot < 0 || slot >= kMaxSlots || m_db == nullptr) {
		return;
	}
	const std::string id = steamid != nullptr ? steamid : "";
	const std::string map = MapName();
	m_steamids[static_cast<std::size_t>(slot)] = id;
	if (map.empty()) {
		return;
	}
	// g_bPlayerActive[iSlot] = false of the original (it only becomes true with the row loaded).
	m_state[static_cast<std::size_t>(slot)].active = false;
	Statement load = LoadStatement(m_table, id, map);
	m_db->Query(std::move(load.sql), std::move(load.params), [this, slot, id, map](const plgdb::Result& result) {
		SlotState& state = m_state[static_cast<std::size_t>(slot)];
		if (ApplyLoaded(state, result)) {
			return;
		}
		// No row for this (steam, map): the original defaulted the counters and inserted the row.
		ResetSlot(state);
		if (m_db != nullptr) {
			Statement insert = InsertStatement(m_table, id, map);
			m_db->Query(std::move(insert.sql), std::move(insert.params));
		}
	});
}

void LrMapsPlugin::OnResetPlayerStats(int slot, const char* steamid) {
	if (slot < 0 || slot >= kMaxSlots || m_db == nullptr) {
		return;
	}
	const std::string id = steamid != nullptr ? steamid : "";
	m_steamids[static_cast<std::size_t>(slot)] = id;
	// OnResetLoadedHook() of the original: the UPDATE (countplays included) and the local reset.
	Statement reset = ResetStatement(m_table, id);
	m_db->Query(std::move(reset.sql), std::move(reset.params));
	ResetSlot(m_state[static_cast<std::size_t>(slot)]);
}

// --- game -----------------------------------------------------------------------------------------

std::string LrMapsPlugin::SteamIdOf(int slot) const {
	// engine->GetPlayerNetworkIDString(slot) in the original.
	const std::string network_id = pisex::g_pPlayers->GetSteamID(slot);
	std::string id = ConvertSteamId(network_id);
	if (id.empty() && slot >= 0 && slot < kMaxSlots) {
		id = m_steamids[static_cast<std::size_t>(slot)];
	}
	return id;
}

void LrMapsPlugin::SaveData(int slot) {
	if (slot < 0 || slot >= kMaxSlots || m_db == nullptr) {
		return;
	}
	const std::string map = MapName();
	if (map.empty()) {
		return; // the UPDATE could not match a row without the key's second half; already logged
	}
	const std::string id = SteamIdOf(slot);
	if (id.empty()) {
		pisex::ErrorLog("%s no steam id for slot %d - the map statistics were not saved", kLogTag, slot);
		return;
	}
	Statement statement =
		SaveStatement(m_table, m_state[static_cast<std::size_t>(slot)], id, map);
	m_db->Query(std::move(statement.sql), std::move(statement.params));
}

void LrMapsPlugin::Tick() {
	// lr_maps::GameFrame of the original: a wall-clock second between the playtime ticks.
	if (m_last_tick == 0) {
		m_last_tick = std::time(nullptr);
		return;
	}
	if (std::time(nullptr) - m_last_tick < 1) {
		return;
	}
	m_last_tick = std::time(nullptr);
	if (g_pLRCore->CheckCountPlayers()) {
		TickSecond(m_state);
	}
}

void LrMapsPlugin::OnPlayerDeath(pisex::IGameEvent* event) {
	if (event == nullptr || !g_pLRCore->CheckCountPlayers()) {
		return;
	}
	lr_maps::OnPlayerDeath(m_state, event->GetInt("attacker"), event->GetInt("userid"));
}

void LrMapsPlugin::OnRoundEnd(pisex::IGameEvent* event) {
	if (event == nullptr || !g_pLRCore->CheckCountPlayers()) {
		return;
	}
	const int winner = event->GetInt("winner");
	for (int slot = 0; slot < kMaxSlots; ++slot) {
		SlotState& state = m_state[static_cast<std::size_t>(slot)];
		if (!state.active) {
			continue;
		}
		// pPlayerController && m_hPawn() && m_steamID() > 0 of the original (with the s2sdk
		// equivalent of its IsClientFullyAuthenticated).
		if (!pisex::g_pPlayers->IsAuthenticated(slot) || pisex::GetPawnEntity(slot) == 0 ||
			pisex::g_pPlayers->GetSteamID64(slot) <= 0) {
			continue;
		}
		CountRound(state, pisex::g_pPlayers->GetTeam(slot), winner);
		if (g_pLRCore->GetSettingsValue(levels_ranks::LR_DB_SaveDataPlayer_Mode)) {
			SaveData(slot);
		}
	}
}

void LrMapsPlugin::OnBombPlanted(pisex::IGameEvent* event) {
	if (event == nullptr || !g_pLRCore->CheckCountPlayers()) {
		return;
	}
	lr_maps::OnBombPlanted(m_state, event->GetInt("userid"));
}

void LrMapsPlugin::OnBombDefused(pisex::IGameEvent* event) {
	if (event == nullptr || !g_pLRCore->CheckCountPlayers()) {
		return;
	}
	lr_maps::OnBombDefused(m_state, event->GetInt("userid"));
}

void LrMapsPlugin::OnHostageKilled(pisex::IGameEvent* event) {
	if (event == nullptr || !g_pLRCore->CheckCountPlayers()) {
		return;
	}
	lr_maps::OnHostageKilled(m_state, event->GetInt("userid"));
}

void LrMapsPlugin::OnHostageRescued(pisex::IGameEvent* event) {
	if (event == nullptr || !g_pLRCore->CheckCountPlayers()) {
		return;
	}
	lr_maps::OnHostageRescued(m_state, event->GetInt("userid"));
}

void LrMapsPlugin::OnClientDisconnect(int32_t slot) {
	// lr_maps::OnClientDisconnect of the original.
	if (slot < 0 || slot >= kMaxSlots) {
		return;
	}
	m_state[static_cast<std::size_t>(slot)].active = false;
	SaveData(static_cast<int>(slot));
	m_steamids[static_cast<std::size_t>(slot)].clear(); // the slot is free again
}

// --- lifecycle ------------------------------------------------------------------------------------

plg::PluginResult LrMapsPlugin::OnPluginStart() {
	try {
		pisex::Init("levels_ranks");

		// SH_ADD_HOOK_MEMFUNC(IServerGameClients, ClientDisconnect, ...) of the original.
		if (__s2sdk_OnClientDisconnect_Post_Register != nullptr) {
			s2sdk::OnClientDisconnect_Post_Register(&OnClientDisconnectListener);
			m_disconnect_registered = true;
		}

		// StartupServer() of the original: the map name (per map, like the Pisex callback).
		pisex::g_pUtils->MapStartHook([this](const char* map) { m_map = map != nullptr ? map : ""; });

		// AllPluginsLoaded() of the original: the six event hooks.
		pisex::g_pUtils->HookEvent("player_death",
								   [](const char*, pisex::IGameEvent* event, bool) { g_plugin.OnPlayerDeath(event); });
		pisex::g_pUtils->HookEvent("round_end",
								   [](const char*, pisex::IGameEvent* event, bool) { g_plugin.OnRoundEnd(event); });
		pisex::g_pUtils->HookEvent("bomb_planted",
								   [](const char*, pisex::IGameEvent* event, bool) { g_plugin.OnBombPlanted(event); });
		pisex::g_pUtils->HookEvent("bomb_defused",
								   [](const char*, pisex::IGameEvent* event, bool) { g_plugin.OnBombDefused(event); });
		pisex::g_pUtils->HookEvent("hostage_killed",
								   [](const char*, pisex::IGameEvent* event, bool) { g_plugin.OnHostageKilled(event); });
		pisex::g_pUtils->HookEvent("hostage_rescued",
								   [](const char*, pisex::IGameEvent* event, bool) { g_plugin.OnHostageRescued(event); });

		if (g_pLRCore->CoreIsLoaded()) {
			OnCoreIsReady();
		} else {
			g_pLRCore->HookOnCoreIsReady([this] { OnCoreIsReady(); });
		}
		return plg::PluginResult{};
	} catch (const std::exception& ex) {
		pisex::ErrorLog("%s OnPluginStart failed: %s", kLogTag, ex.what());
	} catch (...) {
		pisex::ErrorLog("%s OnPluginStart failed with an unknown exception", kLogTag);
	}
	return plg::PluginResult{}; // never Failed: that stops every plugify plugin
}

plg::PluginResult LrMapsPlugin::OnPluginUpdate(std::chrono::milliseconds /*dt*/) {
	try {
		pisex::Poll();
		if (m_db != nullptr) {
			m_db->Poll();
		}
		Tick();
	} catch (const std::exception& ex) {
		pisex::ErrorLog("%s OnPluginUpdate failed: %s", kLogTag, ex.what());
	} catch (...) {
		pisex::ErrorLog("%s OnPluginUpdate failed with an unknown exception", kLogTag);
	}
	return plg::PluginResult{};
}

plg::PluginResult LrMapsPlugin::OnPluginEnd() {
	try {
		if (m_disconnect_registered && __s2sdk_OnClientDisconnect_Post_Unregister != nullptr) {
			s2sdk::OnClientDisconnect_Post_Unregister(&OnClientDisconnectListener);
			m_disconnect_registered = false;
		}
		g_pLRCore->UnhookOnPlayerLoaded();
		g_pLRCore->UnhookOnResetPlayerStats();
		g_pLRCore->UnhookOnCoreIsReady();
		pisex::Shutdown();
		m_db.reset();
	} catch (...) {
		// nothing left to do at unload
	}
	return plg::PluginResult{};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
