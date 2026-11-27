#pragma once

// Pure, engine-free model for Event FX: CS2's own particle effects (explosions, fire, blood,
// sparks, fireworks) spawned in the world when a demo event happens. A preset says which event
// fires it (a kill, a headshot, a grenade going off, the bomb, a timeline marker), which effects
// play, and where: at the event itself (the grenade's detonation point, the victim, the bomb) or
// in front of the camera. EventFx.cpp does the engine side; PlanSpawns here turns one firing into
// the list of particle systems to create, where and for how long.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace Filmmaker::EventFx {

inline constexpr float kPi = 3.14159265358979323846f;

enum class TriggerKind : int {
	Kill = 0,
	Headshot = 1,
	FlashbangPop = 2,
	HeGrenadePop = 3,
	SmokePop = 4,
	MolotovPop = 5,
	BombExplode = 6,
	TimelineMarker = 7,
	Count
};

inline const char* TriggerToken(TriggerKind k) {
	switch (k) {
	case TriggerKind::Kill: return "kill";
	case TriggerKind::Headshot: return "headshot";
	case TriggerKind::FlashbangPop: return "flashbang";
	case TriggerKind::HeGrenadePop: return "hegrenade";
	case TriggerKind::SmokePop: return "smoke";
	case TriggerKind::MolotovPop: return "molotov";
	case TriggerKind::BombExplode: return "bomb";
	case TriggerKind::TimelineMarker: return "marker";
	default: return "kill";
	}
}

inline bool TriggerFromToken(const char* s, TriggerKind& out) {
	if (!s) return false;
	for (int i = 0; i < (int)TriggerKind::Count; ++i)
		if (0 == _stricmp(s, TriggerToken((TriggerKind)i))) { out = (TriggerKind)i; return true; }
	return false;
}

// ---- The effect catalog -------------------------------------------------------------------------
// Every system below is a real CS2 particle system (checked against pak01_dir.vpk, and each seen playing in CS2 when spawned this way, 2026-11-27).
// `life` is how long the effect is kept before it is removed (looping fire would burn forever);
// `lift` raises it off the point it is placed at.
struct EffectDef {
	const char* key;
	const char* label;
	const char* category;
	const char* systems[2];
	float life;
	float lift;
};

inline const EffectDef kEffects[] = {
	{"he",            "HE grenade blast",   "explosions", {"particles/explosions_fx/explosion_hegrenade.vpcf", nullptr}, 5, 0},
	{"he_dirt",       "HE dirt blast",      "explosions", {"particles/explosions_fx/explosion_hegrenade_dirt.vpcf", nullptr}, 5, 0},
	{"he_snow",       "HE snow blast",      "explosions", {"particles/explosions_fx/explosion_hegrenade_snow.vpcf", nullptr}, 5, 0},
	{"he_indoor",     "HE indoor blast",    "explosions", {"particles/explosions_fx/explosion_hegrenade_interior.vpcf", nullptr}, 5, 0},
	{"c4",            "Bomb blast",         "explosions", {"particles/explosions_fx/explosion_c4_500.vpcf", nullptr}, 8, 0},
	{"c4_indoor",     "Bomb indoor blast",  "explosions", {"particles/explosions_fx/explosion_c4_interior.vpcf", nullptr}, 8, 0},
	{"c4_short",      "Bomb short blast",   "explosions", {"particles/explosions_fx/explosion_c4_short.vpcf", nullptr}, 6, 0},
	{"heavy_charge",  "Heavy charge",       "explosions", {"particles/explosions_fx/explosion_coop_mission_c4.vpcf", nullptr}, 8, 0},
	{"basic",         "Small explosion",    "explosions", {"particles/explosions_fx/explosion_basic.vpcf", nullptr}, 4, 0},
	{"water",         "Water blast",        "explosions", {"particles/explosions_fx/explosion_basic_water.vpcf", nullptr}, 5, 0},
	{"molotov",       "Molotov burst",      "fire",       {"particles/inferno_fx/molotov_explosion.vpcf", nullptr}, 4, 0},
	{"molotov_air",   "Molotov air burst",  "fire",       {"particles/inferno_fx/explosion_molotov_air.vpcf", nullptr}, 4, 0},
	{"incendiary",    "Incendiary burst",   "fire",       {"particles/inferno_fx/incendiary_explosion.vpcf", nullptr}, 4, 0},
	{"falling_fire",  "Falling fire",       "fire",       {"particles/inferno_fx/explosion_molotov_fallingfire.vpcf", nullptr}, 4, 0},
	{"fire",          "Fire",               "fire",       {"particles/burning_fx/env_fire_medium.vpcf", nullptr}, 6, 0},
	{"fire_large",    "Large fire",         "fire",       {"particles/burning_fx/env_fire_large.vpcf", nullptr}, 6, 0},
	{"blood",         "Blood burst",        "blood",      {"particles/blood_impact/blood_impact_heavy.vpcf", nullptr}, 3, 0},
	{"blood_head",    "Headshot blood",     "blood",      {"particles/blood_impact/blood_impact_headshot.vpcf", nullptr}, 3, 0},
	{"blood_mist",    "Blood mist",         "blood",      {"particles/blood_impact/blood_impact_red_01_mist.vpcf", nullptr}, 3, 0},
	{"blood_goop",    "Blood goop",         "blood",      {"particles/blood_impact/blood_impact_goop_heavy.vpcf", nullptr}, 3, 0},
	{"flash",         "Flashbang burst",    "sparks",     {"particles/explosions_fx/explosion_flashbang.vpcf", nullptr}, 3, 0},
	{"smoke_puff",    "Smoke puff",         "smoke",      {"particles/explosions_fx/explosion_smoke_disperse.vpcf", nullptr}, 6, 0},
	{"debris",        "Debris",             "smoke",      {"particles/explosions_fx/explosion_c4_debris.vpcf", nullptr}, 5, 0},
	{"ground_smoke",  "Lingering smoke",    "smoke",      {"particles/explosions_fx/explosion_c4_ground_residual_smoke.vpcf", nullptr}, 8, 0},
	{"fireworks",     "Fireworks",          "party",      {"particles/inferno_fx/firework_crate_explosion_01.vpcf", nullptr}, 6, 0},
	{"fireworks_big", "Big fireworks",      "party",      {"particles/inferno_fx/firework_crate_explosion_02.vpcf", nullptr}, 6, 0},
	{"spark_shower",  "Spark shower",       "party",      {"particles/inferno_fx/firework_crate_shower_01b.vpcf", nullptr}, 6, 0},
};
inline constexpr int kEffectCount = (int)(sizeof(kEffects) / sizeof(kEffects[0]));

inline const EffectDef* FindEffect(const std::string& key) {
	for (const EffectDef& e : kEffects)
		if (key == e.key) return &e;
	return nullptr;
}

// ---- Presets ------------------------------------------------------------------------------------
enum class Where : int {
	AtEvent = 0,         // the grenade's detonation point, the victim, the bomb (the camera when there is none)
	FrontOfCamera = 1,   // in view, a little ahead of the camera
	Count
};

inline constexpr int kPresetEffects = 3;
inline constexpr int kMaxCopies = 6;
inline constexpr int kMaxSpawnsPerFire = 24;

struct EventPreset {
	int id = 0;
	std::string name;
	TriggerKind trigger = TriggerKind::HeGrenadePop;
	bool enabled = true;
	std::string effects[kPresetEffects];  // catalog keys, "" = none
	Where where = Where::AtEvent;
	float lift = 0.0f;     // units above the point
	int copies = 1;        // 1..kMaxCopies copies, scattered within `spread`
	float spread = 0.0f;   // units
	float delay = 0.0f;    // seconds after the event
	float life = 0.0f;     // seconds kept; 0 = each effect's own default
	bool lightBurst = false;
	bool lightFlashStyle = false; // true = white flash light, false = warm blast light
	float shake = 0.0f;    // 0..1 camera shake, weaker with distance from the camera
};

inline float Clamp(float v, float lo, float hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

// One particle system to create.
struct PlannedSpawn {
	std::string system;
	float pos[3] = { 0, 0, 0 };
	double start = 0.0;  // clock time to create it
	double life = 0.0;   // seconds until it is removed
};

// Copy `i` of `copies` sits on a ring of radius `spread` (copy 0 at the centre), turned by a
// per-firing seed so two firings do not stack exactly; each later copy also starts a little later.
inline void CopyOffset(int i, int copies, float spread, uint32_t seed, float& dx, float& dy) {
	dx = dy = 0.0f;
	if (i <= 0 || copies <= 1 || spread <= 0.0f) return;
	const float turn = (float)(seed % 360u) * kPi / 180.0f;
	const float a = turn + 2.0f * kPi * (float)(i - 1) / (float)(copies - 1);
	const float r = spread * (0.55f + 0.45f * (float)((seed >> 3) % 7u) / 6.0f);
	dx = std::cos(a) * r;
	dy = std::sin(a) * r;
}

inline std::vector<PlannedSpawn> PlanSpawns(const EventPreset& p, const float at[3], double now, uint32_t seed) {
	std::vector<PlannedSpawn> out;
	const int copies = (std::max)(1, (std::min)(p.copies, kMaxCopies));
	for (int c = 0; c < copies; ++c) {
		float dx = 0, dy = 0;
		CopyOffset(c, copies, p.spread, seed, dx, dy);
		for (int e = 0; e < kPresetEffects; ++e) {
			const EffectDef* def = FindEffect(p.effects[e]);
			if (!def) continue;
			for (const char* system : def->systems) {
				if (!system || (int)out.size() >= kMaxSpawnsPerFire) continue;
				PlannedSpawn s;
				s.system = system;
				s.pos[0] = at[0] + dx;
				s.pos[1] = at[1] + dy;
				s.pos[2] = at[2] + p.lift + def->lift;
				s.start = now + (double)Clamp(p.delay, 0.0f, 10.0f) + 0.08 * c;
				s.life = p.life > 0.0f ? (double)Clamp(p.life, 0.2f, 60.0f) : (double)def->life;
				out.push_back(s);
			}
		}
	}
	return out;
}

// Every system a set of presets can spawn, once each (what to precache).
inline std::vector<std::string> SystemsOf(const std::vector<EventPreset>& presets) {
	std::vector<std::string> out;
	for (const EventPreset& p : presets) {
		if (!p.enabled) continue;
		for (const std::string& key : p.effects)
			if (const EffectDef* def = FindEffect(key))
				for (const char* s : def->systems)
					if (s && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
	}
	return out;
}

// ---- Camera shake -------------------------------------------------------------------------------
// A quick rise and a quadratic fall, in [0, 1].
inline float EvaluateEnvelope(double ageSec, float attackSec, float decaySec) {
	if (ageSec < 0.0) return 0.0f;
	const float atk = (std::max)(attackSec, 0.005f);
	const float dec = (std::max)(decaySec, 0.02f);
	if ((float)ageSec >= atk + dec) return 0.0f;
	if ((float)ageSec < atk) {
		const float t = (float)ageSec / atk;
		return t * t * (3.0f - 2.0f * t);
	}
	const float inv = 1.0f - Clamp(((float)ageSec - atk) / dec, 0.0f, 1.0f);
	return inv * inv;
}

inline constexpr float kShakeAttack = 0.02f, kShakeDecay = 0.7f;

// How much of a preset's shake reaches a camera `distance` units from the blast: all of it up
// close, fading to a fifth by 2000 units. A blast with no position shakes fully.
inline float ShakeFalloff(float distance) {
	if (!(distance >= 0.0f)) return 1.0f;
	return Clamp(1.0f - distance / 2500.0f, 0.2f, 1.0f);
}

struct ActiveShake {
	double start = 0.0;
	float strength = 0.0f; // 0..1, falloff applied
	uint32_t seed = 1;
	bool useWallClock = false;
};

struct ShakeFrame {
	bool active = false;
	float pitch = 0.0f, yaw = 0.0f, roll = 0.0f;
};

inline ShakeFrame EvaluateShakes(const std::vector<ActiveShake>& shakes, double demoTime, double wallTime) {
	ShakeFrame out;
	for (const ActiveShake& s : shakes) {
		const double age = (s.useWallClock ? wallTime : demoTime) - s.start;
		const float env = EvaluateEnvelope(age, kShakeAttack, kShakeDecay) * Clamp(s.strength, 0.0f, 1.0f);
		if (env <= 1e-4f) continue;
		out.active = true;
		const float phase = (float)age * 14.0f * 2.0f * kPi;
		const float k = (float)(s.seed % 97u) * 0.37f;
		out.pitch += std::sin(phase + k) * 3.0f * env;
		out.yaw += std::cos(phase * 0.83f + k * 1.7f) * 2.2f * env;
		out.roll += std::sin(phase * 1.19f + k * 2.3f) * 2.0f * env;
	}
	return out;
}

// ---- Built-in presets ---------------------------------------------------------------------------
inline EventPreset MakePreset(int id, const char* name, TriggerKind trigger, const char* a, const char* b, const char* c) {
	EventPreset p;
	p.id = id; p.name = name; p.trigger = trigger;
	p.effects[0] = a ? a : ""; p.effects[1] = b ? b : ""; p.effects[2] = c ? c : "";
	return p;
}

inline std::vector<EventPreset> DefaultPresets() {
	std::vector<EventPreset> out;
	EventPreset he = MakePreset(1, "HE: bigger blast", TriggerKind::HeGrenadePop, "he", "he_dirt", "debris");
	he.lightBurst = true; he.shake = 0.25f;
	out.push_back(he);
	EventPreset bomb = MakePreset(2, "Bomb: huge blast", TriggerKind::BombExplode, "c4", "heavy_charge", "debris");
	bomb.lightBurst = true; bomb.shake = 0.5f; bomb.copies = 3; bomb.spread = 140.0f;
	out.push_back(bomb);
	EventPreset molly = MakePreset(3, "Molotov: fireball", TriggerKind::MolotovPop, "molotov", "molotov_air", "falling_fire");
	molly.lightBurst = true;
	out.push_back(molly);
	EventPreset kill = MakePreset(4, "Kill: blood burst", TriggerKind::Kill, "blood", "blood_mist", nullptr);
	kill.lift = 40.0f;
	out.push_back(kill);
	EventPreset head = MakePreset(5, "Headshot: blood spray", TriggerKind::Headshot, "blood_head", "blood_goop", nullptr);
	out.push_back(head);
	EventPreset flash = MakePreset(6, "Flash: spark burst", TriggerKind::FlashbangPop, "flash", "spark_shower", nullptr);
	flash.lightBurst = true; flash.lightFlashStyle = true;
	out.push_back(flash);
	EventPreset smoke = MakePreset(7, "Smoke: debris puff", TriggerKind::SmokePop, "smoke_puff", "debris", nullptr);
	smoke.enabled = false;
	out.push_back(smoke);
	EventPreset marker = MakePreset(8, "Marker: fireworks", TriggerKind::TimelineMarker, "fireworks", "fireworks_big", "spark_shower");
	marker.where = Where::FrontOfCamera; marker.enabled = false; marker.copies = 2; marker.spread = 120.0f;
	out.push_back(marker);
	return out;
}
inline constexpr int kFirstCustomPresetId = 9;

} // namespace Filmmaker::EventFx
