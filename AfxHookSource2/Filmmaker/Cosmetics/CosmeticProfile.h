#pragma once

// Offline demo-only cosmetic OVERRIDE PROFILES, keyed by SteamID64.
//
// A profile holds the cosmetic choices the user made for one player (weapon skin, knife, gloves,
// agent). Profiles are keyed by the player's SteamID64 -- NOT by pawn/entity index -- so the
// override follows the player across pawn recreation, round restarts, death/respawn, observer
// target switches, demo seeks and entity recreation. The apply loop (CosmeticOverrideSystem)
// resolves the current owner of each econ entity by XUID and looks the profile up by SteamID.
//
// Profiles are PER DEMO. They are saved in the demo's project.mvm ("cosmetics" section, see
// Demo/DemoProjectStore.h) and loaded again when that demo is opened, so leaving a demo and coming
// back -- or restarting the game -- keeps every pick. Opening a DIFFERENT demo starts from its own
// saved picks (or none): SteamIDs are the players of one recording, so nothing leaks between demos.
// CosmeticOverrideSystem owns when to load and save. This is a movie-making tool for OFFLINE demo
// playback only; it is never used for live/online play.

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Filmmaker {

// One weapon/knife/gloves item override. `set` distinguishes "user picked something" from "left
// alone". paintKit == 0 is a legal finish (vanilla) when set == true.
struct CosmeticItem {
	bool set = false;     // true if the user overrode this item
	int defIndex = 0;     // item definition index; for a weapons-map entry this equals the map key (the
	                      // exact weapon this skin is for); for knives a real knife def (e.g. 515 =
	                      // Butterfly) drives a def/model swap
	int paintKit = 0;     // paint kit / finish id (0 = vanilla finish)
	float wear = 0.0f;    // 0..1 float wear
	int seed = 0;         // pattern seed
	int statTrak = -1;    // -1 = none, >= 0 = StatTrak kill count
};

// Agent (player pawn model) override. Render-only model swap; see CosmeticOverrideSystem for the
// (research-gated) application path.
struct CosmeticAgent {
	bool set = false;
	int defIndex = 0;        // econ agent definition index (informational)
	std::string model;       // vmdl path, e.g. "agents/models/tm_phoenix/tm_phoenix.vmdl"
};

// All cosmetic overrides for one player. Keyed externally by SteamID64 in the store.
//
// Weapons are stored PER WEAPON DEFINITION INDEX (the `weapons` map), so a player can carry a distinct
// skin on EVERY weapon type at once (AK + M4 + AWP + Deagle + USP ...), each independent and persistent
// as the player switches guns. The map key is the weapon's own def index (e.g. 7 = AK-47, 16 = M4A4);
// the value's defIndex mirrors that key. Knife/gloves/agent stay single slots (a player carries one of
// each); the knife slot keys its model/type swap by the chosen target knife def.
struct CosmeticProfile {
	std::string name;                              // last-seen display name (informational)
	std::unordered_map<int, CosmeticItem> weapons; // weaponDefIndex -> skin override (rifles/SMG/pistols/etc.)
	CosmeticItem knife;                            // melee slot
	CosmeticItem gloves;                           // hand wraps
	CosmeticAgent agent;                           // player model

	// Returns the weapon override for defIndex, or nullptr if none is set.
	CosmeticItem* FindWeapon(int defIndex);
	const CosmeticItem* FindWeapon(int defIndex) const;

	// True when nothing is overridden (used to prune empty profiles on save).
	bool Empty() const;
};

// SteamID64-keyed profile store. Owns the global "enabled" flag (runtime only; always on in a demo).
class CosmeticProfileStore {
public:
	// Returns the existing profile for steamId, inserting an empty one if absent.
	CosmeticProfile& GetOrCreate(uint64_t steamId);
	// Returns the profile for steamId, or nullptr if none exists.
	CosmeticProfile* Find(uint64_t steamId);
	const CosmeticProfile* Find(uint64_t steamId) const;

	void ClearPlayer(uint64_t steamId);
	void ClearAll();

	bool Empty() const { return m_profiles.empty(); }
	const std::unordered_map<uint64_t, CosmeticProfile>& All() const { return m_profiles; }

	bool Enabled() const { return m_enabled; }
	void SetEnabled(bool e) { m_enabled = e; }

	// Swap the whole profile map (a theme's snapshot / restore; see CosmeticOverrideSystem::BeginTheme).
	void ReplaceAll(const std::unordered_map<uint64_t, CosmeticProfile>& profiles) { m_profiles = profiles; }

	// The demo project's "cosmetics" section. Serialize() is EMPTY when no profile holds anything,
	// which the caller writes as "remove the section". Deserialize() replaces every profile; false
	// (and an empty store) for an unreadable section.
	std::string Serialize() const;
	bool Deserialize(const std::string& text);

private:
	std::unordered_map<uint64_t, CosmeticProfile> m_profiles;
	bool m_enabled = true;
};

} // namespace Filmmaker
