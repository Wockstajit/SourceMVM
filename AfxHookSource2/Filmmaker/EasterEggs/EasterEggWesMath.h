#pragma once

// Wes Anderson egg: what kind of round is about to be played, from what the two teams are holding
// when freeze time ends (everyone has bought). Engine-free so the thresholds are tested
// (Automation/tests/easter-egg-wes-math-tests.cpp).
//
//   knife    nobody owns a firearm (FACEIT-style knife round for sides)
//   pistol   every player's gear is worth at most $1000 -- $800 start money plus the default pistol
//   per team, from the AVERAGE gear value of its living players:
//     eco    under $2000   (saving: pistols, maybe armor)
//     force  under $3700   (spent what they had: SMGs, cheap rifles, partial armor)
//     full   $3700 and up  (rifles + armor + utility)

#include <string>

namespace Filmmaker {
namespace WesRound {

enum class Buy { Eco, Force, Full };
enum class Kind { Unknown, Knife, Pistol, BothFull, BothEco, BothForce, OneSaves, OneForces, ForceVsEco };

constexpr double kPistolMaxPerPlayer = 1000.0;
constexpr double kEcoBelow = 2000.0;
constexpr double kForceBelow = 3700.0;

inline Buy TeamBuy(double avgEquipment) {
	if (avgEquipment < kEcoBelow) return Buy::Eco;
	if (avgEquipment < kForceBelow) return Buy::Force;
	return Buy::Full;
}

struct Snapshot {
	int playersT = 0, playersCT = 0;     // living players counted on each side
	double avgT = 0.0, avgCT = 0.0;      // average gear value ($) per side
	double maxEquipment = 0.0;           // the most valuable single player's gear ($)
	int firearms = 0;                    // owned guns (not knives, grenades, C4, zeus)
};

struct Result {
	Kind kind = Kind::Unknown;
	int team = 0; // the side the title is about (2 = T, 3 = CT), 0 = both / none
};

inline Result Classify(const Snapshot& s) {
	Result r;
	if (s.playersT <= 0 || s.playersCT <= 0) return r;
	if (s.firearms == 0) { r.kind = Kind::Knife; return r; }
	if (s.maxEquipment <= kPistolMaxPerPlayer) { r.kind = Kind::Pistol; return r; }
	const Buy t = TeamBuy(s.avgT), ct = TeamBuy(s.avgCT);
	if (t == ct) {
		r.kind = t == Buy::Full ? Kind::BothFull : (t == Buy::Eco ? Kind::BothEco : Kind::BothForce);
		return r;
	}
	// The poorer side is the story.
	const bool tPoorer = static_cast<int>(t) < static_cast<int>(ct);
	const Buy poor = tPoorer ? t : ct, rich = tPoorer ? ct : t;
	r.team = tPoorer ? 2 : 3;
	if (poor == Buy::Eco && rich == Buy::Full) r.kind = Kind::OneSaves;
	else if (poor == Buy::Force && rich == Buy::Full) r.kind = Kind::OneForces;
	else r.kind = Kind::ForceVsEco; // the forcing side against a saving side
	if (r.kind == Kind::ForceVsEco) r.team = tPoorer ? 3 : 2; // titled after the side that forced
	return r;
}

inline const char* TeamName(int team) { return team == 2 ? "Terrorists" : (team == 3 ? "Counter-Terrorists" : "Teams"); }

// The chapter card's title. ASCII only (a Panorama Label through a JSON attribute).
inline std::string Title(const Result& r) {
	const std::string team = TeamName(r.team);
	switch (r.kind) {
	case Kind::Knife: return "In Which Everyone Brings a Knife";
	case Kind::Pistol: return "The Pistol Round";
	case Kind::BothFull: return "Everybody Buys Everything";
	case Kind::BothEco: return "Nobody Buys Anything";
	case Kind::BothForce: return "Two Force Buys, Badly Planned";
	case Kind::OneSaves: return "The " + team + " Count Their Pennies";
	case Kind::OneForces: return "The " + team + " Force-Buy, Against All Advice";
	case Kind::ForceVsEco: return "The " + team + " Spend Everything Anyway";
	default: return "";
	}
}

} // namespace WesRound
} // namespace Filmmaker
