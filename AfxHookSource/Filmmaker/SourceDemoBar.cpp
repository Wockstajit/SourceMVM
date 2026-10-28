#include "stdafx.h"

#include "SourceDemoBar.h"
#include "SourceDemoBarModel.h"
#include "SourceDemoIndex.h"
#include "SourceDemoMarkers.h"
#include "SourceDemoRewind.h"
#include "SourceDirector.h"
#include "SourceEngine.h"

#include "WrpVEngineClient.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <AfxHookSource2/Filmmaker/Movie/DemoScrub.h>
#include <Shared/AfxConsole.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

extern WrpVEngineClient* g_VEngineClient;

namespace SourceFilmmaker {

namespace {

using namespace Ui;
using Clock = std::chrono::steady_clock;

struct PlayerInfo {
	std::string name;
	int team = 0;
};

struct State {
	unsigned markersGeneration = 0;
	DemoMarkers markers;
	std::unordered_map<int, PlayerInfo> players; // user id -> name and team, refreshed twice a second
	int observedIndex = -1;
	std::string observedName;
	Clock::time_point playersAt;
	bool loopOn = false;
	DemoBarMath::LoopRegion loop;
	bool wrapPending = false;
	Clock::time_point wrapAt;
	// The last seek, until the demo lands on it (DemoBar_SeekTarget).
	bool seekPending = false;
	int seekTarget = 0;
	Clock::time_point seekAt;
};
State g;

void Command(const char* command) {
	if (!g_VEngineClient) return;
	g_VEngineClient->ClientCmd_Unrestricted(command);
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("demo.bar", "command %s", command);
}

void RefreshPlayers() {
	const Clock::time_point now = Clock::now();
	if (now - g.playersAt < std::chrono::milliseconds(500)) return;
	g.playersAt = now;
	std::vector<PlayerState> players;
	Players_Snapshot(players);
	g.observedIndex = Players_ObservedIndex();
	g.observedName.clear();
	for (const PlayerState& player : players) {
		if (player.userId >= 0) g.players[player.userId] = PlayerInfo{ player.name, player.team };
		if (player.index == g.observedIndex) g.observedName = player.name;
	}
}

bool LookupPlayer(int userId, std::string& name, int& team) {
	auto it = g.players.find(userId);
	if (it == g.players.end()) return false;
	name = it->second.name;
	team = it->second.team;
	return true;
}

void UpdateMarkers(const DemoIndex& index, int lastTick) {
	if (index.generation == g.markersGeneration) return;
	if (index.status != DemoIndex::Status::Ready && index.status != DemoIndex::Status::Failed) return;
	g.markersGeneration = index.generation;
	g.markers = index.scan ? BuildDemoMarkers(index.scan->events, lastTick > 0 ? lastTick : index.scan->header.playbackTicks) : DemoMarkers();
	g.loopOn = false; // a new demo starts without the old one's loop
	g.loop = DemoBarMath::LoopRegion();
}

} // namespace

bool DemoBar_LoopOn() { return g.loopOn; }

void DemoBar_SeekTo(int target, int pause) {
	const DemoState demo = Demo_State();
	if (!demo.playing) return;
	const bool paused = pause < 0 ? demo.paused : pause == 1;
	const int tick = DemoBarMath::SeekTick(target, demo.totalTicks);
	// Both ways are the engine's own seek; back, it rewinds in place (SourceDemoRewind.h).
	char command[64];
	std::snprintf(command, sizeof(command), "demo_gototick %d 0 %d", tick, paused ? 1 : 0);
	g.seekPending = true;
	g.seekTarget = tick;
	g.seekAt = Clock::now();
	Command(command);
}

bool DemoBar_SeekTarget(int& target) {
	if (!g.seekPending) return false;
	const DemoState demo = Demo_State();
	// A seek stops on the demo's next packet, up to a few ticks past its tick (SourceDemoRewind.h).
	const int past = DemoRewind_Installed() ? DemoRewind_SeekLandsPast() : 1;
	if (!demo.playing || (demo.tick >= g.seekTarget - 1 && demo.tick <= g.seekTarget + past)
		|| Clock::now() - g.seekAt > std::chrono::seconds(5)) {
		g.seekPending = false;
		return false;
	}
	target = g.seekTarget;
	return true;
}

void DemoBar_BuildModel(DemoBarModel& model) {
	model = DemoBarModel();
	const DemoState demo = Demo_State();
	model.demo = demo.playing;
	if (!demo.playing) return;
	model.paused = demo.paused;
	model.tick = demo.tick;
	model.lastTick = demo.totalTicks;
	model.intervalPerTick = demo.intervalPerTick > 0 ? demo.intervalPerTick : 0.015;
	model.timescale = demo.timescale > 0 ? demo.timescale : 1.0;

	const DemoIndex index = DemoIndex_Get();
	UpdateMarkers(index, demo.totalTicks);
	RefreshPlayers();
	model.observed = g.observedName;
	model.cameraMode = DirectorMath::CameraButtonName(Players_ObserverMode());
	DemoBarFillMarkers(model, g.markers, &LookupPlayer);
	switch (index.status) {
	case DemoIndex::Status::Scanning: model.markerNote = "Reading the demo's kills and rounds..."; break;
	case DemoIndex::Status::Failed: model.markerNote = "No markers: " + index.error; break;
	case DemoIndex::Status::None: model.markerNote = "No markers: the demo started before the hook saw it"; break;
	default: break;
	}
	model.loopOn = g.loopOn;
	model.loop = g.loop;
}

DemoBarModel::Workspace DemoBar_Apply(const std::vector<DemoBarAction>& actions) {
	DemoBarModel::Workspace open = DemoBarModel::Workspace::None;
	for (const DemoBarAction& action : actions) {
		char command[96];
		switch (action.kind) {
		case DemoBarAction::Kind::TogglePause:
			Command("demo_togglepause");
			break;
		case DemoBarAction::Kind::Seek:
			DemoBar_SeekTo(action.tick);
			break;
		case DemoBarAction::Kind::Scrub:
			// The world follows the scrubber live and lands a release exactly (CS2's DemoScrub).
			if (action.value != 0) Filmmaker::DemoScrub_Release((double)action.tick);
			else Filmmaker::DemoScrub_Drag((double)action.tick);
			break;
		case DemoBarAction::Kind::Speed:
			std::snprintf(command, sizeof(command), "demo_timescale %.4f", action.value);
			Command(command);
			break;
		case DemoBarAction::Kind::ToggleLoop:
			DemoBar_LoopCommand("toggle");
			break;
		case DemoBarAction::Kind::LoopRange:
			DemoBar_SetLoop(action.tick, (int)action.value);
			break;
		case DemoBarAction::Kind::CameraEditor:
			open = DemoBarModel::Workspace::Editor;
			break;
		case DemoBarAction::Kind::Config:
			open = DemoBarModel::Workspace::Config;
			break;
		case DemoBarAction::Kind::NextCamera:
			Director_StepCamera(1);
			break;
		}
	}
	return open;
}

void DemoBar_Frame() {
	if (!g.loopOn || !g.loop.Valid()) return;
	const DemoState demo = Demo_State();
	if (!demo.playing) return;
	if (g.wrapPending) {
		// Wait for the seek to land before judging the playhead again.
		if (demo.tick < g.loop.out || Clock::now() - g.wrapAt > std::chrono::seconds(3)) g.wrapPending = false;
		return;
	}
	if (DemoBarMath::LoopWraps(g.loop, demo.tick, !demo.paused)) {
		g.wrapPending = true;
		g.wrapAt = Clock::now();
		DemoBar_SeekTo(g.loop.in);
	}
}

void DemoBar_SetLoop(int in, int out) {
	const DemoState demo = Demo_State();
	const DemoBarMath::LoopRegion r = DemoBarMath::LoopFromFlags(in < 0 ? 0 : in, out < 0 ? 0 : out);
	g.loop = r;
	if (demo.totalTicks > 0 && g.loop.out > demo.totalTicks) g.loop.out = demo.totalTicks;
	if (!g.loop.Valid()) g.loop = r;
	g.wrapPending = false;
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("demo.loop", "range in=%d out=%d", g.loop.in, g.loop.out);
}

void DemoBar_LoopCommand(const char* action) {
	const DemoState demo = Demo_State();
	bool on = g.loopOn;
	if (0 == _strnicmp(action, "range ", 6)) { // range <in> <out>
		int in = 0, out = 0;
		if (std::sscanf(action + 6, "%d %d", &in, &out) == 2) DemoBar_SetLoop(in, out);
		advancedfx::Message("loop: %s, in %d, out %d\n", g.loopOn ? "on" : "off", g.loop.in, g.loop.out);
		return;
	}
	if (0 == _stricmp(action, "on")) on = true;
	else if (0 == _stricmp(action, "off")) on = false;
	else if (0 == _stricmp(action, "toggle")) on = !on;
	else if (0 != _stricmp(action, "state")) {
		advancedfx::Message("mirv_filmmaker loop on|off|toggle|state | range <in> <out>\n");
		return;
	}
	if (on && !g.loopOn) {
		if (!g.loop.Valid()) g.loop = DemoBarMath::SeedLoop(demo.tick, demo.totalTicks);
		g.loopOn = true;
		g.wrapPending = false;
		if (demo.playing && (demo.tick < g.loop.in || demo.tick >= g.loop.out)) DemoBar_SeekTo(g.loop.in);
	} else if (!on) {
		g.loopOn = false;
	}
	advancedfx::Message("loop: %s, in %d, out %d\n", g.loopOn ? "on" : "off", g.loop.in, g.loop.out);
}

} // namespace SourceFilmmaker
