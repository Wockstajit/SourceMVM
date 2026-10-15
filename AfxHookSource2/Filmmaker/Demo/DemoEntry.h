#pragma once

// Plain data describing a single demo file and (when available) its scoreboard.

#include <string>
#include <vector>
#include <cstdint>

namespace Filmmaker {

// One player's result for a single scored round (the round-performance timeline).
struct RoundStat {
	int kills = 0;
	int headshots = 0;
	int died = 0; // 0/1
	int won = 0;  // 0/1 - did this player's team win the round
	int side = 0; // 0 = CT, 1 = T (this player's side that round)
	int mvp = 0;  // 0/1
};

// One player row, reconstructed from the <demo>.dem.info match sidecar.
// Names are not present in that file, so they are shown as "[unknown]"
// (this is exactly why CS2's own Downloaded page shows [unknown] too).
struct ScoreboardPlayer {
	uint32_t accountId = 0; // Steam account id (steamID3); 0 = unknown
	std::string name = "[unknown]";
	int kills = 0;
	int assists = 0;
	int deaths = 0;
	int score = 0;
	int mvps = 0;
	int teamIndex = 0; // 0 = first team group, 1 = second team group
	std::vector<RoundStat> perRound; // empty until a full demo parse has run
};

// One recorded loadout event from the demo pre-scan (.fmjson v5): a weapon/C4 drop or
// pickup at a specific demo tick, used by the Follow Camera's Preview-Tick jump.
struct DemoEvent {
	int tick = -1;
	std::string type;       // "weapon_drop" | "bomb_dropped" | "bomb_pickup" | "item_pickup"
	std::string item;       // e.g. "weapon_ak47", "c4"
	uint32_t accountId = 0; // Steam account id of the player involved (0 = unknown)
};

struct DemoEntry {
	std::wstring path;        // full path on disk
	std::string fileName;     // file name only (UTF-8), for display
	std::string folder;       // containing folder (UTF-8), for display
	std::string map;          // map name from the .dem header (may be empty)
	int durationSeconds = 0;  // playback length from the .dem header (0 = unknown)
	int networkProtocol = 0;  // compatibility classification from CDemoFileHeader
	int buildNum = 0;         // recording CS2 build from CDemoFileHeader
	int64_t dateModified = 0; // file last-write time, unix seconds
	int64_t matchTime = 0;    // when the match was actually played (.dem.info matchtime), unix seconds; 0 = unknown

	bool hasScoreboard = false;
	bool hasRounds = false; // a full parse produced the per-round timeline
	// Removed from the list via the UI's delete button. The file is still on disk and the entry
	// still occupies its index (so Watch(index) stays stable); BuildJson just skips it.
	bool hidden = false;
	int teamScore0 = 0;
	int teamScore1 = 0;
	std::vector<ScoreboardPlayer> players;
};

} // namespace Filmmaker
