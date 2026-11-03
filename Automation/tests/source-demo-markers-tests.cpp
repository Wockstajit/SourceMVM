// SourceDemoMarkers: what the CS:S demo bar marks, built from a demo's game events.

#include "../../AfxHookSource/Filmmaker/SourceDemoMarkers.h"

#include <cstdio>

using namespace SourceFilmmaker;
using DemoFile::Event;
using DemoFile::Field;

static int g_failed = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n", __LINE__, #x); ++g_failed; } } while (0)

static Field Int(const char* key, int value) {
	Field f;
	f.key = key;
	f.value = value;
	return f;
}

static Field Text(const char* key, const char* text) {
	Field f;
	f.key = key;
	f.type = Field::Type::String;
	f.text = text;
	return f;
}

static Event Make(int tick, const char* name, std::vector<Field> fields = {}) {
	Event e;
	e.tick = tick;
	e.name = name;
	e.fields = std::move(fields);
	return e;
}

// The SourceTV test demo's events: it starts mid-round.
static void TestDemoStartingMidRound() {
	const std::vector<Event> events = {
		Make(490, "player_death", { Int("userid", 21), Int("attacker", 15), Text("weapon", "usp"), Int("headshot", 1) }),
		Make(490, "round_end", { Int("winner", 3), Int("reason", 7) }),
		Make(917, "round_start", { Int("timelimit", 180) }),
		Make(3284, "bomb_planted", { Int("userid", 21) }),
		Make(4250, "player_death", { Int("userid", 14), Int("attacker", 16), Text("weapon", "m4a1") }),
		Make(4310, "player_death", { Int("userid", 16), Int("attacker", 17), Text("weapon", "mp5navy") }),
	};
	const DemoMarkers m = BuildDemoMarkers(events, 4410);
	CHECK(m.kills.size() == 3);
	CHECK(m.kills[0].tick == 490 && m.kills[0].victim == 21 && m.kills[0].attacker == 15);
	CHECK(m.kills[0].headshot && m.kills[0].weapon == "usp" && m.kills[0].victimTeam == 0);
	CHECK(!m.kills[1].headshot);
	CHECK(m.killTicks.size() == 3 && m.killTicks[2] == 4310);
	CHECK(m.rounds.size() == 2);
	if (m.rounds.size() == 2) {
		CHECK(m.rounds[0].start == 0 && m.rounds[0].end == 917 && m.rounds[0].winner == 3 && m.rounds[0].endTick == 490);
		CHECK(m.rounds[1].start == 917 && m.rounds[1].end == 4410 && m.rounds[1].winner == 0 && m.rounds[1].number == 2);
	}
	CHECK(m.roundStarts.size() == 1 && m.roundStarts[0] == 917);
	CHECK(m.bombs.size() == 1 && m.bombs[0].kind == BombMarker::Kind::Planted && m.bombs[0].tick == 3284);
}

static void TestTeamsFollowPlayerTeamEvents() {
	const std::vector<Event> events = {
		Make(0, "round_start"),
		Make(5, "player_team", { Int("userid", 3), Int("team", 2), Int("disconnect", 0) }),
		Make(10, "player_death", { Int("userid", 3), Int("attacker", 4) }),
		Make(20, "player_team", { Int("userid", 3), Int("team", 3), Int("disconnect", 0) }),
		Make(30, "player_death", { Int("userid", 3), Int("attacker", 4) }),
		Make(40, "player_team", { Int("userid", 3), Int("team", 0), Int("disconnect", 1) }),
		Make(50, "player_death", { Int("userid", 3), Int("attacker", 0) }),
		Make(60, "round_start"),
		Make(60, "round_start"), // a repeat on the same tick is one round
		Make(70, "bomb_defused"),
		Make(80, "bomb_exploded"),
	};
	const DemoMarkers m = BuildDemoMarkers(events, 100);
	CHECK(m.kills.size() == 3);
	if (m.kills.size() == 3) CHECK(m.kills[0].victimTeam == 2 && m.kills[1].victimTeam == 3 && m.kills[2].victimTeam == 0);
	CHECK(m.rounds.size() == 2 && m.rounds[0].start == 0 && m.rounds[0].end == 60 && m.rounds[1].end == 100);
	CHECK(m.roundStarts.size() == 2);
	CHECK(m.bombs.size() == 2 && m.bombs[0].kind == BombMarker::Kind::Defused && m.bombs[1].kind == BombMarker::Kind::Exploded);
}

static void TestNoEvents() {
	const DemoMarkers m = BuildDemoMarkers({}, 2721);
	CHECK(m.kills.empty() && m.bombs.empty() && m.roundStarts.empty());
	CHECK(m.rounds.size() == 1 && m.rounds[0].start == 0 && m.rounds[0].end == 2721); // one unshaded span
	CHECK(BuildDemoMarkers({}, 0).rounds.empty());
}

int main() {
	TestDemoStartingMidRound();
	TestTeamsFollowPlayerTeamEvents();
	TestNoEvents();
	if (g_failed) { std::printf("%d check(s) failed\n", g_failed); return 1; }
	std::printf("SourceDemoMarkersTests passed\n");
	return 0;
}
