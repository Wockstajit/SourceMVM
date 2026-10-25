#pragma once

// Customize Player's target and roster rules (Cosmetics/CustomizeTarget.cpp), engine-free so
// Automation/tests/customize-target-math-tests.cpp can pin them.
//
// THE TARGET. CONFIG and the Camera Editor open the same modal and must agree on whose loadout it
// edits, so there is one rule, in this order:
//   1. the player picked in the modal's player list, while that player is still in the demo;
//   2. the player CS2 is spectating, in first OR third person (never through our free cam);
//   3. the last target, while still in the demo, so free cam keeps the player you were on;
//   4. nobody, and the modal asks for a pick.
// A pick waits for the view to arrive (CS2 takes a frame or two to switch, and a dead player's view
// is our free cam, so "arrived" means nobody is spectated). Once the view has arrived, or the wait
// ran out, moving the view to somebody else ends the pick: the user stepped players on CS2's bar.

#include <cstdlib>
#include <string>
#include <vector>

namespace Filmmaker {
namespace CustomizeTargetMath {

// One player as CS2's own demo bar lists it (HudDemoController.GetDemoPlayers()).
struct DemoPlayer {
	int controller = -1; // nEntIndex
	int pawn = -1;       // nPawnEntIndex: the player pawn while alive, the observer pawn once dead
	bool alive = false;
	int team = 0;        // 2 T, 3 CT
	int slot = -1;       // nPlayerSlot
};

// "controller,pawn,alive,team,slot;..." as CameraTimelineJs.h publishSpectate() writes it. A row
// that does not parse is skipped, and so is anyone not on T or CT: Customize edits players.
inline std::vector<DemoPlayer> ParseDemoPlayers(const std::string& text) {
	std::vector<DemoPlayer> out;
	size_t at = 0;
	while (at < text.size()) {
		size_t end = text.find(';', at);
		if (end == std::string::npos) end = text.size();
		const std::string row = text.substr(at, end - at);
		at = end + 1;
		int v[5] = {};
		int n = 0;
		const char* p = row.c_str();
		while (n < 5 && *p) {
			char* next = nullptr;
			const long value = std::strtol(p, &next, 10);
			if (next == p) break;
			v[n++] = (int)value;
			p = next;
			if (*p == ',') ++p;
			else break;
		}
		if (n != 5 || v[0] < 0 || (v[3] != 2 && v[3] != 3)) continue;
		DemoPlayer d;
		d.controller = v[0]; d.pawn = v[1]; d.alive = v[2] != 0; d.team = v[3]; d.slot = v[4];
		out.push_back(d);
	}
	return out;
}

// The controller whose LIVING pawn is the spectated pawn. A dead player's row names its observer
// pawn, which CS2 never spectates, so only a living player can match; -1 for nobody.
inline int ControllerForSpectatedPawn(const std::vector<DemoPlayer>& players, int pawn) {
	if (pawn < 0) return -1;
	for (const DemoPlayer& d : players)
		if (d.alive && d.pawn == pawn) return d.controller;
	return -1;
}

enum class Source { None, Pick, Spectated, Last };

inline const char* SourceName(Source s) {
	switch (s) {
	case Source::Pick: return "pick";
	case Source::Spectated: return "spectated";
	case Source::Last: return "last";
	default: return "none";
	}
}

constexpr int kPickSettleFrames = 120; // ~2 s: a view switch that has not landed by then never will

struct TargetState {
	int pick = -1;       // the controller picked in the modal
	int expect = -1;     // what the spectated controller reads once the pick's view has landed
	bool waiting = false;
	int waitFrames = 0;
	int last = -1;       // the last resolved target
};

struct Target {
	int controller = -1;
	Source source = Source::None;
};

// A living player's view lands on them (expect = their controller); a dead player's view is our
// free cam, which spectates nobody (expect = -1).
inline void Pick(TargetState& st, int controller, bool alive) {
	st.pick = controller;
	st.expect = alive ? controller : -1;
	st.waiting = true;
	st.waitFrames = 0;
}

inline void ClearPick(TargetState& st) {
	st.pick = -1;
	st.expect = -1;
	st.waiting = false;
	st.waitFrames = 0;
}

// One frame. `spectated` is the controller the view is looking through (-1 in any free cam);
// `inRoster(controller)` says whether that controller is still a T or CT player of this demo.
template <class InRoster>
inline Target Resolve(TargetState& st, int spectated, InRoster inRoster) {
	if (st.pick >= 0 && !inRoster(st.pick)) ClearPick(st);
	if (st.pick >= 0 && st.waiting) {
		if (spectated == st.expect || ++st.waitFrames > kPickSettleFrames) st.waiting = false;
	}
	if (st.pick >= 0 && !st.waiting && spectated >= 0 && spectated != st.pick) ClearPick(st);

	Target t;
	if (st.pick >= 0) { t.controller = st.pick; t.source = Source::Pick; }
	else if (spectated >= 0 && inRoster(spectated)) { t.controller = spectated; t.source = Source::Spectated; }
	else if (st.last >= 0 && inRoster(st.last)) { t.controller = st.last; t.source = Source::Last; }
	st.last = t.controller;
	return t;
}

} // namespace CustomizeTargetMath
} // namespace Filmmaker
