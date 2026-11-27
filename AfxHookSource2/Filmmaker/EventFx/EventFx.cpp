#define NOMINMAX
#include "EventFx.h"

#include "../Cosmetics/CosmeticEntityAccess.h"
#include "../Debug/MvmDebugLog.h"
#include "../Movie/CameraBridge.h"
#include "../Movie/CameraPath.h"
#include "../Movie/ParticleFxInternal.h"
#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"
#include "../Props/Props.h"
#include "../../ClientEntitySystem.h"
#include "../../MirvTime.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/igameevents.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/tier1/utlstring.h"
#include "../../../Shared/AfxConsole.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace Filmmaker {
namespace {

std::mutex g_mutex;
bool g_masterEnabled = false;
std::vector<EventFx::EventPreset> g_presets = EventFx::DefaultPresets();
int g_nextPresetId = EventFx::kFirstCustomPresetId;
int g_selectedPresetId = 1;
uint32_t g_seed = 1;

// A planned particle waiting for its start, or alive until its end. Main thread only.
struct LiveSpawn {
	EventFx::PlannedSpawn plan;
	bool wallClock = false; // Test: runs on the wall clock, so it plays while the demo is paused
	int particle = -1;
	bool started = false;
};
std::vector<LiveSpawn> g_spawns;
// Firings from the event hook, turned into spawns on the main thread.
struct Firing { EventFx::EventPreset preset; bool hasPos = false; float pos[3] = {0, 0, 0}; bool wallClock = false; };
std::vector<Firing> g_firings;

std::vector<EventFx::ActiveShake> g_shakes;
std::atomic<bool> g_hasShakes{ false };

// Deduplication for double server/client event delivery.
struct DedupStamp { int trigger = -1; int tick = -1; ULONGLONG wallMs = 0; };
std::vector<DedupStamp> g_recentTriggers;
int g_lastDemoTick = -1;
double g_lastDemoTime = -1.0;

// Systems loaded for the enabled presets on this level (the precache rule).
std::vector<std::string> g_warmed;
ULONGLONG g_levelSince = 0;

double DemoClock() {
	double t = 0.0;
	if (!g_MirvTime.GetCurrentDemoTime(t)) t = g_MirvTime.curtime_get();
	return std::isfinite(t) ? t : 0.0;
}
double WallClock() { return (double)GetTickCount64() * 0.001; }

EventFx::EventPreset* FindPresetLocked(int id) {
	for (auto& p : g_presets)
		if (p.id == id) return &p;
	return nullptr;
}

// ---- Where an event happened ----------------------------------------------------------------------
bool Finite3(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

// The victim of a player_death: their eye for a headshot, else their feet (a preset lifts it).
bool PlayerPosition(int userid, bool eye, float out[3]) {
	__try {
		auto controller = CosmeticEntFromIndex(userid + 1);
		if (!controller || !controller->IsPlayerController()) return false;
		const auto handle = controller->GetPlayerPawnHandle();
		auto pawn = handle.IsValid() ? CosmeticEntFromIndex(handle.GetEntryIndex()) : nullptr;
		if (!pawn || !pawn->IsPlayerPawn()) return false;
		if (eye) pawn->GetRenderEyeOrigin(out);
		else pawn->GetOrigin(out[0], out[1], out[2]);
		return Finite3(out) && (out[0] != 0.0f || out[1] != 0.0f);
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// bomb_exploded carries no position, and by the time it fires the planted bomb entity is often
// gone (seen in a demo, 2026-11-27). So bomb_planted remembers where the planter stood; the
// planted_c4 entity is still tried first.
bool g_bombKnown = false;
float g_bombPos[3] = {0, 0, 0};
bool BombPosition(float out[3]) {
	__try {
		const int highest = (std::min)(GetHighestEntityIndex(), 32768);
		for (int i = 0; i <= highest; ++i) {
			auto e = CosmeticEntFromIndex(i);
			const char* cls = e ? e->GetClassName() : nullptr;
			if (cls && !std::strcmp(cls, "planted_c4")) { e->GetOrigin(out[0], out[1], out[2]); return Finite3(out); }
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
	if (!g_bombKnown) return false;
	std::memcpy(out, g_bombPos, sizeof(g_bombPos));
	return true;
}

// A little ahead of the camera, where an effect with no place of its own shows.
void InFrontOfCamera(float out[3], float ahead) {
	double org[3] = {}, ang[3] = {}, fov = 90.0;
	CameraBridge_GetCurrentCamera(org, ang, fov);
	const double pitch = ang[0] * 3.14159265358979 / 180.0, yaw = ang[1] * 3.14159265358979 / 180.0;
	out[0] = (float)(org[0] + std::cos(pitch) * std::cos(yaw) * ahead);
	out[1] = (float)(org[1] + std::cos(pitch) * std::sin(yaw) * ahead);
	out[2] = (float)(org[2] - std::sin(pitch) * ahead - 24.0);
}

// ---- Firing --------------------------------------------------------------------------------------
void QueueFiringLocked(const EventFx::EventPreset& p, bool hasPos, const float* pos, bool wallClock) {
	if (g_firings.size() >= 32) return;
	Firing f; f.preset = p; f.hasPos = hasPos; f.wallClock = wallClock;
	if (hasPos) std::memcpy(f.pos, pos, sizeof(f.pos));
	g_firings.push_back(f);
}

void TriggerKindLocked(EventFx::TriggerKind kind, bool hasPos, const float* pos) {
	int tick = 0;
	g_MirvTime.GetCurrentDemoTick(tick);
	const ULONGLONG wall = GetTickCount64();
	for (const auto& r : g_recentTriggers)
		if (r.trigger == (int)kind && (r.tick == tick || (wall - r.wallMs) < 120)) return;
	if (g_recentTriggers.size() >= 32) g_recentTriggers.erase(g_recentTriggers.begin());
	g_recentTriggers.push_back({ (int)kind, tick, wall });

	// Effects placed in the map (Scene Objects) play on their event whether or not presets are on.
	Props_FireTrigger(EventFx::TriggerToken(kind));
	if (!g_masterEnabled) return;
	for (const auto& p : g_presets)
		if (p.enabled && p.trigger == kind) QueueFiringLocked(p, hasPos, pos, false);
}

// Main thread: a firing becomes its particle spawns, its light burst and its shake.
void StartFiring(const Firing& f) {
	const EventFx::EventPreset& p = f.preset;
	float at[3];
	if (p.where == EventFx::Where::AtEvent && f.hasPos) std::memcpy(at, f.pos, sizeof(at));
	else InFrontOfCamera(at, 220.0f);
	const double now = f.wallClock ? WallClock() : DemoClock();
	const uint32_t seed = ++g_seed;
	for (const auto& s : EventFx::PlanSpawns(p, at, now, seed)) {
		if (g_spawns.size() >= 64) break;
		LiveSpawn l; l.plan = s; l.wallClock = f.wallClock;
		g_spawns.push_back(l);
	}
	if (p.lightBurst)
		fx::NativeLightFx_OnDetonate(p.lightFlashStyle, (int)(seed & 0xFFFF), at[0], at[1], at[2] + p.lift + 16.0f);
	if (p.shake > 0.001f) {
		double org[3] = {}, ang[3] = {}, fov = 90.0;
		CameraBridge_GetCurrentCamera(org, ang, fov);
		const float dx = at[0] - (float)org[0], dy = at[1] - (float)org[1], dz = at[2] - (float)org[2];
		EventFx::ActiveShake s;
		s.start = now; s.seed = seed; s.useWallClock = f.wallClock;
		s.strength = p.shake * EventFx::ShakeFalloff(std::sqrt(dx * dx + dy * dy + dz * dz));
		s.hz = EventFx::ShakeHz(p.shake, p.shakeSpeed);
		s.seconds = EventFx::ShakeSeconds(p.shake, p.shakeLength);
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_shakes.size() >= 16) g_shakes.erase(g_shakes.begin());
		g_shakes.push_back(s);
		g_hasShakes.store(true, std::memory_order_release);
	}
	MvmDebugLog_LinefAlways("eventfx.fire", "preset=%d '%s' at %.1f %.1f %.1f (event pos %d) spawns=%zu",
		p.id, p.name.c_str(), at[0], at[1], at[2], f.hasPos ? 1 : 0, g_spawns.size());
}

void StopAllSpawns() {
	for (auto& s : g_spawns)
		if (s.particle >= 0) Props_DestroyEffect(s.particle);
	g_spawns.clear();
}

typedef bool (*SaveKV3AsJSON_t)(const SOURCESDK::CS2::KeyValues3* kv,
	SOURCESDK::CS2::CUtlString* error, SOURCESDK::CS2::CUtlString* output);

SaveKV3AsJSON_t GetSaveKV3AsJSON() {
	static bool s_tried = false;
	static SaveKV3AsJSON_t s_fn = nullptr;
	if (!s_tried) {
		s_tried = true;
		HMODULE tier0 = GetModuleHandleA("tier0.dll");
		if (tier0)
			s_fn = (SaveKV3AsJSON_t)GetProcAddress(tier0, "?SaveKV3AsJSON@@YA_NPEBVKeyValues3@@PEAVCUtlString@@1@Z");
	}
	return s_fn;
}

bool EventDataToJson(SOURCESDK::CS2::CGameEvent* event, JsonValue& out) {
	SaveKV3AsJSON_t fn = GetSaveKV3AsJSON();
	if (!fn) return false;
	SOURCESDK::CS2::CUtlString error;
	SOURCESDK::CS2::CUtlString text;
	if (!fn(event->GetDataKeys(), &error, &text) || !text.Get())
		return false;
	return JsonParse(text.Get(), out) && out.type == JsonValue::Type::Object;
}

const char* WhereToken(EventFx::Where w) { return w == EventFx::Where::FrontOfCamera ? "camera" : "event"; }

} // namespace

void EventFx_OnLevelInit() {
	std::lock_guard<std::mutex> lock(g_mutex);
	g_firings.clear();
	g_spawns.clear(); // the level took the particles with it
	g_shakes.clear();
	g_recentTriggers.clear();
	g_warmed.clear();
	g_bombKnown = false;
	g_levelSince = GetTickCount64();
	g_lastDemoTick = -1;
	g_lastDemoTime = -1.0;
	g_hasShakes.store(false, std::memory_order_release);
}

void EventFx_OnGameEvent(SOURCESDK::CS2::CGameEvent* event) {
	if (!event) return;
	const char* name = event->GetName();
	if (!name || !*name) return;
	const bool isDeath = !std::strcmp(name, "player_death");
	const bool isGrenade = !std::strcmp(name, "flashbang_detonate") || !std::strcmp(name, "hegrenade_detonate")
		|| !std::strcmp(name, "smokegrenade_detonate") || !std::strcmp(name, "inferno_startburn");
	const bool isBomb = !std::strcmp(name, "bomb_exploded");
	if (!std::strcmp(name, "bomb_planted")) {
		JsonValue planted;
		const JsonValue* v = EventDataToJson(event, planted) ? planted.Find("userid") : nullptr;
		float at[3];
		if (v && PlayerPosition(v->AsInt(-1), false, at)) {
			std::lock_guard<std::mutex> lock(g_mutex);
			std::memcpy(g_bombPos, at, sizeof(at));
			g_bombKnown = true;
		}
		return;
	}
	if (!std::strcmp(name, "round_start")) { std::lock_guard<std::mutex> lock(g_mutex); g_bombKnown = false; return; }
	if (!isDeath && !isGrenade && !isBomb) return;

	bool headshot = false, hasPos = false, hasHead = false;
	float pos[3] = {0, 0, 0}, head[3] = {0, 0, 0};
	JsonValue root;
	const bool data = (isDeath || isGrenade) && EventDataToJson(event, root);
	if (isDeath && data) {
		if (const JsonValue* v = root.Find("headshot")) headshot = v->AsBool(false) || v->AsInt(0) != 0;
		if (const JsonValue* v = root.Find("userid")) {
			const int userid = v->AsInt(-1);
			hasPos = userid >= 0 && PlayerPosition(userid, false, pos);
			hasHead = userid >= 0 && PlayerPosition(userid, true, head);
		}
	} else if (isGrenade && data) {
		const JsonValue* jx = root.Find("x");
		const JsonValue* jy = root.Find("y");
		const JsonValue* jz = root.Find("z");
		if (jx && jy && jz) {
			pos[0] = (float)jx->AsNumber(0.0); pos[1] = (float)jy->AsNumber(0.0); pos[2] = (float)jz->AsNumber(0.0);
			hasPos = Finite3(pos) && (pos[0] != 0.0f || pos[1] != 0.0f);
		}
	} else if (isBomb) {
		hasPos = BombPosition(pos);
	}

	std::lock_guard<std::mutex> lock(g_mutex);
	if (isDeath) {
		TriggerKindLocked(EventFx::TriggerKind::Kill, hasPos, pos);
		if (headshot) TriggerKindLocked(EventFx::TriggerKind::Headshot, hasHead || hasPos, hasHead ? head : pos);
	} else if (!std::strcmp(name, "flashbang_detonate")) {
		TriggerKindLocked(EventFx::TriggerKind::FlashbangPop, hasPos, pos);
	} else if (!std::strcmp(name, "hegrenade_detonate")) {
		TriggerKindLocked(EventFx::TriggerKind::HeGrenadePop, hasPos, pos);
	} else if (!std::strcmp(name, "smokegrenade_detonate")) {
		TriggerKindLocked(EventFx::TriggerKind::SmokePop, hasPos, pos);
	} else if (!std::strcmp(name, "inferno_startburn")) {
		TriggerKindLocked(EventFx::TriggerKind::MolotovPop, hasPos, pos);
	} else if (isBomb) {
		TriggerKindLocked(EventFx::TriggerKind::BombExplode, hasPos, pos);
	}
}

void EventFx_RunMainThreadFrame() {
	int tick = 0;
	const bool haveTick = g_MirvTime.GetCurrentDemoTick(tick);
	const double now = DemoClock(), wall = WallClock();

	std::vector<Firing> firings;
	std::vector<std::string> warm;
	bool rewound = false;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		rewound = g_lastDemoTime >= 0.0 && now < g_lastDemoTime - 0.15;
		if (rewound) {
			g_shakes.erase(std::remove_if(g_shakes.begin(), g_shakes.end(),
				[](const EventFx::ActiveShake& s) { return !s.useWallClock; }), g_shakes.end());
			g_recentTriggers.clear();
		}
		g_lastDemoTime = now;
		// A timeline marker crossed while the demo plays forward.
		if (haveTick && g_lastDemoTick >= 0 && tick > g_lastDemoTick && (tick - g_lastDemoTick) <= 64) {
			for (const auto& m : CameraPathRef().Markers()) {
				if (m.tick > g_lastDemoTick && m.tick <= tick) {
					TriggerKindLocked(EventFx::TriggerKind::TimelineMarker, false, nullptr);
					break;
				}
			}
		}
		if (haveTick) g_lastDemoTick = tick;
		firings.swap(g_firings);
		g_shakes.erase(std::remove_if(g_shakes.begin(), g_shakes.end(), [now, wall](const EventFx::ActiveShake& s) {
			const double age = (s.useWallClock ? wall : now) - s.start;
			return age < -0.5 || age > (double)(EventFx::kShakeAttack + s.seconds) + 0.05;
		}), g_shakes.end());
		g_hasShakes.store(!g_shakes.empty(), std::memory_order_release);
		// The precache rule: every system an enabled preset can spawn is loaded once the level has
		// settled, one a frame, so a blast in the middle of playback never stalls on a load.
		if (g_masterEnabled && GetTickCount64() - g_levelSince > 3000)
			for (const auto& s : EventFx::SystemsOf(g_presets))
				if (std::find(g_warmed.begin(), g_warmed.end(), s) == g_warmed.end()) { warm.push_back(s); break; }
		for (const auto& s : warm) g_warmed.push_back(s);
	}
	for (const auto& s : warm) {
		const bool ok = Props_PrecacheEffect(s.c_str());
		MvmDebugLog_LinefAlways("eventfx.precache", "%s -> %d", s.c_str(), ok ? 1 : 0);
	}

	// A rewind takes the demo-clock effects away with the moment they belonged to.
	if (rewound) {
		g_spawns.erase(std::remove_if(g_spawns.begin(), g_spawns.end(), [](LiveSpawn& s) {
			if (s.wallClock) return false;
			if (s.particle >= 0) Props_DestroyEffect(s.particle);
			return true;
		}), g_spawns.end());
	}
	for (const Firing& f : firings) StartFiring(f);
	g_spawns.erase(std::remove_if(g_spawns.begin(), g_spawns.end(), [now, wall](LiveSpawn& s) {
		const double clock = s.wallClock ? wall : now;
		if (!s.started && clock >= s.plan.start) {
			s.started = true;
			s.particle = Props_SpawnEffect(s.plan.system.c_str(), s.plan.pos);
			if (s.particle < 0) {
				MvmDebugLog_LinefAlways("eventfx.spawn", "refused %s", s.plan.system.c_str());
				return true;
			}
		}
		if (s.started && clock >= s.plan.start + s.plan.life) {
			Props_DestroyEffect(s.particle);
			return true;
		}
		return false;
	}), g_spawns.end());
}

bool EventFx_ApplyCameraImpulse(double curTime, float& pitch, float& yaw, float& roll, float& fov) {
	(void)fov;
	if (!g_hasShakes.load(std::memory_order_acquire)) return false;
	std::vector<EventFx::ActiveShake> copy;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		copy = g_shakes;
	}
	const auto ev = EventFx::EvaluateShakes(copy, curTime, WallClock());
	if (!ev.active) return false;
	pitch += ev.pitch;
	yaw += ev.yaw;
	roll += ev.roll;
	return true;
}

void EventFx_AppendStateJson(std::ostringstream& out) {
	std::lock_guard<std::mutex> lock(g_mutex);
	JsonBuilder j;
	j.BeginObject();
	j.BoolField("on", g_masterEnabled);
	j.IntField("selected", g_selectedPresetId);
	j.IntField("live", (int)g_spawns.size());
	j.Key("presets"); j.BeginArray();
	for (const auto& p : g_presets) {
		j.BeginObject();
		j.IntField("id", p.id);
		j.StringField("name", p.name);
		j.StringField("trigger", EventFx::TriggerToken(p.trigger));
		j.BoolField("enabled", p.enabled);
		j.Key("effects"); j.BeginArray();
		for (const auto& e : p.effects) j.ValueString(e);
		j.EndArray();
		j.StringField("where", WhereToken(p.where));
		j.DoubleField("lift", p.lift);
		j.IntField("copies", p.copies);
		j.DoubleField("spread", p.spread);
		j.DoubleField("delay", p.delay);
		j.DoubleField("life", p.life);
		j.BoolField("lightBurst", p.lightBurst);
		j.BoolField("lightFlash", p.lightFlashStyle);
		j.DoubleField("shake", p.shake);
		j.DoubleField("shakeSpeed", p.shakeSpeed);
		j.DoubleField("shakeLength", p.shakeLength);
		j.EndObject();
	}
	j.EndArray();
	j.EndObject();
	out << ",\"eventFx\":" << j.Str();
}

void EventFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 3) {
		advancedfx::Message(
			"%s eventfx on|off|toggle|reset\n"
			"%s eventfx test <id> | select <id> | enable <id> <0|1> | add | remove <id> | place <id>\n"
			"%s eventfx set <id> trigger|effect0|effect1|effect2|where|lift|copies|spread|delay|life|lightBurst|lightFlash|shake|shakeSpeed|shakeLength|name <value>\n"
			"%s eventfx effects  (the effects a preset can spawn)\n",
			cmd, cmd, cmd, cmd);
		return;
	}
	const char* sub = args->ArgV(2);
	if (0 == _stricmp(sub, "on")) { std::lock_guard<std::mutex> l(g_mutex); g_masterEnabled = true; return; }
	if (0 == _stricmp(sub, "off")) {
		std::lock_guard<std::mutex> l(g_mutex);
		g_masterEnabled = false; g_firings.clear(); g_shakes.clear(); g_hasShakes.store(false);
		StopAllSpawns();
		return;
	}
	if (0 == _stricmp(sub, "toggle")) { std::lock_guard<std::mutex> l(g_mutex); g_masterEnabled = !g_masterEnabled; return; }
	if (0 == _stricmp(sub, "reset")) {
		std::lock_guard<std::mutex> l(g_mutex);
		g_masterEnabled = false;
		g_presets = EventFx::DefaultPresets();
		g_nextPresetId = EventFx::kFirstCustomPresetId;
		g_selectedPresetId = 1;
		g_firings.clear(); g_shakes.clear(); g_hasShakes.store(false);
		StopAllSpawns();
		return;
	}
	if (0 == _stricmp(sub, "effects")) {
		for (const auto& e : EventFx::kEffects)
			advancedfx::Message("%-14s %-20s %s\n", e.key, e.label, e.systems[0]);
		return;
	}
	if (0 == _stricmp(sub, "select") && argc >= 4) {
		std::lock_guard<std::mutex> l(g_mutex);
		g_selectedPresetId = std::atoi(args->ArgV(3));
		return;
	}
	if (0 == _stricmp(sub, "test") && argc >= 4) {
		// On the wall clock, so it plays while the demo is paused; in front of the camera unless the
		// preset is placed there anyway.
		std::lock_guard<std::mutex> l(g_mutex);
		if (auto* p = FindPresetLocked(std::atoi(args->ArgV(3)))) {
			for (const auto& s : EventFx::SystemsOf({ *p })) Props_PrecacheEffect(s.c_str());
			QueueFiringLocked(*p, false, nullptr, true);
		}
		return;
	}
	if (0 == _stricmp(sub, "place") && argc >= 4) {
		// The preset's effects as Scene Objects at the crosshair, playing on the preset's event.
		EventFx::EventPreset copy;
		{
			std::lock_guard<std::mutex> l(g_mutex);
			auto* p = FindPresetLocked(std::atoi(args->ArgV(3)));
			if (!p) return;
			copy = *p;
		}
		int placed = 0;
		for (const auto& key : copy.effects)
			if (const auto* def = EventFx::FindEffect(key))
				for (const char* s : def->systems)
					if (s && Props_AddTriggeredEffect(s, EventFx::TriggerToken(copy.trigger),
						copy.life > 0 ? copy.life : def->life, copy.name.c_str(), copy.lift + def->lift)) ++placed;
		advancedfx::Message("%s eventfx: placed %d effect(s) of '%s' at the crosshair; they play on %s.\n",
			cmd, placed, copy.name.c_str(), EventFx::TriggerToken(copy.trigger));
		return;
	}
	if (0 == _stricmp(sub, "enable") && argc >= 5) {
		std::lock_guard<std::mutex> l(g_mutex);
		if (auto* p = FindPresetLocked(std::atoi(args->ArgV(3)))) p->enabled = std::atoi(args->ArgV(4)) != 0;
		return;
	}
	if (0 == _stricmp(sub, "add")) {
		std::lock_guard<std::mutex> l(g_mutex);
		EventFx::EventPreset p = EventFx::MakePreset(g_nextPresetId++, "", EventFx::TriggerKind::Kill, "blood", nullptr, nullptr);
		p.name = "Custom " + std::to_string(p.id);
		g_selectedPresetId = p.id;
		g_presets.push_back(p);
		return;
	}
	if (0 == _stricmp(sub, "remove") && argc >= 4) {
		const int id = std::atoi(args->ArgV(3));
		std::lock_guard<std::mutex> l(g_mutex);
		g_presets.erase(std::remove_if(g_presets.begin(), g_presets.end(),
			[id](const EventFx::EventPreset& p) { return p.id == id; }), g_presets.end());
		if (g_selectedPresetId == id) g_selectedPresetId = g_presets.empty() ? -1 : g_presets.front().id;
		return;
	}
	if (0 == _stricmp(sub, "set") && argc >= 6) {
		const int id = std::atoi(args->ArgV(3));
		const char* f = args->ArgV(4);
		const char* val = args->ArgV(5);
		const float fv = (float)std::atof(val);
		using EventFx::Clamp;
		std::lock_guard<std::mutex> l(g_mutex);
		auto* p = FindPresetLocked(id);
		if (!p) return;
		if (0 == _stricmp(f, "trigger")) EventFx::TriggerFromToken(val, p->trigger);
		else if (0 == _strnicmp(f, "effect", 6) && f[6] >= '0' && f[6] < '0' + EventFx::kPresetEffects && !f[7]) {
			const std::string key = std::strcmp(val, "-") ? val : "";
			if (key.empty() || EventFx::FindEffect(key)) p->effects[f[6] - '0'] = key;
			else advancedfx::Warning("%s eventfx: unknown effect '%s' (see `eventfx effects`).\n", cmd, val);
		}
		else if (0 == _stricmp(f, "where")) p->where = !_stricmp(val, "camera") ? EventFx::Where::FrontOfCamera : EventFx::Where::AtEvent;
		else if (0 == _stricmp(f, "lift")) p->lift = Clamp(fv, -200.0f, 400.0f);
		else if (0 == _stricmp(f, "copies")) p->copies = (std::max)(1, (std::min)(EventFx::kMaxCopies, std::atoi(val)));
		else if (0 == _stricmp(f, "spread")) p->spread = Clamp(fv, 0.0f, 600.0f);
		else if (0 == _stricmp(f, "delay")) p->delay = Clamp(fv, 0.0f, 10.0f);
		else if (0 == _stricmp(f, "life")) p->life = Clamp(fv, 0.0f, 60.0f);
		else if (0 == _stricmp(f, "lightBurst")) p->lightBurst = fv > 0.5f;
		else if (0 == _stricmp(f, "lightFlash")) p->lightFlashStyle = fv > 0.5f;
		else if (0 == _stricmp(f, "shake")) p->shake = Clamp(fv, 0.0f, 1.0f);
		else if (0 == _stricmp(f, "shakeSpeed")) p->shakeSpeed = fv <= 0.0f ? 0.0f : Clamp(fv, 1.0f, 30.0f);
		else if (0 == _stricmp(f, "shakeLength")) p->shakeLength = fv <= 0.0f ? 0.0f : Clamp(fv, 0.1f, 5.0f);
		else if (0 == _stricmp(f, "name")) { std::string n = val; for (int i = 6; i < argc; ++i) { n += ' '; n += args->ArgV(i); } p->name = n.substr(0, 40); }
		else { advancedfx::Warning("%s eventfx: unknown field '%s'.\n", cmd, f); return; }
		g_warmed.clear(); // a new effect is loaded before it can fire
		return;
	}
	advancedfx::Warning("%s eventfx: unknown subcommand '%s'.\n", cmd, sub);
}

} // namespace Filmmaker
