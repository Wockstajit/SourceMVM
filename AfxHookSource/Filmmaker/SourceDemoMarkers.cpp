#include "SourceDemoMarkers.h"

#include <unordered_map>

namespace SourceFilmmaker {

DemoMarkers BuildDemoMarkers(const std::vector<DemoFile::Event>& events, int lastTick) {
	DemoMarkers markers;
	std::unordered_map<int, int> teams; // user id -> team, as of the event being read
	std::vector<int> starts;
	std::vector<std::pair<int, int>> ends; // tick, winner

	for (const DemoFile::Event& event : events) {
		if (event.name == "player_team") {
			if (event.Int("disconnect", 0)) teams.erase(event.Int("userid"));
			else teams[event.Int("userid")] = event.Int("team", 0);
		} else if (event.name == "player_death") {
			KillMarker kill;
			kill.tick = event.tick;
			kill.victim = event.Int("userid");
			kill.attacker = event.Int("attacker", 0);
			kill.headshot = event.Int("headshot", 0) != 0;
			kill.weapon = event.Text("weapon");
			auto it = teams.find(kill.victim);
			kill.victimTeam = it != teams.end() ? it->second : 0;
			markers.kills.push_back(kill);
			markers.killTicks.push_back(kill.tick);
		} else if (event.name == "round_start") {
			if (starts.empty() || starts.back() != event.tick) starts.push_back(event.tick);
		} else if (event.name == "round_end") {
			ends.emplace_back(event.tick, event.Int("winner", 0));
		} else if (event.name == "bomb_planted" || event.name == "bomb_defused" || event.name == "bomb_exploded") {
			BombMarker bomb;
			bomb.tick = event.tick;
			bomb.kind = event.name == "bomb_planted" ? BombMarker::Kind::Planted
				: event.name == "bomb_defused" ? BombMarker::Kind::Defused : BombMarker::Kind::Exploded;
			markers.bombs.push_back(bomb);
		}
	}

	// Cells run from one round_start to the next. A demo that starts mid-round gets a first cell
	// from its first tick, so the round in progress is not left unshaded.
	std::vector<int> bounds;
	if (starts.empty() || starts.front() > 0) bounds.push_back(0);
	bounds.insert(bounds.end(), starts.begin(), starts.end());
	for (size_t i = 0; i < bounds.size(); ++i) {
		RoundCell cell;
		cell.start = bounds[i];
		cell.end = i + 1 < bounds.size() ? bounds[i + 1] : (lastTick > cell.start ? lastTick : cell.start);
		cell.number = (int)i + 1;
		for (const auto& end : ends) {
			if (end.first >= cell.start && end.first < cell.end) {
				cell.winner = end.second;
				cell.endTick = end.first;
			}
		}
		if (cell.end > cell.start) markers.rounds.push_back(cell);
	}
	markers.roundStarts = starts;
	return markers;
}

} // namespace SourceFilmmaker
