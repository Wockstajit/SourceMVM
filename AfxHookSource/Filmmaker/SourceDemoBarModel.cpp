#include "SourceDemoBarModel.h"

#include <cstdio>

namespace SourceFilmmaker {

namespace {

std::string NameOf(int userId, const PlayerLookup& lookup) {
	std::string name;
	int team = 0;
	if (lookup && lookup(userId, name, team) && !name.empty()) return name;
	char text[16];
	std::snprintf(text, sizeof(text), "#%d", userId);
	return text;
}

} // namespace

Ui::Color DemoBarTeamColor(int team) {
	if (team == 2) return Ui::Palette::TeamT;
	if (team == 3) return Ui::Palette::TeamCT;
	return Ui::Theme::Accent;
}

std::string DemoBarKillText(const KillMarker& kill, const PlayerLookup& lookup) {
	std::string text;
	const bool self = kill.attacker <= 0 || kill.attacker == kill.victim;
	if (self) text = NameOf(kill.victim, lookup) + " died";
	else text = NameOf(kill.attacker, lookup) + " killed " + NameOf(kill.victim, lookup);
	if (!kill.weapon.empty() || kill.headshot) {
		text += " (";
		text += kill.weapon.empty() ? "?" : kill.weapon;
		if (kill.headshot) text += ", headshot";
		text += ")";
	}
	return text;
}

void DemoBarFillMarkers(Ui::DemoBarModel& model, const DemoMarkers& markers, const PlayerLookup& lookup) {
	for (const KillMarker& kill : markers.kills) {
		Ui::DemoBarKill k;
		k.tick = kill.tick;
		int team = kill.victimTeam;
		if (!team && lookup) {
			std::string name;
			lookup(kill.victim, name, team);
		}
		k.color = DemoBarTeamColor(team);
		k.headshot = kill.headshot;
		k.text = DemoBarKillText(kill, lookup);
		model.kills.push_back(std::move(k));
	}
	for (const RoundCell& cell : markers.rounds) {
		Ui::DemoBarRound r;
		r.start = cell.start;
		r.end = cell.end;
		r.number = cell.number;
		r.endTick = cell.endTick;
		r.winner = cell.winner == 2 || cell.winner == 3 ? DemoBarTeamColor(cell.winner) : Ui::Rgba(0, 0, 0, 0);
		model.rounds.push_back(r);
	}
	model.roundStarts = markers.roundStarts;
	for (const BombMarker& bomb : markers.bombs) {
		Ui::DemoBarBomb b;
		b.tick = bomb.tick;
		b.label = bomb.kind == BombMarker::Kind::Planted ? "C4" : (bomb.kind == BombMarker::Kind::Defused ? "DEFUSED" : "BOOM");
		model.bombs.push_back(b);
	}
}

const char* DemoBarCameraName(int mode) {
	switch (mode) {
	case 1: return "DEATH CAM";
	case 2: return "FREEZE CAM";
	case 3: return "FIXED CAM";
	case 4: return "FIRST PERSON";
	case 5: return "CHASE CAM";
	case 6: return "FREE CAM";
	default: return "CAMERA";
	}
}

int DemoBarNextCameraMode(int mode) {
	switch (mode) {
	case 4: return 5;
	case 5: return 6;
	default: return 4;
	}
}

} // namespace SourceFilmmaker
