#include "../../AfxHookSource2/Filmmaker/Cosmetics/CustomizeTargetMath.h"

#include <cstdio>
#include <set>

using namespace Filmmaker::CustomizeTargetMath;

namespace {
std::set<int> g_roster;
bool InRoster(int controller) { return g_roster.count(controller) != 0; }
}

int main() {
	int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

	// CS2's demo bar rows: controller,pawn,alive,team,slot. Spectators (team 1, the GOTV
	// DemoRecorder) and rows that do not parse are not players.
	{
		const std::vector<DemoPlayer> p = ParseDemoPlayers("1,955,0,2,0;2,188,1,3,1;11,-1,0,1,10;junk;3,1001,1,3");
		CHECK(p.size() == 2);
		CHECK(p[0].controller == 1 && p[0].pawn == 955 && !p[0].alive && p[0].team == 2 && p[0].slot == 0);
		CHECK(p[1].controller == 2 && p[1].pawn == 188 && p[1].alive && p[1].team == 3 && p[1].slot == 1);
		CHECK(ParseDemoPlayers("").empty());
	}

	// The spectated controller is the LIVING row whose pawn is spectated. A dead player's row carries
	// its observer pawn, which must never resolve (an observer pawn sits at the spectated player's eye).
	{
		const std::vector<DemoPlayer> p = ParseDemoPlayers("1,955,0,2,0;2,188,1,3,1;8,155,0,3,7");
		CHECK(ControllerForSpectatedPawn(p, 188) == 2);
		CHECK(ControllerForSpectatedPawn(p, 155) == -1);
		CHECK(ControllerForSpectatedPawn(p, 955) == -1);
		CHECK(ControllerForSpectatedPawn(p, -1) == -1);
	}

	g_roster = { 1, 2, 3, 5, 8 };

	// No pick: the spectated player, first or third person alike; free cam keeps the last one.
	{
		TargetState st;
		Target t = Resolve(st, 2, InRoster);
		CHECK(t.controller == 2 && t.source == Source::Spectated);
		t = Resolve(st, -1, InRoster);
		CHECK(t.controller == 2 && t.source == Source::Last);
		t = Resolve(st, 3, InRoster);
		CHECK(t.controller == 3 && t.source == Source::Spectated);
	}

	// Nobody spectated and nothing before: no target, the modal asks for a pick.
	{
		TargetState st;
		const Target t = Resolve(st, -1, InRoster);
		CHECK(t.controller == -1 && t.source == Source::None);
	}

	// Picking a living player while watching someone else: CS2 takes a few frames to switch, and the
	// pick must hold through them instead of snapping back to the old player.
	{
		TargetState st;
		Resolve(st, 2, InRoster);
		Pick(st, 3, true);
		for (int i = 0; i < 5; ++i) CHECK(Resolve(st, 2, InRoster).controller == 3);
		Target t = Resolve(st, 3, InRoster);
		CHECK(t.controller == 3 && t.source == Source::Pick);
		// Landed. Stepping to another player on CS2's bar ends the pick.
		t = Resolve(st, 5, InRoster);
		CHECK(t.controller == 5 && t.source == Source::Spectated);
		CHECK(st.pick == -1);
	}

	// From free cam the free cam turns off first, so the OLD spectated player shows for a frame before
	// the picked one: that must not count as the user moving on.
	{
		TargetState st;
		Pick(st, 3, true);
		CHECK(Resolve(st, -1, InRoster).controller == 3);
		CHECK(Resolve(st, 2, InRoster).controller == 3);
		CHECK(Resolve(st, 3, InRoster).controller == 3);
	}

	// A dead player's first person is our free cam, which spectates nobody. The pick holds there and
	// ends when the user goes back to watching somebody.
	{
		TargetState st;
		Resolve(st, 2, InRoster);
		Pick(st, 8, false);
		CHECK(Resolve(st, 2, InRoster).controller == 8); // free cam not on yet
		for (int i = 0; i < 300; ++i) CHECK(Resolve(st, -1, InRoster).controller == 8);
		const Target t = Resolve(st, 2, InRoster);
		CHECK(t.controller == 2 && t.source == Source::Spectated);
	}

	// A switch that never lands stops waiting, and then what is really on screen wins.
	{
		TargetState st;
		Pick(st, 3, true);
		int frames = 0;
		while (Resolve(st, 2, InRoster).controller == 3 && frames < 1000) ++frames;
		CHECK(frames == kPickSettleFrames);
		CHECK(Resolve(st, 2, InRoster).source == Source::Spectated);
	}

	// A picked player who leaves the demo is dropped, and so is a last target who left.
	{
		TargetState st;
		Pick(st, 5, true);
		CHECK(Resolve(st, 5, InRoster).controller == 5);
		g_roster.erase(5);
		Target t = Resolve(st, -1, InRoster);
		CHECK(t.controller == -1 && t.source == Source::None);
		g_roster.insert(5);
		Resolve(st, 5, InRoster);
		g_roster.erase(5);
		t = Resolve(st, -1, InRoster);
		CHECK(t.controller == -1);
		g_roster.insert(5);
	}

	// ClearPick hands the target back to the spectated player at once.
	{
		TargetState st;
		Pick(st, 8, false);
		Resolve(st, -1, InRoster);
		ClearPick(st);
		CHECK(Resolve(st, 2, InRoster).source == Source::Spectated);
		CHECK(std::string(SourceName(Source::Pick)) == "pick");
		CHECK(std::string(SourceName(Source::None)) == "none");
	}

	if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
	std::printf("customize target math: ok\n");
	return 0;
}
