#include "RoundHeatmap.h"
#include "RoundHeatmapMath.h"

#include "../Filmmaker.h" // PlayingDemoPath
#include "../Demo/DemoInfoHelper.h"
#include "../Debug/MvmDebugLog.h"
#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"
#include "../Platform/TextEncoding.h"
#include "../../MirvTime.h"
#include "../../../Shared/AfxConsole.h"

#include <Windows.h>
#include <DirectXPackedVector.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

extern CMirvTime g_MirvTime;

namespace Filmmaker::Heatmap {
namespace {

enum class Load { Idle, Loading, Ready, Failed };

std::mutex g_mutex; // guards everything below that is not atomic
std::atomic<bool> g_on{ false };
float g_radius = kRadiusDefault;
int g_teams = 0; // 0 = everyone, 2 = T, 3 = CT

// The playing demo's paths, read by the loader thread.
std::wstring g_demo;
std::shared_ptr<const DemoPaths> g_paths;
Load g_load = Load::Idle;
std::string g_error;
uint64_t g_pathsSerial = 0;
std::thread g_loader;
std::atomic<bool> g_loaderCancel{ false };

// The grid, built by the builder thread for one round, team filter and window.
std::thread g_builder;
std::atomic<bool> g_building{ false };
std::shared_ptr<const GpuGrid> g_grid;
uint64_t g_gridSerial = 0;
Window g_window;
int g_gridRound = -1, g_gridTeams = -1;
uint64_t g_gridPaths = 0;
size_t g_gridSegments = 0;
int g_gridPlayers = 0;
float g_gridMaxVisits = 0;
double g_buildMs = 0;
bool g_shownOnce = false; // a grid was published since the switch went on (scrub cache revision)

int g_round = -1; // the round at the demo tick, -1 = none
int g_tick = 0;

std::atomic<float> g_camX{ 0 }, g_camY{ 0 }, g_camZ{ 0 };
std::atomic<bool> g_haveCamera{ false };

const char* TeamsToken(int teams) { return teams == 2 ? "t" : teams == 3 ? "ct" : "all"; }

const char* StateToken() {
	if (!g_on.load()) return "off";
	if (g_demo.empty()) return "nodemo";
	switch (g_load) {
	case Load::Loading: return "loading";
	case Load::Failed: return "error";
	case Load::Ready:
		if (g_round < 0) return "noround";
		return g_paths && MatchRoundNumber(*g_paths, g_round) == 0 ? "warmup" : "ready";
	default: return "idle";
	}
}

// The helper's error JSON in place of a paths file.
std::string HelperError(const std::string& bytes) {
	JsonValue root;
	if (JsonParse(bytes, root) && root.type == JsonValue::Type::Object)
		if (const JsonValue* e = root.Find("error")) return "the demo could not be read: " + e->AsString("unknown error");
	return "the demo scan returned something unreadable";
}

void JoinLoader() {
	g_loaderCancel.store(true);
	if (g_loader.joinable()) g_loader.join();
	g_loaderCancel.store(false);
}

void JoinBuilderIfDone(bool wait) {
	if (!g_builder.joinable()) return;
	if (!wait && g_building.load()) return;
	g_builder.join();
}

// Main thread, g_mutex not held.
void StartLoad(const std::wstring& demo) {
	JoinLoader();
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_demo = demo;
		g_paths.reset();
		g_grid.reset();
		++g_gridSerial;
		g_load = Load::Loading;
		g_error.clear();
		g_round = -1;
	}
	g_loader = std::thread([demo] {
		const auto start = std::chrono::steady_clock::now();
		std::string error;
		const std::string bytes = ReadRoundPathsViaHelper(demo, error, &g_loaderCancel);
		if (g_loaderCancel.load()) return;
		auto paths = std::make_shared<DemoPaths>();
		bool ok = false;
		if (!bytes.empty()) {
			if (LooksLikePaths(bytes.data(), bytes.size())) ok = ParsePaths(bytes.data(), bytes.size(), *paths, error);
			else error = HelperError(bytes);
		}
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_demo != demo) return;
		if (ok) {
			g_paths = paths;
			g_load = Load::Ready;
			++g_pathsSerial;
		} else {
			g_load = Load::Failed;
			g_error = error.empty() ? "the demo scan failed" : error;
		}
		MvmDebugLog_LinefAlways("heatmap", "paths %s demo=%s map=%s rounds=%zu bytes=%zu %.0f ms%s%s", ok ? "ready" : "failed",
			WideToUtf8(demo).c_str(), paths->map.c_str(), paths->rounds.size(), bytes.size(), ms, ok ? "" : " error=",
			ok ? "" : g_error.c_str());
	});
}

// Main thread, g_mutex held: start building the grid for this round around the camera.
void StartBuild(int round, float cx, float cy) {
	const auto paths = g_paths;
	const Window window = WindowAround(cx, cy, g_radius);
	const int teams = g_teams;
	const uint64_t pathsSerial = g_pathsSerial;
	g_window = window;
	g_gridRound = round;
	g_gridTeams = teams;
	g_gridPaths = pathsSerial;
	g_building.store(true);
	g_builder = std::thread([paths, round, window, teams, pathsSerial] {
		const auto start = std::chrono::steady_clock::now();
		Grid grid;
		BuildGrid(paths->rounds[size_t(round)], paths->sampleStep, window, grid, teams);
		std::shared_ptr<GpuGrid> gpu;
		if (!grid.texels.empty()) {
			gpu = std::make_shared<GpuGrid>();
			gpu->x0 = window.x0;
			gpu->y0 = window.y0;
			gpu->cell = window.cell;
			gpu->size = window.size;
			gpu->radius = window.radius;
			gpu->baseZ = grid.baseZ;
			gpu->texels.resize(grid.texels.size());
			for (size_t i = 0; i < grid.texels.size(); ++i)
				gpu->texels[i] = DirectX::PackedVector::XMConvertFloatToHalf(grid.texels[i]);
		}
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			// Superseded while building (a seek, a team switch, a new demo): the next frame builds again.
			if (g_gridRound == round && g_gridTeams == teams && g_gridPaths == pathsSerial && g_window.x0 == window.x0
				&& g_window.y0 == window.y0 && g_window.cell == window.cell) {
				g_grid = gpu;
				++g_gridSerial;
				g_gridSegments = grid.segments;
				g_gridPlayers = grid.players;
				g_gridMaxVisits = grid.maxVisits;
				g_buildMs = ms;
				g_shownOnce = true;
			}
		}
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("heatmap", "grid round=%d teams=%s centre=(%.0f %.0f) radius=%.0f %dx%d cell=%.2f baseZ=%.0f segments=%zu players=%d maxVisits=%.0f %.1f ms",
				round + 1, TeamsToken(teams), window.cx, window.cy, window.radius, window.size, window.size, window.cell,
				grid.baseZ, grid.segments, grid.players, grid.maxVisits, ms);
		g_building.store(false);
	});
}

void SetOn(bool on) {
	const bool was = g_on.exchange(on);
	if (on && !was) PrepareShaders();
	if (!on) {
		std::lock_guard<std::mutex> lock(g_mutex);
		g_grid.reset();
		++g_gridSerial;
		g_gridRound = -1;
		g_window = Window{};
		g_shownOnce = false;
	}
	MvmDebugLog_LinefAlways("heatmap", "switch %s", on ? "on" : "off");
}

void PrintStatus() {
	std::lock_guard<std::mutex> lock(g_mutex);
	advancedfx::Message("mirv_filmmaker heatmap: %s, radius %.0f, teams %s, state %s\n", g_on.load() ? "on" : "off", g_radius,
		TeamsToken(g_teams), StateToken());
	if (!g_demo.empty()) advancedfx::Message("  demo: %s\n", WideToUtf8(g_demo).c_str());
	if (g_load == Load::Failed) advancedfx::Message("  error: %s\n", g_error.c_str());
	if (g_paths)
		advancedfx::Message("  paths: %s, %d match rounds (%zu with warmup); tick %d is %s %d\n", g_paths->map.c_str(),
			MatchRoundCount(*g_paths), g_paths->rounds.size(), g_tick,
			MatchRoundNumber(*g_paths, g_round) ? "round" : "pre-match round", MatchRoundNumber(*g_paths, g_round));
	if (g_grid)
		advancedfx::Message("  grid: round %d, %dx%d texels of %.2f units around (%.0f, %.0f), %zu segments, %d players, up to %.0f visits, built in %.1f ms\n",
			g_gridRound + 1, g_grid->size, g_grid->size, g_grid->cell, g_window.cx, g_window.cy, g_gridSegments, g_gridPlayers,
			g_gridMaxVisits, g_buildMs);
}

} // namespace

void RunMainThreadFrame() {
	if (!g_on.load()) return;
	JoinBuilderIfDone(false);
	const std::wstring demo = PlayingDemoPath();
	bool load = false;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (demo.empty()) {
			if (!g_demo.empty()) {
				g_demo.clear();
				g_paths.reset();
				g_grid.reset();
				++g_gridSerial;
				g_load = Load::Idle;
				g_round = -1;
				g_gridRound = -1;
			}
			return;
		}
		load = demo != g_demo;
	}
	if (load) {
		StartLoad(demo);
		return;
	}
	int tick = 0;
	if (!g_MirvTime.GetCurrentDemoTick(tick)) return;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_load != Load::Ready || !g_paths) return;
	g_tick = tick;
	const int round = RoundAt(*g_paths, tick);
	if (round != g_round && MvmDebugLog_Active())
		MvmDebugLog_Linef("heatmap", "tick %d is round %d of %zu", tick, round + 1, g_paths->rounds.size());
	g_round = round;
	if (round < 0) {
		if (g_grid) { g_grid.reset(); ++g_gridSerial; }
		g_gridRound = -1;
		return;
	}
	if (!g_haveCamera.load() || g_building.load() || g_builder.joinable()) return;
	const float cx = g_camX.load(), cy = g_camY.load();
	if (round == g_gridRound && g_teams == g_gridTeams && g_pathsSerial == g_gridPaths && !NeedsRebuild(g_window, cx, cy, g_radius))
		return;
	StartBuild(round, cx, cy);
}

void RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* op = argc > 2 ? args->ArgV(2) : "status";
	// The Config panel appends "quiet": a radius drag would otherwise print a status line per step.
	const bool quiet = argc > 3 && 0 == _stricmp(args->ArgV(argc - 1), "quiet");
	if (0 == _stricmp(op, "on") || 0 == _stricmp(op, "1")) SetOn(true);
	else if (0 == _stricmp(op, "off") || 0 == _stricmp(op, "0")) SetOn(false);
	else if (0 == _stricmp(op, "toggle")) SetOn(!g_on.load());
	else if (0 == _stricmp(op, "radius")) {
		if (argc < 4) { advancedfx::Warning("usage: %s heatmap radius <%.0f-%.0f>\n", cmd, kRadiusMin, kRadiusMax); return; }
		std::lock_guard<std::mutex> lock(g_mutex);
		g_radius = ClampRadius(float(std::atof(args->ArgV(3))));
	} else if (0 == _stricmp(op, "teams")) {
		const char* v = argc > 3 ? args->ArgV(3) : "";
		const int teams = 0 == _stricmp(v, "t") ? 2 : 0 == _stricmp(v, "ct") ? 3 : 0 == _stricmp(v, "all") ? 0 : -1;
		if (teams < 0) { advancedfx::Warning("usage: %s heatmap teams all|t|ct\n", cmd); return; }
		std::lock_guard<std::mutex> lock(g_mutex);
		g_teams = teams;
	} else if (0 != _stricmp(op, "status")) {
		advancedfx::Message(
			"%s heatmap on|off|toggle   paint where the players walked this round on the floor around the camera\n"
			"%s heatmap radius <units>  how far around the camera (%.0f to %.0f, default %.0f)\n"
			"%s heatmap teams all|t|ct  whose paths\n"
			"%s heatmap status\n",
			cmd, cmd, kRadiusMin, kRadiusMax, kRadiusDefault, cmd, cmd);
		return;
	}
	if (!quiet) PrintStatus();
}

void AppendStateJson(std::ostringstream& out) {
	std::lock_guard<std::mutex> lock(g_mutex);
	JsonBuilder j;
	j.BeginObject();
	j.BoolField("on", g_on.load());
	j.DoubleField("radius", g_radius);
	j.StringField("teams", TeamsToken(g_teams));
	j.StringField("state", StateToken());
	// The match's own numbering: warmup and pre-match rounds are state "warmup" and not counted.
	j.IntField("round", g_paths ? MatchRoundNumber(*g_paths, g_round) : 0);
	j.IntField("rounds", g_paths ? MatchRoundCount(*g_paths) : 0);
	// Players near the camera in the grid for this round; -1 while that grid is still being built.
	const bool current = g_round >= 0 && g_gridRound == g_round && !g_building.load();
	j.IntField("players", current ? g_gridPlayers : -1);
	j.StringField("error", g_load == Load::Failed ? g_error : std::string());
	j.EndObject();
	out << ",\"heatmap\":" << j.Str();
}

bool Enabled() { return g_on.load(); }

uint64_t PictureRevision() {
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_on.load()) return 0;
	uint64_t h = 0x9e3779b97f4a7c15ull;
	auto mix = [&](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
	uint32_t radius = 0;
	std::memcpy(&radius, &g_radius, sizeof(radius));
	mix(radius);
	mix(uint64_t(g_teams));
	mix(g_pathsSerial);
	mix(g_shownOnce ? 1 : 0);
	return h;
}

std::shared_ptr<const GpuGrid> PublishedGrid(uint64_t& serial) {
	std::lock_guard<std::mutex> lock(g_mutex);
	serial = g_gridSerial;
	return g_grid;
}

void NoteCamera(float x, float y, float z) {
	if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return;
	g_camX.store(x);
	g_camY.store(y);
	g_camZ.store(z);
	g_haveCamera.store(true);
}

void Shutdown() {
	g_on.store(false);
	JoinLoader();
	JoinBuilderIfDone(true);
	ShutdownRender();
}

} // namespace Filmmaker::Heatmap
