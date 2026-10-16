#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Props.h"
#include "PropsMath.h"
#include "PropsEffectProfiles.h"
#include "ClientEntitySpawn.h"
#include "PropPreview.h"
#include "PropEditMath.h"
#include "../Environment/EnvironmentScene.h"
#include "../Environment/EnvironmentAnimatedCasters.h"
#include "SceneUndoClock.h"
#include "../Environment/EnvironmentSurfaceMath.h"
#include "../Movie/CameraBridge.h"
#include "../Movie/MovieMode.h"
#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"
#include "../Debug/MvmDebugLog.h"
#include "../../ClientEntitySystem.h"
#include "../../hlaeFolder.h"
#include "../../../Shared/AfxConsole.h"

#include <Windows.h>

bool AfxStreams_IsRcording(); // RenderSystemDX11Hooks: a recording is running

// CS2's game particle manager, resolved by MirvColors (RagdollWeapon and native rain use it too):
// what an EFFECT is placed with. destroyParticle is optional (null when its signature is missing).
struct ParticleData;
extern unsigned char* (__fastcall *g_Original_getParticleManager)();
extern int* (__fastcall *g_Original_createParticle)(unsigned char*, int*, const char*, int,
	int64_t, int64_t, int64_t, int64_t);
extern void (__fastcall *g_Original_updateParticle)(unsigned char*, int, int, ParticleData*, int);
extern void (__fastcall *g_Original_destroyParticle)(unsigned char*, int, bool, bool);

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Filmmaker {

namespace {

using Props::Vec3;

struct Placed {
	int id = 0;
	std::string model;
	Vec3 position, angles;
	float scale = 1;
	bool visible = true;
	// Scene Objects outliner: its group ("" = none), whether it is kept out of renders (still
	// shown live), and whether it is locked against picking and dragging in the view.
	std::string group;
	bool render = true, locked = false;
	unsigned char* entity = nullptr; // live client entity, or null until (re)spawned
	int entityIndex = -1;
	int particle = -1;                // an EFFECT's particle-manager index instead (no entity)
	bool failed = false;              // the engine refused it; do not retry every frame
	bool held = false;                // loaded but not spawned: the last spawn on this map never survived
};

// Live in the world: a prop's entity, or an effect's particle.
bool Live(const Placed& p) { return p.entity || p.particle >= 0; }
// Forgets the live handles of a copy (undo snapshots, duplicates, a level change).
void ClearLive(Placed& p) { p.entity = nullptr; p.entityIndex = -1; p.particle = -1; p.failed = false; }

std::vector<Placed> g_props;
std::string g_map;          // the map g_props belongs to
int g_nextId = 1, g_selected = 0;
int g_hover = 0; // what the cursor is over in the view: a prop id, minus an Environment object id, or 0
unsigned g_revision = 0;
std::string g_message;
ULONGLONG g_mapSince = 0;   // when g_map became current: spawning waits for the level to settle
ULONGLONG g_lastCheck = 0;
// Crash-loop guard. A placed object that takes the game down is saved with the map, so every
// later load of that demo spawned it again and crashed or hung again: the demo could not be opened
// at all (user report 2026-10-13). <map>.json.spawning exists from a prop spawn until the game has
// survived kSpawnSettleMs past the last one; finding it on the next load means the last spawn never
// survived, and the saved props stay on disk WITHOUT being spawned until "props restore".
constexpr ULONGLONG kSpawnSettleMs = 15000;
bool g_spawnArmed = false;    // the .spawning marker is on disk
ULONGLONG g_lastSpawn = 0;
unsigned g_framesSinceSpawn = 0; // main-thread frames since the last spawn: a crawling game has not settled
bool g_restoreHeld = false;   // this map's saved props were not spawned (previous spawn never survived)

// ---- undo ---------------------------------------------------------------------------------------
// Ctrl+Z in Config takes back the last change to the scene's objects: what a placement, paste,
// duplicate or one paint stroke ADDED is removed, what a removal took away is put back, and a
// move, turn, size or inspector edit puts the object back as it was. Steps are stamped from the
// SceneUndoClock so the newest change wins against the Environment's lights and meshes. Per map:
// loading another map's scene clears it.
struct UndoStep {
	std::vector<int> added;
	std::vector<Placed> removed; // as they were, entity pointers cleared
	std::vector<Placed> changed; // still there: their state before the change
	uint64_t stamp = 0;
	std::string merge;           // edits with the same key in quick succession (a slider) are one step
	ULONGLONG at = 0;
};
std::vector<UndoStep> g_undo;
const size_t kMaxUndo = 64;

void PushUndo(UndoStep step) {
	if (step.added.empty() && step.removed.empty() && step.changed.empty()) return;
	for (Placed& p : step.removed) ClearLive(p);
	for (Placed& p : step.changed) ClearLive(p);
	step.stamp = NextSceneUndoStamp();
	step.at = GetTickCount64();
	if (g_undo.size() >= kMaxUndo) g_undo.erase(g_undo.begin());
	g_undo.push_back(std::move(step));
}
// `before` is the prop as it was. A repeat of the same `merge` key within 600 ms (a slider being
// dragged, a value being typed) keeps the first step's before-state.
void PushChanged(const Placed& before, std::string merge = {}) {
	if (!merge.empty() && !g_undo.empty() && g_undo.back().merge == merge && GetTickCount64() - g_undo.back().at < 600) {
		g_undo.back().stamp = NextSceneUndoStamp();
		g_undo.back().at = GetTickCount64();
		return;
	}
	UndoStep s;
	s.changed.push_back(before);
	s.merge = std::move(merge);
	PushUndo(std::move(s));
}
bool SamePose(const Placed& a, const Placed& b) {
	auto same = [](const Vec3& x, const Vec3& y) { return std::abs(x.x - y.x) < 1e-3f && std::abs(x.y - y.y) < 1e-3f && std::abs(x.z - y.z) < 1e-3f; };
	return same(a.position, b.position) && same(a.angles, b.angles) && std::abs(a.scale - b.scale) < 1e-4f;
}
void PushAdded(int id) { UndoStep s; s.added.push_back(id); PushUndo(std::move(s)); }

// ---- catalog ------------------------------------------------------------------------------------
// The searchable list: the current map's own models first (a workshop map ships its custom models
// inside its package, and they can only be placed while that map is loaded), then the game's.
std::vector<std::string> g_catalog;
std::vector<char> g_fromMap;           // parallel to g_catalog: 1 = from the current map's package
std::vector<std::string> g_gameModels; // pak01_dir.vpk, read once
std::vector<std::string> g_mapModels;  // the current workshop map's package, per map
bool g_catalogLoaded = false;
std::string g_query;
Props::Category g_category = Props::Category::All;
// How many results the panel shows: it starts with 24 and asks for 20 more as its list is
// scrolled near the bottom (a growing list, no pages).
int g_limit = 24;
const int kMaxLimit = 4000;
// The panel state is rebuilt every frame; the search over ~13k paths runs only when its inputs change.
Props::SearchResult g_found;
std::string g_foundKey = "";

std::filesystem::path GameCsgoDir() {
	// client.dll lives in game/csgo/bin/win64; the VPK index is game/csgo/pak01_dir.vpk.
	wchar_t path[MAX_PATH] = {};
	HMODULE client = GetModuleHandleW(L"client.dll");
	if (!client || !GetModuleFileNameW(client, path, MAX_PATH)) return {};
	return std::filesystem::path(path).parent_path().parent_path().parent_path();
}

// CS2's effects as audited by Automation/tools/audit-particle-effects.py (PropsEffectProfiles.h),
// by lower-case path; null for one it did not see (a workshop map's own).
const Props::EffectProfile* EffectProfileFor(const std::string& effect) {
	static const std::unordered_map<std::string, const Props::EffectProfile*> byPath = [] {
		std::unordered_map<std::string, const Props::EffectProfile*> m;
		for (const auto& p : Props::kEffectProfiles) m.emplace(p.path, &p);
		return m;
	}();
	const auto it = byPath.find(Props::Lower(effect));
	return it == byPath.end() ? nullptr : it->second;
}

// A VPK's header and directory tree (pak01_dir.vpk, or a single-file workshop package).
bool ReadVpkTree(const std::filesystem::path& vpk, std::vector<uint8_t>& data) {
	std::ifstream in(vpk, std::ios::binary);
	if (!in) return false;
	data.assign(28, 0);
	in.read(reinterpret_cast<char*>(data.data()), 28);
	if (in.gcount() != 28) return false;
	uint32_t signature = 0, treeSize = 0;
	std::memcpy(&signature, data.data(), 4);
	std::memcpy(&treeSize, data.data() + 8, 4);
	if (signature != 0x55aa1234u || treeSize > 512u * 1024 * 1024) return false;
	data.resize(28 + size_t(treeSize));
	in.read(reinterpret_cast<char*>(data.data() + 28), treeSize);
	data.resize(28 + size_t(in.gcount()));
	return true;
}

// The placeable models listed in a VPK tree (appended).
bool TreeModels(const std::vector<uint8_t>& data, std::vector<std::string>& out) {
	std::vector<std::string> all;
	if (!Props::ParseVpkModelList(data.data(), data.size(), all)) return false;
	for (auto& m : all)
		if (Props::IsPlaceable(m)) out.push_back(std::move(m));
	// World effects (fire, embers, water streams, drips, steam). Of CS2's own, only the ones that
	// play on their own at one point (EffectProfileFor): child-only parts, one-shot bursts and
	// effects that need a model or a parent are hidden. A workshop map's own effects are unaudited
	// and listed.
	std::vector<std::string> effects;
	Props::ParseVpkEffectList(data.data(), data.size(), effects);
	for (auto& e : effects) {
		if (!Props::IsPlaceableEffect(e)) continue;
		const Props::EffectProfile* profile = EffectProfileFor(e);
		if (profile && !profile->playable) continue;
		out.push_back(std::move(e));
	}
	return true;
}

// The current map's own package when it is a workshop map, with its tree. Workshop packages are
// named by item ID (steamapps/workshop/content/730/<id>/<id>.vpk or <id>_dir.vpk), so the one
// carrying the map (nested as maps/<map>.vpk) is the map's. Official maps keep their props in pak01
// and have nothing extra here.
std::filesystem::path WorkshopMapVpk(const std::string& map, std::vector<uint8_t>& data) {
	std::string name = map;
	const size_t slash = name.find_last_of("/\\");
	if (slash != std::string::npos) name = name.substr(slash + 1);
	if (name.empty()) return {};
	std::error_code official;
	if (std::filesystem::exists(GameCsgoDir() / "maps" / (name + ".vpk"), official)) return {};
	// game/csgo -> game -> Counter-Strike Global Offensive -> common -> steamapps
	const auto steamapps = GameCsgoDir().parent_path().parent_path().parent_path().parent_path();
	std::error_code ec;
	for (const auto& item : std::filesystem::directory_iterator(steamapps / "workshop" / "content" / "730", ec)) {
		std::error_code ec2;
		for (const auto& file : std::filesystem::directory_iterator(item.path(), ec2)) {
			if (file.path().extension() != ".vpk") continue;
			if (ReadVpkTree(file.path(), data) && Props::VpkHasMap(data.data(), data.size(), name)) return file.path();
		}
	}
	return {};
}

void RebuildCatalog() {
	g_catalog.clear();
	g_fromMap.clear();
	g_catalog.reserve(g_mapModels.size() + g_gameModels.size());
	for (const auto& m : g_mapModels) { g_catalog.push_back(m); g_fromMap.push_back(1); }
	for (const auto& m : g_gameModels) { g_catalog.push_back(m); g_fromMap.push_back(0); }
	g_foundKey.clear();
}

void LoadCatalog() {
	if (g_catalogLoaded) return;
	g_catalogLoaded = true;
	const auto vpk = GameCsgoDir() / "pak01_dir.vpk";
	std::vector<uint8_t> tree;
	if (!ReadVpkTree(vpk, tree) || !TreeModels(tree, g_gameModels)) g_message = "The game's model index could not be read";
	RebuildCatalog();
	MvmDebugLog_LinefAlways("props.catalog", "vpk=%s placeable=%zu map=%zu",
		vpk.string().c_str(), g_gameModels.size(), g_mapModels.size());
}

// A new map: its own models (if it is a workshop map) join the front of the catalog.
void LoadMapModels(const std::string& map) {
	g_mapModels.clear();
	std::vector<uint8_t> tree;
	const auto vpk = map.empty() ? std::filesystem::path() : WorkshopMapVpk(map, tree);
	if (!vpk.empty()) {
		TreeModels(tree, g_mapModels);
		MvmDebugLog_LinefAlways("props.catalog", "workshop map %s vpk=%s models=%zu",
			map.c_str(), vpk.string().c_str(), g_mapModels.size());
	}
	if (g_catalogLoaded) RebuildCatalog();
}

// ---- persistence (per map) ----------------------------------------------------------------------
std::filesystem::path StorePath(const std::string& map) {
	return std::filesystem::path(GetHlaeRoamingAppDataFolderW()) / L"source_mvm" / L"props" / (map + ".json");
}

void Save() {
	if (g_map.empty()) return;
	JsonBuilder j;
	j.BeginObject();
	j.IntField("v", 1);
	j.StringField("map", g_map);
	j.Key("props"); j.BeginArray();
	for (const Placed& p : g_props) {
		j.BeginObject();
		j.IntField("id", p.id);
		j.StringField("model", p.model);
		j.Key("pos"); j.BeginArray(); j.ValueDouble(p.position.x); j.ValueDouble(p.position.y); j.ValueDouble(p.position.z); j.EndArray();
		j.Key("ang"); j.BeginArray(); j.ValueDouble(p.angles.x); j.ValueDouble(p.angles.y); j.ValueDouble(p.angles.z); j.EndArray();
		j.DoubleField("scale", p.scale);
		j.BoolField("visible", p.visible);
		j.StringField("group", p.group);
		j.BoolField("render", p.render);
		j.BoolField("locked", p.locked);
		j.EndObject();
	}
	j.EndArray();
	j.EndObject();
	std::error_code ec;
	const auto path = StorePath(g_map);
	std::filesystem::create_directories(path.parent_path(), ec);
	const auto temp = path.wstring() + L".tmp";
	{ std::ofstream out(temp, std::ios::binary | std::ios::trunc); out << j.Str(); }
	std::filesystem::rename(temp, path, ec);
}

std::filesystem::path SpawnMarkerPath(const std::string& map) {
	std::filesystem::path p = StorePath(map);
	p += L".spawning";
	return p;
}

// Before a placed prop is spawned: the marker goes to disk once, then each spawn only renews the
// settle clock. Written before the spawn so a crash inside it still leaves the marker behind.
void ArmSpawnMarker() {
	g_lastSpawn = GetTickCount64();
	g_framesSinceSpawn = 0;
	if (g_spawnArmed || g_map.empty()) return;
	std::error_code ec;
	const auto path = SpawnMarkerPath(g_map);
	std::filesystem::create_directories(path.parent_path(), ec);
	{ std::ofstream out(path, std::ios::binary | std::ios::trunc); out << "spawning\n"; }
	g_spawnArmed = true;
}

void DisarmSpawnMarker(const std::string& map) {
	std::error_code ec;
	if (!map.empty()) std::filesystem::remove(SpawnMarkerPath(map), ec);
	g_spawnArmed = false;
}

Vec3 ReadVec(const JsonValue* v) {
	Vec3 out{};
	if (v && v->type == JsonValue::Type::Array && v->arr.size() == 3)
		out = {float(v->arr[0].AsNumber()), float(v->arr[1].AsNumber()), float(v->arr[2].AsNumber())};
	return out;
}

void Load(const std::string& map) {
	g_props.clear();
	g_undo.clear();
	g_selected = 0;
	g_nextId = 1;
	std::ifstream in(StorePath(map), std::ios::binary);
	if (!in) return;
	const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	JsonValue root;
	if (!JsonParse(text, root)) return;
	const JsonValue* list = root.Find("props");
	if (!list || list->type != JsonValue::Type::Array) return;
	for (const JsonValue& v : list->arr) {
		Placed p;
		p.id = v.Find("id") ? v.Find("id")->AsInt() : g_nextId;
		p.model = v.Find("model") ? v.Find("model")->AsString() : "";
		p.position = ReadVec(v.Find("pos"));
		p.angles = ReadVec(v.Find("ang"));
		p.scale = v.Find("scale") ? float(v.Find("scale")->AsNumber(1)) : 1.0f;
		p.visible = !v.Find("visible") || v.Find("visible")->AsBool(true);
		p.group = v.Find("group") ? v.Find("group")->AsString() : "";
		p.render = !v.Find("render") || v.Find("render")->AsBool(true);
		p.locked = v.Find("locked") && v.Find("locked")->AsBool(false);
		if (p.model.empty()) continue;
		g_nextId = std::max(g_nextId, p.id + 1);
		g_props.push_back(p);
	}
}

// ---- live entities ------------------------------------------------------------------------------
// Client-only entities live ABOVE the networked range (index 16384+; a spawned prop measured 16956),
// so GetHighestEntityIndex -- the networked high-water mark -- must not bound the lookup.
unsigned char* EntityAt(int index) {
	if (index < 0 || index >= 0x8000 || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) return nullptr;
	__try { return static_cast<unsigned char*>(g_GetEntityFromIndex(*g_pEntityList, index)); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool Alive(const Placed& p) {
	if (p.particle >= 0) return true; // an effect: the manager keeps it until we destroy it
	return p.entity && EntityAt(p.entityIndex) == p.entity;
}

// EFFECTS go through CS2's game particle manager, the way RagdollWeapon and native rain place
// theirs. They were info_particle_system entities first, and those broke after an Environment
// reset or a delete (user report, 2026-10-09): removing the entity released the effect, and the
// next one spawned by name came up empty. The manager's create resolves the system by name with a
// blocking load every time, so a placement always plays; destroy ends exactly that one effect.
const int kEffectControlPoints = 18;

// `values`/`count`: the points the effect reads as numbers (see below). Plain arrays: SEH below.
int CreateEffectAt(const char* name, const Vec3& position, const Props::EffectValue* values, size_t count) {
	if (!g_Original_getParticleManager || !g_Original_createParticle || !g_Original_updateParticle) return -1;
	int index = -1;
	__try {
		if (unsigned char* manager = g_Original_getParticleManager()) {
			g_Original_createParticle(manager, &index, name, 8, 0, 0, 0, 0);
			if (index >= 0) {
				// EVERY control point at the effect, not just CP0. Effects author their layout as
				// drivers on CP0..CP18 (env_fire_medium uses all 19); a CP left unset stays at the
				// WORLD ORIGIN, so effects drew stretched toward the map's centre or off-screen --
				// "not centered", and "some work, some don't" (2026-10-09). A map's
				// info_particle_system puts its unset points on itself the same way.
				float at[3] = {position.x, position.y, position.z};
				for (int cp = 0; cp <= kEffectControlPoints; ++cp)
					g_Original_updateParticle(manager, index, cp, reinterpret_cast<ParticleData*>(at), 0);
				// ...except the points the effect reads as NUMBERS (its audited profile): there the
				// world position was a random setting -- molotov_fire01 burns for 10 x CP3.x, so it
				// burned for hours on one side of a map and never on the other.
				for (size_t i = 0; i < count; ++i) {
					float value[3] = {values[i].value[0], values[i].value[1], values[i].value[2]};
					g_Original_updateParticle(manager, index, values[i].cp, reinterpret_cast<ParticleData*>(value), 0);
				}
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) { index = -1; }
	return index;
}

int CreateEffect(const char* name, const Vec3& position) {
	const Props::EffectProfile* profile = EffectProfileFor(name);
	const auto values = Props::ParseEffectValues(profile ? profile->values : nullptr);
	return CreateEffectAt(name, position, values.data(), values.size());
}

void DestroyEffect(int index) {
	if (index < 0 || !g_Original_getParticleManager || !g_Original_destroyParticle) return;
	__try {
		if (unsigned char* manager = g_Original_getParticleManager())
			g_Original_destroyParticle(manager, index, true, false);
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void Despawn(Placed& p) {
	if (p.particle >= 0) DestroyEffect(p.particle);
	else if (Alive(p)) Props::RemoveClientEntity(p.entity);
	p.entity = nullptr;
	p.entityIndex = -1;
	p.particle = -1;
}

// A recording is running (RenderSystemDX11Hooks). Props kept out of renders are taken out of the
// world for its length.
bool g_recording = false;

// Whether a prop should be in the world right now.
bool Shown(const Placed& p) { return p.visible && !(g_recording && !p.render); }

// Wind for foliage: CS2's foliage shader sways with the level's env_wind, and most maps have none,
// so placed trees and grass stand still. One client-side wind entity covers the whole level.
float g_windStrength = 0;              // 0 = off; otherwise the gusts' top speed
unsigned char* g_wind = nullptr;
int g_windIndex = -1;

void DespawnWind() {
	if (g_wind && EntityAt(g_windIndex) == g_wind) Props::RemoveClientEntity(g_wind);
	g_wind = nullptr;
	g_windIndex = -1;
}

void SpawnWind() {
	if (!(g_windStrength > 0)) return;
	char minWind[16], maxWind[16], minGust[16], maxGust[16];
	std::snprintf(minWind, sizeof(minWind), "%d", int(g_windStrength * 0.4f));
	std::snprintf(maxWind, sizeof(maxWind), "%d", int(g_windStrength * 0.7f));
	std::snprintf(minGust, sizeof(minGust), "%d", int(g_windStrength * 0.8f));
	std::snprintf(maxGust, sizeof(maxGust), "%d", int(g_windStrength));
	const char* const keys[] = {"minwind", minWind, "maxwind", maxWind, "mingust", minGust, "maxgust", maxGust,
		"mingustdelay", "2", "maxgustdelay", "6", "gustduration", "3", "gustdirchange", "25", "windradius", "-1", nullptr};
	g_wind = Props::SpawnClientEntity("env_wind_clientside", "mvm_wind", "", {0, 0, 0}, {0, 30, 0}, 1, keys);
	g_windIndex = Props::ClientEntityIndex(g_wind);
	MvmDebugLog_LinefAlways("props.wind", "strength=%.0f entity=%p index=%d", g_windStrength, g_wind, g_windIndex);
}

void Spawn(Placed& p) {
	if (!Shown(p) || p.failed || p.held) return;
	ArmSpawnMarker();
	if (Props::IsEffect(p.model)) {
		// Loaded first (the precache rule), so the first placement of a new effect never hitches.
		Props::PrecacheModel(p.model.c_str());
		p.particle = CreateEffect(p.model.c_str(), p.position);
		MvmDebugLog_LinefAlways("props.effect", "create %s at %.1f %.1f %.1f -> index %d",
			p.model.c_str(), p.position.x, p.position.y, p.position.z, p.particle);
	} else {
		p.entity = Props::SpawnClientEntity("prop_dynamic", "mvm_prop", p.model.c_str(), p.position, p.angles, p.scale);
		p.entityIndex = Props::ClientEntityIndex(p.entity);
	}
	if (!Live(p)) {
		p.failed = true;
		g_message = "The engine refused " + Props::DisplayName(p.model);
	}
}

// A changed prop is replaced, not edited in place: removing and spawning again is the one path the
// engine fully supports for a client entity, and it costs one spawn per edit.
void Respawn(Placed& p) {
	LARGE_INTEGER f, t0, t1;
	QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
	Despawn(p); p.failed = false; Spawn(p); ++g_revision;
	QueryPerformanceCounter(&t1);
	if (MvmDebugLog_Active()) {
		static ULONGLONG s_last = 0;
		const ULONGLONG now = GetTickCount64();
		if (now - s_last >= 250) {
			s_last = now;
			MvmDebugLog_Linef("props.respawn", "id=%d %s %.2f ms", p.id, p.model.c_str(), double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart));
		}
	}
}
// Puts a prop at its current pose. A spawned prop cannot simply be moved: writing its scene node's
// pose moves the node but not the model drawn from it (tested 2026-10-10: the box followed the
// cursor, the model stayed where it spawned -- its mesh hangs off child nodes with their own baked
// world transforms). So it is re-created at the new pose.
void ApplyPose(Placed& p) { Respawn(p); }

Placed* Find(int id) {
	for (Placed& p : g_props) if (p.id == id) return &p;
	return nullptr;
}

std::string CurrentMap() { return Environment::CurrentMapName(); }

// The surface under the crosshair, or a point ahead of the camera when the map's collision is not
// ready. The prop faces the camera.
void AimPose(Vec3& position, Vec3& angles) {
	double origin[3] = {}, view[3] = {}, fov = 90;
	CameraBridge_GetCurrentCamera(origin, view, fov);
	const double pitch = view[0] * 3.14159265358979 / 180.0, yaw = view[1] * 3.14159265358979 / 180.0;
	const Vec3 eye{float(origin[0]), float(origin[1]), float(origin[2])};
	const Vec3 forward{float(std::cos(pitch) * std::cos(yaw)), float(std::cos(pitch) * std::sin(yaw)), float(-std::sin(pitch))};
	position = eye + forward * 240.0f;
	if (auto world = Environment::CollisionGeometrySnapshot()) {
		Environment::SurfaceHit hit;
		if (Environment::SurfaceRaycast(*world, eye, forward, 8192, hit)) position = hit.position;
	}
	angles = {0.0f, float(view[1] + 180.0), 0.0f};
}

// ---- in-world editor ----------------------------------------------------------------------------
// Placing: a copy of the model follows the surface under the cursor until a click drops it.
// Editing: the selected prop carries move arrows and three turn rings (turn, tilt, roll); dragging
// one moves it live.
// Viewport coordinates (u, v) are normalized over the rendered frame, as the Config overlay sends them.
Placed g_ghost;
bool g_placing = false, g_ghostDirty = false;
float g_ghostFloorYaw = 0; // the placed copy's turn on a floor (a wall turns it to face the room)
std::atomic<bool> g_editorBusy{false}; // placing or dragging: Esc belongs to the editor
// Axis: an arrow. Turn: a ring. Surface: the prop itself, dragged along whatever is under the cursor
// (near the bottom of the view is close, toward the horizon is far), as placing does.
enum class Grab { None, Axis, Turn, Surface };
Vec3 g_surfaceOffset; // where the prop sat relative to the surface point first clicked (floor only)
Grab g_grab = Grab::None;
Environment::GizmoDrag g_drag;
Props::Ring g_ring = Props::Ring::Turn; // the ring being dragged
Vec3 g_ringAxis{0, 0, 1};               // its axis, fixed for the drag
float g_turnAnchor = 0;                 // the ring angle under the cursor when the drag began
Placed g_before;     // the grabbed prop as it was when the drag began (Esc puts it back)
bool g_grabDirty = false;
ULONGLONG g_lastLive = 0;
// A drag respawns the prop at most this often: smooth enough to follow, without a spawn per frame.
const ULONGLONG kLiveRespawnMs = 0; // instant: the pose follows the cursor every frame it moves
const int kRingPoints = 32;

// Painting: a brush (a circle on the ground under the cursor) sprays copies while the mouse is
// held. A brush holds one model or several (grass, ferns and rocks mixed); each copy picks one of
// them at random. The copies are ordinary props filed under one group (the "folder"), so the
// outliner hides, locks or removes a whole painted patch at once.
struct Brush {
	bool active = false;
	std::vector<std::string> models;
	std::string group;
	float radius = 160, spacing = 48, size = 1, jitter = 0.25f, turn = 180;
	float density = 1; // 0.05..1: how full a stroke may make the circle (PropEditMath.h PaintBudget)
	bool hasCenter = false, erasing = false;
	Vec3 center;
	uint32_t seed = 1;
	std::unordered_map<std::string, float> minsZ; // each model's own bottom, read from its first copy
	ULONGLONG lastStroke = 0;
	bool dirty = false; // painted since the last save
	// The copies this press (mouse down to up) has placed: one undo step. Spacing and density count
	// every copy in the brush's group, so holding still or painting over a patch again fills its
	// gaps up to the density and never piles copies on top of each other.
	std::vector<int> stroke;
	std::vector<Placed> erased; // what this press erased, for its undo step
} g_brush;
const ULONGLONG kStrokeMs = 45;   // one scatter per this long while the mouse is held
const int kStrokeBatch = 6;       // copies per scatter: enough to fill quickly, few enough not to hitch
const size_t kMaxProps = 2000;    // client entities are not free; a level full of grass stops here

void SyncBusy() { g_editorBusy = g_placing || g_grab != Grab::None || g_brush.active; }

// The surface point under viewport (u, v), or a point ahead along the ray when nothing is hit.
// `normal` (optional) is the surface's, turned toward the camera; straight up when the ray hits
// nothing and the point floats in front of the camera.
bool SurfaceUnder(float u, float v, Vec3& out, Vec3* normal = nullptr) {
	Environment::Matrix vp;
	Vec3 eye, dir;
	if (!Environment::RenderedCamera(vp, eye) || !Props::CursorRay(vp, eye, u, v, dir)) return false;
	out = eye + dir * 240.0f;
	if (normal) *normal = {0, 0, 1};
	if (auto world = Environment::CollisionGeometrySnapshot()) {
		Environment::SurfaceHit hit;
		if (Environment::SurfaceRaycast(*world, eye, dir, 8192, hit)) {
			out = hit.position;
			if (normal) *normal = Environment::Dot(hit.normal, dir) > 0 ? hit.normal * -1.0f : hit.normal;
		}
	}
	return true;
}

// The floor under `at`: the map's collision straight down from a little above it.
bool FloorZ(Vec3 at, float& z) {
	auto world = Environment::CollisionGeometrySnapshot();
	Environment::SurfaceHit hit;
	if (!world || !Environment::SurfaceRaycast(*world, {at.x, at.y, at.z + Props::kFloorProbeLift}, {0, 0, -1}, 8192, hit))
		return false;
	z = hit.position.z;
	return true;
}

// Model-space bounds of a live object. An effect has no model and no collision, so it gets a
// handle box standing on its origin, big enough to click and to carry the gizmo.
bool PropBounds(const Placed& p, Vec3& mins, Vec3& maxs) {
	if (!Live(p)) return false;
	if (Props::IsEffect(p.model)) { mins = {-16, -16, 0}; maxs = {16, 16, 48}; return true; }
	return Props::ClientEntityBounds(p.entity, mins, maxs);
}

// Model-space bottom of a live prop (0 when unreadable: most props have their origin at the base).
float MinsZ(const Placed& p) {
	Vec3 mins, maxs;
	return PropBounds(p, mins, maxs) ? mins.z : 0.0f;
}

// Rests the prop's lowest point on the floor under it.
bool SnapToFloor(Placed& p) {
	float z = 0;
	if (!FloorZ(p.position, z)) return false;
	p.position.z = Props::RestingZ(z, MinsZ(p), p.scale);
	return true;
}

// Re-creates a prop at its current pose while it is dragged or follows the cursor: on every move
// (kLiveRespawnMs 0), so the model is never behind the cursor. Respawn logs its own cost
// (props.respawn) so a slow model shows up in mvm_debug.
bool LiveUpdate(Placed& p) {
	const ULONGLONG now = GetTickCount64();
	if (now - g_lastLive < kLiveRespawnMs) return false;
	g_lastLive = now;
	Respawn(p);
	return true;
}

void StopPlacing() {
	if (g_placing) Despawn(g_ghost);
	g_placing = g_ghostDirty = false;
	SyncBusy();
}

void CancelGrab() {
	if (g_grab == Grab::None) return;
	if (Placed* p = Find(g_before.id)) {
		p->position = g_before.position;
		p->angles = g_before.angles;
		ApplyPose(*p);
	}
	g_grab = Grab::None;
	g_grabDirty = false;
	g_drag.Cancel();
	SyncBusy();
}

// Placing is a free-cam job: fly CS2's free cam to the spot (hold a mouse button to look), click to drop.
void EnterFreeCam() {
	if (MovieModeRef().GetMode() != MovieMode::Mode::FreeCam) MovieModeRef().SetMode(MovieMode::Mode::FreeCam);
}

// The pose a gizmo is drawn for: the prop being placed, else the selected live prop.
const Placed* GizmoTarget() {
	if (g_brush.active) return nullptr;
	if (g_placing) return Live(g_ghost) ? &g_ghost : nullptr;
	const Placed* p = Find(g_selected);
	return p && Alive(*p) ? p : nullptr;
}

std::vector<Vec3> StrokeNearBrush();
int BrushBudget();
// "palm tree 01", or "3 models" for a mixed brush.
std::string BrushName() {
	if (g_brush.models.empty()) return "";
	if (g_brush.models.size() == 1) return Props::DisplayName(g_brush.models[0]);
	return std::to_string(g_brush.models.size()) + " models";
}
bool InBrushGroup(const Placed& q) { return !g_brush.group.empty() && q.group == g_brush.group; }

// The brush outline on the ground, projected for the Config overlay.
void AppendBrush(JsonBuilder& j) {
	j.Key("paint"); j.BeginObject();
	j.BoolField("active", g_brush.active);
	j.Key("models"); j.BeginArray();
	for (const std::string& m : g_brush.models) j.ValueString(m);
	j.EndArray();
	j.StringField("name", BrushName());
	j.StringField("group", g_brush.group);
	j.DoubleField("radius", g_brush.radius);
	j.DoubleField("spacing", g_brush.spacing);
	j.DoubleField("size", g_brush.size);
	j.DoubleField("jitter", g_brush.jitter);
	j.DoubleField("turn", g_brush.turn);
	j.DoubleField("density", g_brush.density);
	j.BoolField("erasing", g_brush.erasing);
	int count = 0;
	for (const Placed& q : g_props) if (!g_brush.group.empty() && q.group == g_brush.group) ++count;
	j.IntField("count", count);
	Environment::Matrix vp;
	Vec3 eye;
	if (g_brush.active && g_brush.hasCenter && Environment::RenderedCamera(vp, eye)) {
		j.Key("ring"); j.BeginArray();
		for (int i = 0; i < kRingPoints; ++i) {
			const auto q = Environment::GizmoProject(vp, Props::AxisRingPoint(g_brush.center, {0, 0, 1}, g_brush.radius, i, kRingPoints));
			j.BeginArray(); j.ValueDouble(q.x); j.ValueDouble(q.y); j.ValueBool(q.visible); j.EndArray();
		}
		j.EndArray();
		// Where the next copies will land: the same scatter the stroke runs (a copy of the seed),
		// each on the ground under it.
		if (!g_brush.erasing) {
			uint32_t seed = g_brush.seed;
			j.Key("dots"); j.BeginArray();
			for (const Vec3& at : Props::Scatter(g_brush.center, g_brush.radius, g_brush.spacing, BrushBudget(), seed, StrokeNearBrush(), Props::kPaintScatterAttempts)) {
				float z = at.z;
				FloorZ({at.x, at.y, g_brush.center.z + Props::PaintProbeLift(g_brush.radius)}, z);
				const auto q = Environment::GizmoProject(vp, {at.x, at.y, z});
				j.BeginArray(); j.ValueDouble(q.x); j.ValueDouble(q.y); j.ValueBool(q.visible); j.EndArray();
			}
			j.EndArray();
		}
	}
	j.EndObject();
}

// The brush group's copies near the brush: what the next scatter keeps its spacing from.
std::vector<Vec3> StrokeNearBrush() {
	std::vector<Vec3> existing;
	const float reach = g_brush.radius + g_brush.spacing;
	for (const Placed& q : g_props) {
		if (!InBrushGroup(q)) continue;
		const float dx = q.position.x - g_brush.center.x, dy = q.position.y - g_brush.center.y;
		if (dx * dx + dy * dy <= reach * reach) existing.push_back(q.position);
	}
	return existing;
}

// How many copies the next scatter may add: the density's share of the circle, less the brush
// group's copies already inside it, at most one batch.
int BrushBudget() {
	int inside = 0;
	const float r2 = g_brush.radius * g_brush.radius;
	for (const Placed& q : g_props) {
		if (!InBrushGroup(q)) continue;
		const float dx = q.position.x - g_brush.center.x, dy = q.position.y - g_brush.center.y;
		if (dx * dx + dy * dy <= r2) ++inside;
	}
	return Props::PaintBudget(g_brush.radius, g_brush.spacing, g_brush.density, inside, kStrokeBatch);
}

// One press of the brush (mouse down to up) is one undo step: what it painted and what it erased.
void EndBrushPress() {
	UndoStep step;
	step.added = g_brush.stroke;
	step.removed = std::move(g_brush.erased);
	g_brush.erased.clear();
	g_brush.stroke.clear();
	PushUndo(std::move(step));
}

// One scatter under the brush: new copies in the gaps of what this stroke already placed there.
// The points are exactly the ones the brush outline previews (same seed, same spacing, same budget).
void PaintStroke() {
	if (!g_brush.active || !g_brush.hasCenter || g_brush.models.empty()) return;
	if (g_props.size() >= kMaxProps) { g_message = "That is the most props a scene can hold"; return; }
	const auto points = Props::Scatter(g_brush.center, g_brush.radius, g_brush.spacing, BrushBudget(), g_brush.seed, StrokeNearBrush(), Props::kPaintScatterAttempts);
	for (const Vec3& at : points) {
		if (g_props.size() >= kMaxProps) break;
		float z = 0;
		// The ground under each copy: probed from a little above the brush's centre height, so a
		// stroke across a kerb or a slope follows it.
		if (!FloorZ({at.x, at.y, g_brush.center.z + Props::PaintProbeLift(g_brush.radius)}, z)) continue;
		Placed q;
		q.id = g_nextId++;
		q.model = g_brush.models[Props::PaintPick(int(g_brush.models.size()), g_brush.seed)];
		q.group = g_brush.group;
		q.scale = Props::PaintScale(g_brush.size, g_brush.jitter, g_brush.seed);
		q.angles = {0, Props::PaintYaw(g_brush.turn, g_brush.seed), 0};
		q.position = {at.x, at.y, z};
		// Each model's own bottom, read once from its first copy, rests every copy on the ground.
		// Once known, a copy spawns straight at its resting height (one spawn, not two). An effect
		// has no model bounds and plays from where it stands.
		auto mz = g_brush.minsZ.find(q.model);
		const bool effect = Props::IsEffect(q.model);
		if (!effect && mz != g_brush.minsZ.end()) q.position.z = Props::RestingZ(z, mz->second, q.scale);
		Spawn(q);
		if (!Live(q)) continue;
		if (!effect && mz == g_brush.minsZ.end()) {
			mz = g_brush.minsZ.emplace(q.model, MinsZ(q)).first;
			if (std::abs(mz->second) > 0.01f) { q.position.z = Props::RestingZ(z, mz->second, q.scale); Respawn(q); }
		}
		g_props.push_back(q);
		g_brush.stroke.push_back(q.id);
		g_brush.dirty = true;
	}
	++g_revision;
}

// Removes this group's copies under the brush.
void PaintErase() {
	if (!g_brush.active || !g_brush.hasCenter) return;
	const float r2 = g_brush.radius * g_brush.radius;
	size_t before = g_props.size();
	for (Placed& q : g_props) {
		if (q.group != g_brush.group) continue;
		const float dx = q.position.x - g_brush.center.x, dy = q.position.y - g_brush.center.y;
		if (dx * dx + dy * dy <= r2) { g_brush.erased.push_back(q); Despawn(q); q.id = -q.id; }
	}
	g_props.erase(std::remove_if(g_props.begin(), g_props.end(), [](const Placed& q) { return q.id < 0; }), g_props.end());
	if (g_props.size() != before) { g_brush.dirty = true; ++g_revision; }
}

void StopPainting() {
	if (!g_brush.active) return;
	g_brush.active = false;
	g_brush.hasCenter = false;
	EndBrushPress();
	if (g_brush.dirty) { Save(); g_brush.dirty = false; }
	SyncBusy();
}

void AppendGizmo(JsonBuilder& j) {
	const Placed* p = GizmoTarget();
	Environment::Matrix vp;
	Vec3 eye;
	if (!p || !Environment::RenderedCamera(vp, eye)) return;
	auto point = [&](Vec3 w) {
		const auto q = Environment::GizmoProject(vp, w);
		j.BeginArray(); j.ValueDouble(q.x); j.ValueDouble(q.y); j.ValueBool(q.visible); j.EndArray();
	};
	const float size = Environment::GizmoWorldSize(vp, p->position);
	j.Key("gizmo"); j.BeginObject();
	j.Key("origin"); point(p->position);
	j.Key("axes"); j.BeginArray();
	for (int i = 0; i < 3; ++i) point(p->position + Environment::GizmoAxis(i) * size);
	j.EndArray();
	// turn, tilt, roll
	j.Key("rings"); j.BeginArray();
	for (int r = 0; r < 3; ++r) {
		const Vec3 axis = Props::RingAxis(p->angles, Props::Ring(r));
		j.BeginArray();
		for (int i = 0; i < kRingPoints; ++i) point(Props::AxisRingPoint(p->position, axis, size * 0.8f, i, kRingPoints));
		j.EndArray();
	}
	j.EndArray();
	Vec3 mins, maxs;
	if (PropBounds(*p, mins, maxs)) {
		Vec3 corners[8];
		Props::BoxCorners(p->position, p->angles, p->scale, mins, maxs, corners);
		j.Key("bounds"); j.BeginArray();
		for (const Vec3& c : corners) point(c);
		j.EndArray();
	}
	j.EndObject();
}

// A marker's reach on screen (fraction of the frame, either side of its centre): the same size
// however far the light or mesh is, so a distant one is as easy to click as a near one. Matches
// the 26 px icon the view draws, with a little slack.
constexpr float kMarkerReach = 0.02f;

// What a click at (u, v) picks: a CS2 asset (its id), an Environment mesh or light (minus its id),
// or nothing (0). The nearest box under the cursor wins; a light's dot is drawn over everything, so
// a cursor on a dot always picks that light.
int PickAt(float u, float v) {
	Environment::Matrix vp;
	Vec3 eye;
	if (!Environment::RenderedCamera(vp, eye)) return 0;
	std::vector<Props::PickCandidate> candidates;
	for (const Placed& q : g_props) {
		Vec3 mins, maxs, corners[8];
		if (q.locked || !Alive(q) || !PropBounds(q, mins, maxs)) continue;
		Props::BoxCorners(q.position, q.angles, q.scale, mins, maxs, corners);
		const Vec3 d = q.position - eye;
		candidates.push_back({q.id, Props::ProjectRect(vp, corners, 8), std::sqrt(Environment::Dot(d, d))});
	}
	int markers = 0;
	for (const auto& box : Environment::ScenePickBoxes()) {
		// Every light and mesh carries an icon drawn over everything: a cursor on it picks that
		// object, ahead of whatever stands in front of it.
		const auto c = Environment::GizmoProject(vp, box.center);
		if (c.visible) {
			Props::ScreenRect r;
			r.x0 = c.x - kMarkerReach; r.x1 = c.x + kMarkerReach;
			r.y0 = c.y - kMarkerReach; r.y1 = c.y + kMarkerReach;
			r.valid = true;
			candidates.push_back({-box.id, r, 0.f});
			++markers;
		}
		if (box.light) continue;
		// A mesh is also picked by its own box, like a CS2 asset.
		const Vec3 d = box.center - eye;
		candidates.push_back({-box.id, Props::ProjectRect(vp, box.corners, 8), std::sqrt(Environment::Dot(d, d))});
	}
	const int picked = Props::Pick(candidates, u, v);
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("props.pick", "at %.4f %.4f: %zu candidates (%d markers) -> %d", u, v, candidates.size(), markers, picked);
	return picked;
}

// Over the view: every light's dot (its type and colour), and the outline and name of whatever
// the cursor is over, so a click is never a guess.
void AppendMarkers(JsonBuilder& j) {
	Environment::Matrix vp;
	Vec3 eye;
	if (!Environment::RenderedCamera(vp, eye)) return;
	auto point = [&](Vec3 w) {
		const auto q = Environment::GizmoProject(vp, w);
		j.BeginArray(); j.ValueDouble(q.x); j.ValueDouble(q.y); j.ValueBool(q.visible); j.EndArray();
	};
	const auto boxes = Environment::ScenePickBoxes();
	j.Key("markers"); j.BeginArray();
	for (const auto& box : boxes) {
		j.BeginObject();
		j.IntField("id", box.id);
		j.StringField("kind", box.kind);
		j.StringField("name", box.name);
		j.BoolField("selected", box.selected);
		char color[16];
		snprintf(color, sizeof(color), "#%02x%02x%02x",
			int(std::clamp(box.color.x, 0.f, 1.f) * 255.f + .5f), int(std::clamp(box.color.y, 0.f, 1.f) * 255.f + .5f),
			int(std::clamp(box.color.z, 0.f, 1.f) * 255.f + .5f));
		j.StringField("color", color);
		j.Key("at"); point(box.center);
		j.EndObject();
	}
	j.EndArray();
	if (!g_hover) return;
	j.Key("hover"); j.BeginObject();
	if (g_hover > 0) {
		const Placed* p = Find(g_hover);
		Vec3 mins, maxs, corners[8];
		if (p && Alive(*p) && PropBounds(*p, mins, maxs)) {
			Props::BoxCorners(p->position, p->angles, p->scale, mins, maxs, corners);
			j.StringField("name", Props::DisplayName(p->model));
			j.StringField("kind", Props::IsEffect(p->model) ? "Effect" : "CS2 asset");
			j.Key("bounds"); j.BeginArray();
			for (const Vec3& c : corners) point(c);
			j.EndArray();
		}
	} else {
		for (const auto& box : boxes) {
			if (box.id != -g_hover) continue;
			j.StringField("name", box.name);
			j.StringField("kind", box.light ? "Light" : "Mesh");
			j.BoolField("light", box.light);
			j.Key("at"); point(box.center);
			if (!box.light) {
				j.Key("bounds"); j.BeginArray();
				for (const Vec3& c : box.corners) point(c);
				j.EndArray();
			}
		}
	}
	j.EndObject();
}

int ParseCategory(const char* s) {
	if (!s) return 0;
	if (!_stricmp(s, "foliage")) return 1;
	if (!_stricmp(s, "props")) return 2;
	if (!_stricmp(s, "map")) return 3;
	if (!_stricmp(s, "effects")) return 4;
	return 0;
}
const char* CategoryToken(Props::Category c) {
	switch (c) {
	case Props::Category::Foliage: return "foliage";
	case Props::Category::Props: return "props";
	case Props::Category::Map: return "map";
	case Props::Category::Effects: return "effects";
	default: return "all";
	}
}

} // namespace

bool Props_EditorBusy() { return g_editorBusy; }

void Props_OnShutdown() {
	if (g_spawnArmed) DisarmSpawnMarker(g_map);
}

void Props_StopEditing() {
	StopPainting();
	CancelGrab();
	StopPlacing();
}

namespace {
// Every frame: a dragged asset's shadow capture must follow its draw. The margin
// covers the one frame between this publish and the render thread's upload.
void PublishCasterBoxes() {
	std::vector<Environment::PlacedCasterBox> boxes;
	const auto add = [&](const Placed& p) {
		Vec3 mins, maxs, corners[8];
		if (Props::IsEffect(p.model) || !Alive(p) || !PropBounds(p, mins, maxs)) return;
		Props::BoxCorners(p.position, p.angles, p.scale, mins, maxs, corners);
		Environment::PlacedCasterBox box{corners[0], corners[0]};
		for (const Vec3& c : corners) {
			box.low = {std::min(box.low.x, c.x), std::min(box.low.y, c.y), std::min(box.low.z, c.z)};
			box.high = {std::max(box.high.x, c.x), std::max(box.high.y, c.y), std::max(box.high.z, c.z)};
		}
		box.low = box.low - Vec3{8, 8, 8};
		box.high = box.high + Vec3{8, 8, 8};
		boxes.push_back(box);
	};
	for (const Placed& p : g_props) if (Shown(p)) add(p);
	if (g_placing) add(g_ghost);
	Environment::SetPlacedCasterBoxes(boxes);
}
} // namespace

void Props_RunMainThreadFrame() {
	PropPreview_RunFrame(); // every frame: the preview's fit must land before it is drawn
	PublishCasterBoxes();
	if (g_spawnArmed) ++g_framesSinceSpawn;
	// Every frame, so the first recorded frame already lacks the props kept out of renders.
	if (const bool recording = AfxStreams_IsRcording(); recording != g_recording) {
		g_recording = recording;
		for (Placed& p : g_props) {
			if (p.render) continue;
			if (recording) Despawn(p); else Spawn(p);
		}
		++g_revision;
	}
	// A throttled live move that the cursor has since stopped on still lands.
	if (g_placing && g_ghostDirty && LiveUpdate(g_ghost)) g_ghostDirty = false;
	if (g_grab != Grab::None && g_grabDirty)
		if (Placed* p = Find(g_before.id); p && LiveUpdate(*p)) g_grabDirty = false;
	const ULONGLONG now = GetTickCount64();
	if (now - g_lastCheck < 250) return;
	g_lastCheck = now;
	const std::string map = CurrentMap();
	if (map != g_map) {
		// A new level: the old entities went with it.
		g_placing = g_ghostDirty = false;
		g_grab = Grab::None;
		SyncBusy();
		for (Placed& p : g_props) { p.entity = nullptr; p.entityIndex = -1; p.particle = -1; }
		g_wind = nullptr; g_windIndex = -1;
		// Leaving a level the game survived on: its spawns are not why anything crashed.
		if (g_spawnArmed) DisarmSpawnMarker(g_map);
		g_map = map;
		g_mapSince = now;
		g_restoreHeld = false;
		if (!map.empty()) Load(map); else g_props.clear();
		std::error_code ec;
		if (!map.empty() && std::filesystem::exists(SpawnMarkerPath(map), ec)) {
			g_restoreHeld = !g_props.empty();
			for (Placed& p : g_props) p.held = true;
			if (g_restoreHeld)
				g_message = "The game closed while objects on this map were being placed, so the " +
					std::to_string(g_props.size()) + " saved object(s) were not loaded. Load them again, or clear them.";
			else
				DisarmSpawnMarker(map);
			MvmDebugLog_LinefAlways("props.guard", "map=%s previous spawn never settled: held=%d saved=%zu",
				map.c_str(), int(g_restoreHeld), g_props.size());
		}
		LoadMapModels(map);
		++g_revision;
		return;
	}
	if (g_map.empty() || now - g_mapSince < 3000) return; // let the level finish loading
	if (g_spawnArmed && now - g_lastSpawn > kSpawnSettleMs && g_framesSinceSpawn >= 600)
		DisarmSpawnMarker(g_map); // it survived, and kept drawing
	// A demo seek (full update) can clear client entities; put ours back.
	for (Placed& p : g_props) {
		if (!Shown(p) || p.failed || p.held) continue;
		if (!Alive(p)) { p.entity = nullptr; Spawn(p); ++g_revision; }
	}
	if (g_windStrength > 0 && !(g_wind && EntityAt(g_windIndex) == g_wind)) SpawnWind();
}

void Props_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* sub = argc >= 3 ? args->ArgV(2) : "";
	auto arg = [&](int i) -> const char* { return argc > i ? args->ArgV(i) : ""; };
	try {
		if (!_stricmp(sub, "search")) {
			LoadCatalog();
			g_query = arg(3);
			g_category = Props::Category(ParseCategory(arg(4)));
			g_limit = std::clamp(*arg(5) ? atoi(arg(5)) : 24, 1, kMaxLimit);
			++g_revision;
			return;
		}
		if (!_stricmp(sub, "wind")) { // wind <speed>: sway for foliage across the level; 0 is off
			g_windStrength = std::max(0.0f, std::min(200.0f, float(atof(arg(3)))));
			DespawnWind();
			if (!g_map.empty()) SpawnWind();
			++g_revision;
			return;
		}
		if (!_stricmp(sub, "preview")) { // the model the CS2 ASSETS preview panel shows; off stops
			PropPreview_Set(_stricmp(arg(3), "off") ? arg(3) : "");
			++g_revision;
			return;
		}
		if (g_map.empty() && _stricmp(sub, "status") && *sub) throw std::runtime_error("Load a map or demo first");
		if (!_stricmp(sub, "add")) {
			Placed p;
			p.id = g_nextId++;
			p.model = arg(3);
			if (p.model.empty()) throw std::runtime_error("usage: props add <models/....vmdl>");
			AimPose(p.position, p.angles);
			Spawn(p);
			if (!Live(p)) throw std::runtime_error("The engine refused " + p.model);
			g_props.push_back(p);
			PushAdded(p.id);
			g_selected = p.id;
			g_message.clear();
			Save();
			++g_revision;
			return;
		}
		if (!_stricmp(sub, "undo")) { // the last change to a scene object: CS2 asset, light or mesh
			if (g_placing) { StopPlacing(); ++g_revision; return; } // mid-placement: just drop the copy
			if (g_grab != Grab::None) { CancelGrab(); ++g_revision; return; } // mid-drag: put it back
			const uint64_t envStamp = Environment::LastUndoStamp();
			if (envStamp && (g_undo.empty() || envStamp > g_undo.back().stamp)) {
				std::string error;
				g_message = Environment::UndoLastEdit(error) ? "Undid last scene edit" : error;
				++g_revision;
				return;
			}
			if (g_undo.empty()) { g_message = "Nothing to undo"; ++g_revision; return; }
			UndoStep step = std::move(g_undo.back());
			g_undo.pop_back();
			for (Placed& before : step.changed) {
				Placed* cur = Find(before.id);
				if (!cur) continue;
				Placed restored = before;
				restored.entity = cur->entity; restored.entityIndex = cur->entityIndex;
				restored.particle = cur->particle; restored.failed = cur->failed;
				*cur = restored;
				if (!Shown(*cur)) Despawn(*cur); else Respawn(*cur);
			}
			if (!step.changed.empty()) {
				g_message = "Undid change";
				Save();
				++g_revision;
				return;
			}
			for (Placed& q : g_props)
				if (std::find(step.added.begin(), step.added.end(), q.id) != step.added.end()) Despawn(q);
			g_props.erase(std::remove_if(g_props.begin(), g_props.end(), [&](const Placed& q) {
				return std::find(step.added.begin(), step.added.end(), q.id) != step.added.end(); }), g_props.end());
			if (std::find(step.added.begin(), step.added.end(), g_selected) != step.added.end()) g_selected = 0;
			for (Placed& q : step.removed) {
				if (Find(q.id)) continue;
				Spawn(q);
				g_props.push_back(q);
			}
			g_message = step.removed.empty() ? "Undid " + std::to_string(step.added.size()) + " placed"
				: "Put back " + std::to_string(step.removed.size()) + " removed";
			Save();
			++g_revision;
			return;
		}
		if (!_stricmp(sub, "place")) { // place <model>: it follows the cursor (hover) until drop
			if (!*arg(3)) throw std::runtime_error("usage: props place <models/....vmdl>");
			CancelGrab();
			StopPlacing();
			g_ghost = Placed{};
			g_ghost.model = arg(3);
			AimPose(g_ghost.position, g_ghost.angles);
			g_ghostFloorYaw = g_ghost.angles.y;
			Spawn(g_ghost);
			if (!Live(g_ghost)) throw std::runtime_error("The engine refused " + g_ghost.model);
			g_placing = true;
			g_selected = 0;
			SyncBusy();
			EnterFreeCam();
			g_message.clear();
			++g_revision;
			return;
		}
		if (!_stricmp(sub, "hover")) { // hover <u> <v>: the placed copy follows the surface there
			// On a floor it stands on it; on a wall it lies flat against it, front to the room; under
			// a ceiling it hangs from it (PropEditMath.h PoseOnSurface). The turn it had on the floor
			// comes back when it leaves a wall.
			Vec3 at, normal;
			if (!g_placing || !SurfaceUnder(float(atof(arg(3))), float(atof(arg(4))), at, &normal)) return;
			Vec3 mins{0, 0, 0}, maxs{0, 0, 0};
			Vec3 pose = at, angles = g_ghost.angles;
			if (Props::IsEffect(g_ghost.model) || !PropBounds(g_ghost, mins, maxs)) {
				// An effect plays from where it stands: no bounds to rest or turn by.
			} else {
				const auto placed = Props::PoseOnSurface(at, normal, mins, maxs, g_ghost.scale, g_ghostFloorYaw);
				pose = placed.position;
				angles = placed.angles;
			}
			const Vec3 d = pose - g_ghost.position, da = angles - g_ghost.angles;
			if (Environment::Dot(d, d) < 0.25f && Environment::Dot(da, da) < 0.01f) return;
			g_ghost.position = pose;
			g_ghost.angles = angles;
			g_ghostDirty = !LiveUpdate(g_ghost);
			return;
		}
		if (!_stricmp(sub, "drop")) { // the copy being placed becomes a saved prop, selected
			if (!g_placing) return;
			Placed p = g_ghost;
			g_ghost = Placed{};
			g_placing = false;
			p.id = g_nextId++;
			if (g_ghostDirty || !Alive(p)) Respawn(p);
			g_ghostDirty = false;
			g_props.push_back(p);
			PushAdded(p.id);
			g_selected = p.id;
			SyncBusy();
			Save();
			++g_revision;
			return;
		}
		if (!_stricmp(sub, "cancel")) { Props_StopEditing(); ++g_revision; return; }
		if (!_stricmp(sub, "paste")) { // paste <id>: a copy of that prop (size, turn, group) to place with the cursor
			const Placed* from = Find(atoi(arg(3)));
			if (!from) throw std::runtime_error("Nothing to paste: the copied prop is gone");
			const Placed source = *from;
			CancelGrab();
			StopPlacing();
			StopPainting();
			g_ghost = Placed{};
			g_ghost.model = source.model;
			g_ghost.scale = source.scale;
			g_ghost.group = source.group;
			Vec3 aimAngles;
			AimPose(g_ghost.position, aimAngles);
			g_ghost.angles = source.angles;
			g_ghostFloorYaw = source.angles.y;
			Spawn(g_ghost);
			if (!Live(g_ghost)) throw std::runtime_error("The engine refused " + g_ghost.model);
			g_placing = true;
			g_selected = 0;
			SyncBusy();
			EnterFreeCam();
			g_message.clear();
			++g_revision;
			return;
		}
		if (!_stricmp(sub, "paint")) { // paint start <model> | stop | set <key> <v> | hover|stroke|erase <u> <v> | release
			const std::string what = arg(3);
			if (what == "start" || what == "models") {
				// start <model> [model ...] begins painting; models <model> [model ...] swaps what an
				// active brush sprays without leaving paint mode (a new mix paints into its own group).
				std::vector<std::string> models;
				for (int i = 4; i < argc && models.size() < 16; ++i)
					if (*arg(i) && std::find(models.begin(), models.end(), arg(i)) == models.end()) models.push_back(arg(i));
				if (models.empty()) throw std::runtime_error("usage: props paint start|models <models/....vmdl> [more models]");
				if (what == "models" && !g_brush.active) throw std::runtime_error("Not painting");
				if (what == "start") {
					CancelGrab();
					StopPlacing();
					StopPainting();
					g_brush.seed = uint32_t(GetTickCount64()) | 1u;
				} else {
					EndBrushPress(); // what the old brush painted stays one undo step
				}
				g_brush.models = models;
				g_brush.group = "Painted " + (models.size() == 1 ? Props::DisplayName(models[0]) : std::string("mix"));
				g_brush.minsZ.clear();
				g_brush.stroke.clear();
				for (const std::string& m : models) Props::PrecacheModel(m.c_str()); // so strokes never hitch on a load
				g_brush.active = true;
				g_selected = 0;
				SyncBusy();
				EnterFreeCam();
				g_message.clear();
			} else if (what == "stop") {
				StopPainting();
			} else if (what == "set") {
				const std::string key = arg(4);
				const float v = float(atof(arg(5)));
				if (key == "radius") g_brush.radius = std::max(16.0f, std::min(1024.0f, v));
				else if (key == "spacing") g_brush.spacing = std::max(4.0f, std::min(512.0f, v));
				else if (key == "size") g_brush.size = std::max(0.1f, std::min(5.0f, v));
				else if (key == "jitter") g_brush.jitter = std::max(0.0f, std::min(0.9f, v));
				else if (key == "turn") g_brush.turn = std::max(0.0f, std::min(180.0f, v));
				else if (key == "density") g_brush.density = std::max(0.05f, std::min(1.0f, v / 100.0f)); // percent
				else if (key == "erase") g_brush.erasing = v != 0;
				else throw std::runtime_error("paint set radius|spacing|size|jitter|turn|density|erase <value>");
			} else if (what == "hover" || what == "stroke" || what == "erase") {
				Vec3 at;
				if (!g_brush.active || !SurfaceUnder(float(atof(arg(4))), float(atof(arg(5))), at)) return;
				g_brush.center = at;
				g_brush.hasCenter = true;
				if (what != "hover") {
					const ULONGLONG now = GetTickCount64();
					if (now - g_brush.lastStroke < kStrokeMs) { ++g_revision; return; }
					g_brush.lastStroke = now;
					if (what == "erase" || g_brush.erasing) PaintErase(); else PaintStroke();
				}
			} else if (what == "release") {
				EndBrushPress(); // this press is one undo step
				if (g_brush.dirty) { Save(); g_brush.dirty = false; }
			} else throw std::runtime_error("paint start|models|stop|set|hover|stroke|erase|release");
			++g_revision;
			return;
		}
		if (!_stricmp(sub, "hoverat")) { // hoverat <u> <v>: what a click there would pick (the label)
			const int hover = PickAt(float(atof(arg(3))), float(atof(arg(4))));
			if (hover != g_hover) { g_hover = hover; ++g_revision; }
			return;
		}
		if (!_stricmp(sub, "pickgrab")) { // pickgrab <u> <v>: pick, and a CS2 asset is grabbed to drag along surfaces
			Environment::Matrix vp;
			Vec3 eye;
			if (!Environment::RenderedCamera(vp, eye)) return;
			const float u = float(atof(arg(3))), v = float(atof(arg(4)));
			const int picked = PickAt(u, v);
			g_selected = picked > 0 ? picked : 0;
			Environment::SelectInView(picked < 0 ? -picked : 0);
			Placed* q = picked > 0 ? Find(picked) : nullptr;
			Vec3 hit;
			if (q && Alive(*q) && !q->locked && SurfaceUnder(u, v, hit)) {
				CancelGrab();
				g_surfaceOffset = {q->position.x - hit.x, q->position.y - hit.y, 0};
				g_grab = Grab::Surface;
				g_before = *q;
				SyncBusy();
			}
			++g_revision;
			return;
		}
		if (!_stricmp(sub, "pick")) { // pick <u> <v>: select the prop under that point, or none
			Environment::Matrix vp;
			Vec3 eye;
			if (!Environment::RenderedCamera(vp, eye)) return;
			const int picked = PickAt(float(atof(arg(3))), float(atof(arg(4))));
			g_selected = picked > 0 ? picked : 0;
			// A mesh or light: selected with its move arrows armed, no Edit position needed.
			Environment::SelectInView(picked < 0 ? -picked : 0);
			++g_revision;
			return;
		}
		if (!_stricmp(sub, "grab")) { // grab <id> x|y|z|turn|tilt|roll <u> <v>: start dragging a handle
			Placed* q = Find(atoi(arg(3)));
			if (!q || !Alive(*q)) throw std::runtime_error("No such prop");
			if (q->locked) throw std::runtime_error("It is locked in the outliner");
			CancelGrab();
			Environment::Matrix vp;
			Vec3 eye, dir;
			const float u = float(atof(arg(5))), v = float(atof(arg(6)));
			if (!Environment::RenderedCamera(vp, eye)) return;
			const std::string handle = arg(4);
			const int ring = handle == "turn" ? 0 : handle == "tilt" ? 1 : handle == "roll" ? 2 : -1;
			if (handle == "surface") {
				Vec3 hit;
				if (!SurfaceUnder(u, v, hit)) return;
				g_surfaceOffset = {q->position.x - hit.x, q->position.y - hit.y, 0};
				g_grab = Grab::Surface;
			} else if (ring >= 0) {
				g_ring = Props::Ring(ring);
				g_ringAxis = Props::RingAxis(q->angles, g_ring);
				if (!Props::CursorRay(vp, eye, u, v, dir) || !Props::RingAngle(eye, dir, q->position, g_ringAxis, g_turnAnchor)) return;
				g_grab = Grab::Turn;
			} else {
				const int axis = handle == "x" ? 0 : handle == "y" ? 1 : handle == "z" ? 2 : -1;
				if (axis < 0) throw std::runtime_error("grab x|y|z|turn|tilt|roll|surface");
				if (!g_drag.Begin(q->id, axis, q->position, eye, vp, u, v)) return; // axis points at the camera
				g_grab = Grab::Axis;
			}
			g_before = *q;
			g_selected = q->id;
			SyncBusy();
			++g_revision;
			return;
		}
		if (!_stricmp(sub, "drag")) { // drag <u> <v> [snap]: move the grabbed handle there
			Placed* q = Find(g_before.id);
			if (g_grab == Grab::None || !q) return;
			const float u = float(atof(arg(3))), v = float(atof(arg(4)));
			// Snap (Shift): an arrow keeps the prop resting on the floor under it (the up arrow drops
			// it onto the floor below); a ring turns in 15-degree steps.
			const bool snap = atoi(arg(5)) != 0;
			if (g_grab == Grab::Axis) {
				if (!g_drag.Move(u, v)) return;
				q->position = g_drag.current;
				if (snap) SnapToFloor(*q);
			} else if (g_grab == Grab::Surface) {
				// The surface under the cursor, keeping the grab offset on a floor so the prop does
				// not jump to the cursor; its turn is kept, and its bottom rests on what it is over.
				// On a wall or a ceiling it is posed the way placing poses it (PoseOnSurface).
				Vec3 hit, normal;
				if (!SurfaceUnder(u, v, hit, &normal)) return;
				Vec3 mins, maxs;
				const bool bounds = !Props::IsEffect(q->model) && PropBounds(*q, mins, maxs);
				const float nz = normal.z / std::max(1e-4f, std::sqrt(Environment::Dot(normal, normal)));
				if (nz >= 0.5f) {
					q->position = {hit.x + g_surfaceOffset.x, hit.y + g_surfaceOffset.y, hit.z};
					q->angles = g_before.angles;
					if (bounds) q->position.z = Props::RestingZ(hit.z, mins.z, q->scale);
				} else if (bounds) {
					const auto placed = Props::PoseOnSurface(hit, normal, mins, maxs, q->scale, g_before.angles.y);
					q->position = placed.position;
					q->angles = placed.angles;
				} else {
					q->position = hit;
				}
			} else {
				Environment::Matrix vp;
				Vec3 eye, dir;
				float angle = 0;
				if (!Environment::RenderedCamera(vp, eye) || !Props::CursorRay(vp, eye, u, v, dir) ||
				    !Props::RingAngle(eye, dir, q->position, g_ringAxis, angle)) return;
				q->angles = Props::TurnOnRing(g_before.angles, g_ring, Props::WrapDegrees(angle - g_turnAnchor));
				if (snap) {
					float& a = g_ring == Props::Ring::Tilt ? q->angles.x : g_ring == Props::Ring::Roll ? q->angles.z : q->angles.y;
					a = Props::SnapDegrees(a, 15.0f);
				}
			}
			g_grabDirty = !LiveUpdate(*q);
			++g_revision;
			return;
		}
		if (!_stricmp(sub, "release")) { // end the drag and keep the result
			if (g_grab == Grab::None) return;
			if (Placed* q = Find(g_before.id)) {
				if (g_grabDirty) ApplyPose(*q);
				if (!SamePose(*q, g_before)) PushChanged(g_before);
				Save();
			}
			g_drag.Commit();
			g_grab = Grab::None;
			g_grabDirty = false;
			SyncBusy();
			++g_revision;
			return;
		}
		if (!_stricmp(sub, "select")) { g_selected = atoi(arg(3)); ++g_revision; return; }
		if (!_stricmp(sub, "restore")) { // spawn the saved objects the crash-loop guard held back
			for (Placed& p : g_props) p.held = false;
			g_restoreHeld = false;
			g_message.clear();
			MvmDebugLog_LinefAlways("props.guard", "map=%s restore requested: %zu object(s)", g_map.c_str(), g_props.size());
			++g_revision; // the main-thread pass spawns them (and re-arms the marker first)
			return;
		}
		if (!_stricmp(sub, "clear")) {
			g_restoreHeld = false;
			g_message.clear();
			UndoStep step;
			step.removed = g_props;
			PushUndo(std::move(step));
			for (Placed& p : g_props) Despawn(p);
			g_props.clear(); g_selected = 0;
			Save(); ++g_revision;
			return;
		}
		Placed* p = Find(atoi(arg(3)));
		if (!_stricmp(sub, "remove")) { // remove <id> [<id> ...]: one save for a whole selection or group
			// The panel sends a big group in several of these (a console line holds ~60 ids), and an
			// id may already be gone: remove what is still here instead of refusing the whole batch.
			std::vector<int> ids;
			for (int i = 3; i < argc; ++i) ids.push_back(atoi(arg(i)));
			UndoStep step;
			for (Placed& q : g_props)
				if (std::find(ids.begin(), ids.end(), q.id) != ids.end()) { step.removed.push_back(q); Despawn(q); }
			if (step.removed.empty()) throw std::runtime_error("No such prop");
			PushUndo(std::move(step));
			g_props.erase(std::remove_if(g_props.begin(), g_props.end(), [&](const Placed& q) {
				return std::find(ids.begin(), ids.end(), q.id) != ids.end(); }), g_props.end());
			if (std::find(ids.begin(), ids.end(), g_selected) != ids.end()) g_selected = 0;
			Save(); ++g_revision;
			return;
		}
		if (!_stricmp(sub, "duplicate")) {
			if (!p) throw std::runtime_error("No such prop");
			Placed copy = *p;
			copy.id = g_nextId++;
			ClearLive(copy);
			copy.position.x += 48;
			Spawn(copy);
			g_props.push_back(copy);
			PushAdded(copy.id);
			g_selected = copy.id;
			Save(); ++g_revision;
			return;
		}
		if (!_stricmp(sub, "floor")) { // rest the prop on the floor under it
			if (!p) throw std::runtime_error("No such prop");
			const Placed before = *p;
			if (!SnapToFloor(*p)) throw std::runtime_error("No floor under it (map collision not ready?)");
			PushChanged(before);
			ApplyPose(*p); Save();
			return;
		}
		if (!_stricmp(sub, "here")) { // move to the surface under the crosshair, keep its turn
			if (!p) throw std::runtime_error("No such prop");
			const Placed before = *p;
			Vec3 angles;
			AimPose(p->position, angles);
			PushChanged(before);
			ApplyPose(*p); Save();
			return;
		}
		if (!_stricmp(sub, "set")) { // props set <id> <field> <value...>
			if (!p) throw std::runtime_error("No such prop");
			const std::string field = arg(4);
			PushChanged(*p, "set " + std::to_string(p->id) + " " + field);
			if (field == "group") { // set <id> group <name>|-  (outliner): no respawn needed
				const std::string name = !strcmp(arg(5), "-") ? "" : arg(5);
				if (name.size() > 40) throw std::runtime_error("Group names are at most 40 characters");
				p->group = name;
				Save(); ++g_revision;
				return;
			}
			if (field == "locked") { p->locked = atoi(arg(5)) != 0; Save(); ++g_revision; return; }
			if (field == "render") { p->render = atoi(arg(5)) != 0; Save(); ++g_revision; return; }
			const float v = float(atof(arg(5)));
			if (field == "x") p->position.x = v;
			else if (field == "y") p->position.y = v;
			else if (field == "z") p->position.z = v;
			else if (field == "pitch") p->angles.x = v;
			else if (field == "yaw") p->angles.y = v;
			else if (field == "roll") p->angles.z = v;
			else if (field == "scale") p->scale = std::max(0.05f, std::min(20.0f, v));
			else if (field == "visible") p->visible = v != 0;
			else if (field == "pose" && argc >= 11) { // pose x y z pitch yaw roll
				p->position = {float(atof(arg(5))), float(atof(arg(6))), float(atof(arg(7)))};
				p->angles = {float(atof(arg(8))), float(atof(arg(9))), float(atof(arg(10)))};
			} else throw std::runtime_error("Unknown field " + field);
			if (!Shown(*p)) { Despawn(*p); ++g_revision; } else Respawn(*p);
			Save();
			return;
		}
		if (!_stricmp(sub, "status") || !*sub) {
			advancedfx::Message("%s props: spawner %s, map '%s', %zu placed, catalog %zu models\n", cmd,
				Props::ClientEntitySpawnAvailable() ? "available" : "UNAVAILABLE", g_map.c_str(), g_props.size(), g_catalog.size());
			if (!g_mapModels.empty())
				advancedfx::Message("%s props: %zu of them come with this workshop map\n", cmd, g_mapModels.size());
			for (const Placed& q : g_props)
				advancedfx::Message("  #%d %s (%.0f %.0f %.0f) yaw %.0f scale %.2f %s\n", q.id, q.model.c_str(),
					q.position.x, q.position.y, q.position.z, q.angles.y, q.scale, Alive(q) ? "live" : (q.visible ? "pending" : "hidden"));
			advancedfx::Message(
				"%s props search \"<words>\" [all|foliage|props|effects|map] [count]\n"
				"%s props add <models/....vmdl>        - place on the surface under the crosshair\n"
				"%s props place <model> | hover <u> <v> | drop | cancel - place with the cursor (free cam)\n"
				"%s props pick <u> <v> | grab <id> x|y|z|turn <u> <v> | drag <u> <v> [snap] | release - edit in the view\n"
				"%s props floor <id>                   - rest it on the floor under it\n"
				"%s props set <id> group <name>|- | render 0|1 | locked 0|1 - outliner\n"
				"%s props here|remove|duplicate|select <id>\n"
				"%s props set <id> x|y|z|pitch|yaw|roll|scale|visible <value>\n"
				"%s props clear                         - remove every prop on this map\n"
				"%s props restore                       - load saved props held back after a crash while placing\n",
				cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
			if (g_restoreHeld)
				advancedfx::Message("%s props: %zu saved prop(s) held back: the last spawn on this map never survived.\n",
					cmd, g_props.size());
			return;
		}
		throw std::runtime_error(std::string("Unknown props command ") + sub);
	} catch (const std::exception& e) {
		g_message = e.what();
		++g_revision;
		advancedfx::Warning("%s props: %s\n", cmd, e.what());
	}
}

void Props_AppendStateJson(std::ostringstream& out) {
	JsonBuilder j;
	j.BeginObject();
	j.IntField("revision", g_revision);
	j.StringField("map", g_map);
	j.StringField("message", g_message);
	j.BoolField("held", g_restoreHeld);
	j.IntField("selected", g_selected);
	j.Key("placed"); j.BeginArray();
	for (const Placed& p : g_props) {
		j.BeginObject();
		j.IntField("id", p.id);
		j.StringField("model", p.model);
		j.StringField("name", Props::DisplayName(p.model));
		j.StringField("folder", Props::Folder(p.model));
		j.StringField("source", Props::SourceMap(p.model));
		j.Key("pos"); j.BeginArray(); j.ValueDouble(p.position.x); j.ValueDouble(p.position.y); j.ValueDouble(p.position.z); j.EndArray();
		j.Key("ang"); j.BeginArray(); j.ValueDouble(p.angles.x); j.ValueDouble(p.angles.y); j.ValueDouble(p.angles.z); j.EndArray();
		j.DoubleField("scale", p.scale);
		j.BoolField("visible", p.visible);
		j.BoolField("live", Alive(p));
		j.StringField("group", p.group);
		j.BoolField("render", p.render);
		j.BoolField("locked", p.locked);
		j.EndObject();
	}
	j.EndArray();
	j.StringField("mapLabel", Props::MapLabel(g_map));
	j.DoubleField("wind", g_windStrength);
	j.StringField("query", g_query);
	j.StringField("category", CategoryToken(g_category));
	j.IntField("limit", g_limit);
	j.StringField("preview", PropPreview_Model());
	j.BoolField("placing", g_placing);
	j.StringField("placingName", g_placing ? Props::DisplayName(g_ghost.model) : "");
	static const char* const kRingNames[] = {"turn", "tilt", "roll"};
	j.StringField("grab", g_grab == Grab::Axis ? "axis" : g_grab == Grab::Turn ? kRingNames[int(g_ring)]
		: g_grab == Grab::Surface ? "move" : "");
	AppendGizmo(j);
	AppendBrush(j);
	AppendMarkers(j);
	if (g_catalogLoaded) {
		const std::string key = g_query + '' + CategoryToken(g_category) + '' + std::to_string(g_limit) + '' + g_map;
		if (key != g_foundKey) {
			g_foundKey = key;
			g_found = Props::Search(g_catalog, g_query, g_category, Props::MapToken(g_map), 0, g_limit, &g_fromMap);
		}
		const auto& found = g_found;
		j.IntField("total", found.total);
		j.Key("results"); j.BeginArray();
		for (int i : found.indices) {
			j.BeginObject();
			j.StringField("model", g_catalog[i]);
			j.StringField("name", Props::DisplayName(g_catalog[i]));
			j.StringField("folder", Props::Folder(g_catalog[i]));
			// A workshop map's own models come from that map, whatever their folder.
			const bool own = i < int(g_fromMap.size()) && g_fromMap[i];
			j.StringField("source", own ? Props::MapLabel(g_map) : Props::SourceMap(g_catalog[i]));
			j.EndObject();
		}
		j.EndArray();
	} else {
		j.IntField("total", -1);
	}
	j.EndObject();
	std::string json = j.Str();
	std::string extra;
	PropPreview_AppendJson(extra);
	json.insert(json.size() - 1, extra); // inside the closing brace
	out << ",\"props\":" << json;
}

} // namespace Filmmaker
