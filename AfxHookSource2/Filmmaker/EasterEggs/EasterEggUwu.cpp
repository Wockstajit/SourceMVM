#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "EasterEggUwu.h"

#include "EasterEggAudioMath.h"
#include "EasterEggGunMute.h"
#include "EasterEggs.h"
#include "Osu/OsuGame.h"
#include "Osu/OsuPlatform.h"

#include "../Cosmetics/CosmeticEntityAccess.h"
#include "../Cosmetics/CosmeticOverrideSystem.h"
#include "../Movie/CameraBridge.h"
#include "../Movie/GunWeaponMath.h"
#include "../Movie/ParticleFx.h"
#include "../Movie/RagdollBodies.h"
#include "../Platform/JsonParser.h"
#include "../Platform/TextEncoding.h"
#include "../Render/RenderSessionDriver.h"
#include "../World/WorldModulation.h"
#include "../Debug/MvmDebugLog.h"
#include "../../MirvTime.h"
#include "../../hlaeFolder.h"
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/icvar.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/igameevents.h"

#include <Windows.h>
#include <xaudio2.h>
#pragma comment(lib, "xaudio2.lib")

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace fs = std::filesystem;

namespace Filmmaker {
namespace {

// Distances in Source units (a player is ~72 tall, a long corridor ~1500). Gunshots carry farther
// than a death; both are gone across the map.
constexpr double kShotRef = 450.0, kShotMax = 3000.0;
constexpr double kBangRef = 400.0, kBangMax = 2200.0;
constexpr int kVoices = 10;         // overlapping copies of one clip (full-auto fire)
constexpr int kMaxPerFrame = 12;    // a demo seek replays a pile of events at once: stay sane
// XAudio2 volumes are linear amplitude and may exceed 1 (gain). Pops sit well over the game's own
// mix so they read through the action; the song stays under everything.
constexpr float kBangVolume = 2.4f, kPopVolume = 3.2f;
constexpr float kMusicVolume = 0.10f; // the background song, looping

struct Clip {
	std::vector<int16_t> pcm; // 16-bit MONO 44.1 kHz (panned by the output matrix)
	IXAudio2SourceVoice* voices[kVoices] = {};
	int next = 0;
};

IXAudio2* g_xa = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
unsigned g_masterChannels = 2;
Clip g_bang, g_pops[4];
std::vector<int16_t> g_musicPcm; // 16-bit STEREO 44.1 kHz
std::vector<float> g_beat;       // beat strength every 10 ms of the song (EggAudio::BeatEnvelope)
double g_bpm = 0.0;
std::atomic<double> g_pulse{ 0.0 };
double g_pulseQpcMs = 0.0;
constexpr int kBeatHop = 441; // 10 ms at 44.1 kHz
IXAudio2SourceVoice* g_music = nullptr;
bool g_musicPlaying = false;
int g_popCount = 0;
std::atomic<bool> g_loaded{ false }, g_loading{ false };
std::string g_loadNote = "not loaded";
std::mt19937 g_rng{ (unsigned)GetTickCount64() };

// Events: deduped on the event thread, played on the main thread (entities + camera live there).
struct Pending { bool death; int userid; };
std::mutex g_mx;
std::vector<Pending> g_queue;
unsigned long long g_lastFire = ~0ull, g_lastDeath = ~0ull; // (tick << 32 | userid) of the last one
std::atomic<unsigned long long> g_played{ 0 }, g_silent{ 0 };

// ---- the UwU look: female agents, pink / purple skins (a cosmetics THEME) ----
// The female agents, by the game's own voice sets (items_game.txt vo_prefix *_fem*).
const char* const kFemaleCT[] = {
	"agents/models/ctm_swat/ctm_swat_variantf.vmdl", "agents/models/ctm_swat/ctm_swat_variantk.vmdl",
	"agents/models/ctm_diver/ctm_diver_varianta.vmdl", "agents/models/ctm_gendarmerie/ctm_gendarmerie_variantc.vmdl",
};
const char* const kFemaleT[] = {
	"agents/models/tm_professional/tm_professional_varg.vmdl", "agents/models/tm_professional/tm_professional_varj.vmdl",
	"agents/models/tm_jungle_raider/tm_jungle_raider_variante.vmdl", "agents/models/tm_jungle_raider/tm_jungle_raider_variantf.vmdl",
	"agents/models/tm_jungle_raider/tm_jungle_raider_variantf2.vmdl",
};
// A pink or purple finish for every gun (weapon def index -> paint kit; names in Data/cosmetics.json).
const int kPinkSkins[][2] = {
	{ 1, 1090 },  // Desert Eagle | Ocean Drive
	{ 2, 1263 },  // Dual Berettas | Rose Nacre
	{ 3, 464 },   // Five-SeveN | Neon Kimono
	{ 4, 988 },   // Glock-18 | Neo-Noir
	{ 7, 707 },   // AK-47 | Neon Rider
	{ 8, 727 },   // AUG | Midnight Lily
	{ 9, 640 },   // AWP | Fever Dream
	{ 10, 1127 }, // FAMAS | Rapid Eye Movement
	{ 11, 739 },  // G3SG1 | Violet Murano
	{ 13, 661 },  // Galil AR | Sugar Rush
	{ 14, 933 },  // M249 | Midnight Palm
	{ 16, 695 },  // M4A4 | Neo-Noir
	{ 17, 433 },  // MAC-10 | Neon Rider
	{ 19, 156 },  // P90 | Death by Kitty
	{ 23, 161 },  // MP5-SD | Neon Squeezer
	{ 24, 131 },  // UMP-45 | Neo-Noir
	{ 25, 1046 }, // XM1014 | XOXO
	{ 26, 692 },  // PP-Bizon | Night Riot
	{ 27, 909 },  // MAG-7 | Popdog
	{ 28, 1260 }, // Negev | Sour Grapes
	{ 29, 638 },  // Sawed-Off | Wasteland Princess
	{ 30, 1286 }, // Tec-9 | Garter-9
	{ 32, 878 },  // P2000 | Coral Halftone
	{ 33, 893 },  // MP7 | Neon Ply
	{ 34, 262 },  // MP9 | Rose Iron
	{ 35, 537 },  // Nova | Hyper Beast
	{ 36, 1273 }, // P250 | Plum Netting
	{ 38, 883 },  // SCAR-20 | Wild Berry
	{ 39, 98 },   // SG 553 | Ultraviolet
	{ 40, 956 },  // SSG 08 | Fever Dream
	{ 60, 1319 }, // M4A1-S | Rose Hex
	{ 61, 653 },  // USP-S | Neo-Noir
	{ 63, 1329 }, // CZ75-Auto | Pink Pearl
	{ 64, 1389 }, // R8 Revolver | Mauve Aside
};
constexpr int kKnifeDef = 507, kKnifePaint = 419;     // Karambit | Doppler (Phase 2)
constexpr int kGloveDef = 5030, kGlovePaint = 10037;  // Sport Gloves | Pandora's Box
bool g_themeOn = false;
double g_themeDressQpc = 0.0;
std::set<unsigned long long> g_dressed; // steamIds (or 1<<63 | controller handle for bots) dressed this theme
int g_dressedCount = 0;

// The UwU world: a violet tint over map, props and sky (the World tint card's "violet" swatch).
constexpr int kTintR = 176, kTintG = 107, kTintB = 255;
const ModCategory kTintCats[] = { ModCategory::World, ModCategory::Props, ModCategory::SunClouds };
struct UserTint { int r = 255, g = 255, b = 255; bool enabled = false; };
UserTint g_worldStash[3];
bool g_worldStashed = false;

void StashWorld() {
	if (g_worldStashed) return;
	for (ModCategory c : kTintCats) {
		auto& tint = g_worldStash[(int)c];
		WorldModulation_GetTint(c, &tint.r, &tint.g, &tint.b, &tint.enabled);
	}
	g_worldStashed = true;
}

void RestoreWorld() {
	if (!g_worldStashed) return;
	for (ModCategory c : kTintCats) {
		const auto& tint = g_worldStash[(int)c];
		WorldModulation_SetTint(c, tint.r, tint.g, tint.b);
		WorldModulation_SetEnabled(c, tint.enabled);
	}
	g_worldStashed = false;
}

// Forced while UwU is on: re-asserted if anything (the World tint card, a preset) changes it.
void ForceVioletWorld() {
	for (ModCategory c : kTintCats) {
		int r = 0, g = 0, b = 0;
		bool enabled = false;
		WorldModulation_GetTint(c, &r, &g, &b, &enabled);
		if (enabled && r == kTintR && g == kTintG && b == kTintB) continue;
		WorldModulation_SetTint(c, kTintR, kTintG, kTintB);
		WorldModulation_SetEnabled(c, true);
	}
}

void DressPlayers() {
	CosmeticPlayerRef players[64];
	int count = 0;
	CosmeticEnumeratePlayers(players, 64, &count);
	CosmeticOverrideSystem& cos = CosmeticsRef();
	for (int i = 0; i < count; ++i) {
		const CosmeticPlayerRef& p = players[i];
		if (p.team != 2 && p.team != 3) continue; // spectators / unassigned
		const unsigned long long key = p.steamId ? p.steamId : ((1ull << 63) | p.controllerHandle);
		// Dressed already -- unless the cosmetics store dropped the profile (a new demo clears it).
		if (g_dressed.count(key)) {
			const CosmeticProfile* prof = p.steamId ? cos.Store().Find(p.steamId) : nullptr;
			if (!p.steamId || (prof && prof->agent.set)) continue;
		}
		// The same player always gets the same agent (stable across toggles).
		const unsigned long long h = key * 0x9E3779B97F4A7C15ull;
		const char* model = p.team == 3
			? kFemaleCT[(h >> 33) % (sizeof(kFemaleCT) / sizeof(kFemaleCT[0]))]
			: kFemaleT[(h >> 33) % (sizeof(kFemaleT) / sizeof(kFemaleT[0]))];
		if (p.steamId) {
			cos.SetAgent(p.steamId, model, 0);
			for (const auto& w : kPinkSkins) cos.SetWeapon(p.steamId, w[0], w[1], 0.01f, (int)(h % 1000), -1);
			cos.SetKnife(p.steamId, kKnifeDef, kKnifePaint, 0.01f, (int)(h % 1000));
			cos.SetGloves(p.steamId, kGloveDef, kGlovePaint, 0.05f, (int)(h % 1000));
		} else {
			cos.SetPlayerAgent(p, model); // bots: the agent only (their guns have no owner profile)
		}
		g_dressed.insert(key);
		g_dressedCount++;
	}
}

// UwU on/off follow-ups (main thread).
bool g_wasOn = false;
std::string g_savedHBlood = "1", g_savedABlood = "1";

fs::path Dir() {
	const char* h = GetHlaeFolder();
	return h ? fs::path(Utf8ToWide(h)) / L"easter-eggs" / L"uwu" : fs::path();
}

bool Decode(const fs::path& file, std::vector<int16_t>& out, int channels = 1) {
	std::error_code ec;
	if (!fs::exists(file, ec)) return false;
	std::string bytes;
	const std::wstring cmd = L"\"" + OsuPlatform::Ffmpeg().wstring() + L"\" -v error -nostdin -i \"" + file.wstring()
		+ L"\" -vn -ac " + std::to_wstring(channels) + L" -ar 44100 -f s16le -";
	if (!OsuPlatform::RunCapture(cmd, 20000, bytes) || bytes.size() < 2) return false;
	out.resize(bytes.size() / 2);
	std::memcpy(out.data(), bytes.data(), out.size() * 2);
	return true;
}

bool MakeVoices(Clip& c) {
	WAVEFORMATEX fmt{};
	fmt.wFormatTag = WAVE_FORMAT_PCM; fmt.nChannels = 1; fmt.nSamplesPerSec = 44100;
	fmt.wBitsPerSample = 16; fmt.nBlockAlign = 2; fmt.nAvgBytesPerSec = 44100 * 2;
	for (auto& v : c.voices)
		if (FAILED(g_xa->CreateSourceVoice(&v, &fmt))) { v = nullptr; return false; }
	return true;
}

void LoadImpl();
// The decode thread: nothing it throws may escape (an uncaught exception ends the game).
void Load() {
	try { LoadImpl(); }
	catch (...) { MvmDebugLog_LinefAlways("egg.uwu", "sound loading failed (exception)"); g_loadNote = "FAILED (exception)"; g_loaded.store(true); }
}
void LoadImpl() {
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const fs::path dir = Dir();
	MvmDebugLog_LinefAlways("egg.uwu", "loading sounds from %s", WideToUtf8(dir.wstring()).c_str());
	Clip bang, pops[4];
	const bool bangOk = Decode(dir / L"bang.mp3", bang.pcm);
	int popCount = 0;
	for (int i = 0; i < 4; ++i)
		if (Decode(dir / (L"pop" + std::to_wstring(i + 1) + L".mp3"), pops[popCount].pcm)) popCount++;
	std::vector<int16_t> music;
	const bool musicOk = Decode(dir / L"music.mp3", music, 2);
	std::lock_guard<std::mutex> lock(g_mx);
	if (!g_xa) {
		if (FAILED(XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) g_xa = nullptr;
		else if (FAILED(g_xa->CreateMasteringVoice(&g_master))) { g_xa->Release(); g_xa = nullptr; }
		if (g_master) { XAUDIO2_VOICE_DETAILS d{}; g_master->GetVoiceDetails(&d); g_masterChannels = std::max(1u, d.InputChannels); }
	}
	if (g_xa) {
		g_bang = std::move(bang);
		if (bangOk) MakeVoices(g_bang);
		for (int i = 0; i < popCount; ++i) { g_pops[i] = std::move(pops[i]); MakeVoices(g_pops[i]); }
		g_popCount = popCount;
		if (musicOk) {
			// One voice, the whole song looping forever; RunFrame starts / stops (pauses) it.
			g_musicPcm = std::move(music);
			g_beat = EggAudio::BeatEnvelope(g_musicPcm.data(), g_musicPcm.size() / 2, 2, 44100, kBeatHop);
			g_bpm = EggAudio::EstimateBpm(g_beat, kBeatHop / 44100.0);
			WAVEFORMATEX fmt{};
			fmt.wFormatTag = WAVE_FORMAT_PCM; fmt.nChannels = 2; fmt.nSamplesPerSec = 44100;
			fmt.wBitsPerSample = 16; fmt.nBlockAlign = 4; fmt.nAvgBytesPerSec = 44100 * 4;
			if (SUCCEEDED(g_xa->CreateSourceVoice(&g_music, &fmt))) {
				XAUDIO2_BUFFER buf{};
				buf.pAudioData = reinterpret_cast<const BYTE*>(g_musicPcm.data());
				buf.AudioBytes = (UINT32)(g_musicPcm.size() * 2);
				buf.LoopCount = XAUDIO2_LOOP_INFINITE;
				if (FAILED(g_music->SubmitSourceBuffer(&buf))) { g_music->DestroyVoice(); g_music = nullptr; }
				else g_music->SetVolume(kMusicVolume);
			} else g_music = nullptr;
		}
	}
	char note[160];
	snprintf(note, sizeof(note), "%s: bang %s, %d/4 pops, music %s (~%.0f BPM), %u output channels", g_xa ? "ready" : "NO XAUDIO2",
		bangOk ? "ok" : "MISSING", popCount, g_music ? "ok" : "MISSING", g_bpm, g_masterChannels);
	g_loadNote = note;
	MvmDebugLog_LinefAlways("egg.uwu", "sounds %s", note);
	g_loaded.store(true);
}

// One clip at world position `src` (null: centred at full volume).
void PlayAt(Clip& c, const double* src, double ref, double maxDist, float volume) {
	if (c.pcm.empty() || !c.voices[0]) return;
	double cam[3] = { 0, 0, 0 }, ang[3] = { 0, 0, 0 }, fov = 90;
	CameraBridge_GetCurrentCamera(cam, ang, fov);
	EggAudio::Spatial s;
	if (src) s = EggAudio::Spatialize(cam, ang[1], src, ref, maxDist);
	else { s.gain = 1.0; s.left = s.right = 0.70710678; }
	if (s.gain < 0.01) { g_silent.fetch_add(1); return; } // too far to hear
	IXAudio2SourceVoice* v = c.voices[c.next];
	c.next = (c.next + 1) % kVoices;
	v->Stop(0);
	v->FlushSourceBuffers();
	XAUDIO2_BUFFER buf{};
	buf.pAudioData = reinterpret_cast<const BYTE*>(c.pcm.data());
	buf.AudioBytes = (UINT32)(c.pcm.size() * 2);
	buf.Flags = XAUDIO2_END_OF_STREAM;
	if (FAILED(v->SubmitSourceBuffer(&buf))) return;
	// Mono source -> the front left / right speakers (the first two outputs in every layout).
	float m[8] = {};
	m[0] = (float)s.left;
	if (g_masterChannels > 1) m[1] = (float)s.right; else m[0] = (float)s.gain;
	v->SetOutputMatrix(g_master, 1, std::min(g_masterChannels, 8u), m);
	v->SetVolume(volume);
	v->Start(0);
	g_played.fetch_add(1);
}

void RunClientCmd(const char* cmd) {
	if (g_pEngineToClient && cmd) g_pEngineToClient->ExecuteClientCmd(0, cmd, true);
}
std::string ReadBool(const char* name, const char* fallback) {
	using namespace SOURCESDK::CS2;
	if (!g_pCVar) return fallback;
	Cvar_s* h = g_pCVar->GetCvar(g_pCVar->FindConVar(name, false).Get());
	if (!h) return fallback;
	return h->m_Value.m_bValue ? "1" : "0";
}

} // namespace

void EasterEggUwu_RunFrame() {
	const bool on = EasterEgg_Active(EasterEgg::EGirl);
	if (on != g_wasOn) {
		g_wasOn = on;
		if (on) {
			StashWorld();
			// Blood off, remembering what it was; the game's gunshots muted.
			g_savedHBlood = ReadBool("violence_hblood", "1");
			g_savedABlood = ReadBool("violence_ablood", "1");
			RunClientCmd("violence_hblood 0");
			RunClientCmd("violence_ablood 0");
			ParticleFx_SetBloodSuppressed(true); // demo blood ignores the cvars: block the systems
			RunClientCmd("r_cleardecals");        // and wipe splats already on the floor (one-shot)
			EasterEggGunMute_Ensure();
			EasterEggGunMute_SetEnabled(true);
			CosmeticsRef().BeginTheme(); // female agents + pink skins, the user's own looks kept aside
			g_themeOn = true;
			g_dressed.clear();
			g_dressedCount = 0;
			g_themeDressQpc = 0.0;
			ForceVioletWorld();
		} else {
			RunClientCmd(("violence_hblood " + g_savedHBlood).c_str());
			RunClientCmd(("violence_ablood " + g_savedABlood).c_str());
			ParticleFx_SetBloodSuppressed(false);
			EasterEggGunMute_SetEnabled(false);
			if (g_themeOn) { CosmeticsRef().EndTheme(); g_themeOn = false; }
			RestoreWorld(); // only the three categories UwU owns; preserve effects and strength
			std::lock_guard<std::mutex> lock(g_mx);
			g_queue.clear();
		}
		MvmDebugLog_LinefAlways("egg.uwu", "%s: blood %s, game gunshots %s", on ? "ON" : "OFF",
			on ? "off" : "restored", on ? "muted" : "back");
	}
	// The background song: while UwU is on, except while osu! (its own music) or a render runs.
	{
		std::lock_guard<std::mutex> lock(g_mx);
		const bool want = on && g_music && !Osu_Active() && !RenderSessionDriver_IsActive();
		if (want != g_musicPlaying && g_music) {
			if (want) g_music->Start(0); else g_music->Stop(0); // Stop keeps the position: a pause
			g_musicPlaying = want;
		}
		// The beat at the song's exact playback position (the voice's own sample count), with a
		// quick decay so each kick reads as a pulse.
		const double now = OsuPlatform::QpcMs();
		const double dt = g_pulseQpcMs > 0.0 ? std::min(200.0, now - g_pulseQpcMs) : 0.0;
		g_pulseQpcMs = now;
		double pulse = g_pulse.load() * std::exp(-dt / 140.0);
		if (g_musicPlaying && g_music && !g_beat.empty() && !g_musicPcm.empty()) {
			XAUDIO2_VOICE_STATE st{};
			g_music->GetState(&st, 0);
			const unsigned long long frames = g_musicPcm.size() / 2;
			const size_t idx = (size_t)((st.SamplesPlayed % frames) / kBeatHop);
			if (idx < g_beat.size()) pulse = std::max(pulse, (double)g_beat[idx]);
		} else {
			pulse = 0.0;
		}
		g_pulse.store(pulse);
	}
	if (!on) return;
	// Dress everyone once a second: players join, bots spawn, a demo seek recreates controllers.
	if (g_themeOn) {
		const double now = OsuPlatform::QpcMs();
		if (now - g_themeDressQpc > 1000.0) { g_themeDressQpc = now; DressPlayers(); ForceVioletWorld(); }
	}
	if (!g_loaded.load() && !g_loading.load()) {
		g_loading.store(true);
		std::thread(Load).detach();
	}
	std::vector<Pending> pending;
	{
		std::lock_guard<std::mutex> lock(g_mx);
		pending.swap(g_queue);
		if (!g_loaded.load() || RenderSessionDriver_IsActive()) return;
		int n = 0;
		for (const Pending& p : pending) {
			if (++n > kMaxPerFrame) break;
			double pos[3];
			bool havePos = false;
			if (CEntityInstance* pawn = RagdollBodies::PawnOfUser(p.userid, nullptr)) {
				float f[3];
				if (RagdollBodies::SafeOrigin(pawn, f)) { pos[0] = f[0]; pos[1] = f[1]; pos[2] = f[2] + 50.0; havePos = true; }
			}
			if (p.death) PlayAt(g_bang, havePos ? pos : nullptr, kBangRef, kBangMax, kBangVolume);
			else if (g_popCount > 0) PlayAt(g_pops[g_rng() % g_popCount], havePos ? pos : nullptr, kShotRef, kShotMax, kPopVolume);
		}
	}
}

void EasterEggUwu_OnGameEvent(SOURCESDK::CS2::CGameEvent* event) {
	if (!event || !EasterEgg_Active(EasterEgg::EGirl)) return;
	const char* name = event->GetName();
	if (!name) return;
	const bool death = 0 == _stricmp(name, "player_death");
	if (!death && 0 != _stricmp(name, "weapon_fire")) return;
	JsonValue r;
	if (!RagdollBodies::EventJson(event, r)) return;
	const int userid = RagdollBodies::JInt(r, "userid");
	if (userid < 0) return;
	if (!death && !IsGunFire(RagdollBodies::JStr(r, "weapon"))) return;
	// The server and client-side paths both deliver each event: the demo tick + userid tells one
	// shot from two (two real shots or deaths never share a tick for one player).
	int tick = -1;
	g_MirvTime.GetCurrentDemoTick(tick);
	const unsigned long long sig = ((unsigned long long)(unsigned int)tick << 32) | (unsigned int)userid;
	std::lock_guard<std::mutex> lock(g_mx);
	unsigned long long& last = death ? g_lastDeath : g_lastFire;
	if (sig == last) return;
	last = sig;
	if (g_queue.size() < 64) g_queue.push_back({ death, userid });
}

double EasterEggUwu_MusicPulse() { return g_pulse.load(); }

void EasterEggUwu_GetUserTint(ModCategory category, int* r, int* g, int* b, bool* enabled) {
	const int i = (int)category;
	if (g_worldStashed && i >= 0 && i < 3) {
		const auto& tint = g_worldStash[i];
		*r = tint.r; *g = tint.g; *b = tint.b; *enabled = tint.enabled;
	} else WorldModulation_GetTint(category, r, g, b, enabled);
}

std::string EasterEggUwu_Status() {
	bool hooked = false;
	unsigned long long seen = 0, dropped = 0;
	EasterEggGunMute_Stats(hooked, seen, dropped);
	char buf[320];
	snprintf(buf, sizeof(buf), "uwu: sounds %s | played %llu, too far %llu | gunshot mute %s (%llu events seen, %llu muted) | blood blocked %llu, splats skipped %llu | dressed %d",
		g_loadNote.c_str(), g_played.load(), g_silent.load(), hooked ? "hooked" : "NOT hooked", seen, dropped, ParticleFx_BloodBlocked(), ParticleFx_BloodDecalsSkipped(), g_dressedCount);
	return buf;
}

} // namespace Filmmaker
