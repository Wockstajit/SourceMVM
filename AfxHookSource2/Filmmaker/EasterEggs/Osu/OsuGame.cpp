// Windows.h (pulled in below and by WindowInputHooks.h) must not define min/max macros here.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "OsuGame.h"

#include "OsuAudio.h"
#include "OsuDifficulty.h"
#include "OsuHitsounds.h"
#include "OsuLibrary.h"
#include "OsuMath.h"
#include "OsuPlatform.h"
#include "OsuSkin.h"

#include "../../Filmmaker.h"
#include "../../MouseCapture.h"
#include "../../Render/RenderSessionDriver.h"
#include "../../Platform/JsonBuilder.h"
#include "../../Platform/RawMouse.h"
#include "../../Platform/TextEncoding.h"
#include "../../Debug/MvmDebugLog.h"
#include "../../../WindowInputHooks.h" // g_UiCursorProbe.hwnd
#include "../../../../Shared/AfxConsole.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <set>
#include <thread>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace Filmmaker {
namespace {

using namespace Osu;
using OsuPlatform::QpcMs;
using OsuLibrary::Diff;
using OsuLibrary::Set;

enum class Mode { Closed = 0, Select, Loading, Play, Pause, Fail, Results };
const char* ModeName(Mode m) {
	switch (m) {
	case Mode::Select: return "select"; case Mode::Loading: return "loading"; case Mode::Play: return "play";
	case Mode::Pause: return "pause"; case Mode::Fail: return "fail"; case Mode::Results: return "results";
	default: return "closed";
	}
}

// ---- input (window-proc thread -> main thread) ---------------------------------------------------
enum { kInHit = 1, kInRelease, kInKey, kInChar, kInClick, kInWheel, kInVolWheel };
enum { kKeyK1 = 0, kKeyK2, kKeyM1, kKeyM2 };
enum Key {
	kUp = 1, kDown, kLeft, kRight, kEnter, kEsc, kBackspace, kDelete, kF1, kF2, kShiftF2, kTab, kSpace,
	kPgUp, kPgDn, kHome, kEnd, kKeyR, kKeyQ,
};
struct Input { int kind; int code; double qpc; int x, y; };
std::mutex g_inMx;
std::vector<Input> g_inQueue;
std::atomic<int> g_modeAtomic{ 0 };
std::atomic<bool> g_held[4];
std::atomic<bool> g_modsOpenAtomic{ false };
std::atomic<bool> g_lmbDown{ false }; // left button held, any mode (the options dials drag with it)

// ---- shared state -------------------------------------------------------------------------------
Mode g_m = Mode::Closed;
bool g_prevMouseFree = false;
uint32_t g_mods = 0;
double g_offset = 0.0; // ms; positive = hit later

// ---- settings (%APPDATA%\HLAE\osu\settings.ini) ----------------------------------------------
// Cursor sensitivity while playing (osu! "cursor sensitivity": the pointer moves by mouse delta x
// this, decoupled from the Windows pointer), and osu!'s three volumes: master scales both music
// (the song) and effects (hitsounds).
double g_sens = 1.0;
double g_volMaster = 0.7, g_volMusic = 0.8, g_volEffects = 0.8;
bool g_optionsOpen = false;
// The volume overlay (osu!'s scroll-for-volume): which meter the wheel moved last, and when.
int g_volSel = 0; // 0 master, 1 music, 2 effects
double g_volShownAt = -1e9;
constexpr double kVolShowMs = 1000.0, kVolFadeMs = 300.0;

std::filesystem::path SettingsPath() { return OsuPlatform::RoamingRoot() / L"settings.ini"; }
void ApplyVolumes() {
	OsuAudio::SetVolume((int)std::lround(std::clamp(g_volMaster * g_volMusic, 0.0, 1.0) * 1000.0));
	OsuHitsounds::SetVolume(std::clamp(g_volMaster * g_volEffects, 0.0, 1.0));
}
void SaveSettings() {
	std::error_code ec;
	std::filesystem::create_directories(OsuPlatform::RoamingRoot(), ec);
	std::ofstream f(SettingsPath(), std::ios::binary | std::ios::trunc);
	if (!f) return;
	char b[256];
	snprintf(b, sizeof(b), "sensitivity=%.2f\nmaster=%.2f\nmusic=%.2f\neffects=%.2f\n", g_sens, g_volMaster, g_volMusic, g_volEffects);
	f << b;
}
// First run (no settings.ini yet): start from the player's osu!lazer settings (framework.ini:
// CursorSensitivity, VolumeUniversal / VolumeMusic / VolumeEffect).
void ImportLazerSettings() {
	wchar_t app[MAX_PATH] = {};
	if (!GetEnvironmentVariableW(L"APPDATA", app, MAX_PATH)) return;
	const std::string ini = OsuPlatform::ReadFile(std::filesystem::path(app) / L"osu" / L"framework.ini");
	if (ini.empty()) return;
	auto get = [&](const char* key, double& out, double lo, double hi) {
		size_t pos = 0;
		while (pos < ini.size()) {
			size_t nl = ini.find('\n', pos);
			if (nl == std::string::npos) nl = ini.size();
			const std::string line = ini.substr(pos, nl - pos);
			pos = nl + 1;
			const size_t eq = line.find('=');
			if (eq != std::string::npos && Trim(line.substr(0, eq)) == key) { out = std::clamp(atof(line.c_str() + eq + 1), lo, hi); return; }
		}
	};
	get("CursorSensitivity", g_sens, 0.1, 6.0);
	get("VolumeUniversal", g_volMaster, 0.0, 1.0);
	get("VolumeMusic", g_volMusic, 0.0, 1.0);
	get("VolumeEffect", g_volEffects, 0.0, 1.0);
	MvmDebugLog_LinefAlways("osu.settings", "imported from osu!lazer: sensitivity %.2f master %.2f music %.2f effects %.2f",
		g_sens, g_volMaster, g_volMusic, g_volEffects);
}
void LoadSettings() {
	static bool loaded = false;
	if (loaded) return;
	loaded = true;
	std::error_code ec;
	if (!std::filesystem::exists(SettingsPath(), ec)) { ImportLazerSettings(); SaveSettings(); }
	const std::string text = OsuPlatform::ReadFile(SettingsPath());
	size_t pos = 0;
	while (pos < text.size()) {
		size_t nl = text.find('\n', pos);
		if (nl == std::string::npos) nl = text.size();
		const std::string line = text.substr(pos, nl - pos);
		pos = nl + 1;
		const size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		const std::string k = Trim(line.substr(0, eq));
		const double v = atof(line.c_str() + eq + 1);
		if (k == "sensitivity") g_sens = std::clamp(v, 0.1, 6.0);
		else if (k == "master") g_volMaster = std::clamp(v, 0.0, 1.0);
		else if (k == "music") g_volMusic = std::clamp(v, 0.0, 1.0);
		else if (k == "effects") g_volEffects = std::clamp(v, 0.0, 1.0);
	}
	ApplyVolumes();
}

// clicks forwarded to the overlay (it owns the hit-testing of its own panels)
int g_clickSeq = 0; double g_clickX = 0, g_clickY = 0;

// ---- song select ---------------------------------------------------------------------------------
std::shared_ptr<const std::vector<Set>> g_lib;
uint64_t g_libRev = 0;
std::string g_search;
int g_sort = 0; // title, artist, stars, length, bpm, creator, source
const char* const kSortNames[] = { "title", "artist", "difficulty", "length", "bpm", "mapper", "source" };
constexpr int kSortCount = 7;
std::vector<int> g_filtered;     // indices into *g_lib
int g_selSet = 0, g_selDiff = 0; // g_selSet indexes g_filtered
std::string g_selKey;            // survives refiltering
bool g_modsOpen = false;
std::vector<std::string> g_randomHistory;
std::mt19937 g_rng{ (unsigned)GetTickCount64() };
uint64_t g_viewRev = 1;
double g_previewAt = 0.0; bool g_previewPending = false;

// ---- star rating with mods (OsuDifficulty.h, lazer's calculator) --------------------------------
// Lazer stores the no-mod rating; anything the mods change (rate, EZ / HR, HD, FL) -- and maps with
// no stored rating at all (osu!stable, bundled) -- are computed on a worker thread, newest request
// first, and cached per (map, mods). Until a value is ready the stored one is shown.
constexpr uint32_t kSrMods = kModEZ | kModHR | kModDT | kModNC | kModHT | kModHD | kModFL;
struct SrJob { std::wstring osu; uint32_t mods; std::string key; double fallback; };
std::mutex g_srMx;
std::condition_variable g_srCv;
std::vector<SrJob> g_srQueue;
std::map<std::string, double> g_srCache;
std::set<std::string> g_srQueued;
std::atomic<bool> g_srDirty{ false };
bool g_srThread = false;

double ComputeStars(const std::wstring& osu, uint32_t mods, double fallback) {
	Beatmap b;
	if (!ParseBeatmap(OsuPlatform::ReadFile(osu), b) || b.mode != 0) return fallback;
	ApplyModsToDifficulty(mods, b.cs, b.ar, b.od, b.hp);
	ApplyStacking(b);
	::Filmmaker::Osu::Diff::Mods m;
	m.rate = ModRate(mods); m.hidden = (mods & kModHD) != 0; m.flashlight = (mods & kModFL) != 0;
	return ::Filmmaker::Osu::Diff::StarRating(b, m);
}

void SrWorker() {
	for (;;) {
		SrJob job;
		{
			std::unique_lock<std::mutex> lock(g_srMx);
			g_srCv.wait(lock, [] { return !g_srQueue.empty(); });
			job = g_srQueue.back(); // newest first: what is on screen now
			g_srQueue.pop_back();
		}
		double v = job.fallback;
		try { v = ComputeStars(job.osu, job.mods, job.fallback); } catch (...) {}
		{
			std::lock_guard<std::mutex> lock(g_srMx);
			g_srCache[job.key] = v;
			g_srQueued.erase(job.key);
		}
		g_srDirty.store(true);
	}
}

// The star rating of `d` with `mods`, as lazer would show it.
double Stars(const OsuLibrary::Diff& d, uint32_t mods) {
	const uint32_t m = mods & kSrMods;
	if (!m && d.stars > 0) return d.stars;
	std::string key = WideToUtf8(d.osu) + "|" + std::to_string(m);
	std::lock_guard<std::mutex> lock(g_srMx);
	auto it = g_srCache.find(key);
	if (it != g_srCache.end()) return it->second;
	if (g_srQueued.insert(key).second) {
		g_srQueue.push_back({ d.osu, m, key, d.stars });
		if (!g_srThread) { g_srThread = true; std::thread(SrWorker).detach(); }
		g_srCv.notify_one();
	}
	return d.stars;
}

const Set* SelSet() {
	if (!g_lib || g_filtered.empty()) return nullptr;
	g_selSet = std::clamp(g_selSet, 0, (int)g_filtered.size() - 1);
	return &(*g_lib)[g_filtered[g_selSet]];
}
const Diff* SelDiff() {
	const Set* s = SelSet();
	if (!s || s->diffs.empty()) return nullptr;
	g_selDiff = std::clamp(g_selDiff, 0, (int)s->diffs.size() - 1);
	return &s->diffs[g_selDiff];
}

std::string Lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }

void Refilter() {
	g_filtered.clear();
	if (!g_lib) return;
	std::vector<std::string> terms;
	{
		std::string t;
		for (char c : Lower(g_search) + " ") {
			if (c == ' ') { if (!t.empty()) terms.push_back(t); t.clear(); }
			else t += c;
		}
	}
	for (int i = 0; i < (int)g_lib->size(); ++i) {
		const Set& s = (*g_lib)[i];
		bool ok = true;
		for (const std::string& t : terms) if (s.search.find(t) == std::string::npos) { ok = false; break; }
		if (ok) g_filtered.push_back(i);
	}
	auto ci = [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; };
	const auto& L = *g_lib;
	std::stable_sort(g_filtered.begin(), g_filtered.end(), [&](int a, int b) {
		const Set& x = L[a]; const Set& y = L[b];
		switch (g_sort) {
		case 1: if (_stricmp(x.artist.c_str(), y.artist.c_str())) return ci(x.artist, y.artist); return ci(x.title, y.title);
		case 2: return x.maxStars < y.maxStars;
		case 3: return x.lengthMs < y.lengthMs;
		case 4: return x.bpm < y.bpm;
		case 5: if (_stricmp(x.creator.c_str(), y.creator.c_str())) return ci(x.creator, y.creator); return ci(x.title, y.title);
		case 6: if (x.source != y.source) return x.source < y.source; return ci(x.title, y.title);
		default: if (_stricmp(x.title.c_str(), y.title.c_str())) return ci(x.title, y.title); return ci(x.artist, y.artist);
		}
	});
	g_selSet = 0;
	for (int i = 0; i < (int)g_filtered.size(); ++i) if (L[g_filtered[i]].key == g_selKey) { g_selSet = i; break; }
	g_viewRev++;
}

void SelectionChanged() {
	if (const Set* s = SelSet()) g_selKey = s->key;
	g_previewPending = true;
	g_previewAt = QpcMs();
	g_viewRev++;
}

void SelectSet(int idx, int diff) {
	if (g_filtered.empty()) return;
	const int n = (int)g_filtered.size();
	g_selSet = ((idx % n) + n) % n;
	const Set* s = SelSet();
	g_selDiff = diff < 0 ? (s ? (int)s->diffs.size() - 1 : 0) : diff;
	if (diff == -2) g_selDiff = 0;
	SelectionChanged();
}

void MoveDiff(int delta) {
	const Set* s = SelSet();
	if (!s) return;
	const int nd = (int)s->diffs.size();
	const int d = g_selDiff + delta;
	if (d < 0) { SelectSet(g_selSet - 1, -1); return; }
	if (d >= nd) { SelectSet(g_selSet + 1, -2); return; }
	g_selDiff = d;
	SelectionChanged();
}

void Random(bool back) {
	if (g_filtered.empty()) return;
	if (back) {
		if (g_randomHistory.empty()) return;
		const std::string key = g_randomHistory.back(); g_randomHistory.pop_back();
		for (int i = 0; i < (int)g_filtered.size(); ++i) if ((*g_lib)[g_filtered[i]].key == key) { SelectSet(i, -2); return; }
		return;
	}
	if (const Set* s = SelSet()) g_randomHistory.push_back(s->key);
	SelectSet((int)(g_rng() % g_filtered.size()), -2);
}

void Preview() {
	const Diff* d = SelDiff();
	if (!d) return;
	const double rate = ModRate(g_mods);
	const double start = d->preview > 0 ? d->preview : d->lastMs * 0.4;
	if (OsuAudio::CurrentSource() == d->audioSrc) return; // same song: keep it playing
	OsuAudio::Open(d->audioSrc, d->audioName, start, rate, (g_mods & kModNC) != 0, true, start);
}

// ---- gameplay -----------------------------------------------------------------------------------
struct ObjState {
	int head = -1; double headAt = 0.0;
	int result = -1; double resultAt = 0.0;
	size_t nextCp = 0; int cpHit = 0, nodesDone = 0;
	bool tracking = false, tailHit = false;
	int spinTicks = 0, bonus = 0;
	double spin = 0.0, lastAngle = 0.0; bool haveAngle = false;
};
struct Pop { int id; double x, y; int res; double at; };

Beatmap g_map;
Diff g_diff;
std::string g_title, g_artist, g_version, g_mapper;
std::vector<ObjState> g_state;
uint32_t g_playMods = 0;
double g_rate = 1.0, g_scoreMult = 1.0, g_diffMult = 1.0;
double g_t = 0.0, g_leadStart = 0.0;
double g_clockQpc0 = 0.0, g_clockSong0 = 0.0;
uint64_t g_audioId = 0;
bool g_audioStarted = false, g_audioFailed = false, g_needSync = false;
unsigned g_lastPollSeq = 0;
double g_loadStart = 0.0;
long long g_score = 0;
int g_combo = 0, g_maxCombo = 0, g_n300 = 0, g_n100 = 0, g_n50 = 0, g_nMiss = 0;
double g_hp = 1.0, g_drainRate = 0.0, g_drainStart = 0.0;
int g_comboResult = 2; // Health::ComboEndBonus input for the combo in progress
std::deque<Pop> g_pops;
uint64_t g_objsRev = 1;
std::string g_objsSig;
double g_cx = 256, g_cy = 192;
int g_menuSel = 0;
std::string g_failReason;
int g_lastDiffIdx = -1;

constexpr double kNudgeMsPerFrame = 0.75, kResyncMs = 250.0;
double SongNow(double qpc) { return g_clockSong0 + (qpc - g_clockQpc0) * g_rate; }
void Rebase(double songMs) { g_clockQpc0 = QpcMs(); g_clockSong0 = songMs; }

Windows W() { return HitWindows(g_map.od); }
double R() { return Radius(g_map.cs); }

bool ClientRect(RECT& rc) {
	HWND hwnd = g_UiCursorProbe.hwnd;
	return hwnd && GetClientRect(hwnd, &rc) && rc.right > 0 && rc.bottom > 0;
}
bool ToOsu(int cx, int cy, double& ox, double& oy) {
	RECT rc{};
	if (!ClientRect(rc)) return false;
	const Playfield p = PlayfieldFor(rc.right, rc.bottom);
	ox = (cx - p.ox) / p.scale; oy = (cy - p.oy) / p.scale;
	return true;
}
bool OsCursorClient(int& x, int& y) {
	POINT pt{};
	HWND hwnd = g_UiCursorProbe.hwnd;
	if (!hwnd || !GetCursorPos(&pt) || !ScreenToClient(hwnd, &pt)) return false;
	x = pt.x; y = pt.y;
	return true;
}

// ---- raw mouse (sensitivity) --------------------------------------------------------------------
// osu! (lazer, raw input) moves its pointer by the mouse's RAW counts x sensitivity -- no Windows
// pointer acceleration. While playing, the shared raw-mouse sink (Platform/RawMouse.h) counts them;
// the frame (and every press) adds the new counts, scaled, to our own pointer g_vx, g_vy (client
// pixels). The OS pointer is only clipped to the window so it never leaves it. (Pinning the OS
// pointer to the centre and reading its moves -- the first version -- carried Windows acceleration
// and lost motion between re-centres: it felt wrong.)
std::mutex g_relMx;
std::atomic<bool> g_relOn{ false };
double g_vx = 0.0, g_vy = 0.0;
long long g_rawSeenX = 0, g_rawSeenY = 0; // g_relMx: the raw totals already absorbed

void RelAbsorb() { // g_relMx held: the raw counts so far, scaled, into our pointer
	long long tx = 0, ty = 0;
	RawMouse_Totals(tx, ty);
	const long long dx = tx - g_rawSeenX, dy = ty - g_rawSeenY;
	g_rawSeenX = tx; g_rawSeenY = ty;
	if (!dx && !dy) return;
	RECT rc{};
	HWND hwnd = g_UiCursorProbe.hwnd;
	if (!hwnd || !GetClientRect(hwnd, &rc)) return;
	g_vx = std::clamp(g_vx + dx * g_sens, 0.0, (double)std::max(0L, rc.right - 1));
	g_vy = std::clamp(g_vy + dy * g_sens, 0.0, (double)std::max(0L, rc.bottom - 1));
}

bool CursorClient(int& x, int& y) {
	if (g_relOn.load()) {
		std::lock_guard<std::mutex> lock(g_relMx);
		if (g_relOn.load()) { RelAbsorb(); x = (int)std::lround(g_vx); y = (int)std::lround(g_vy); return true; }
	}
	return OsCursorClient(x, y);
}

Vec2 EndPos(const HitObject& o) {
	if (o.type & kSlider) return SliderBall(o, o.endTime);
	if (o.type & kSpinner) return { 256, 192 };
	return o.pos;
}

// Autoplay's cursor: glide to each object, ride the slider ball, circle the spinner.
Vec2 AutoCursor(double t) {
	const auto& O = g_map.objects;
	if (O.empty()) return { 256, 192 };
	size_t i = 0;
	while (i < O.size() && std::max(O[i].endTime, O[i].time) < t) ++i;
	if (i >= O.size()) return EndPos(O.back());
	const HitObject& o = O[i];
	if (t >= o.time) {
		if (o.type & kSlider) return SliderBall(o, t);
		if (o.type & kSpinner) { const double a = t * 0.025; return { 256 + 60 * std::cos(a), 192 + 60 * std::sin(a) }; }
		return o.pos;
	}
	const Vec2 from = i > 0 ? EndPos(O[i - 1]) : Vec2{ 256, 192 };
	const double t0 = i > 0 ? std::max(O[i - 1].endTime, O[i - 1].time) : o.time - 1000.0;
	const double span = std::max(1.0, o.time - t0);
	const double start = std::max(t0, o.time - 600.0); // long gaps: wait, then travel
	double f = std::clamp((t - start) / std::max(1.0, o.time - start), 0.0, 1.0);
	(void)span;
	f = 1.0 - (1.0 - f) * (1.0 - f) * (1.0 - f); // ease out
	const Vec2 to = (o.type & kSpinner) ? Vec2{ 316, 192 } : o.pos;
	return from * (1.0 - f) + to * f;
}

void Fail(const char* why) {
	if (g_m != Mode::Play) return;
	g_m = Mode::Fail;
	g_failReason = why;
	g_menuSel = 0;
	OsuAudio::Pause();
	MvmDebugLog_LinefAlways("osu.play", "failed (%s) at %.0f ms", why, g_t);
}

bool Unfailable() { return (g_playMods & (kModNF | kModAT)) != 0; }

// The samples of one node of an object: a circle, a slider's head (0) / repeats / tail (slides),
// a spinner's end.
void PlayNode(const HitObject& o, int node, double t) {
	// Default (mapsounds off): just the skin's plain click on every node osu! sounds -- a circle,
	// a slider's head, each repeat and its end -- with no whistles/finishes/claps, no soft/drum
	// sets, no ticks or spinner sounds and no mapper volume changes.
	if (!OsuHitsounds::BeatmapSamples()) {
		if (!(o.type & kSpinner)) OsuHitsounds::Play({ "normal-hitnormal", 0, 100, "" });
		return;
	}
	int sound = o.sound, ns = 0, as = 0;
	if (o.type & kSlider) {
		if (node < (int)o.edgeSounds.size()) sound = o.edgeSounds[node];
		if (node < (int)o.edgeSets.size()) { ns = o.edgeSets[node].first; as = o.edgeSets[node].second; }
	}
	std::vector<SampleRef> refs;
	HitSamples(g_map, o, t, sound, ns, as, refs);
	for (const SampleRef& r : refs) OsuHitsounds::Play(r);
}
void BreakCombo() {
	if (g_combo >= 20) OsuHitsounds::PlayNamed("combobreak", 1.0);
	g_combo = 0;
}

// The combo in progress restarts its "clean" state at the first judgement of a new-combo object.
void ComboStart(int id) { if (id >= 0 && id < (int)g_map.objects.size() && Health::NewCombo(g_map, (size_t)id)) g_comboResult = 2; }
void ComboDowngrade(int to) { g_comboResult = std::min(g_comboResult, to); }
// One judgement's health (lazer HealthProcessor.ApplyResultInternal). Failing is checked here, on
// judgements only -- never by drain itself, so drain can empty the bar but the next hit refills it;
// only a judgement that leaves the bar empty fails. Bonus results (spinner ticks) and a slider's own
// ignore-result cannot fail (DrainingHealthProcessor.CheckDefaultFailCondition).
void Heal(double amount, bool canFail = true) {
	g_hp = std::clamp(g_hp + amount, 0.0, 1.0);
	if (canFail && g_m == Mode::Play && g_hp < 1e-7 && !Unfailable()) Fail("health");
}

enum HealthMode { kHealthJudgement, kHealthSliderEnd };
void AddHit(int value, double x, double y, int id, bool countsAsObject = true, HealthMode health = kHealthJudgement) {
	if (value > 0) {
		const double comboBonus = std::max(0, g_combo) * g_diffMult * g_scoreMult / 25.0;
		g_score += (long long)std::llround(value * g_scoreMult + value * comboBonus);
		g_combo++;
		g_maxCombo = std::max(g_maxCombo, g_combo);
	} else {
		BreakCombo();
	}
	if (countsAsObject) {
		switch (value) {
		case 300: g_n300++; break;
		case 100: g_n100++; break;
		case 50: g_n50++; break;
		default: g_nMiss++; break;
		}
		// osu!'s health. A circle or spinner heals by its judgement; a slider's own result heals
		// nothing (its head, ticks, repeats and tail already did) but always counts as hit.
		double gain = 0.0;
		bool hit = value > 0;
		ComboStart(id);
		if (health == kHealthJudgement) {
			if (value == 100) ComboDowngrade(1);
			else if (value < 100) ComboDowngrade(0);
			gain = Health::ForResult(value, g_map.hp);
		} else {
			hit = true;
		}
		if (hit && id >= 0 && Health::LastInCombo(g_map, (size_t)id)) gain += Health::ComboEndBonus(g_comboResult);
		Heal(gain, health == kHealthJudgement);
		g_pops.push_back({ id, x, y, value, g_t });
	}
	if (value == 0 && (g_playMods & kModSD)) Fail("sudden death");
	if (value != 300 && (g_playMods & kModPF)) Fail("perfect");
}

void BeginPlay() {
	const Diff* d = SelDiff();
	const Set* s = SelSet();
	if (!d || !s) return;
	Beatmap b;
	std::string err;
	if (!ParseBeatmap(OsuPlatform::ReadFile(d->osu), b, &err)) {
		MvmDebugLog_LinefAlways("osu.play", "parse failed: %s", err.c_str());
		return;
	}
	g_playMods = g_mods;
	ApplyModsToDifficulty(g_playMods, b.cs, b.ar, b.od, b.hp);
	if (g_playMods & kModHR) FlipY(b);
	ApplyStacking(b); // after the mods, like osu! (preempt from the final AR, flipped positions)
	double playStars = d->stars;
	if ((g_playMods & kSrMods) || playStars <= 0) {
		::Filmmaker::Osu::Diff::Mods sm;
		sm.rate = ModRate(g_playMods); sm.hidden = (g_playMods & kModHD) != 0; sm.flashlight = (g_playMods & kModFL) != 0;
		playStars = ::Filmmaker::Osu::Diff::StarRating(b, sm);
	}
	g_drainStart = b.objects.empty() ? 0.0 : b.objects.front().time;
	g_drainRate = Health::DrainRate(b, g_drainStart);
	OsuHitsounds::Prepare(b, d->resolve);
	g_map = std::move(b);
	g_diff = *d;
	g_diff.stars = playStars;
	g_title = s->title; g_artist = s->artist; g_version = d->version; g_mapper = d->creator;
	g_rate = ModRate(g_playMods);
	g_scoreMult = ModMultiplier(g_playMods);
	g_diffMult = std::clamp(std::round((g_map.hp + g_map.cs + g_map.od) / 38.0 * 5.0), 2.0, 6.0);
	g_state.assign(g_map.objects.size(), ObjState());
	g_score = 0; g_combo = g_maxCombo = g_n300 = g_n100 = g_n50 = g_nMiss = 0; g_hp = 1.0; g_comboResult = 2;
	g_pops.clear();
	const double first = g_map.objects.empty() ? 0.0 : g_map.objects.front().time;
	g_leadStart = std::min({ 0.0, -g_map.leadIn, first - Preempt(g_map.ar) - 1200.0 });
	g_t = g_leadStart;
	g_audioStarted = false; g_needSync = false; g_audioFailed = false;
	// Open (paused, at the song start) first: playing only begins once the file is ready, so the
	// music can never lag the map by however long the open took.
	OsuAudio::Stop();
	g_audioId = OsuAudio::Open(d->audioSrc, d->audioName, 0.0, g_rate, (g_playMods & kModNC) != 0, false, -1.0);
	g_loadStart = QpcMs();
	g_m = Mode::Loading;
	g_objsRev++;
	g_lastDiffIdx = g_selDiff;
	MvmDebugLog_LinefAlways("osu.play", "%s - %s [%s] objects=%zu mods=0x%x rate=%.2f AR=%.1f OD=%.1f CS=%.1f",
		g_artist.c_str(), g_title.c_str(), g_version.c_str(), g_map.objects.size(), g_playMods, g_rate, g_map.ar, g_map.od, g_map.cs);
}

void StartPlayClock() {
	Rebase(g_leadStart);
	g_t = g_leadStart;
	g_m = Mode::Play;
}

void StartAudioAt(double songMs) {
	OsuAudio::PlayFrom(songMs);
	g_audioStarted = true;
	g_needSync = true;
	g_lastPollSeq = OsuAudio::PollSeq();
}

bool CanSkip() {
	if (g_m != Mode::Play || g_map.objects.empty()) return false;
	return g_t < g_map.objects.front().time - Preempt(g_map.ar) - 2000.0;
}
void Skip() {
	if (!CanSkip()) return;
	const double target = g_map.objects.front().time - Preempt(g_map.ar) - 1000.0;
	Rebase(target);
	g_t = target;
	if (!g_audioFailed && target >= 0.0) StartAudioAt(target);
}

// A circle or slider head that was not hit: missed now (`at` = when, for its fade).
void MissHead(size_t i, double at) {
	const HitObject& o = g_map.objects[i];
	ObjState& s = g_state[i];
	s.head = 0; s.headAt = at;
	if (o.type & kCircle) { s.result = 0; s.resultAt = at; AddHit(0, o.pos.x, o.pos.y, (int)i); }
	else { ComboDowngrade(0); Heal(Health::Miss(g_map.hp)); BreakCombo(); if (g_playMods & kModSD) Fail("sudden death"); }
	g_objsRev++;
}

// A press at song time te on osu! position (ox, oy). Note lock exactly as lazer's default
// StartTimeOrderedHitPolicy: the earliest unjudged circle / slider head under the cursor (among the
// ones on screen) takes the click. It is blocked (the circle shakes) only while an earlier unjudged
// circle / slider head has not reached its start time yet; otherwise it is judged and every earlier
// unjudged circle / slider head is missed on the spot. (The old rule -- only the earliest pending
// object may be hit at all -- swallowed hits in fast streams after one mis-aim, leaving the notes
// the player had clearly hit on screen until their own miss windows ran out.)
void Press(double te, double ox, double oy) {
	const Windows w = W();
	const double r = R(), pre = Preempt(g_map.ar);
	int target = -1;
	for (size_t i = 0; i < g_map.objects.size(); ++i) {
		const HitObject& o = g_map.objects[i];
		if (o.time - pre > te) break; // not on screen yet (objects are in time order)
		if ((o.type & kSpinner) || g_state[i].head != -1) continue;
		const double d = std::sqrt((ox - o.pos.x) * (ox - o.pos.x) + (oy - o.pos.y) * (oy - o.pos.y));
		if (d <= r) { target = (int)i; break; }
	}
	if (target < 0) return;
	const HitObject& o = g_map.objects[target];
	ObjState& s = g_state[target];
	for (int k = target - 1; k >= 0; --k) {
		const HitObject& b = g_map.objects[k];
		if ((b.type & kSpinner) || g_state[k].head != -1) continue;
		if (te < b.time) return; // the last earlier unjudged circle has not reached its time: shake
		break;
	}
	const int j = Judge(te - o.time, w);
	if (j < 0) return; // too early: the circle shakes, nothing is judged
	for (int k = 0; k < target; ++k) {
		const HitObject& b = g_map.objects[k];
		if (!(b.type & kSpinner) && g_state[k].head == -1) MissHead((size_t)k, te);
	}
	s.head = j; s.headAt = te;
	if (j > 0) PlayNode(o, 0, o.time);
	if (o.type & kCircle) { s.result = j; s.resultAt = te; AddHit(j, o.pos.x, o.pos.y, target); }
	else {
		// A slider head is judged like a circle (lazer): its health and the combo's cleanliness.
		if (j == 100) ComboDowngrade(1); else if (j < 100) ComboDowngrade(0);
		Heal(Health::ForResult(j, g_map.hp));
		if (j > 0) { AddHit(30, o.pos.x, o.pos.y, target, false); s.tracking = true; }
		else { BreakCombo(); if (g_playMods & kModSD) Fail("sudden death"); }
	}
	g_objsRev++;
}

void FinishSlider(size_t i) {
	const HitObject& o = g_map.objects[i];
	ObjState& s = g_state[i];
	const int total = 1 + (int)o.checkpoints.size();
	const int got = (s.head > 0 ? 1 : 0) + s.cpHit;
	const int j = got == total ? 300 : got * 2 >= total ? 100 : got > 0 ? 50 : 0;
	s.result = j; s.resultAt = o.endTime;
	const Vec2 end = EndPos(o);
	if (s.tailHit) PlayNode(o, o.slides, o.endTime);
	AddHit(j, end.x, end.y, (int)i, true, kHealthSliderEnd);
	g_objsRev++;
}

void Update(double dtReal) {
	const Windows w = W();
	const double r = R();
	const bool autoplay = (g_playMods & kModAT) != 0;
	if (autoplay) { const Vec2 a = AutoCursor(g_t); g_cx = a.x; g_cy = a.y; }
	else { int x, y; double ox, oy; if (CursorClient(x, y) && ToOsu(x, y, ox, oy)) { g_cx = ox; g_cy = oy; } }
	bool held = g_held[0].load() || g_held[1].load() || g_held[2].load() || g_held[3].load();
	for (size_t i = 0; i < g_map.objects.size() && g_m == Mode::Play; ++i) {
		const HitObject& o = g_map.objects[i];
		ObjState& s = g_state[i];
		if (s.result != -1) continue;
		if (o.time > g_t + 50.0) break;
		const bool active = g_t >= o.time && g_t <= o.endTime;
		const bool heldHere = held || (autoplay && active);
		if (o.type & kSpinner) {
			if (active && heldHere) {
				if (autoplay) s.spin += dtReal * g_rate * 0.03;
				else {
					const double a = std::atan2(g_cy - 192.0, g_cx - 256.0);
					if (s.haveAngle) {
						double d = a - s.lastAngle;
						while (d > 3.14159265) d -= 6.2831853;
						while (d < -3.14159265) d += 6.2831853;
						s.spin += std::fabs(d);
					}
					s.lastAngle = a; s.haveAngle = true;
				}
			} else s.haveAngle = false;
			// Each full spin judges one spinner tick (bonus health), up to the spinner's tick count.
			const int ticks = SpinnerTicks(o.endTime - o.time, g_map.od);
			const int reqForBonus = SpinsRequired(o.endTime - o.time, g_map.od) + 2;
			while (s.spinTicks < ticks && s.spin / 6.2831853 >= s.spinTicks + 1) {
				// osu!stable scoring: 100 per spin, and every spin past the bonus threshold (lazer's
				// LargeBonus ticks) is worth 1000 with the skin's spinnerbonus sound.
				if (s.spinTicks < reqForBonus) { Heal(Health::kSpinTick, false); g_score += 100; }
				else { Heal(Health::kSpinBonus, false); g_score += 1000; s.bonus++; OsuHitsounds::PlayNamed("spinnerbonus", 1.0); }
				s.spinTicks++;
			}
			if (g_t > o.endTime) {
				const int req = SpinsRequired(o.endTime - o.time, g_map.od);
				const int j = SpinnerJudge(req <= 0 ? 1.0 : s.spin / 6.2831853 / req);
				s.result = j; s.resultAt = o.endTime;
				if (j > 0) PlayNode(o, 0, o.endTime);
				AddHit(j, 256, 192, (int)i);
				g_objsRev++;
			}
			continue;
		}
		if (autoplay && s.head == -1 && g_t >= o.time) {
			s.head = 300; s.headAt = o.time;
			PlayNode(o, 0, o.time);
			if (o.type & kCircle) { s.result = 300; s.resultAt = o.time; AddHit(300, o.pos.x, o.pos.y, (int)i); }
			else { Heal(Health::kGreat); AddHit(30, o.pos.x, o.pos.y, (int)i, false); }
			g_objsRev++;
		}
		// A head nobody hit in time.
		if (s.head == -1 && g_t > o.time + w.w50) MissHead(i, o.time + w.w50);
		if (o.type & kSlider) {
			// osu!'s follow circle: tracking starts with the cursor on the ball (radius r) while a
			// key is down, and holds while it stays within 2.4 r.
			if (active) {
				const Vec2 ball = SliderBall(o, g_t);
				const double d = std::sqrt((g_cx - ball.x) * (g_cx - ball.x) + (g_cy - ball.y) * (g_cy - ball.y));
				s.tracking = autoplay || (heldHere && d <= (s.tracking ? r * 2.4 : r));
			} else if (g_t < o.time) {
				s.tracking = false;
			}
			while (s.nextCp < o.checkpoints.size() && o.checkpoints[s.nextCp] <= g_t) {
				const double cp = o.checkpoints[s.nextCp];
				const bool tick = s.nextCp < o.checkpointIsTick.size() && o.checkpointIsTick[s.nextCp];
				const bool last = s.nextCp + 1 == o.checkpoints.size();
				if (s.tracking) {
					s.cpHit++; g_combo++; g_maxCombo = std::max(g_maxCombo, g_combo);
					g_score += (long long)((tick ? 10 : 30) * g_scoreMult);
					Heal(tick ? Health::kTick : Health::kNode);
					if (tick) { if (OsuHitsounds::BeatmapSamples()) OsuHitsounds::Play(TickSample(g_map, o, cp)); }
					// The end is judged 36 ms early, but osu! sounds it at the real end (FinishSlider).
					else if (last) s.tailHit = true;
					else PlayNode(o, s.nodesDone + 1, cp);
				} else if (!last) {
					// A missed tick or repeat (LargeTickMiss) hurts, breaks combo and makes the combo
					// less clean. Missing only the tail costs nothing but cleanliness.
					Heal(Health::TickMiss(g_map.hp));
					ComboDowngrade(1);
					BreakCombo();
					if (g_playMods & kModSD) Fail("sudden death");
				} else {
					ComboDowngrade(1);
				}
				if (!tick) s.nodesDone++;
				s.nextCp++;
			}
			if (g_t >= o.endTime && g_m == Mode::Play) FinishSlider(i);
		}
	}
	// Drain only while there is something to play: not in the intro, not in breaks.
	// Drain: the per-map rate osu! solves for, in song time, from the first object, never in breaks.
	// Only the part of this frame inside [first object, last object's end] drains (lazer clamps both
	// ends of the frame to that window), and draining alone never fails -- see Heal.
	if (g_m == Mode::Play && !g_map.objects.empty() && !Health::NoDrain(g_map, g_t)) {
		const HitObject& lastObj = g_map.objects.back();
		const double end = (lastObj.type & (kSlider | kSpinner)) ? lastObj.endTime : lastObj.time;
		const double now = std::clamp(g_t, g_drainStart, end);
		const double before = std::clamp(g_t - dtReal * g_rate, g_drainStart, end);
		g_hp = std::clamp(g_hp - (now - before) * g_drainRate, 0.0, 1.0);
	}
	while (!g_pops.empty() && g_t - g_pops.front().at > 900.0) g_pops.pop_front();

	const double lastEnd = g_map.objects.empty() ? 0.0 : std::max(g_map.objects.back().endTime, g_map.objects.back().time);
	if (g_m == Mode::Play && g_t > lastEnd + 1500.0) {
		g_m = Mode::Results;
		g_menuSel = 0;
		// The song keeps playing behind the results, like osu!.
		MvmDebugLog_LinefAlways("osu.results", "score=%lld acc=%.2f%% combo=%d 300=%d 100=%d 50=%d miss=%d mods=0x%x",
			g_score, Accuracy(g_n300, g_n100, g_n50, g_nMiss) * 100.0, g_maxCombo, g_n300, g_n100, g_n50, g_nMiss, g_playMods);
	}
}

void ToSelect() {
	g_m = Mode::Select;
	g_modsOpen = false; g_optionsOpen = false;
	OsuAudio::Stop();
	SelectionChanged();
}

// The volume overlay's meters, in client pixels (EasterEggOsuJs.h osuDrawVolume draws the same):
// a column at the right edge -- music above, master (largest) in the middle, effects below.
struct VolMeter { double x, y, r; };
void VolLayout(double W, double H, VolMeter out[3]) {
	const double cx = W - 0.13 * H;
	out[0] = { cx, 0.5 * H, 0.085 * H };          // master
	out[1] = { cx, 0.5 * H - 0.2 * H, 0.06 * H }; // music
	out[2] = { cx, 0.5 * H + 0.2 * H, 0.06 * H }; // effects
}
void AdjustVolume(int wheelDelta, int x, int y) {
	RECT rc{};
	if (!ClientRect(rc)) return;
	VolMeter m[3];
	VolLayout(rc.right, rc.bottom, m);
	// osu!: the hovered meter takes the wheel; otherwise master.
	int sel = 0;
	for (int i = 0; i < 3; ++i) {
		const double dx = x - m[i].x, dy = y - m[i].y;
		if (dx * dx + dy * dy <= m[i].r * m[i].r * 1.44) sel = i;
	}
	g_volSel = sel;
	double* v = sel == 0 ? &g_volMaster : (sel == 1 ? &g_volMusic : &g_volEffects);
	*v = std::clamp(std::round((*v + 0.05 * wheelDelta / (double)WHEEL_DELTA) * 100.0) / 100.0, 0.0, 1.0);
	g_volShownAt = QpcMs();
	ApplyVolumes();
	SaveSettings();
}

void SetOption(const std::string& k, double v) {
	if (k == "sens") g_sens = std::clamp(std::round(v * 100.0) / 100.0, 0.1, 6.0);
	else if (k == "master") g_volMaster = std::clamp(v, 0.0, 1.0);
	else if (k == "music") g_volMusic = std::clamp(v, 0.0, 1.0);
	else if (k == "effects") g_volEffects = std::clamp(v, 0.0, 1.0);
	else return;
	ApplyVolumes();
	SaveSettings();
	g_viewRev++;
}

// Raw mouse on while playing (main thread, every frame); off, and the pointer released, otherwise.
void ClipToClient(HWND hwnd) {
	RECT rc{};
	if (!GetClientRect(hwnd, &rc)) return;
	POINT a{ rc.left, rc.top }, b{ rc.right, rc.bottom };
	ClientToScreen(hwnd, &a); ClientToScreen(hwnd, &b);
	const RECT clip{ a.x, a.y, b.x, b.y };
	ClipCursor(&clip);
}
void UpdateRelative() {
	HWND hwnd = g_UiCursorProbe.hwnd;
	const bool want = g_m == Mode::Play && !(g_playMods & kModAT) && hwnd && GetForegroundWindow() == hwnd
		&& RawMouse_State() >= 0;
	std::lock_guard<std::mutex> lock(g_relMx);
	if (want && !g_relOn.load()) {
		int x = 0, y = 0;
		RECT rc{};
		if (!OsCursorClient(x, y) || !GetClientRect(hwnd, &rc)) return;
		RawMouse_Claim(kRawMouseOsu, true);
		g_vx = std::clamp((double)x, 0.0, (double)rc.right - 1); g_vy = std::clamp((double)y, 0.0, (double)rc.bottom - 1);
		RawMouse_Totals(g_rawSeenX, g_rawSeenY);
		ClipToClient(hwnd);
		g_relOn.store(true);
	} else if (want) {
		RelAbsorb();
		ClipToClient(hwnd); // the window may have moved
	} else if (g_relOn.load()) {
		g_relOn.store(false);
		RawMouse_Claim(kRawMouseOsu, false);
		ClipCursor(nullptr);
		POINT p{ (LONG)std::lround(g_vx), (LONG)std::lround(g_vy) };
		if (hwnd) { ClientToScreen(hwnd, &p); SetCursorPos(p.x, p.y); } // the pointer comes back where ours was
	}
	// Registration failed: give the pointer back (sensitivity then does nothing; logged once).
	if (g_relOn.load() && RawMouse_State() < 0) {
		g_relOn.store(false);
		RawMouse_Claim(kRawMouseOsu, false);
		ClipCursor(nullptr);
	}
}

void Act(const std::string& a) {
	if (a == "play") { if (g_m == Mode::Select) BeginPlay(); return; }
	if (a == "mods") { g_modsOpen = !g_modsOpen; g_optionsOpen = false; g_viewRev++; return; }
	if (a == "options") { if (g_m == Mode::Select) { g_optionsOpen = !g_optionsOpen; g_modsOpen = false; g_viewRev++; } return; }
	if (a == "random") { Random(false); return; }
	if (a == "sort") { g_sort = (g_sort + 1) % kSortCount; Refilter(); return; }
	if (a == "back") {
		if (g_m == Mode::Select) Osu_Close();
		else if (g_m == Mode::Results || g_m == Mode::Fail) ToSelect();
		return;
	}
	if (a == "continue" && g_m == Mode::Pause) {
		g_m = Mode::Play;
		Rebase(g_t);
		if (g_audioStarted) { OsuAudio::Resume(); g_needSync = true; g_lastPollSeq = OsuAudio::PollSeq(); }
		return;
	}
	if (a == "retry" && (g_m == Mode::Pause || g_m == Mode::Fail || g_m == Mode::Results)) { BeginPlay(); return; }
	if (a == "quit" && (g_m == Mode::Pause || g_m == Mode::Fail || g_m == Mode::Results)) { ToSelect(); return; }
	if (a == "skip") { Skip(); return; }
	if (a == "pause" && g_m == Mode::Play) { g_m = Mode::Pause; g_menuSel = 0; OsuAudio::Pause(); return; }
}

void MenuNav(int key, int count, const char* const* actions) {
	// The pause / fail menus are a row of circles: left / right move too (up / down still work).
	if (key == kUp || key == kLeft) g_menuSel = (g_menuSel + count - 1) % count;
	else if (key == kDown || key == kRight) g_menuSel = (g_menuSel + 1) % count;
	else if (key == kEnter) Act(actions[g_menuSel]);
}

void HandleKey(int key) {
	switch (g_m) {
	case Mode::Select:
		if (g_modsOpen && (key == kEsc || key == kF1 || key == kEnter)) { g_modsOpen = false; g_viewRev++; return; }
		if (g_optionsOpen && (key == kEsc || key == kEnter)) { g_optionsOpen = false; g_viewRev++; return; }
		switch (key) {
		case kUp: MoveDiff(-1); break;
		case kDown: MoveDiff(+1); break;
		case kLeft: SelectSet(g_selSet - 1, -2); break;
		case kRight: SelectSet(g_selSet + 1, -2); break;
		case kPgUp: SelectSet(g_selSet - 8, -2); break;
		case kPgDn: SelectSet(g_selSet + 8, -2); break;
		case kHome: SelectSet(0, -2); break;
		case kEnd: SelectSet((int)g_filtered.size() - 1, -2); break;
		case kEnter: BeginPlay(); break;
		case kF1: Act("mods"); break;
		case kF2: Random(false); break;
		case kShiftF2: Random(true); break;
		case kTab: Act("sort"); break;
		case kBackspace: if (!g_search.empty()) { do { g_search.pop_back(); } while (!g_search.empty() && ((unsigned char)g_search.back() & 0xC0) == 0x80); Refilter(); } break;
		case kDelete: if (!g_search.empty()) { g_search.clear(); Refilter(); } break;
		case kEsc: if (!g_search.empty()) { g_search.clear(); Refilter(); } else Osu_Close(); break;
		default: break;
		}
		break;
	case Mode::Loading:
		if (key == kEsc) ToSelect();
		break;
	case Mode::Play:
		if (key == kEsc) { g_m = Mode::Pause; g_menuSel = 0; OsuAudio::Pause(); }
		else if (key == kSpace) Skip();
		break;
	case Mode::Pause: {
		static const char* const acts[] = { "continue", "retry", "quit" };
		if (key == kEsc) Act("continue");
		else if (key == kKeyR) Act("retry");
		else if (key == kKeyQ) Act("quit");
		else MenuNav(key, 3, acts);
		break;
	}
	case Mode::Fail: case Mode::Results: {
		static const char* const acts[] = { "retry", "quit" };
		if (key == kEsc) Act("quit");
		else if (key == kKeyR) Act("retry");
		else MenuNav(key, 2, acts);
		break;
	}
	default: break;
	}
}

void HandleChar(unsigned ch) {
	if (g_m != Mode::Select) return;
	if (g_modsOpen) {
		size_t n; const ModInfo* t = ModTable(n);
		for (size_t i = 0; i < n; ++i)
			if (toupper((int)ch) == t[i].key) { g_mods = ToggleMod(g_mods, t[i].bit); g_viewRev++; OsuAudio::Stop(); g_previewPending = true; g_previewAt = QpcMs(); }
		return;
	}
	if (ch < 32 || ch == 127) return;
	if (ch < 0x80) g_search += (char)ch;
	else { std::wstring w(1, (wchar_t)ch); g_search += WideToUtf8(w); }
	Refilter();
	SelectionChanged();
}

// ---- JSON helpers -------------------------------------------------------------------------------
void AppendRgb(JsonBuilder& j, int c) {
	char buf[16];
	snprintf(buf, sizeof(buf), "#%06x", c & 0xFFFFFF);
	j.ValueString(buf);
}
void ModsJson(JsonBuilder& j, uint32_t mods) {
	j.Key("mods"); j.BeginArray();
	size_t n; const ModInfo* t = ModTable(n);
	for (size_t i = 0; i < n; ++i) if (mods & t[i].bit) j.ValueString(t[i].acronym);
	j.EndArray();
}

std::string BuildView() {
	JsonBuilder v;
	v.BeginObject();
	int done = 0, total = 0;
	OsuLibrary::Progress(done, total);
	v.BoolField("scanning", OsuLibrary::Scanning());
	v.IntField("done", done); v.IntField("total", total);
	v.StringField("search", g_search);
	v.StringField("sort", kSortNames[g_sort]);
	v.IntField("sets", g_lib ? (int64_t)g_lib->size() : 0);
	v.IntField("matched", (int64_t)g_filtered.size());
	v.BoolField("modsOpen", g_modsOpen);
	v.Key("opt"); v.BeginObject();
	v.BoolField("open", g_optionsOpen);
	v.DoubleField("sens", g_sens); v.DoubleField("master", g_volMaster);
	v.DoubleField("music", g_volMusic); v.DoubleField("effects", g_volEffects);
	v.StringField("skin", OsuSkin::Name());
	v.EndObject();
	ModsJson(v, g_mods);
	v.DoubleField("mult", ModMultiplier(g_mods));
	v.Key("modTable"); v.BeginArray();
	{
		size_t n; const ModInfo* t = ModTable(n);
		for (size_t i = 0; i < n; ++i) {
			v.BeginObject();
			v.StringField("a", t[i].acronym); v.StringField("n", t[i].name);
			v.DoubleField("m", t[i].multiplier); v.StringField("k", std::string(1, t[i].key));
			v.IntField("g", t[i].group); v.BoolField("on", (g_mods & t[i].bit) != 0);
			v.EndObject();
		}
	}
	v.EndArray();
	const double rate = ModRate(g_mods);
	v.IntField("selSet", g_selSet); v.IntField("selDiff", g_selDiff);
	// The carousel window around the selection: set rows, with the selected set expanded.
	v.Key("rows"); v.BeginArray();
	if (g_lib && !g_filtered.empty()) {
		const Set* sel = SelSet();
		const int expanded = sel ? (int)sel->diffs.size() : 0;
		auto rowOf = [&](int setIdx, int diff) {
			int flat = setIdx;
			if (setIdx > g_selSet) flat += expanded;
			if (setIdx == g_selSet && diff >= 0) flat += 1 + diff;
			return flat;
		};
		const int selFlat = rowOf(g_selSet, g_selDiff);
		const int lo = selFlat - 9, hi = selFlat + 12;
		const int firstSet = std::max(0, g_selSet - 12), lastSet = std::min((int)g_filtered.size() - 1, g_selSet + 12);
		for (int si = firstSet; si <= lastSet; ++si) {
			const Set& s = (*g_lib)[g_filtered[si]];
			const int setFlat = rowOf(si, -1);
			if (setFlat >= lo && setFlat <= hi) {
				v.BeginObject();
				v.StringField("k", "set");
				v.IntField("p", setFlat - selFlat); v.IntField("s", si); v.IntField("d", -1);
				v.BoolField("sel", si == g_selSet);
				v.StringField("t", s.title); v.StringField("a", s.artist); v.StringField("c", s.creator);
				v.StringField("src", s.source);
				v.Key("st"); v.BeginArray();
				for (size_t d = 0; d < s.diffs.size() && d < 18; ++d) v.ValueDouble(Stars(s.diffs[d], g_mods));
				v.EndArray();
				v.EndObject();
			}
			if (si != g_selSet) continue;
			for (int d = 0; d < (int)s.diffs.size(); ++d) {
				const int f = rowOf(si, d);
				if (f < lo || f > hi) continue;
				const Diff& df = s.diffs[d];
				v.BeginObject();
				v.StringField("k", "diff");
				v.IntField("p", f - selFlat); v.IntField("s", si); v.IntField("d", d);
				v.BoolField("sel", d == g_selDiff);
				v.StringField("v", df.version); v.StringField("c", df.creator);
				v.DoubleField("star", Stars(df, g_mods));
				v.EndObject();
			}
		}
	}
	v.EndArray();
	if (const Diff* d = SelDiff()) {
		const Set* s = SelSet();
		double cs = d->cs, ar = d->ar, od = d->od, hp = d->hp;
		ApplyModsToDifficulty(g_mods, cs, ar, od, hp);
		v.Key("info"); v.BeginObject();
		v.StringField("title", s->title); v.StringField("artist", s->artist);
		v.StringField("mapper", d->creator); v.StringField("version", d->version);
		v.StringField("src", s->source); v.StringField("from", s->mapSource);
		v.DoubleField("stars", Stars(*d, g_mods));
		v.DoubleField("len", (d->lastMs) / rate / 1000.0);
		v.DoubleField("drain", d->drainMs / rate / 1000.0);
		v.DoubleField("bpm", d->bpmMain * rate); v.DoubleField("bpmMin", d->bpmMin * rate); v.DoubleField("bpmMax", d->bpmMax * rate);
		v.IntField("circles", d->circles); v.IntField("sliders", d->sliders); v.IntField("spinners", d->spinners);
		v.DoubleField("cs", cs); v.DoubleField("hp", hp);
		v.DoubleField("od", EffectiveOd(od, rate)); v.DoubleField("ar", EffectiveAr(ar, rate));
		v.DoubleField("cs0", d->cs); v.DoubleField("hp0", d->hp); v.DoubleField("od0", d->od); v.DoubleField("ar0", d->ar);
		v.EndObject();
	}
	v.EndObject();
	return v.Str();
}

} // namespace

bool Osu_Active() { return g_modeAtomic.load() != (int)Mode::Closed; }

void Osu_Open() {
	if (Osu_Active()) return;
	g_prevMouseFree = MouseCapture_IsFree();
	MouseCapture_SetFree(true);
	LoadSettings();
	OsuSkin::Load();
	for (auto& h : g_held) h.store(false);
	g_m = Mode::Select;
	g_modeAtomic.store((int)g_m);
	OsuLibrary::StartScan();
	g_lib = OsuLibrary::Sets(); g_libRev = 0; // force a refilter on the first frame
	g_viewRev++;
	SelectionChanged();
	MvmDebugLog_LinefAlways("osu.open", "roaming=%s", WideToUtf8(OsuPlatform::RoamingRoot().wstring()).c_str());
}

void Osu_Close() {
	if (!Osu_Active()) return;
	OsuAudio::Stop();
	g_m = Mode::Closed;
	g_modeAtomic.store((int)g_m);
	g_modsOpen = false; g_modsOpenAtomic.store(false); g_optionsOpen = false;
	UpdateRelative(); // mode is Closed: releases a pinned pointer
	MouseCapture_SetFree(g_prevMouseFree);
	{ std::lock_guard<std::mutex> lock(g_inMx); g_inQueue.clear(); }
}

void Osu_RunFrame() {
	if (g_m == Mode::Closed) return;
	// osu! is overlay-only (its own XAudio2 voice, Panorama chrome), so it plays on through a render
	// -- unless the job captures after the UI draws (HUD Game/Full), where the overlay is hidden and
	// would otherwise be baked into the take.
	if (RenderSessionDriver_WantsChromeHidden()) { Osu_Close(); return; }

	if (g_srDirty.exchange(false)) g_viewRev++;
	if (OsuLibrary::Revision() != g_libRev) {
		g_libRev = OsuLibrary::Revision();
		g_lib = OsuLibrary::Sets();
		Refilter();
		SelectionChanged();
	}

	std::vector<Input> events;
	{ std::lock_guard<std::mutex> lock(g_inMx); events.swap(g_inQueue); }
	static double s_lastQpc = 0.0;
	const double nowQpc = QpcMs();
	const double dt = s_lastQpc > 0.0 ? std::min(100.0, nowQpc - s_lastQpc) : 0.0;
	s_lastQpc = nowQpc;

	for (const Input& in : events) {
		switch (in.kind) {
		case kInKey: HandleKey(in.code); break;
		case kInChar: HandleChar((unsigned)in.code); break;
		case kInWheel: if (g_m == Mode::Select && !g_modsOpen && !g_optionsOpen) SelectSet(g_selSet + (in.code > 0 ? -1 : 1), -2); break;
		case kInVolWheel: AdjustVolume(in.code, in.x, in.y); break;
		case kInClick: {
			RECT rc{};
			if (ClientRect(rc)) { g_clickSeq++; g_clickX = (double)in.x / rc.right; g_clickY = (double)in.y / rc.bottom; }
			break;
		}
		case kInHit:
			if (g_m == Mode::Play && !(g_playMods & kModAT)) {
				double ox, oy;
				if (ToOsu(in.x, in.y, ox, oy)) Press(SongNow(in.qpc), ox, oy);
			}
			break;
		default: break;
		}
	}

	if (g_m == Mode::Select && g_previewPending && nowQpc - g_previewAt > 220.0) {
		g_previewPending = false;
		Preview();
	}
	if (g_m == Mode::Loading) {
		const bool ready = OsuAudio::Ready(g_audioId) && OsuHitsounds::Ready();
		if (ready || nowQpc - g_loadStart > 15000.0) {
			g_audioFailed = !OsuAudio::Ready(g_audioId) || OsuAudio::Failed(g_audioId);
			if (g_audioFailed) MvmDebugLog_LinefAlways("osu.play", "audio unavailable: playing in silence");
			StartPlayClock();
		}
	}
	if (g_m == Mode::Play) {
		if (!g_audioStarted) {
			g_t = SongNow(nowQpc);
			if (g_t >= 0.0 && !g_audioFailed) StartAudioAt(g_t);
		} else {
			// THE GAME CLOCK: a steady QPC line that never runs backwards, eased toward the audio
			// (MCI's MP3 position is coarse and sometimes steps back); hard re-based once when the
			// audio is first heard and on a big disagreement.
			const unsigned seq = OsuAudio::PollSeq();
			if (seq != g_lastPollSeq && OsuAudio::Playing()) {
				g_lastPollSeq = seq;
				const double audio = OsuAudio::Position() - g_offset;
				const double err = audio - SongNow(nowQpc);
				if (g_needSync || std::fabs(err) > kResyncMs) {
					if (!g_needSync || err > 0.0 || std::fabs(err) > kResyncMs) {
						Rebase(audio);
						if (std::fabs(err) > kResyncMs) g_t = audio;
					}
					g_needSync = false;
				} else {
					g_clockSong0 += std::clamp(err * 0.05, -kNudgeMsPerFrame, kNudgeMsPerFrame);
				}
			}
			g_t = std::max(g_t, SongNow(nowQpc));
		}
		Update(dt);
	} else if (g_m == Mode::Results) {
		while (!g_pops.empty()) g_pops.pop_front();
	}
	UpdateRelative();
	g_modeAtomic.store((int)g_m);
	g_modsOpenAtomic.store(g_modsOpen);
}

bool Osu_HidesOsCursor() {
	const int m = g_modeAtomic.load();
	return m == (int)Mode::Play;
}

bool Osu_OnWindowMessage(unsigned msg, unsigned long long wParam, long long lParam) {
	(void)lParam;
	if (!Osu_Active()) return false;
	const int mode = g_modeAtomic.load();
	const bool playing = mode == (int)Mode::Play;
	auto push = [](int kind, int code) {
		int x = 0, y = 0;
		CursorClient(x, y); // our pointer when sensitivity pins the OS one
		std::lock_guard<std::mutex> lock(g_inMx);
		g_inQueue.push_back({ kind, code, QpcMs(), x, y });
	};
	auto hitKey = [&](int key, bool down) {
		if (down && !g_held[key].load()) { g_held[key].store(true); push(kInHit, key); }
		else if (!down) { g_held[key].store(false); push(kInRelease, key); }
	};
	switch (msg) {
	case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: {
		const bool down = msg != WM_KEYUP;
		const int vk = (int)wParam;
		if (vk == VK_OEM_3) return false; // the console key stays CS2's
		if (playing && (vk == 'Z' || vk == 'X')) { hitKey(vk == 'Z' ? kKeyK1 : kKeyK2, down); return true; }
		if (!playing && (vk == 'Z' || vk == 'X') && !down) { g_held[vk == 'Z' ? kKeyK1 : kKeyK2].store(false); }
		if (!down) return true;
		int key = 0;
		switch (vk) {
		case VK_UP: key = kUp; break; case VK_DOWN: key = kDown; break;
		case VK_LEFT: key = kLeft; break; case VK_RIGHT: key = kRight; break;
		case VK_RETURN: key = kEnter; break; case VK_ESCAPE: key = kEsc; break;
		case VK_BACK: key = kBackspace; break; case VK_DELETE: key = kDelete; break;
		case VK_F1: key = kF1; break;
		case VK_F2: key = (GetKeyState(VK_SHIFT) & 0x8000) ? kShiftF2 : kF2; break;
		case VK_TAB: key = kTab; break;
		case VK_SPACE: key = playing ? kSpace : 0; break;
		case VK_PRIOR: key = kPgUp; break; case VK_NEXT: key = kPgDn; break;
		case VK_HOME: key = kHome; break; case VK_END: key = kEnd; break;
		case 'R': key = mode == (int)Mode::Select ? 0 : kKeyR; break;
		case 'Q': key = mode == (int)Mode::Select ? 0 : kKeyQ; break;
		default: break;
		}
		if (key) push(kInKey, key);
		return true;
	}
	case WM_CHAR:
		if (mode == (int)Mode::Select) {
			const unsigned ch = (unsigned)wParam;
			if (ch >= 32 && ch != 127 && !(ch == ' ' && g_modsOpenAtomic.load())) push(kInChar, (int)ch);
		}
		return true;
	case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
		g_lmbDown.store(true);
		if (playing) hitKey(kKeyM1, true); else push(kInClick, 0);
		return true;
	case WM_LBUTTONUP:
		g_lmbDown.store(false);
		if (playing) hitKey(kKeyM1, false); else g_held[kKeyM1].store(false);
		return true;
	case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: if (playing) hitKey(kKeyM2, true); return true;
	case WM_RBUTTONUP: if (playing) hitKey(kKeyM2, false); else g_held[kKeyM2].store(false); return true;
	case WM_MBUTTONDOWN: case WM_MBUTTONUP: return true;
	case WM_MOUSEWHEEL: {
		// osu!: the wheel is volume in game; in song select it browses, and Alt + wheel is volume.
		const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
		if (mode == (int)Mode::Select && !alt) push(kInWheel, GET_WHEEL_DELTA_WPARAM(wParam));
		else push(kInVolWheel, GET_WHEEL_DELTA_WPARAM(wParam));
		return true;
	}
	default: return false;
	}
}

void Osu_BuildJson(std::string& frame, std::string& objs, uint64_t& objsRev, std::string& list, uint64_t& listRev) {
	const Mode m = g_m;
	const bool inGame = m == Mode::Loading || m == Mode::Play || m == Mode::Pause || m == Mode::Fail || m == Mode::Results;
	RECT rc{};
	double mx = 0.5, my = 0.5;
	{ int x, y; if (ClientRect(rc) && CursorClient(x, y)) { mx = (double)x / rc.right; my = (double)y / rc.bottom; } }

	JsonBuilder f;
	f.BeginObject();
	f.StringField("m", ModeName(m));
	f.DoubleField("t", g_t);
	f.DoubleField("cx", g_cx); f.DoubleField("cy", g_cy);
	f.DoubleField("mx", mx); f.DoubleField("my", my);
	f.BoolField("held", g_held[0].load() || g_held[1].load() || g_held[2].load() || g_held[3].load() || ((g_playMods & kModAT) && m == Mode::Play));
	f.Key("clk"); f.BeginArray(); f.ValueInt(g_clickSeq); f.ValueDouble(g_clickX); f.ValueDouble(g_clickY); f.EndArray();
	f.IntField("menu", g_menuSel);
	f.BoolField("lmb", g_lmbDown.load());
	f.BoolField("shift", (GetKeyState(VK_SHIFT) & 0x8000) != 0);
	{
		const double age = QpcMs() - g_volShownAt;
		const double a = age < kVolShowMs ? 1.0 : std::max(0.0, 1.0 - (age - kVolShowMs) / kVolFadeMs);
		if (a > 0.0) {
			f.Key("vol"); f.BeginObject();
			f.DoubleField("a", a); f.IntField("sel", g_volSel);
			f.DoubleField("m", g_volMaster); f.DoubleField("mu", g_volMusic); f.DoubleField("e", g_volEffects);
			f.EndObject();
		}
	}
	if (inGame) {
		f.DoubleField("pre", Preempt(g_map.ar)); f.DoubleField("fade", FadeIn(g_map.ar)); f.DoubleField("r", R());
		f.IntField("score", g_score); f.IntField("combo", g_combo); f.IntField("maxCombo", g_maxCombo);
		f.DoubleField("acc", Accuracy(g_n300, g_n100, g_n50, g_nMiss));
		f.DoubleField("hp", g_hp);
		f.IntField("n300", g_n300); f.IntField("n100", g_n100); f.IntField("n50", g_n50); f.IntField("nMiss", g_nMiss);
		std::string grade = Grade(g_n300, g_n100, g_n50, g_nMiss);
		if ((g_playMods & (kModHD | kModFL)) && (grade == "S" || grade == "SS")) grade += "H"; // silver
		f.StringField("grade", grade);
		const double first = g_map.objects.empty() ? 0.0 : g_map.objects.front().time;
		const double lastEnd = g_map.objects.empty() ? 1.0 : std::max(1.0, g_map.objects.back().endTime);
		f.DoubleField("prog", std::clamp((g_t - first) / std::max(1.0, lastEnd - first), 0.0, 1.0));
		// The fail screen's timeline, in play time (the rate applied): song length and where it ended.
		f.DoubleField("len", lastEnd / g_rate);
		f.DoubleField("tPlay", std::max(0.0, g_t) / g_rate);
		f.DoubleField("lead", std::clamp((g_t - g_leadStart) / std::max(1.0, first - g_leadStart), 0.0, 1.0));
		f.StringField("title", g_title); f.StringField("artist", g_artist); f.StringField("version", g_version);
		f.StringField("mapper", g_mapper);
		f.DoubleField("stars", g_diff.stars);
		ModsJson(f, g_playMods);
		f.BoolField("hd", (g_playMods & kModHD) != 0);
		f.BoolField("fl", (g_playMods & kModFL) != 0);
		f.BoolField("auto", (g_playMods & kModAT) != 0);
		f.BoolField("skip", CanSkip());
		f.BoolField("silent", g_audioFailed);
		f.StringField("why", g_failReason);
		f.Key("pops"); f.BeginArray();
		for (const Pop& p : g_pops) {
			f.BeginArray(); f.ValueInt(p.id); f.ValueDouble(p.x); f.ValueDouble(p.y); f.ValueInt(p.res); f.ValueDouble(g_t - p.at); f.EndArray();
		}
		f.EndArray();
		for (size_t i = 0; i < g_map.objects.size(); ++i) {
			const HitObject& o = g_map.objects[i];
			if (!(o.type & kSpinner) || g_t < o.time - 300.0 || g_t > o.endTime + 300.0) continue;
			f.Key("spin"); f.BeginObject();
			f.DoubleField("rot", g_state[i].spin);
			const int req = SpinsRequired(o.endTime - o.time, g_map.od);
			f.DoubleField("prog", req <= 0 ? 1.0 : std::min(1.0, g_state[i].spin / 6.2831853 / req));
			f.DoubleField("left", std::clamp((o.endTime - g_t) / std::max(1.0, o.endTime - o.time), 0.0, 1.0));
			f.IntField("bonus", g_state[i].bonus * 1000);
			f.EndObject();
			break;
		}
	}
	f.EndObject();
	frame = f.Str();

	// Objects in view: [t - 900, t + preempt], widened for osu!'s follow points: they appear up to
	// 800 ms before the NEXT object (so it is sent early) and fade out 400 ms after reaching it (so the
	// previous object stays). Rebuilt only when that set or a judgement changes.
	if (inGame) {
		const double pre = Preempt(g_map.ar);
		const double ahead = std::max(pre, 800.0 * std::min(1.0, pre / 450.0)) + 50.0;
		size_t first = 0, last = 0;
		bool any = false;
		for (size_t i = 0; i < g_map.objects.size(); ++i) {
			const HitObject& o = g_map.objects[i];
			if (o.time - ahead > g_t) break;
			const bool nextNear = i + 1 < g_map.objects.size() && g_map.objects[i + 1].time + 450.0 >= g_t;
			if (std::max(o.endTime, o.time) + 900.0 < g_t && !nextNear) continue;
			if (!any) { first = i; any = true; }
			last = i;
		}
		const std::string sig = any ? std::to_string(first) + "-" + std::to_string(last) : "none";
		if (sig != g_objsSig) { g_objsSig = sig; g_objsRev++; }
		if (objsRev != g_objsRev) {
			JsonBuilder o;
			o.BeginObject();
			// Combo colours: the beatmap's, unless the skin has its own and either the map has none or
			// lazer's "Beatmap skins" is off (then the skin's always win, as in osu!).
			std::vector<int> skinCols;
			OsuSkin::ComboColours(skinCols);
			const bool useSkinCols = !skinCols.empty() && (g_map.colours.empty() || !OsuSkin::BeatmapSkins());
			o.Key("cols"); o.BeginArray();
			if (useSkinCols) for (int c : skinCols) AppendRgb(o, c);
			else if (g_map.colours.empty()) { AppendRgb(o, 0xffc000); AppendRgb(o, 0x00ca00); AppendRgb(o, 0x127cff); AppendRgb(o, 0xf21839); }
			else for (int c : g_map.colours) AppendRgb(o, c);
			o.EndArray();
			OsuSkin::AppendJson(o);
			o.Key("o"); o.BeginArray();
			if (any) for (size_t i = first; i <= last; ++i) {
				const HitObject& h = g_map.objects[i];
				const ObjState& s = g_state[i];
				o.BeginObject();
				o.IntField("i", (int)i); o.IntField("k", h.type);
				o.DoubleField("x", h.pos.x); o.DoubleField("y", h.pos.y);
				o.DoubleField("t", h.time); o.DoubleField("e", h.endTime);
				o.IntField("n", h.comboNumber); o.IntField("c", h.comboIndex);
				o.IntField("head", s.head); o.DoubleField("headAt", s.headAt);
				o.IntField("res", s.result); o.DoubleField("resAt", s.resultAt);
				if (h.type & kSlider) {
					o.IntField("s", h.slides); o.DoubleField("sp", h.span);
					o.Key("p"); o.BeginArray();
					for (const Vec2& v : h.path) { o.ValueInt((int)std::lround(v.x * 10)); o.ValueInt((int)std::lround(v.y * 10)); }
					o.EndArray();
					o.Key("tk"); o.BeginArray();
					for (size_t k = 0; k < h.checkpoints.size() && k < h.checkpointIsTick.size(); ++k)
						if (h.checkpointIsTick[k]) o.ValueInt((int)std::lround(h.checkpoints[k]));
					o.EndArray();
				}
				o.EndObject();
			}
			o.EndArray();
			o.EndObject();
			objs = o.Str();
			objsRev = g_objsRev;
		}
	}

	if (m == Mode::Select && listRev != g_viewRev) {
		list = BuildView();
		listRev = g_viewRev;
	}
}

void Osu_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* sub = argc >= 3 ? args->ArgV(2) : "status";
	if (0 == _stricmp(sub, "open")) { Osu_Open(); return; }
	if (0 == _stricmp(sub, "close")) { Osu_Close(); return; }
	if (0 == _stricmp(sub, "rescan")) { OsuLibrary::StartScan(argc >= 4 && 0 == _stricmp(args->ArgV(3), "full")); return; }
	if (0 == _stricmp(sub, "pick") && argc >= 5) {
		// The overlay: a click on a carousel row. Clicking the selected difficulty plays it.
		if (g_m != Mode::Select) return;
		const int s = atoi(args->ArgV(3)), d = atoi(args->ArgV(4));
		if (s == g_selSet && d == g_selDiff) { BeginPlay(); return; }
		if (s == g_selSet && d < 0) return;
		SelectSet(s, d < 0 ? -2 : d);
		return;
	}
	if (0 == _stricmp(sub, "act") && argc >= 4) { Act(args->ArgV(3)); return; }
	// The overlay: the pointer is over a pause / fail menu circle -- select it (as the arrow keys do).
	if (0 == _stricmp(sub, "menu") && argc >= 4) {
		const int count = g_m == Mode::Pause ? 3 : (g_m == Mode::Fail || g_m == Mode::Results) ? 2 : 0;
		const int i = atoi(args->ArgV(3));
		if (i >= 0 && i < count) g_menuSel = i;
		return;
	}
	if (0 == _stricmp(sub, "set") && argc >= 5) { SetOption(args->ArgV(3), atof(args->ArgV(4))); return; }
	if (0 == _stricmp(sub, "skin")) {
		if (argc >= 4 && 0 == _stricmp(args->ArgV(3), "reload")) OsuSkin::Reload();
		advancedfx::Message("osu %s\n", OsuSkin::Status().c_str());
		return;
	}
	if (0 == _stricmp(sub, "mod") && argc >= 4) {
		size_t n; const ModInfo* t = ModTable(n);
		for (size_t i = 0; i < n; ++i)
			if (0 == _stricmp(args->ArgV(3), t[i].acronym)) { g_mods = ToggleMod(g_mods, t[i].bit); g_viewRev++; OsuAudio::Stop(); g_previewPending = true; g_previewAt = QpcMs(); }
		return;
	}
	if (0 == _stricmp(sub, "search")) {
		g_search.clear();
		for (int i = 3; i < argc; ++i) { if (i > 3) g_search += ' '; g_search += args->ArgV(i); }
		Refilter(); SelectionChanged();
		return;
	}
	if (0 == _stricmp(sub, "play")) { if (!Osu_Active()) Osu_Open(); BeginPlay(); return; }
	if (0 == _stricmp(sub, "offset") && argc >= 4) { g_offset = atof(args->ArgV(3)); advancedfx::Message("osu offset %.0f ms\n", g_offset); return; }
	if (0 == _stricmp(sub, "volume") && argc >= 4) { SetOption("music", atof(args->ArgV(3)) / 1000.0); return; }
	if (0 == _stricmp(sub, "probe")) {
		// Where osu! puts every visible object right now, in client pixels: compare with a capture.
		RECT rc{};
		if (!ClientRect(rc)) { advancedfx::Message("osu probe: no window\n"); return; }
		const Playfield p = PlayfieldFor(rc.right, rc.bottom);
		const double pre = Preempt(g_map.ar), r = R();
		advancedfx::Message("osu probe: client %ldx%ld scale %.4f origin (%.1f, %.1f) t=%.0f CS %.2f radius %.2f osu px = %.1f px, preempt %.0f\n",
			rc.right, rc.bottom, p.scale, p.ox, p.oy, g_t, g_map.cs, r, r * p.scale, pre);
		for (size_t i = 0; i < g_map.objects.size(); ++i) {
			const HitObject& o = g_map.objects[i];
			if (o.time - pre > g_t || std::max(o.endTime, o.time) + 200.0 < g_t) continue;
			advancedfx::Message("  #%zu n=%d t=%.0f type=%d stack=%d osu (%.1f, %.1f) -> px (%.1f, %.1f)\n", i, o.comboNumber, o.time,
				o.type, o.stack, o.pos.x, o.pos.y, p.ox + o.pos.x * p.scale, p.oy + o.pos.y * p.scale);
		}
		return;
	}
	if (0 == _stricmp(sub, "mapsounds")) {
		if (argc >= 4) OsuHitsounds::SetBeatmapSamples(0 == _stricmp(args->ArgV(3), "on"));
		advancedfx::Message("osu mapsounds %s (off: only the click on circles and slider heads; on: the map's full hitsounding)\n", OsuHitsounds::BeatmapSamples() ? "on" : "off");
		return;
	}
	if (0 == _stricmp(sub, "hitvolume") && argc >= 4) { SetOption("effects", atof(args->ArgV(3)) / 100.0); return; }
	if (0 == _stricmp(sub, "status")) {
		int done = 0, total = 0; OsuLibrary::Progress(done, total);
		advancedfx::Message("osu: mode=%s sets=%d matched=%d sel=%d/%d search='%s' mods=0x%x scan=%s %d/%d\n"
			"     t=%.0f audio=%s pos=%.0f score=%lld combo=%d acc=%.2f%% hp=%.2f\n"
			"     sensitivity %.2fx (raw=%d) volume master %.0f%% music %.0f%% effects %.0f%%\n     %s\n",
			ModeName(g_m), g_lib ? (int)g_lib->size() : 0, (int)g_filtered.size(), g_selSet, g_selDiff, g_search.c_str(), g_mods,
			OsuLibrary::Scanning() ? "running" : "done", done, total,
			g_t, OsuAudio::Playing() ? "playing" : "stopped", OsuAudio::Position(), g_score, g_combo,
			Accuracy(g_n300, g_n100, g_n50, g_nMiss) * 100.0, g_hp,
			g_sens, RawMouse_State(), g_volMaster * 100.0, g_volMusic * 100.0, g_volEffects * 100.0, OsuSkin::Status().c_str());
		return;
	}
	advancedfx::Message(
		"%s osu open|close|status|rescan [full] | play | pick <set> <diff> | act <play|mods|random|sort|back|pause|continue|retry|quit|skip>\n"
		"%s osu mod <EZ|NF|HT|HR|SD|PF|DT|NC|HD|FL|AT> | search <text> | offset <ms> | volume <0-1000> | hitvolume <0-100> | mapsounds on|off\n"
			"    set sens|master|music|effects <value> | skin [reload]\n"
		"    beatmaps: osu!stable and osu!lazer are found automatically; .osz dropped into %%APPDATA%%\\HLAE\\osu\\ are imported\n",
		cmd, cmd);
}

} // namespace Filmmaker
