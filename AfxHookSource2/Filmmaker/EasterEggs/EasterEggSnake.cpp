#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "EasterEggSnake.h"

#include "SnakeMath.h"
#include "Osu/OsuGame.h"
#include "Osu/OsuPlatform.h"

#include "../Render/RenderEtaMath.h"
#include "../Render/RenderSession.h"
#include "../Render/RenderSessionDriver.h"
#include "../Platform/JsonBuilder.h"
#include "../Debug/MvmDebugLog.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

namespace Filmmaker {
namespace {

using OsuPlatform::QpcMs;

// CS2 equipment icons (panorama/images/icons/equipment/<name>.vsvg, loaded as
// file://{images}/icons/equipment/<name>.svg). The food is one of these, picked at random.
const char* const kIcons[] = {
	"ak47", "awp", "m4a1", "m4a1_silencer", "deagle", "glock", "usp_silencer", "p250", "fiveseven", "tec9",
	"cz75a", "revolver", "elite", "mp9", "mac10", "mp7", "mp5sd", "ump45", "p90", "bizon", "famas",
	"galilar", "aug", "sg556", "ssg08", "scar20", "g3sg1", "nova", "xm1014", "mag7", "sawedoff", "negev",
	"m249", "hegrenade", "flashbang", "smokegrenade", "molotov", "incgrenade", "decoy", "c4", "defuser",
	"taser", "knife",
};
constexpr int kIconCount = (int)(sizeof(kIcons) / sizeof(kIcons[0]));
constexpr int kW = 28, kH = 18;

std::atomic<bool> g_open{ false };
std::atomic<bool> g_asking{ false }; // the "play Snake while you wait?" prompt is up
bool g_byRender = false;
Snake::Game g_game;
int g_best = 0;
double g_lastQpc = 0.0, g_acc = 0.0;
std::mutex g_inMx;
// Directions (0..3), kInClose, or the prompt's answers.
constexpr int kInClose = -1, kInYes = -3, kInNo = -4;
std::vector<int> g_inQueue;

// Render watcher.
bool g_jobActive = false, g_shownThisJob = false;
double g_captureStartQpc = 0.0;
long long g_baseDone = 0;
RenderEta g_eta;

void NewGame() { g_game.Reset(kW, kH, (uint32_t)GetTickCount64() | 1u, kIconCount); g_acc = 0.0; }

void WatchRender() {
	const bool active = RenderSessionDriver_IsActive();
	if (active != g_jobActive) {
		g_jobActive = active;
		g_shownThisJob = false;
		g_captureStartQpc = 0.0;
		g_eta = RenderEta();
		if (!active && (g_asking.load() || (g_open.load() && g_byRender))) {
			MvmDebugLog_LinefAlways("egg.snake", "render ended: %s closed", g_asking.load() ? "prompt" : "board");
			Snake_Close();
		}
	}
	if (!active) return;
	const RenderProgress& p = RenderSessionDriver_Progress();
	if (RenderSessionRef().State() != RenderSessionState::Capturing) return;
	const bool physical = p.physicalTotal > 0;
	const long long done = physical ? p.physicalDone : p.framesDone;
	const long long total = physical ? p.physicalTotal : p.framesTotal;
	if (g_captureStartQpc == 0.0) { g_captureStartQpc = QpcMs(); g_baseDone = done; }
	g_eta = EstimateRenderTime((QpcMs() - g_captureStartQpc) / 1000.0, done - g_baseDone, total - g_baseDone);
	// Already playing osu! (or Snake): the player has found something to do, so no prompt this job.
	if (!g_shownThisJob && (Osu_Active() || g_open.load())) g_shownThisJob = true;
	if (!g_shownThisJob && IsLongRender(g_eta)) {
		g_shownThisJob = true;
		MvmDebugLog_LinefAlways("egg.snake", "long render: %.0f s in, ~%.0f s total (%lld/%lld) -> %s", g_eta.elapsedS,
			g_eta.totalS, done, total, RenderSessionDriver_WantsChromeHidden() ? "UI capture, Snake stays hidden" : "asking");
		if (!RenderSessionDriver_WantsChromeHidden() && !g_open.load()) Snake_Ask();
	}
}

void Answer(bool play) {
	if (!g_asking.load()) return;
	g_asking.store(false);
	MvmDebugLog_LinefAlways("egg.snake", "prompt answered: %s", play ? "play" : "no thanks");
	if (!play) return;
	Snake_Open();
	g_byRender = g_jobActive;
}

const char* StateName(Snake::State s) {
	switch (s) {
	case Snake::State::Playing: return "playing";
	case Snake::State::Dead: return "dead";
	case Snake::State::Won: return "won";
	default: return "ready";
	}
}

} // namespace

bool Snake_Active() { return g_open.load() || g_asking.load(); }

void Snake_Open() {
	if (g_open.load()) return;
	g_asking.store(false);
	NewGame();
	g_byRender = false;
	g_lastQpc = QpcMs();
	{ std::lock_guard<std::mutex> lock(g_inMx); g_inQueue.clear(); }
	g_open.store(true);
}

void Snake_Ask() {
	if (g_open.load() || g_asking.load()) return;
	{ std::lock_guard<std::mutex> lock(g_inMx); g_inQueue.clear(); }
	g_asking.store(true);
}

void Snake_Answer(bool play) { Answer(play); }

void Snake_Close() {
	g_open.store(false);
	g_asking.store(false);
	g_byRender = false;
}

void Snake_RunFrame() {
	WatchRender();
	if (!g_open.load() && !g_asking.load()) return;
	std::vector<int> in;
	{ std::lock_guard<std::mutex> lock(g_inMx); in.swap(g_inQueue); }
	if (g_asking.load()) {
		for (int d : in) {
			if (d == kInYes || d == kInNo) { Answer(d == kInYes); break; }
		}
		if (!g_open.load()) return;
		in.clear(); // the "yes" key does not also steer the new board
	}
	for (int d : in) {
		if (d == kInClose) { Snake_Close(); return; }
		if (d < 0) continue;
		if (g_game.state == Snake::State::Dead || g_game.state == Snake::State::Won) { NewGame(); }
		g_game.Turn(d);
	}
	const double now = QpcMs();
	const double dt = std::min(500.0, now - g_lastQpc);
	g_lastQpc = now;
	if (g_game.state != Snake::State::Playing) { g_acc = 0.0; return; }
	// Wall-clock steps: a render can drag the frame rate down, the snake keeps its pace (at most a
	// few steps per frame so one hitch cannot run it into a wall unseen).
	g_acc += dt;
	for (int i = 0; i < 3 && g_acc >= g_game.StepMs() && g_game.state == Snake::State::Playing; ++i) {
		g_acc -= g_game.StepMs();
		g_game.Step();
	}
	if (g_acc > g_game.StepMs()) g_acc = g_game.StepMs();
	g_best = std::max(g_best, g_game.score);
}

bool Snake_OnWindowMessage(unsigned msg, unsigned long long wParam) {
	const bool asking = g_asking.load();
	if (!asking && !g_open.load()) return false;
	if (msg != WM_KEYDOWN && msg != WM_KEYUP && msg != WM_SYSKEYDOWN) return false;
	int d = -2;
	if (asking) {
		switch ((int)wParam) {
		case 'Y': case VK_RETURN: d = kInYes; break;
		case 'N': case VK_ESCAPE: d = kInNo; break;
		default: return false;
		}
	} else {
		switch ((int)wParam) {
		case VK_UP: case 'W': d = Snake::kUp; break;
		case VK_RIGHT: case 'D': d = Snake::kRight; break;
		case VK_DOWN: case 'S': d = Snake::kDown; break;
		case VK_LEFT: case 'A': d = Snake::kLeft; break;
		case VK_ESCAPE: d = kInClose; break;
		default: return false;
		}
	}
	if (msg != WM_KEYUP) { std::lock_guard<std::mutex> lock(g_inMx); if (g_inQueue.size() < 8) g_inQueue.push_back(d); }
	return true;
}

void Snake_BuildJson(std::string& out) {
	JsonBuilder j;
	j.BeginObject();
	j.BoolField("on", g_open.load());
	j.BoolField("ask", g_asking.load());
	if (g_asking.load()) {
		j.DoubleField("eta", g_jobActive && g_eta.valid ? std::max(0.0, g_eta.remainingS) : -1.0);
	} else if (g_open.load()) {
		j.IntField("w", g_game.w); j.IntField("h", g_game.h);
		j.Key("body"); j.BeginArray();
		for (const Snake::Cell& c : g_game.body) { j.ValueInt(c.x); j.ValueInt(c.y); }
		j.EndArray();
		j.IntField("dir", g_game.dir);
		j.Key("food"); j.BeginArray(); j.ValueInt(g_game.food.x); j.ValueInt(g_game.food.y); j.EndArray();
		j.StringField("icon", kIcons[std::clamp(g_game.foodIcon, 0, kIconCount - 1)]);
		j.IntField("score", g_game.score); j.IntField("best", g_best);
		j.StringField("state", StateName(g_game.state));
		j.DoubleField("eta", g_jobActive && g_eta.valid ? std::max(0.0, g_eta.remainingS) : -1.0);
		j.DoubleField("step", g_game.StepMs());
		j.DoubleField("acc", g_acc);
	}
	j.EndObject();
	out = j.Str();
}

std::string Snake_Status() {
	char buf[240];
	snprintf(buf, sizeof(buf), "snake: %s (%s, score %d, best %d) | render watch: %s, %.0f s in, ~%.0f s total, shown %d",
		g_open.load() ? "open" : g_asking.load() ? "asking" : "closed", StateName(g_game.state), g_game.score, g_best, g_jobActive ? "job" : "idle",
		g_eta.elapsedS, g_eta.totalS, g_shownThisJob ? 1 : 0);
	return buf;
}

} // namespace Filmmaker
