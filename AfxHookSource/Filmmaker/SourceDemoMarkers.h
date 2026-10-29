#pragma once

// What the demo bar marks along its scrubber, built from the demo index's events: every kill, the
// rounds as alternating cells, and the bomb. Engine-free; names and team colours that the demo
// file does not carry are filled in from the running game by the bar's host.

#include "SourceDemoFile.h"

#include <string>
#include <vector>

namespace SourceFilmmaker {

struct KillMarker {
	int tick = 0;
	int victim = -1;   // user ids
	int attacker = -1; // 0 or the victim's own id for a suicide or world kill
	bool headshot = false;
	std::string weapon;
	int victimTeam = 0; // from the demo's player_team events; 0 when it has none for this player
};

struct RoundCell {
	int start = 0; // the round_start tick, or the demo's first tick for a round already running
	int end = 0;   // the next round's start, or the demo's last tick
	int number = 0; // 1-based in this demo
	int winner = 0; // team number from round_end, 0 when the demo ends first
	int endTick = -1; // the round_end tick inside this cell, or -1
};

struct BombMarker {
	enum class Kind { Planted, Defused, Exploded };
	int tick = 0;
	Kind kind = Kind::Planted;
};

struct DemoMarkers {
	std::vector<KillMarker> kills;
	std::vector<RoundCell> rounds;
	std::vector<int> roundStarts; // ticks the round buttons step between
	std::vector<int> killTicks;   // ticks the next-kill button steps between
	std::vector<BombMarker> bombs;
};

// Events are in demo order; lastTick is the demo's last tick (its playback ticks).
DemoMarkers BuildDemoMarkers(const std::vector<DemoFile::Event>& events, int lastTick);

} // namespace SourceFilmmaker
