#include "EasterEggs.h"

#include "EasterEggHintMath.h"
#include "EasterEggSnake.h"
#include "EasterEggUwu.h"
#include "EasterEggWesMath.h"
#include "EasterEggWorld.h"
#include "Osu/OsuGame.h"

#include "../Filmmaker.h"
#include "../Render/RenderSessionDriver.h"
#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"
#include "../Debug/MvmDebugLog.h"
#include "../Cosmetics/CosmeticOverrideSystem.h"
#include "../Cosmetics/CosmeticEntityAccess.h"
#include "../../ClientEntitySystem.h"
#include "../../WindowInputHooks.h" // g_UiCursorProbe: game HWND + client-space cursor
#include "../../WrpConsole.h"
#include "../../hlaeFolder.h"
#include "../../../Shared/AfxConsole.h"
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/igameevents.h"

#include <TlHelp32.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

// SchemaSystem.cpp: a client.dll field offset by class / field name.
bool getOffset(ptrdiff_t* offset, std::string moduleName, std::string className, std::string fieldName);

namespace Filmmaker {
namespace {

std::string ReadRoundTitle(); // the Wes card's title for the round on screen (below)

// ---- hints ------------------------------------------------------------------------------------
// Title = the big Gen X wordmark line (short, uppercase reads best). Code = the small decoding
// club-flyer line. Sub = the clue. Keep them VAGUE: a nudge and a wink, never instructions -- the
// fun is in working it out. ASCII only: these go through a JSON attribute into a Panorama Label,
// and the in-game font's glyph coverage is not something to gamble a hint on.
struct Hint { const char* title; const char* code; const char* sub; };
const Hint kHints[] = {
	{ "UWU?", "whisper it", "the console gets lonely. be nice to it." },
	{ "IT'S DANGEROUS TO GO ALONE", "take this", "three letters. very soft. you'll know." },
	{ "IT LOOKS LIKE YOU'RE...", "1997 called", "someone's waiting for you to stop clicking." },
	{ "HEY, LISTEN!", "idle hands", "sit still long enough and you'll get company." },
	{ "WOULD YOU KINDLY", "render responsibly", "we can see your taskbar." },
	{ "THIS IS FINE", "nle.dll", "your render's fine. where it's headed... less so." },
	{ "THE CAKE IS A LIE", "aperture", "the frames aren't, though." },
	{ "WAKE UP, NEO", "follow the sparkles", "some words make the whole screen blush." },
	{ "ALL YOUR FRAGS", "are belong to us", "we mean all of them." },
	{ "LEEROY JENKINS", "at least i have chicken", "no plan. just vibes." },
	{ "PRESS START", "player 2", "most people never see this one. hi." },
	{ "DO A BARREL ROLL", "peppy.wav", "not that one. but you're close." },
	{ "BURN BABY BURN", "1986 + 1976", "a very old code still opens a very loud door." },
	{ "THE EAGLE HAS LANDED", "apollo", "some say it was staged. the bodies know better." },
	{ "GOLDENEYE 007", "four player split", "remember when everyone was easier to hit?" },
	{ "WHY DID THE CHICKEN", "cross the map", "forecast: cloudy, with a chance of poultry." },
	{ "MORE EXPLOSIONS", "budget: yes", "every frag deserves a director who loves fire." },
	{ "PERFECTLY CENTERED", "chapter one", "a little yellow. very deliberate. ask nicely." },
	{ "CLICK THE CIRCLES", "300 300 300", "three letters. one rhythm. the console keeps time." },
	{ "DID YOU HEAR THAT", "tick 0", "probably nothing." },
	{ "SIGNAL LOST", "0x1f", "we'll try again later." },
	{ "YOU WERE HERE", "last session", "so were we." },
	{ "ARE YOU STILL WATCHING", "autoplay", "we are." },
	{ "SLITHER", "long exports", "patience is rewarded. eventually." },
	{ "NOT YET", "soon", "keep going." },
	{ "WE NOTICED", "3am", "go to bed. or don't. we're not your mom." },
	{ "SAY PLEASE", "manners.cfg", "the console has feelings too." },
};

constexpr int kHintCount = (int)(sizeof(kHints) / sizeof(kHints[0]));

// Wes Anderson chapter titles. A round is named for what is happening (EasterEggWesMath.h: knife,
// pistol, eco, force, full buy, read from the teams' gear when freeze time ends); these generic ones
// cover a round it cannot read. ASCII only, like the hints.
const char* const kChapters[] = {
	"A Rather Unfortunate Rush", "Concerning the Bomb", "An Unexpected Flank", "Smoke, Mirrors, and a Molotov",
	"The Clutch (Abridged)", "Several Grenades, Badly Thrown", "Overtime, Probably", "In Which Somebody Peeks",
};
constexpr int kChapterCount = (int)(sizeof(kChapters) / sizeof(kChapters[0]));

// ---- state ------------------------------------------------------------------------------------
std::atomic<bool> g_eggOn[(int)EasterEgg::Count];

std::mutex g_mx; // guards everything below that the command thread can also touch
EasterEggHint::State g_hint;
EasterEggHint::Params g_hintParams;
std::mt19937_64 g_rng{ (uint64_t)time(nullptr) ^ 0x9e3779b97f4a7c15ull };
bool g_loaded = false;
bool g_dirty = false;
unsigned long long g_lastTickMs = 0;
unsigned long long g_lastSaveMs = 0;

// One-shot overlay events. Each bump of an id is acted on exactly once by the JS.
int g_hintId = 0; int g_hintIndex = -1; bool g_hintLoud = false;
int g_hintCancelledThrough = 0;
std::string g_bannerTitle, g_bannerCode, g_bannerSub; // a forced banner (egg toggles) reuses the hint slot
int g_vegasId = 0;
int g_vegasCancelledThrough = 0;
unsigned long long g_vegasCheckGeneration = 0; // protected by g_mx
int g_clippyId = 0; bool g_clippyVisible = false;
unsigned long long g_animUntilMs = 0;

// Wes chapters: bumped from the game-event thread, read when the overlay JSON is built.
std::atomic<int> g_chapter{ 0 };
std::string g_chapterTitle; // this round's card (g_mx)
std::atomic<unsigned long long> g_chapterMs{ 0 };
constexpr unsigned long long kChapterDedupMs = 4000; // a seek replays round_start in a burst

// Vegas: off-thread process check, result read on the main thread.
std::atomic<int> g_vegasCheck{ 0 };   // 0 idle, 1 running, 2 found, 3 not found
bool g_vegasPending = false;          // found, waiting for chrome to be allowed on screen
unsigned long long g_vegasLastMs = 0;
constexpr unsigned long long kVegasCooldownMs = 14ull * 24ull * 60ull * 60ull * 1000ull;

// Clippy: shows up anywhere once the game has sat idle (no mouse, no keys) for a while, and
// says one of a rotating set of lines. Answers are console words handled by `egg clippy <word>`;
// "ok" just sends him away. ASCII only (Panorama Label through a JSON attribute).
std::atomic<unsigned long long> g_lastInputMs{ 0 };
unsigned g_lastProbeSeq = 0;
unsigned long long g_clippyShownMs = 0;
// Every 5 idle minutes he gets ONE roll at a 5% chance; hit or miss, the idle clock starts over.
constexpr unsigned long long kClippyIdleMs = 5ull * 60ull * 1000ull;
constexpr double kClippyChance = 0.05;
constexpr unsigned long long kClippyAutoHideMs = 40ull * 1000ull;

struct ClippyAnswer { const char* label; const char* word; };
struct ClippyLine { const char* title; const char* body; const char* aside; ClippyAnswer answers[3]; };
const ClippyLine kClippyLines[] = {
	{ "It looks like you're making a frag movie!", "Would you like help with that?", "(you've been real quiet. you good?)",
		{ { "Yes", "yes" }, { "Absolutely", "absolutely" }, { "Drop the bass", "bass" } } },
	{ "It looks like you're staring at the screen.", "Would you like to stare at it harder?", "(i can wait. i'm a paperclip.)",
		{ { "Blink", "ok" }, { "Harder", "absolutely" }, { "", "" } } },
	{ "Hi! I noticed you stopped moving.", "Are you rendering, or just thinking?", "(either is fine. i don't judge. much.)",
		{ { "Thinking", "ok" }, { "Rendering", "snake" }, { "", "" } } },
	{ "Fun fact:", "Paperclips can't hold a crosshair either.", "(we have that in common.)",
		{ { "Neat", "ok" }, { "Prove it", "bighead" }, { "", "" } } },
	{ "It looks like you need a break.", "Would you like to click some circles?", "(three hundred, if you're good.)",
		{ { "Click circles", "osu" }, { "No thanks", "ok" }, { "", "" } } },
	{ "Did you know?", "Most clips are 80% walking.", "(the other 20% is walking back.)",
		{ { "Skip the walk", "ok" }, { "Make it cinematic", "wes" }, { "", "" } } },
	{ "It looks like gravity is bothering you.", "Would you like less of it?", "(nobody will notice.)",
		{ { "Less, please", "moon" }, { "I like gravity", "ok" }, { "", "" } } },
	{ "Weather report:", "Clear skies. Mild breeze. Light poultry.", "(i don't make the forecast.)",
		{ { "Bring it", "chickens" }, { "Stay inside", "ok" }, { "", "" } } },
	{ "I've been here the whole time.", "Just so you know.", "(that's not creepy. i'm office software.)",
		{ { "...ok", "ok" }, { "", "" }, { "", "" } } },
	{ "It looks like you're idle.", "Would you like something inspiring?", "\"every frag is a story.\" -- a paperclip",
		{ { "Thanks", "ok" }, { "More", "absolutely" }, { "", "" } } },
	{ "Psst.", "Some words work in the console that shouldn't.", "(i didn't tell you that.)",
		{ { "Which ones?", "ok" }, { "Say a soft one", "yes" }, { "", "" } } },
	{ "Your movie needs more", "...something. I can't put my finger on it.", "(i don't have fingers.)",
		{ { "Explosions", "absolutely" }, { "Disco", "bass" }, { "Symmetry", "wes" } } },
};
constexpr int kClippyLineCount = (int)(sizeof(kClippyLines) / sizeof(kClippyLines[0]));
int g_clippyLine = 0;

// osu!: the pop-up plays first, then the game opens over it.
unsigned long long g_osuOpenAtMs = 0;

// Konami: up up down down left right left right B A.
const int kKonami[] = { VK_UP, VK_UP, VK_DOWN, VK_DOWN, VK_LEFT, VK_RIGHT, VK_LEFT, VK_RIGHT, 'B', 'A' };
std::atomic<int> g_konamiPos{ 0 };
std::atomic<int> g_konamiHits{ 0 };
int g_konamiSeen = 0;

std::atomic<unsigned> g_clickSeq{ 0 };

// Escape switches off whatever is running. Counted on the window thread, acted on in the main
// frame (the eggs' world effects are main-thread work).
std::atomic<int> g_escapeHits{ 0 };
int g_escapeSeen = 0;
std::atomic<bool> g_levelResetPending{ false };

const char* const kEggNames[(int)EasterEgg::Count] = {
	"egirl", "disco", "moon", "bighead", "chickens", "michaelbay", "wes" };

// Name, code line, clue -- when an egg turns on, and when it turns off.
struct Banner { const char* title; const char* code; const char* sub; };
const Banner kOnBanner[(int)EasterEgg::Count] = {
	{ "KAWAII.SYS", "loaded", "oh. it's pink now. say it again if you dare." },
	{ "DISCO INFERNO", "1976, burn baby burn", "the floor is yours." },
	{ "ONE SMALL STEP", "apollo, 1969", "gravity has left the chat." },
	{ "BIGGER", "goldeneye, 1997", "easier to hit. you're welcome." },
	{ "BAWK", "forecast: poultry", "it's raining chickens. hallelujah." },
	{ "MORE EXPLOSIONS", "budget: yes", "every frag gets the director's cut." },
	{ "A FILM BY YOU", "chapter one", "perfectly centered. a little yellow." },
};
const Banner kOffBanner[(int)EasterEgg::Count] = {
	{ "BACK TO WORK", "kawaii.sys unloaded", "the sparkles will remember this." },
	{ "LIGHTS UP", "last call", "everybody go home." },
	{ "HOUSTON", "we're back", "gravity restored. mostly." },
	{ "HEADS DOWN", "back to scale", "everyone's ego returns to normal." },
	{ "CLEAR SKIES", "the flock moved on", "for now." },
	{ "PRACTICAL EFFECTS", "insurance called", "fine. no more fire." },
	{ "FIN.", "roll credits", "the end. or is it." },
};

unsigned long long NowMs() { return GetTickCount64(); }

double U01() { return std::uniform_real_distribution<double>(0.0, 1.0)(g_rng); }

int TodayKey() {
	time_t t = time(nullptr);
	tm lt{};
	localtime_s(&lt, &t);
	return (lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday;
}

std::wstring StatePath() {
	std::wstring path = GetHlaeRoamingAppDataFolderW();
	if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
	path += L"easter_eggs.json";
	return path;
}

void Load() {
	g_loaded = true;
	std::ifstream f(StatePath(), std::ios::binary);
	if (!f) return;
	std::stringstream ss; ss << f.rdbuf();
	JsonValue root;
	if (!JsonParse(ss.str(), root) || root.type != JsonValue::Type::Object) return;
	// No egg state is read back: every egg is a session thing. A relaunch always comes up normal
	// (UwU used to persist, which is how a restart came back pink).
	if (const JsonValue* h = root.Find("hint")) {
		auto num = [&](const char* k, double d) { const JsonValue* v = h->Find(k); return v ? v->AsNumber(d) : d; };
		g_hint.activeSeconds = num("active", 0.0);
		g_hint.nextAtActive = num("nextAt", -1.0);
		g_hint.lastShownUnix = num("lastUnix", 0.0);
		g_hint.dayKey = (int)num("day", 0.0);
		g_hint.shownToday = (int)num("today", 0.0);
		if (const JsonValue* r = h->Find("recent")) {
			if (r->type == JsonValue::Type::Array)
				for (size_t i = 0; i < r->arr.size() && i < 4; ++i) g_hint.recent[i] = r->arr[i].AsInt(-1);
		}
	}
}

void Save() {
	JsonBuilder j;
	j.BeginObject();
	j.Key("hint"); j.BeginObject();
	j.DoubleField("active", g_hint.activeSeconds);
	j.DoubleField("nextAt", g_hint.nextAtActive);
	j.DoubleField("lastUnix", g_hint.lastShownUnix);
	j.IntField("day", g_hint.dayKey);
	j.IntField("today", g_hint.shownToday);
	j.Key("recent"); j.BeginArray();
	for (int r = 0; r < 4; ++r) j.ValueInt(g_hint.recent[r]);
	j.EndArray();
	j.EndObject();
	j.EndObject();
	std::ofstream f(StatePath(), std::ios::binary | std::ios::trunc);
	if (f) f << j.Str();
	g_dirty = false;
	g_lastSaveMs = NowMs();
}

void Animate(unsigned long long ms) {
	const unsigned long long until = NowMs() + ms;
	if (until > g_animUntilMs) g_animUntilMs = until;
}

void ShowHint(int index, bool loud) {
	g_bannerTitle.clear();
	g_hintIndex = index;
	g_hintLoud = loud;
	g_hintId++;
	Animate(12000);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("egg.hint", "show #%d loud=%d active=%.0fs today=%d", index, loud ? 1 : 0, g_hint.activeSeconds, g_hint.shownToday);
}

void ShowBanner(const char* title, const char* code, const char* sub) {
	g_bannerTitle = title; g_bannerCode = code; g_bannerSub = sub ? sub : "";
	g_hintLoud = true;
	g_hintId++;
	Animate(12000);
}

bool GameForeground() {
	HWND hwnd = g_UiCursorProbe.hwnd;
	return hwnd && GetForegroundWindow() == hwnd;
}

// The overlay must never be baked into a take; when a job could capture our chrome, nothing new
// starts (an event already playing is hidden by the JS through "hidden").
bool ChromeHidden() { return RenderSessionDriver_WantsChromeHidden(); }

bool QuietMoment() {
	return !RenderSessionDriver_IsActive() && !CameraEditor_CustomizeModalOpen() && g_vegasCheck.load() != 2
		&& !g_vegasPending && !g_clippyVisible && !Osu_Active();
}

void TickHints(unsigned long long dtMs) {
	if (GameForeground()) {
		g_hint.activeSeconds += dtMs / 1000.0;
		g_dirty = true;
	}
	EasterEggHint::RollDay(g_hint, TodayKey());
	switch (EasterEggHint::Evaluate(g_hint, (double)time(nullptr), g_hintParams)) {
	case EasterEggHint::Verdict::NotScheduled:
		EasterEggHint::Schedule(g_hint, U01(), g_hintParams);
		g_dirty = true;
		break;
	case EasterEggHint::Verdict::Show:
		if (QuietMoment() && GameForeground()) {
			const int index = EasterEggHint::PickHint(g_hint, kHintCount, U01());
			EasterEggHint::RecordShown(g_hint, index, (double)time(nullptr), U01(), g_hintParams);
			ShowHint(index, U01() < 0.4);
			Save();
		}
		break;
	default:
		break;
	}
}

// ---- Sony Vegas -------------------------------------------------------------------------------
// Every VEGAS Pro build ships as vegasNNN.exe (vegas110 .. vegas220); Movie Studio / VEGAS Movie
// Studio variants carry "moviestudio" in the name.
bool IsVegasExe(const wchar_t* exe) {
	std::wstring n(exe);
	for (wchar_t& c : n) c = (wchar_t)towlower(c);
	if (n.size() < 4 || n.compare(n.size() - 4, 4, L".exe") != 0) return false;
	return n.rfind(L"vegas", 0) == 0 || n.find(L"moviestudio") != std::wstring::npos;
}

void StartVegasCheck() {
	std::lock_guard<std::mutex> lock(g_mx);
	int expected = 0;
	if (!g_vegasCheck.compare_exchange_strong(expected, 1)) return;
	const auto generation = ++g_vegasCheckGeneration;
	std::thread([generation] {
		bool found = false;
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap != INVALID_HANDLE_VALUE) {
			PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
			for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
				found = IsVegasExe(pe.szExeFile);
			CloseHandle(snap);
		}
		std::lock_guard<std::mutex> lock(g_mx);
		if (generation == g_vegasCheckGeneration) g_vegasCheck.store(found ? 2 : 3);
	}).detach();
}

void TriggerVegas() {
	g_vegasPending = false;
	g_vegasLastMs = NowMs();
	g_vegasId++;
	Animate(25000);
	MvmDebugLog_LinefAlways("egg.vegas", "intervention #%d", g_vegasId);
}

void TickVegas() {
	const int check = g_vegasCheck.load();
	if (check == 2 || check == 3) {
		g_vegasCheck.store(0);
		if (check == 2 && (g_vegasLastMs == 0 || NowMs() - g_vegasLastMs >= kVegasCooldownMs))
			g_vegasPending = true;
		MvmDebugLog_LinefAlways("egg.vegas", "process check: %s", check == 2 ? "VEGAS RUNNING" : "clean");
	}
	// A take whose capture could include our chrome gets the intervention when it ends instead.
	if (g_vegasPending && !ChromeHidden()) TriggerVegas();
}

// ---- Clippy -----------------------------------------------------------------------------------
void ShowClippy(int line = -1) {
	if (line < 0 || line >= kClippyLineCount) {
		// Any line but the last one he said.
		const int prev = g_clippyLine;
		line = (int)(U01() * kClippyLineCount);
		if (line >= kClippyLineCount) line = kClippyLineCount - 1;
		if (kClippyLineCount > 1 && line == prev) line = (line + 1) % kClippyLineCount;
	}
	g_clippyLine = line;
	g_clippyVisible = true;
	g_clippyShownMs = NowMs();
	g_clippyId++;
	Animate(kClippyAutoHideMs + 2000);
}

void HideClippy() {
	if (!g_clippyVisible) return;
	g_clippyVisible = false;
	Animate(1500);
}

void TickClippy() {
	// The probe's sequence moves on every mouse move/click in the game window.
	const unsigned seq = g_UiCursorProbe.seq;
	if (seq != g_lastProbeSeq) { g_lastProbeSeq = seq; g_lastInputMs.store(NowMs()); }
	const unsigned long long now = NowMs();
	if (g_lastInputMs.load() == 0) g_lastInputMs.store(now);
	if (g_clippyVisible) {
		if (now - g_clippyShownMs > kClippyAutoHideMs) HideClippy();
		return;
	}
	// Anywhere -- menu, demo, editor, even during a render -- with the game in front. Never while a
	// capture could include our chrome (he would be baked into the take); a render that keeps the
	// chrome out of its frames is fine, and there his answers are visual only (see `egg clippy`).
	const bool busy = CameraEditor_CustomizeModalOpen() || g_vegasCheck.load() == 2 || g_vegasPending || Osu_Active();
	if (busy || ChromeHidden() || !GameForeground()) return;
	if (now - g_lastInputMs.load() < kClippyIdleMs) return;
	g_lastInputMs.store(now); // the roll is spent: another 5 idle minutes for the next one
	if (U01() < kClippyChance) ShowClippy();
}

bool HintActiveJson(JsonBuilder& j) {
	if (g_hintId == 0) return false;
	j.Key("hint"); j.BeginObject();
	j.IntField("id", g_hintId);
	j.BoolField("loud", g_hintLoud);
	if (!g_bannerTitle.empty()) {
		j.StringField("title", g_bannerTitle); j.StringField("code", g_bannerCode); j.StringField("sub", g_bannerSub);
	} else if (g_hintIndex >= 0 && g_hintIndex < kHintCount) {
		j.StringField("title", kHints[g_hintIndex].title);
		j.StringField("code", kHints[g_hintIndex].code);
		j.StringField("sub", kHints[g_hintIndex].sub);
	}
	j.EndObject();
	return true;
}

void EnsureLoaded() {
	if (!g_loaded) Load();
}

// Puts the egg's world effect on or off. Called with g_mx held; the world modules never call back.
bool ApplyWorld(EasterEgg egg, bool on) {
	switch (egg) {
	case EasterEgg::Disco: return EasterEggLighting_SetDisco(on);
	case EasterEgg::WesAnderson: return EasterEggLighting_SetWes(on);
	case EasterEgg::MoonGravity: return EasterEggBodies_SetMoon(on);
	case EasterEgg::BigHead: EasterEggBodies_SetBigHead(on); break;
	case EasterEgg::ChickenStorm: EasterEggChickens_Set(on); break;
	case EasterEgg::MichaelBay: EasterEggBay_Set(on); break;
	default: break;
	}
	return true;
}

void Toggle(EasterEgg egg) { EasterEgg_Set(egg, !EasterEgg_Active(egg)); }

// `osu`: the pop-up, then the game a beat later (its layer sits above the pop-up's). Typed again
// while open, it closes.
void OsuCheat() {
	if (Osu_Active()) { Osu_Close(); return; }
	if (ChromeHidden()) { MvmDebugLog_LinefAlways("egg.osu", "not opened: this render captures the UI"); return; }
	std::lock_guard<std::mutex> lock(g_mx);
	if (g_osuOpenAtMs) return; // already on its way
	ShowBanner("CLICK THE CIRCLES", "300 300 300", "ready when you are.");
	g_osuOpenAtMs = NowMs() + 2200;
}

} // namespace

const char* EasterEgg_Name(EasterEgg egg) {
	const int i = (int)egg;
	return (i >= 0 && i < (int)EasterEgg::Count) ? kEggNames[i] : "?";
}

bool EasterEgg_Active(EasterEgg egg) {
	const int i = (int)egg;
	return i >= 0 && i < (int)EasterEgg::Count && g_eggOn[i].load(std::memory_order_relaxed);
}

void EasterEgg_Set(EasterEgg egg, bool on, bool announce) {
	const int i = (int)egg;
	if (i < 0 || i >= (int)EasterEgg::Count) return;
	std::lock_guard<std::mutex> lock(g_mx);
	EnsureLoaded();
	if (!ApplyWorld(egg, on)) {
		advancedfx::Message("egg %s: activation refused; could not save recovery state.\n", kEggNames[i]);
		return;
	}
	// Disco and Wes Anderson both own the scene's light colour: one replaces the other.
	if (on && egg == EasterEgg::Disco) g_eggOn[(int)EasterEgg::WesAnderson].store(false);
	if (on && egg == EasterEgg::WesAnderson) g_eggOn[(int)EasterEgg::Disco].store(false);
	const bool was = g_eggOn[i].load();
	g_eggOn[i].store(on);
	if (on && egg == EasterEgg::WesAnderson && !was) {
		// Switched on mid-game: the first card names the round as it stands.
		const int n = g_chapter.load() + 1;
		std::string title = ReadRoundTitle();
		g_chapterTitle = title.empty() ? kChapters[(n - 1) % kChapterCount] : title;
		g_chapter.store(n);
		g_chapterMs.store(NowMs());
	}
	if (announce && was != on) {
		const Banner& b = on ? kOnBanner[i] : kOffBanner[i];
		ShowBanner(b.title, b.code, b.sub);
	}
	Animate(3000);
	MvmDebugLog_LinefAlways("egg.toggle", "%s=%d", kEggNames[i], on ? 1 : 0);
}

void EasterEggs_RunMainThreadFrame() {
	const bool levelReset = g_levelResetPending.exchange(false);
	{
		std::lock_guard<std::mutex> lock(g_mx);
		EnsureLoaded();
		const unsigned long long now = NowMs();
		const unsigned long long dt = g_lastTickMs ? (now - g_lastTickMs) : 0;
		g_lastTickMs = now;
		// A hitch (load screen, debugger) is not play time.
		TickHints(dt > 1000 ? 0 : dt);
		TickVegas();
		TickClippy();
		if (g_dirty && now - g_lastSaveMs > 60000) Save();
	}
	bool openOsu = false;
	const int escapes = g_escapeHits.load();
	const bool escape = escapes != g_escapeSeen;
	const bool cancel = escape || levelReset;
	g_escapeSeen = escapes;
	{
		std::lock_guard<std::mutex> lock(g_mx);
		if (cancel) {
			g_osuOpenAtMs = 0; // a pending `osu` pop-up does not open after all
			HideClippy();
			g_vegasCancelledThrough = g_vegasId;
			g_vegasPending = false;
			++g_vegasCheckGeneration; // an in-flight process scan cannot revive a dismissed clip
			g_vegasCheck.store(0);
			if (levelReset) {
				g_hintCancelledThrough = g_hintId;
				g_lastInputMs.store(NowMs()); // do not immediately reopen idle Clippy on the new level
				g_konamiPos.store(0);
				g_konamiSeen = g_konamiHits.load();
			}
			Animate(500);
			MvmDebugLog_LinefAlways("egg.cancel", "reason=%s vegas through #%d; pending check discarded",
				levelReset ? "level change" : "escape", g_vegasCancelledThrough);
		}
		if (g_osuOpenAtMs && NowMs() >= g_osuOpenAtMs) { g_osuOpenAtMs = 0; openOsu = true; }
	}
	if (cancel) {
		// Everything off; only the first one says goodbye (a stack of banners would be noise).
		bool announced = false;
		for (int i = 0; i < (int)EasterEgg::Count; ++i) {
			if (!EasterEgg_Active((EasterEgg)i)) continue;
			EasterEgg_Set((EasterEgg)i, false, !levelReset && !announced);
			announced = true;
		}
	}
	if (levelReset) { Osu_Close(); Snake_Close(); }
	// Consume toggles before updating audio/cosmetic ownership or either mini-game.
	EasterEggUwu_RunFrame();
	Snake_RunFrame();
	if (openOsu) Osu_Open();
	// The Konami code is the disco's door (outside the lock: Set takes it).
	const int hits = g_konamiHits.load();
	if (hits != g_konamiSeen) { g_konamiSeen = hits; Toggle(EasterEgg::Disco); }

	EasterEggLighting_RunFrame();
	EasterEggChickens_RunFrame();
	EasterEggBay_RunFrame();
	Osu_RunFrame();
	if (EasterEgg_Active(EasterEgg::Disco) || EasterEgg_Active(EasterEgg::WesAnderson)
		|| EasterEgg_Active(EasterEgg::MichaelBay)) {
		std::lock_guard<std::mutex> lock(g_mx);
		Animate(500);
	}
}

void EasterEggs_RunLateFrame() {
	EasterEggBodies_RunLateFrame();
	EasterEggChickens_RunLateFrame();
}

void EasterEggs_OnGameEvent(SOURCESDK::CS2::CGameEvent* event) {
	EasterEggBay_OnGameEvent(event);
	EasterEggUwu_OnGameEvent(event); // UwU: world-placed bang on deaths, pops on gun shots
	EasterEggWes_OnGameEvent(event);
}

namespace {
// Gear value of a pawn (C_CSPlayerPawn::m_unCurrentEquipmentValue, uint16 dollars). POD-only for SEH.
static ptrdiff_t g_equipOffset = -1;
static int ReadEquipment(void* pawn) {
	if (g_equipOffset <= 0 || !pawn) return -1;
	__try { return *(unsigned short*)((unsigned char*)pawn + g_equipOffset); }
	__except (1) { return -1; }
}
bool IsFirearmClass(const char* cls) {
	if (!cls || strncmp(cls, "weapon_", 7) != 0) return false;
	const char* n = cls + 7;
	static const char* const kNot[] = { "knife", "bayonet", "hegrenade", "flashbang", "smokegrenade", "molotov",
		"incgrenade", "decoy", "c4", "taser", "healthshot", "tagrenade", "snowball", "breachcharge" };
	for (const char* x : kNot) if (strncmp(n, x, strlen(x)) == 0) return false;
	return true;
}
// The round's title from what the two teams hold right now (freeze time over: everyone has bought).
std::string ReadRoundTitle() {
	if (g_equipOffset < 0) {
		ptrdiff_t off = 0;
		g_equipOffset = getOffset(&off, "client.dll", "C_CSPlayerPawn", "m_unCurrentEquipmentValue") ? off : 0;
	}
	WesRound::Snapshot snap;
	double sumT = 0.0, sumCT = 0.0;
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* e = CosmeticEntFromIndex(i);
		if (!e) continue;
		if (e->IsPlayerPawn()) {
			if (e->GetHealth() <= 0) continue;
			const int team = e->GetTeam(), eq = ReadEquipment(e);
			if (eq < 0 || (team != 2 && team != 3)) continue;
			if (team == 2) { snap.playersT++; sumT += eq; } else { snap.playersCT++; sumCT += eq; }
			if (eq > snap.maxEquipment) snap.maxEquipment = eq;
			continue;
		}
		if (IsFirearmClass(e->GetClassName()) && e->GetOwnerEntityHandle().IsValid()) snap.firearms++;
	}
	if (snap.playersT) snap.avgT = sumT / snap.playersT;
	if (snap.playersCT) snap.avgCT = sumCT / snap.playersCT;
	const WesRound::Result r = WesRound::Classify(snap);
	MvmDebugLog_LinefAlways("egg.wes", "round: T %d players avg $%.0f, CT %d avg $%.0f, max $%.0f, %d guns -> kind %d team %d",
		snap.playersT, snap.avgT, snap.playersCT, snap.avgCT, snap.maxEquipment, snap.firearms, (int)r.kind, r.team);
	return WesRound::Title(r);
}
} // namespace

void NextChapter() {
	std::string title = ReadRoundTitle();
	const int n = g_chapter.load() + 1;
	if (title.empty()) title = kChapters[(n - 1) % kChapterCount];
	{
		std::lock_guard<std::mutex> lock(g_mx);
		g_chapterTitle = title;
	}
	g_chapterMs.store(NowMs());
	g_chapter.store(n);
}

// The card comes up as freeze time ends: the buys are done, so the title can say what kind of round it is.
void EasterEggWes_OnGameEvent(SOURCESDK::CS2::CGameEvent* event) {
	if (!event || !EasterEgg_Active(EasterEgg::WesAnderson)) return;
	const char* name = event->GetName();
	if (!name || 0 != _stricmp(name, "round_freeze_end")) return;
	if (NowMs() - g_chapterMs.load() < kChapterDedupMs) return;
	NextChapter();
}

void EasterEggs_LevelInit() {
	// The previous entity graph is gone. Only forget pointers here; restore settings and close
	// media/input owners from the main pump after the engine finishes its level callback.
	CosmeticsRef().ForgetThemeEntities("level init");
	g_levelResetPending.store(true);
	EasterEggBodies_LevelInit();
	EasterEggChickens_LevelInit();
}

void EasterEggs_OnWindowMessage(unsigned msg, unsigned long long wParam) {
	switch (msg) {
	case WM_KEYDOWN: {
		g_lastInputMs.store(NowMs());
		const int vk = (int)wParam;
		// osu! and Snake own Escape while they are open (back / close).
		if (vk == VK_ESCAPE && !Osu_Active() && !Snake_Active()) g_escapeHits.fetch_add(1);
		int pos = g_konamiPos.load();
		if (vk == kKonami[pos]) pos++;
		else pos = (vk == kKonami[0]) ? 1 : 0;
		// A third UP after UP UP keeps the sequence alive at "UP UP".
		if (pos == 0 && vk == VK_UP) pos = 1;
		if (pos == (int)(sizeof(kKonami) / sizeof(kKonami[0]))) { g_konamiHits.fetch_add(1); pos = 0; }
		g_konamiPos.store(pos);
		break;
	}
	case WM_LBUTTONDOWN: case WM_RBUTTONDOWN:
		g_lastInputMs.store(NowMs());
		g_clickSeq.fetch_add(1);
		break;
	case WM_MOUSEWHEEL: case WM_CHAR:
		g_lastInputMs.store(NowMs());
		break;
	default: break;
	}
}

void EasterEggs_OnRenderStarted() {
	StartVegasCheck();
}

bool EasterEggs_BuildOverlayJson(std::string& out) {
	std::lock_guard<std::mutex> lock(g_mx);
	EnsureLoaded();
	const bool egirl = EasterEgg_Active(EasterEgg::EGirl);

	// Cursor in client fractions, and whether a pointer is actually on screen (steering the free
	// cam hides it; then the sparkles drift ambiently instead of trailing a cursor nobody sees).
	double cx = 0.5, cy = 0.5;
	bool cvis = false;
	HWND hwnd = g_UiCursorProbe.hwnd;
	RECT rc{};
	if (hwnd && GetClientRect(hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
		POINT pt{};
		CURSORINFO ci{}; ci.cbSize = sizeof(ci);
		if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) {
			cx = (double)pt.x / rc.right;
			cy = (double)pt.y / rc.bottom;
			cvis = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor
				&& cx >= 0.0 && cx <= 1.0 && cy >= 0.0 && cy <= 1.0 && GetForegroundWindow() == hwnd;
		}
	}

	JsonBuilder j;
	j.BeginObject();
	j.BoolField("hidden", ChromeHidden());
	j.BoolField("egirl", egirl);
	j.DoubleField("pulse", egirl ? EasterEggUwu_MusicPulse() : 0.0); // the vignette's beat
	j.DoubleField("cx", cx);
	j.DoubleField("cy", cy);
	j.BoolField("cvis", cvis);
	j.IntField("click", (int64_t)g_clickSeq.load());
	HintActiveJson(j);
	j.IntField("hintCancelledThrough", g_hintCancelledThrough);
	j.IntField("vegas", g_vegasId);
	j.IntField("vegasCancelledThrough", g_vegasCancelledThrough);
	j.Key("clippy"); j.BeginObject();
	j.IntField("id", g_clippyId);
	j.BoolField("show", g_clippyVisible);
	{
		const ClippyLine& cl = kClippyLines[g_clippyLine];
		j.StringField("title", cl.title);
		j.StringField("body", cl.body);
		j.StringField("aside", cl.aside);
		// answers: "label|word" strings, so the JSON stays flat.
		j.Key("answers"); j.BeginArray();
		for (const ClippyAnswer& a : cl.answers) {
			if (!a.label[0]) continue;
			j.ValueString(std::string(a.label) + "|" + a.word);
		}
		j.EndArray();
	}
	j.EndObject();
	j.BoolField("disco", EasterEgg_Active(EasterEgg::Disco));
	j.IntField("bay", EasterEggBay_BlastCount());
	const bool wes = EasterEgg_Active(EasterEgg::WesAnderson);
	j.Key("wes"); j.BeginObject();
	j.BoolField("on", wes);
	const int chapter = g_chapter.load();
	j.IntField("chapter", chapter);
	j.StringField("title", chapter > 0 ? g_chapterTitle : "");
	j.EndObject();
	j.EndObject();
	out = j.Str();
	return egirl || NowMs() < g_animUntilMs;
}

void EasterEggs_OsuCheat() { OsuCheat(); }

void EasterEggs_CheatCode(const char* code) {
	struct Cheat { const char* code; EasterEgg egg; };
	const Cheat cheats[] = {
		{ "uwu", EasterEgg::EGirl }, { "inferno_disco", EasterEgg::Disco },
		{ "one_small_step", EasterEgg::MoonGravity }, { "bighead", EasterEgg::BigHead },
		{ "bawk", EasterEgg::ChickenStorm }, { "michaelbay", EasterEgg::MichaelBay },
		{ "symmetry", EasterEgg::WesAnderson },
	};
	for (const Cheat& c : cheats) {
		if (0 != _stricmp(code, c.code)) continue;
		Toggle(c.egg);
		advancedfx::Message(EasterEgg_Active(c.egg) ? "...\n" : "ok.\n");
		return;
	}
}

void EasterEggs_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* sub = argc >= 3 ? args->ArgV(2) : "status";
	if (0 == _stricmp(sub, "cancel")) {
		// Same queued main-thread path as Escape outside osu!, without synthesizing window input.
		if (Osu_Active()) { advancedfx::Message("egg cancel: osu owns Escape; close osu first.\n"); return; }
		g_escapeHits.fetch_add(1);
		advancedfx::Message("egg cancel: queued.\n");
		return;
	}
	if (0 == _stricmp(sub, "status")) {
		std::lock_guard<std::mutex> lock(g_mx);
		EnsureLoaded();
		const char* verdicts[] = { "show", "not scheduled", "not yet", "daily cap", "wall gap" };
		const int v = (int)EasterEggHint::Evaluate(g_hint, (double)time(nullptr), g_hintParams);
		advancedfx::Message("eggs:");
		for (int i = 0; i < (int)EasterEgg::Count; ++i) advancedfx::Message(" %s=%d", kEggNames[i], g_eggOn[i].load() ? 1 : 0);
		advancedfx::Message("\nchickens live=%d bay blasts=%d wes chapter=%d\n", EasterEggChickens_Count(),
			EasterEggBay_BlastCount(), g_chapter.load());
		advancedfx::Message("bighead cached models=%d\n", EasterEggBodies_CachedModels());
		advancedfx::Message("hints: active=%.0fs next at %.0fs (%s), today %d/%d, %d hint(s) in table\n",
			g_hint.activeSeconds, g_hint.nextAtActive, verdicts[v], g_hint.shownToday, g_hintParams.maxPerDay, kHintCount);
		advancedfx::Message("vegas: check=%d pending=%d shown=%d | clippy: shown=%d visible=%d line=%d/%d idle=%llus\n",
			g_vegasCheck.load(), g_vegasPending ? 1 : 0, g_vegasId, g_clippyId, g_clippyVisible ? 1 : 0,
			g_clippyLine, kClippyLineCount,
			(NowMs() - g_lastInputMs.load()) / 1000ull);
		advancedfx::Message("%s\n", EasterEggUwu_Status().c_str());
		advancedfx::Message("lighting recovery: %s\n", EasterEggLighting_RecoveryStatus());
		advancedfx::Message("moon recovery: %s\n", EasterEggBodies_MoonRecoveryStatus());
		advancedfx::Message("%s\n", Snake_Status().c_str());
		return;
	}
	for (int i = 0; i < (int)EasterEgg::Count; ++i) {
		if (0 != _stricmp(sub, kEggNames[i])) continue;
		const char* a = argc >= 4 ? args->ArgV(3) : "toggle";
		const EasterEgg egg = (EasterEgg)i;
		const bool on = 0 == _stricmp(a, "toggle") ? !EasterEgg_Active(egg) : (0 == _stricmp(a, "on") || 0 == strcmp(a, "1"));
		EasterEgg_Set(egg, on);
		return;
	}
	if (0 == _stricmp(sub, "snake")) {
		// snake [open|ask|yes|no|close]: ask = the long-render prompt, yes/no = its answers.
		const char* a = argc >= 4 ? args->ArgV(3) : "open";
		if (0 == _stricmp(a, "close")) Snake_Close();
		else if (0 == _stricmp(a, "ask")) Snake_Ask();
		else if (0 == _stricmp(a, "yes")) Snake_Answer(true);
		else if (0 == _stricmp(a, "no")) Snake_Answer(false);
		else Snake_Open();
		advancedfx::Message("%s\n", Snake_Status().c_str());
		return;
	}
	if (0 == _stricmp(sub, "chickenclass")) {
		if (argc >= 4) EasterEggChickens_SetClass(args->ArgV(3));
		advancedfx::Message("chickens spawn as %s\n", EasterEggChickens_Class());
		return;
	}
	if (0 == _stricmp(sub, "boom")) {
		// A Michael Bay blast in front of the camera, no death needed.
		EasterEggBay_TestBlast();
		return;
	}
	if (0 == _stricmp(sub, "chapter")) {
		// Next Wes Anderson chapter card now, named from the round on screen (what freeze-time end does).
		NextChapter();
		return;
	}
	if (0 == _stricmp(sub, "hint")) {
		// hint [index] [loud|faint]: preview a hint without touching the scheduler.
		std::lock_guard<std::mutex> lock(g_mx);
		int index = argc >= 4 ? atoi(args->ArgV(3)) : (int)(U01() * kHintCount);
		if (index < 0 || index >= kHintCount) index = 0;
		const bool loud = argc >= 5 ? 0 == _stricmp(args->ArgV(4), "loud") : U01() < 0.4;
		ShowHint(index, loud);
		advancedfx::Message("hint #%d (%s): %s\n", index, loud ? "loud" : "faint", kHints[index].title);
		return;
	}
	if (0 == _stricmp(sub, "vegas")) {
		// vegas test = play the intervention now; vegas check = run the real process check.
		if (argc >= 4 && 0 == _stricmp(args->ArgV(3), "check")) { StartVegasCheck(); advancedfx::Message("vegas: checking processes...\n"); }
		else { std::lock_guard<std::mutex> lock(g_mx); TriggerVegas(); }
		return;
	}
	if (0 == _stricmp(sub, "clippy")) {
		const char* a = argc >= 4 ? args->ArgV(3) : "show";
		{
			std::lock_guard<std::mutex> lock(g_mx);
			// show [line]: a specific line (0-based) or a random one.
			if (0 == _stricmp(a, "show")) { ShowClippy(argc >= 5 ? atoi(args->ArgV(4)) : -1); return; }
			HideClippy();
		}
		// Every answer is a different door; "ok" (and anything unknown) just sends him away. During a
		// render he is visual only: anything that would change the game (and so the take) is ignored --
		// only the overlays (Snake, osu!) still open.
		if (RenderSessionDriver_IsActive()) {
			if (0 == _stricmp(a, "snake")) Snake_Open();
			else if (0 == _stricmp(a, "osu")) OsuCheat();
			else if (0 != _stricmp(a, "ok")) MvmDebugLog_LinefAlways("egg.clippy", "'%s' ignored: a render is running", a);
			return;
		}
		if (0 == _stricmp(a, "yes")) EasterEgg_Set(EasterEgg::EGirl, true);
		else if (0 == _stricmp(a, "absolutely")) EasterEgg_Set(EasterEgg::MichaelBay, true);
		else if (0 == _stricmp(a, "bass")) EasterEgg_Set(EasterEgg::Disco, true);
		else if (0 == _stricmp(a, "moon")) EasterEgg_Set(EasterEgg::MoonGravity, true);
		else if (0 == _stricmp(a, "chickens")) EasterEgg_Set(EasterEgg::ChickenStorm, true);
		else if (0 == _stricmp(a, "bighead")) EasterEgg_Set(EasterEgg::BigHead, true);
		else if (0 == _stricmp(a, "wes")) EasterEgg_Set(EasterEgg::WesAnderson, true);
		else if (0 == _stricmp(a, "osu")) OsuCheat();
		else if (0 == _stricmp(a, "snake")) Snake_Open();
		return;
	}
	if (0 == _stricmp(sub, "resethints")) {
		std::lock_guard<std::mutex> lock(g_mx);
		g_hint = EasterEggHint::State();
		Save();
		advancedfx::Message("hint scheduler reset.\n");
		return;
	}
	advancedfx::Message(
		"%s egg status | cancel | hint [index] [loud|faint] | vegas [test|check] | clippy show [line]|hide | chapter | boom | resethints\n"
		"%s egg egirl|disco|moon|bighead|chickens|michaelbay|wes [on|off|toggle]\n", cmd, cmd);
}

} // namespace Filmmaker

// Cheat codes. Deliberately top-level and undocumented in help text: finding them is the point.
CON_COMMAND(uwu, "") { Filmmaker::EasterEggs_CheatCode("uwu"); }
CON_COMMAND(inferno_disco, "") { Filmmaker::EasterEggs_CheatCode("inferno_disco"); }
CON_COMMAND(one_small_step, "") { Filmmaker::EasterEggs_CheatCode("one_small_step"); }
CON_COMMAND(bighead, "") { Filmmaker::EasterEggs_CheatCode("bighead"); }
CON_COMMAND(bawk, "") { Filmmaker::EasterEggs_CheatCode("bawk"); }
CON_COMMAND(michaelbay, "") { Filmmaker::EasterEggs_CheatCode("michaelbay"); }
CON_COMMAND(symmetry, "") { Filmmaker::EasterEggs_CheatCode("symmetry"); }
// Circles! An osu!-style rhythm game over the game (Osu/OsuGame.h). Typing it again closes it.
CON_COMMAND(osu, "") { Filmmaker::EasterEggs_OsuCheat(); }
