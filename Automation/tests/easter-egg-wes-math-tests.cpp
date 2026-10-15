// EasterEggWesMath.h: the Wes Anderson chapter card names the round from the teams' gear at the end
// of freeze time. Typical CS2 economies, not formulas restated.
#include "../../AfxHookSource2/Filmmaker/EasterEggs/EasterEggWesMath.h"

#include <cstdio>
#include <string>

using namespace Filmmaker::WesRound;

static int g_failed = 0;
static void Expect(const char* what, const Result& r, Kind kind, int team) {
	if (r.kind != kind || r.team != team) {
		std::printf("FAILED %s: kind %d team %d (want %d / %d) title '%s'\n", what, (int)r.kind, r.team, (int)kind, team, Title(r).c_str());
		++g_failed;
	}
}
static Snapshot S(double avgT, double avgCT, double maxEq, int guns) {
	Snapshot s; s.playersT = 5; s.playersCT = 5; s.avgT = avgT; s.avgCT = avgCT; s.maxEquipment = maxEq; s.firearms = guns; return s;
}

int main() {
	// Knife round: knives only (some kept armor) -- no firearm anywhere.
	Expect("knife round", Classify(S(650, 650, 650, 0)), Kind::Knife, 0);
	// Pistol round: $800 start; a Glock/USP (200) plus kevlar (650) is the most anyone can carry.
	Expect("pistol round", Classify(S(780, 820, 1000, 10)), Kind::Pistol, 0);
	// Round 2: pistol-round winners buy SMGs and armor, the losers save.
	Expect("winners buy, losers save", Classify(S(4200, 900, 5100, 10)), Kind::OneSaves, 3);
	Expect("T save is T", Classify(S(900, 4400, 5000, 10)), Kind::OneSaves, 2);
	// A force buy into a full buy: Galils/MP9s + half armor against AKs + full utility.
	Expect("force into full", Classify(S(2900, 5200, 6200, 10)), Kind::OneForces, 2);
	// Both full buy.
	Expect("gunfight", Classify(S(5200, 5600, 6400, 10)), Kind::BothFull, 0);
	// Both save (e.g. after an exchange of half-buys).
	Expect("double eco", Classify(S(1200, 1500, 1800, 10)), Kind::BothEco, 0);
	// Force against a save: titled after the side that spent.
	Expect("force vs eco", Classify(S(3000, 1100, 3900, 10)), Kind::ForceVsEco, 2);
	// No living players on a side (demo mid-load): no claim.
	Snapshot empty = S(4000, 4000, 5000, 10); empty.playersCT = 0;
	Expect("no players", Classify(empty), Kind::Unknown, 0);
	// Titles exist for every decided kind and name the side.
	if (Title(Classify(S(4200, 900, 5100, 10))) != "The Counter-Terrorists Count Their Pennies") { std::printf("FAILED title\n"); ++g_failed; }
	if (g_failed) return 1;
	std::printf("EasterEggWesMathTests passed\n");
	return 0;
}
