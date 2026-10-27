// CS:S's answers to the follow camera's host (AfxHookSource2/Filmmaker/Movie/FollowCameraHost.h), and
// where the hook runs it. Targets come from the entity adapter: a player is followed at its eyes
// while it lives and at its ragdoll once dead. CS:S has no skeleton reads yet, so the eyes are the
// origin raised by the profile's standing or ducking height (SourceEngine.h EntityState::eye). A
// weapon or the C4 is followed at its origin: on the ground where it lies; in a player's hands at the
// player's hips, where CS:S puts a carried weapon's origin (measured in the SourceTV test demo,
// 2026-10-26: the owner's origin raised 31 units). The Events list is the demo index's deaths, item
// pickups and the C4's drops, pickups and plant. Grenades and Attached's body mounts come with the
// rest of step 11.

#include "stdafx.h"

#include "SourceFollowCamera.h"
#include "SourceCameraPath.h"
#include "SourceDemoBar.h"
#include "SourceDemoIndex.h"
#include "SourceEngine.h"
#include "SourceGameProfile.h"
#include "SourceUi.h"
#include "Ui/DirectorMath.h"
#include "Ui/FollowInspector.h"

#include "RenderView.h"
#include "WrpGlobals.h"
#include "WrpVEngineClient.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <AfxHookSource2/Filmmaker/Movie/CameraBridge.h>
#include <AfxHookSource2/Filmmaker/Movie/FollowCamera.h>
#include <AfxHookSource2/Filmmaker/Movie/FollowCameraHost.h>
#include <AfxHookSource2/Filmmaker/Platform/JsonParser.h>
#include <Shared/AfxConsole.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern WrpVEngineClient* g_VEngineClient;

namespace {

using SourceFilmmaker::EntityState;

const SourceFilmmaker::GameProfile& Profile() { return SourceFilmmaker::GameProfile_Current(); }

bool IsPlayer(const EntityState& e) { return Profile().isPlayerClass && Profile().isPlayerClass(e.className.c_str()); }
bool IsBomb(const EntityState& e) { return Profile().isBombClass && Profile().isBombClass(e.className.c_str()); }
// A weapon that is not the bomb (CS2's IsWeaponClass keeps the C4 apart too).
bool IsWeapon(const EntityState& e) {
	return Profile().isWeaponClass && Profile().isWeaponClass(e.className.c_str()) && !IsBomb(e);
}
bool IsWeaponOrBomb(const EntityState& e) { return IsWeapon(e) || IsBomb(e); }

std::string WeaponName(const std::string& networkClass) {
	const char* name = Profile().weaponName ? Profile().weaponName(networkClass.c_str()) : nullptr;
	return name ? name : networkClass;
}

// Who holds a weapon or the carried C4, -1 for nobody. The planted bomb is nobody's.
int WeaponOwner(const EntityState& e) {
	if (!Profile().isWeaponClass || !Profile().isWeaponClass(e.className.c_str())) return -1;
	return e.weaponOwnerIndex >= 0 ? e.weaponOwnerIndex : e.ownerIndex;
}

std::string PlayerName(int index) {
	std::vector<SourceFilmmaker::PlayerState> players;
	SourceFilmmaker::Players_Snapshot(players);
	for (const SourceFilmmaker::PlayerState& p : players)
		if (p.index == index && !p.name.empty()) return p.name;
	return "Player";
}

Filmmaker::FollowVec3 Vec(const double v[3]) { return Filmmaker::FollowVec3{ v[0], v[1], v[2] }; }

// Where a player is followed: its eyes while it lives, its ragdoll once dead. False while the player
// is dormant (out of the demo's view) or dead without a body; the camera then holds its last target.
bool PlayerPoint(int index, Filmmaker::FollowVec3& out, const char** source = nullptr) {
	EntityState e;
	if (!SourceFilmmaker::Entity_Get(index, e) || !IsPlayer(e) || e.dormant) return false;
	if (e.alive) {
		out = Vec(e.eye);
		if (source) *source = "player-eye";
		return Filmmaker::IsFiniteOrigin(out);
	}
	EntityState body;
	if (e.ragdollIndex < 0 || !SourceFilmmaker::Entity_Get(e.ragdollIndex, body) || body.dormant) return false;
	out = Vec(body.origin);
	if (source) *source = "ragdoll-origin";
	return Filmmaker::IsFiniteOrigin(out);
}

class PlayerProvider final : public Filmmaker::IFollowTargetProvider {
public:
	explicit PlayerProvider(int index) : m_index(index) {}
	bool IsValid() override {
		Filmmaker::FollowVec3 p;
		return PlayerPoint(m_index, p);
	}
	Filmmaker::FollowVec3 GetWorldPosition() override {
		Filmmaker::FollowVec3 p;
		PlayerPoint(m_index, p);
		return p;
	}
	std::string GetDisplayName() override { return PlayerName(m_index); }
	Filmmaker::FollowTargetStatus GetStatus() override {
		EntityState e;
		if (!SourceFilmmaker::Entity_Get(m_index, e) || !IsPlayer(e)) return Filmmaker::FollowTargetStatus::Missing;
		return e.alive ? Filmmaker::FollowTargetStatus::Alive : Filmmaker::FollowTargetStatus::Dead;
	}
	int EntityIndex() const override { return m_index; }
private:
	int m_index;
};

// A weapon, the C4 or any other entity, at its origin.
class EntityProvider final : public Filmmaker::IFollowTargetProvider {
public:
	explicit EntityProvider(int index) : m_index(index) {}
	bool IsValid() override {
		EntityState e;
		return SourceFilmmaker::Entity_Get(m_index, e) && !e.dormant && Filmmaker::IsFiniteOrigin(Vec(e.origin));
	}
	Filmmaker::FollowVec3 GetWorldPosition() override {
		EntityState e;
		return SourceFilmmaker::Entity_Get(m_index, e) ? Vec(e.origin) : Filmmaker::FollowVec3{};
	}
	std::string GetDisplayName() override {
		EntityState e;
		if (!SourceFilmmaker::Entity_Get(m_index, e)) return "Entity";
		return IsWeaponOrBomb(e) ? WeaponName(e.className) : e.className;
	}
	Filmmaker::FollowTargetStatus GetStatus() override {
		return IsValid() ? Filmmaker::FollowTargetStatus::Active : Filmmaker::FollowTargetStatus::Missing;
	}
	int EntityIndex() const override { return m_index; }
private:
	int m_index;
};

// A player's weapon in hand, whichever it is (CS2's HeldWeaponProvider): it re-reads the active
// weapon every frame, so the camera stays on the player's gun through a switch.
class HeldWeaponProvider final : public Filmmaker::IFollowTargetProvider {
public:
	explicit HeldWeaponProvider(int playerIndex) : m_player(playerIndex) {}
	int ActiveWeapon() const {
		EntityState p;
		return SourceFilmmaker::Entity_Get(m_player, p) && IsPlayer(p) ? p.activeWeaponIndex : -1;
	}
	bool IsValid() override {
		EntityState w;
		const int index = ActiveWeapon();
		return index >= 0 && SourceFilmmaker::Entity_Get(index, w) && !w.dormant && Filmmaker::IsFiniteOrigin(Vec(w.origin));
	}
	Filmmaker::FollowVec3 GetWorldPosition() override {
		EntityState w;
		return SourceFilmmaker::Entity_Get(ActiveWeapon(), w) ? Vec(w.origin) : Filmmaker::FollowVec3{};
	}
	std::string GetDisplayName() override {
		EntityState w;
		return SourceFilmmaker::Entity_Get(ActiveWeapon(), w) ? WeaponName(w.className) : std::string("Held weapon");
	}
	Filmmaker::FollowTargetStatus GetStatus() override {
		EntityState p;
		if (!SourceFilmmaker::Entity_Get(m_player, p) || !IsPlayer(p)) return Filmmaker::FollowTargetStatus::Missing;
		if (!p.alive) return Filmmaker::FollowTargetStatus::Dead;
		return IsValid() ? Filmmaker::FollowTargetStatus::Active : Filmmaker::FollowTargetStatus::Missing;
	}
	int EntityIndex() const override { return ActiveWeapon(); }
private:
	int m_player;
};

// The C4 a player carries, read once per demo tick: the follow camera asks every frame while it
// follows a living player, for the retarget after a death.
int CarriedBomb(int playerIndex) {
	static int s_tick = -1, s_player = -1, s_bomb = -1;
	const SourceFilmmaker::DemoState demo = SourceFilmmaker::Demo_State();
	if (demo.tick == s_tick && playerIndex == s_player) return s_bomb;
	s_tick = demo.tick;
	s_player = playerIndex;
	s_bomb = -1;
	std::vector<EntityState> entities;
	SourceFilmmaker::Entities_Snapshot(entities);
	for (const EntityState& e : entities)
		if (IsBomb(e) && WeaponOwner(e) == playerIndex) {
			s_bomb = e.index;
			break;
		}
	return s_bomb;
}

// The demo index's events for the Events list. The main thread hands over the scan and the names its
// user ids had (FollowHost_DemoPath); the follow camera reads them on its worker thread.
struct EventSource {
	std::mutex mutex;
	std::shared_ptr<const SourceFilmmaker::DemoFile::Scan> scan;
	std::unordered_map<int, std::string> names; // user id -> name
} g_events;

// The state JSON's pieces (FollowCameraStateJson.cpp) into the tab's model.
using Filmmaker::JsonValue;

double Num(const JsonValue& o, const char* key, double def = 0) {
	const JsonValue* v = o.Find(key);
	return v ? v->AsNumber(def) : def;
}
int Int(const JsonValue& o, const char* key, int def = 0) { return (int)Num(o, key, def); }
bool Bool(const JsonValue& o, const char* key) {
	const JsonValue* v = o.Find(key);
	return v && v->AsBool();
}
std::string Str(const JsonValue& o, const char* key) {
	const JsonValue* v = o.Find(key);
	return v ? v->AsString() : std::string();
}
void Vec3(const JsonValue& o, const char* key, double out[3]) {
	const JsonValue* v = o.Find(key);
	for (int i = 0; i < 3; ++i)
		out[i] = v && v->type == JsonValue::Type::Array && v->arr.size() > (size_t)i ? v->arr[(size_t)i].AsNumber() : 0.0;
}
const std::vector<JsonValue>& Arr(const JsonValue& o, const char* key) {
	static const std::vector<JsonValue> none;
	const JsonValue* v = o.Find(key);
	return v && v->type == JsonValue::Type::Array ? v->arr : none;
}
std::vector<SourceFilmmaker::Ui::FollowInspectorPoint> Points(const JsonValue& o, const char* key) {
	std::vector<SourceFilmmaker::Ui::FollowInspectorPoint> out;
	for (const JsonValue& p : Arr(o, key)) {
		SourceFilmmaker::Ui::FollowInspectorPoint point;
		point.id = Str(p, "id");
		point.label = Str(p, "label");
		point.kind = Str(p, "kind");
		point.valid = Bool(p, "valid");
		out.push_back(point);
	}
	return out;
}

// The placed camera's marker and the target box, kept for the in-world aids (not drawn yet).
struct Aids {
	bool marker = false;
	double markerPos[3] = { 0, 0, 0 };
	bool box = false;
	float boxPos[3] = { 0, 0, 0 };
} g_aids;

} // namespace

namespace Filmmaker {

int FollowHost_DemoTick() {
	const SourceFilmmaker::DemoState demo = SourceFilmmaker::Demo_State();
	return demo.playing ? demo.tick : 0;
}

double FollowHost_IntervalPerTick() { return SourceFilmmaker::Demo_State().intervalPerTick; }

int FollowHost_FrameCount() {
	WrpGlobals* globals = g_Hook_VClient_RenderView.GetGlobals();
	return globals ? globals->framecount_get() : 0;
}

bool FollowHost_DemoPaused() { return SourceFilmmaker::Demo_State().paused; }

void FollowHost_ResumeDemo() {
	if (g_VEngineClient) g_VEngineClient->ClientCmd_Unrestricted("demo_resume");
}

// As the demo bar seeks: back restarts the demo, then goes forward.
void FollowHost_SeekDemo(int tick) { SourceFilmmaker::DemoBar_SeekTo(tick); }

// The demo index's file, once it has read the demo: a new demo, or the same one read again, is a new
// key, so the follow camera reloads its events.
std::string FollowHost_DemoPath() {
	const SourceFilmmaker::DemoIndex index = SourceFilmmaker::DemoIndex_Get();
	if (index.status != SourceFilmmaker::DemoIndex::Status::Ready || !index.scan) return std::string();
	std::lock_guard<std::mutex> lock(g_events.mutex);
	if (g_events.scan != index.scan) {
		g_events.scan = index.scan;
		std::vector<SourceFilmmaker::PlayerState> players;
		SourceFilmmaker::Players_Snapshot(players);
		for (const SourceFilmmaker::PlayerState& p : players)
			if (p.userId >= 0 && !p.name.empty()) g_events.names[p.userId] = p.name;
	}
	return index.file + "#" + std::to_string(index.generation);
}

// CS:S records no weapon_drop: a death is where the victim's weapons fall, so it stands for CS2's
// death drop. The rest keep CS2's names.
bool FollowHost_ReadDemoEvents(const std::string&, std::atomic<bool>* cancel, std::vector<FollowEventRecord>& events,
	std::string& helperPath, std::string& error) {
	events.clear();
	helperPath = "demo index";
	std::shared_ptr<const SourceFilmmaker::DemoFile::Scan> scan;
	std::unordered_map<int, std::string> names;
	{
		std::lock_guard<std::mutex> lock(g_events.mutex);
		scan = g_events.scan;
		names = g_events.names;
	}
	if (!scan) {
		error = "the demo index has not read this demo";
		return false;
	}
	for (const SourceFilmmaker::DemoFile::Event& e : scan->events) {
		if (cancel && cancel->load()) return false;
		FollowEventRecord f;
		if (e.name == "player_death") f.type = "weapon_drop";
		else if (e.name == "bomb_dropped" || e.name == "bomb_pickup" || e.name == "bomb_planted" || e.name == "item_pickup") f.type = e.name;
		else continue;
		f.tick = e.tick;
		if (e.name == "item_pickup") f.item = e.Text("item");
		else if (e.name != "player_death") f.item = "c4";
		const auto name = names.find(e.Int("userid"));
		if (name != names.end()) f.ownerName = name->second;
		events.push_back(std::move(f));
	}
	return true;
}

bool FollowHost_EditorActive() { return SourceFilmmaker::Ui_EditorOpen(); }

void FollowHost_SetEditorCursor(bool ui) { SourceFilmmaker::Ui_SetEditorMouse(ui); }

// The path's playback stops. AUTO and the TIMELINE dock's drive stand aside by themselves while the
// follow camera owns the view (SourceFollowCamera.h FollowCamera_OwnsView).
void FollowHost_TakeViewFromOtherCameras() {
	if (SourceFilmmaker::CameraPath_OwnsView()) SourceFilmmaker::CameraPath_Stop();
}

void FollowHost_ReturnViewToTimeline() {}

std::unique_ptr<IFollowTargetProvider> FollowHost_MakeProvider(const FollowCameraState& state, FollowPlayerPose*) {
	if (state.targetType == FollowTargetType::Player) return std::make_unique<PlayerProvider>(state.targetEntityIndex);
	if (state.targetType == FollowTargetType::Weapon) {
		// As CS2 chooses: the player's gun in hand (Held, or Auto with a player picked), else the entity.
		const bool held = state.weaponSource == WeaponSource::Held
			|| (state.weaponSource == WeaponSource::Auto && state.weaponPlayerIndex >= 0);
		if (held && state.weaponPlayerIndex >= 0) return std::make_unique<HeldWeaponProvider>(state.weaponPlayerIndex);
	}
	return std::make_unique<EntityProvider>(state.targetEntityIndex);
}

std::vector<FollowTargetCandidate> FollowHost_FindTargets(FollowTargetType type, bool attachPoints, const FollowVec3& from) {
	std::vector<FollowTargetCandidate> out;
	if (type == FollowTargetType::Grenade) return out;
	std::vector<EntityState> entities;
	SourceFilmmaker::Entities_Snapshot(entities);
	std::vector<SourceFilmmaker::PlayerState> players;
	SourceFilmmaker::Players_Snapshot(players);
	if (type == FollowTargetType::Weapon) {
		std::unordered_map<int, const EntityState*> byIndex;
		for (const EntityState& e : entities) byIndex[e.index] = &e;
		for (const EntityState& e : entities) {
			if (!IsWeaponOrBomb(e)) continue;
			FollowTargetCandidate c;
			c.entityIndex = e.index;
			c.handle = e.handle != 0xFFFFFFFFu ? e.handle : 0;
			c.className = e.className;
			c.isWeapon = IsWeapon(e);
			c.isBomb = IsBomb(e);
			c.ownerIndex = WeaponOwner(e);
			c.held = c.ownerIndex >= 0;
			c.dropped = !c.held;
			if (c.held) {
				for (const SourceFilmmaker::PlayerState& p : players)
					if (p.index == c.ownerIndex) c.ownerName = p.name;
				const auto owner = byIndex.find(c.ownerIndex);
				if (owner != byIndex.end()) {
					c.team = owner->second->team;
					c.current = owner->second->activeWeaponIndex == e.index;
				}
			}
			c.name = c.isBomb ? "C4" : WeaponName(e.className);
			c.alive = true;
			// CS2's wording, middle dots and all.
			if (c.held)
				c.status = std::string(c.current ? "current \xC2\xB7 " : "inventory \xC2\xB7 ") + (c.isBomb ? "C4 carried by " : "held by ")
					+ (c.ownerName.empty() ? "player" : c.ownerName);
			else
				c.status = c.isBomb ? "C4 on the ground" : "dropped on floor";
			c.distance = FollowDistance(from, Vec(e.origin));
			if (attachPoints) {
				c.attachPoints = FollowHost_AttachPoints(e.index, FollowTargetType::Weapon);
				c.attachments = AttachPointIds(c.attachPoints);
			}
			out.push_back(std::move(c));
		}
		return out;
	}
	for (const EntityState& e : entities) {
		// Spectators and SourceTV (team 0 and 1 in every Source game) are no one to follow.
		if (!IsPlayer(e) || e.team < 2) continue;
		FollowTargetCandidate c;
		c.entityIndex = e.index;
		c.handle = e.handle != 0xFFFFFFFFu ? e.handle : 0;
		c.name = "Player";
		for (const SourceFilmmaker::PlayerState& p : players)
			if (p.index == e.index && !p.name.empty()) c.name = p.name;
		c.className = e.className;
		c.team = e.team;
		c.alive = e.alive;
		c.status = e.alive ? "alive" : "dead";
		c.distance = FollowDistance(from, Vec(e.alive ? e.eye : e.origin));
		if (attachPoints) {
			c.attachPoints = FollowHost_AttachPoints(e.index, FollowTargetType::Player);
			c.attachments = AttachPointIds(c.attachPoints);
		}
		out.push_back(std::move(c));
	}
	return out;
}

int FollowHost_SpectatedPlayer() {
	return SourceFilmmaker::Players_ObserverMode() == SourceFilmmaker::Ui::DirectorMath::kInEye
		? SourceFilmmaker::Players_ObservedIndex() : -1;
}

// The player's slot, as CS2 reports it.
int FollowHost_PlayerUserId(int entityIndex) { return entityIndex >= 1 ? entityIndex - 1 : -1; }

const std::vector<GrenadeSighting>& FollowHost_GrenadeSightings() {
	static const std::vector<GrenadeSighting> none;
	return none;
}

std::vector<FollowAttachPoint> FollowHost_AttachPoints(int entityIndex, FollowTargetType type) {
	std::vector<FollowAttachPoint> out;
	EntityState e;
	if (!SourceFilmmaker::Entity_Get(entityIndex, e)) return out;
	out.push_back(MakeAttachPoint("entity", "Entity", "entity", true, 0, "entity-origin"));
	if (type == FollowTargetType::Player && IsPlayer(e))
		out.push_back(MakeAttachPoint("eyes", "Eyes", "virtual", true, -1, "player-eye"));
	return out;
}

uint64_t FollowHost_EntityHandle(int entityIndex) {
	EntityState e;
	return SourceFilmmaker::Entity_Get(entityIndex, e) && e.handle != 0xFFFFFFFFu ? e.handle : 0;
}

bool FollowHost_EntityOrigin(int entityIndex, FollowVec3& origin, bool& isPlayer) {
	EntityState e;
	if (!SourceFilmmaker::Entity_Get(entityIndex, e) || e.dormant) return false;
	origin = Vec(e.origin);
	isPlayer = IsPlayer(e);
	return IsFiniteOrigin(origin);
}

bool FollowHost_LivePlayerItems(int entityIndex, int& activeWeaponIndex, int& bombIndex) {
	EntityState e;
	if (!SourceFilmmaker::Entity_Get(entityIndex, e) || !IsPlayer(e) || !e.alive) return false;
	activeWeaponIndex = e.activeWeaponIndex;
	bombIndex = CarriedBomb(entityIndex);
	return true;
}

FollowMountDescription FollowHost_DescribeMount(int entityIndex, FollowTargetType type, const std::string&,
	FollowPlayerPose*, bool) {
	FollowMountDescription d;
	EntityState e;
	if (!SourceFilmmaker::Entity_Get(entityIndex, e)) return d;
	d.entityHandle = e.handle != 0xFFFFFFFFu ? e.handle : 0;
	d.className = e.className;
	d.attachKind = "virtual";
	FollowVec3 p;
	const char* source = "entity-origin";
	d.valid = type == FollowTargetType::Player && IsPlayer(e) ? PlayerPoint(entityIndex, p, &source)
		: !e.dormant && IsFiniteOrigin(Vec(e.origin));
	d.source = source;
	return d;
}

void FollowHost_PrintTargetProbe(int entityIndex) {
	EntityState e;
	if (!SourceFilmmaker::Entity_Get(entityIndex, e)) {
		advancedfx::Message("[followcam][probe] entity=%d: none\n", entityIndex);
		return;
	}
	FollowVec3 p;
	const char* source = "none";
	const bool valid = PlayerPoint(entityIndex, p, &source);
	advancedfx::Message("[followcam][probe] entity=%d class=%s health=%d alive=%d dormant=%d flags=%d origin=(%.3f %.3f %.3f) "
		"eye=(%.3f %.3f %.3f) ragdoll=%d target=%s valid=%d pos=(%.3f %.3f %.3f)\n",
		entityIndex, e.className.c_str(), e.health, e.alive, e.dormant, e.flags, e.origin[0], e.origin[1], e.origin[2],
		e.eye[0], e.eye[1], e.eye[2], e.ragdollIndex, source, valid, p.x, p.y, p.z);
}

void CameraBridge_SetFollowCameraMarker(bool enabled, double x, double y, double z, double, double, double, double) {
	g_aids.marker = enabled;
	g_aids.markerPos[0] = x; g_aids.markerPos[1] = y; g_aids.markerPos[2] = z;
}

void CameraBridge_SetFollowTargetBox(bool enabled, float x, float y, float z, float, float) {
	g_aids.box = enabled;
	g_aids.boxPos[0] = x; g_aids.boxPos[1] = y; g_aids.boxPos[2] = z;
}

} // namespace Filmmaker

namespace SourceFilmmaker {

void FollowCamera_InspectorModel(Ui::FollowInspectorModel& m) {
	m = Ui::FollowInspectorModel();
	JsonValue f;
	if (!Filmmaker::JsonParse(Filmmaker::FollowCameraRef().BuildStateJson(), f)) return;
	m.enabled = Bool(f, "enabled");
	m.hasCamera = Bool(f, "hasCamera");
	m.repositioning = Bool(f, "repositioning");
	m.type = Int(f, "type");
	m.typeName = Str(f, "typeName");
	m.mode = Int(f, "mode");
	m.weaponSource = Int(f, "weaponSource");
	m.weaponPlayerIndex = Int(f, "weaponPlayerIndex", -1);
	m.weaponFilterPlayer = Int(f, "weaponFilterPlayer", -1);
	m.targetIndex = Int(f, "targetIndex", -1);
	m.targetHandle = (uint64_t)Num(f, "targetHandle");
	m.targetValid = Bool(f, "targetValid");
	m.targetName = Str(f, "targetName");
	m.targetDistance = Num(f, "targetDistance");
	m.distanceLevel = Int(f, "distanceLevel");
	Vec3(f, "offset", m.offset);
	Vec3(f, "rotation", m.rotation);
	m.fov = Num(f, "fov", 90);
	m.look = Num(f, "look");
	m.position = Num(f, "position");
	m.prediction = Num(f, "prediction");
	m.deadzone = Num(f, "deadzone");
	m.maxTurn = Num(f, "maxTurn");
	m.deathHold = Num(f, "deathHold");
	m.autoDead = Bool(f, "autoDead");
	m.switchWeapon = Bool(f, "switchWeapon");
	m.switchBomb = Bool(f, "switchBomb");
	m.holdLast = Bool(f, "holdLast");
	m.attachment = Str(f, "attachment");
	m.attachPoints = Points(f, "attachPoints");
	m.message = Str(f, "message");
	m.grenadePending = Bool(f, "grenadePending");
	m.debug = Bool(f, "debug");
	if (const JsonValue* d = f.Find("attachDebug")) {
		m.attachDebugActive = Bool(*d, "active");
		m.attachDebugEntity = Int(*d, "entityIndex", -1);
		m.attachDebugId = Str(*d, "attachId");
		m.attachDebugSource = Str(*d, "source");
		Vec3(*d, "rawTarget", m.attachDebugTarget);
		Vec3(*d, "camera", m.attachDebugCamera);
		m.attachDebugDistance = Num(*d, "distance");
		m.attachDebugAim = Num(*d, "aimError");
		m.attachDebugJitter = Num(*d, "jitter");
		m.attachDebugCameraDelta = Num(*d, "cameraDelta");
		m.attachDebugTargetDelta = Num(*d, "targetDelta");
	}
	for (const JsonValue& p : Arr(f, "players")) {
		Ui::FollowInspectorPlayer player;
		player.index = Int(p, "index", -1);
		player.name = Str(p, "name");
		player.team = Str(p, "team");
		player.alive = Bool(p, "alive");
		m.players.push_back(player);
	}
	for (const JsonValue& c : Arr(f, "candidates")) {
		Ui::FollowInspectorCandidate cand;
		cand.index = Int(c, "index", -1);
		cand.handle = (uint64_t)Num(c, "handle");
		cand.name = Str(c, "name");
		cand.className = Str(c, "className");
		cand.team = Str(c, "team");
		cand.status = Str(c, "status");
		cand.grenadeType = Str(c, "grenadeType");
		cand.ownerName = Str(c, "ownerName");
		cand.alive = Bool(c, "alive");
		cand.isWeapon = Bool(c, "isWeapon");
		cand.isBomb = Bool(c, "isBomb");
		cand.dropped = Bool(c, "dropped");
		cand.throwTick = Int(c, "throwTick", -1);
		cand.tickDelta = Int(c, "tickDelta");
		cand.distance = Num(c, "distance");
		cand.distanceLevel = Int(c, "distanceLevel");
		cand.attachPoints = Points(c, "attachPoints");
		for (const JsonValue& a : Arr(c, "attachments")) cand.attachments.push_back(a.AsString());
		m.candidates.push_back(cand);
	}
	m.eventStatus = Int(f, "eventStatus");
	m.eventScanElapsedMs = (unsigned long long)Num(f, "eventScanElapsedMs");
	m.eventError = Str(f, "eventError");
	m.selectedEvent = Int(f, "selectedEvent", -1);
	for (const JsonValue& e : Arr(f, "events")) {
		Ui::FollowInspectorEvent ev;
		ev.index = Int(e, "index", -1);
		ev.tick = Int(e, "tick");
		ev.type = Str(e, "type");
		ev.item = Str(e, "item");
		ev.ownerName = Str(e, "ownerName");
		m.events.push_back(ev);
	}
}

void FollowCamera_RunCommands(const std::vector<std::string>& commands) {
	for (const std::string& command : commands) {
		if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("follow.ui", "%s", command.c_str());
		if (g_VEngineClient) g_VEngineClient->ClientCmd_Unrestricted(command.c_str());
	}
}

void FollowCamera_Frame() { Filmmaker::FollowCameraRef().RunFrame(Ui_EditorOpen()); }

bool FollowCamera_OnView(float& x, float& y, float& z, float& pitch, float& yaw, float& roll, float& fov) {
	Filmmaker::FollowCamera& follow = Filmmaker::FollowCameraRef();
	if (!follow.OwnsView()) return false;
	// The demo clock during playback, interpolated for this frame (as Smooth View reads it).
	WrpGlobals* globals = g_Hook_VClient_RenderView.GetGlobals();
	const float curTime = globals ? globals->curtime_get() : 0.f;
	const double wallDt = globals ? globals->absoluteframetime_get() : 0.0;
	double ax = x, ay = y, az = z, ap = pitch, ayaw = yaw, ar = roll, af = fov;
	const bool attached = follow.ViewSetupAttachOverride(curTime, ax, ay, az, ap, ayaw, ar, af);
	if (!attached && !follow.ViewSetupLockOnOverride(curTime, wallDt, ax, ay, az, ap, ayaw, ar, af)) return false;
	x = (float)ax; y = (float)ay; z = (float)az;
	pitch = (float)ap; yaw = (float)ayaw; roll = (float)ar; fov = (float)af;
	return true;
}

bool FollowCamera_OwnsView() { return Filmmaker::FollowCameraRef().OwnsView(); }

void FollowCamera_PrintHealth() {
	const Filmmaker::FollowCamera& follow = Filmmaker::FollowCameraRef();
	const Filmmaker::FollowCameraState& s = follow.State();
	advancedfx::Message("  follow:     %s, %s %s, target #%d, camera %s, marker %s, box %s\n",
		s.enabled ? "LIVE" : "idle", s.mode == Filmmaker::FollowMode::Attach ? "attached" : "lock-on",
		Filmmaker::FollowTargetTypeName(s.targetType), s.targetEntityIndex, s.hasCamera ? "placed" : "not placed",
		g_aids.marker ? "shown" : "hidden", g_aids.box ? "shown" : "hidden");
}

} // namespace SourceFilmmaker
