// CS:S's answers to the follow camera's host (AfxHookSource2/Filmmaker/Movie/FollowCameraHost.h), and
// where the hook runs it. Targets come from the entity adapter: a player is followed at its eyes
// while it lives and at its ragdoll once dead. The eyes are the origin raised by the profile's
// standing or ducking height (SourceEngine.h EntityState::eye). A weapon or the C4 is followed at its
// origin: on the ground where it lies; in a player's hands at the player's hips, where CS:S puts a
// carried weapon's origin (measured in the SourceTV test demo, 2026-10-26: the owner's origin raised
// 31 units). Attached rides a mount: a bone of the player's skeleton (its ragdoll's once dead), a
// weapon model's attachment, or the entity itself. The Events list is the demo index's deaths, item
// pickups and the C4's drops, pickups and plant. Grenades come with the rest of step 11.

#include "stdafx.h"

#include "SourceFollowCamera.h"
#include "SourceCameraPath.h"
#include "SourceDemoBar.h"
#include "SourceDemoIndex.h"
#include "SourceEngine.h"
#include "SourceGameProfile.h"
#include "SourceMainThread.h"
#include "SourceUi.h"
#include "Ui/DirectorMath.h"
#include "Ui/FollowInspector.h"
#include "Ui/PathOverlay.h"

#include "RenderView.h"
#include "WrpGlobals.h"
#include "WrpVEngineClient.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <AfxHookSource2/Filmmaker/Movie/CameraBridge.h>
#include <AfxHookSource2/Filmmaker/Movie/FollowCamera.h>
#include <AfxHookSource2/Filmmaker/Movie/FollowCameraHost.h>
#include <AfxHookSource2/Filmmaker/Platform/JsonParser.h>
#include <Shared/AfxConsole.h>

#include <cstring>
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

// Attached's mounts, CS2's ids on CS:S's models. CS:S's player models have no head, chest or hand
// attachment (their attachments are holsters, feet and the like, read from the models in
// cstrike_pak_dir.vpk on 2026-10-26), so CS2's clip_limit, c4, grenade0 and weapon_hand points are the
// ValveBiped bones here; a weapon's world model has one attachment, muzzle_flash.
// A bone's axes are its own (x runs along the bone), and each ValveBiped bone turns them its own
// way. forward and up name the bone axes (1-based, negative for the opposite way) that point where
// the body faces and up, so a mount's frame faces the way the body does. Measured on a running CT
// in the SourceTV test demo (2026-10-26), against where the player looked: every pair lined up
// within 40 degrees and made a right-handed frame.
struct Mount {
	const char* id;
	const char* label;
	const char* kind;
	const char* bone;       // a bone of the model, or
	const char* attachment; // an attachment of it; neither: the entity or a virtual point
	int forward, up;        // a bone's axes for the mount's forward and up
};
const Mount kPlayerMounts[] = {
	{ "entity", "Entity", "entity", nullptr, nullptr, 0, 0 },
	{ "eyes", "Eyes", "virtual", nullptr, nullptr, 0, 0 },
	{ "head", "Head", "body-mount", "ValveBiped.Bip01_Head1", nullptr, -2, 1 },
	{ "chest", "Chest", "body-mount", "ValveBiped.Bip01_Spine2", nullptr, 2, 1 },
	{ "pelvis", "Pelvis", "body-mount", "ValveBiped.Bip01_Pelvis", nullptr, 3, 2 },
	{ "feet", "Feet", "virtual", nullptr, nullptr, 0, 0 },
	{ "weapon_hand_R", "Right hand", "attachment", "ValveBiped.Bip01_R_Hand", nullptr, 1, -3 },
	{ "weapon_hand_L", "Left hand", "attachment", "ValveBiped.Bip01_L_Hand", nullptr, 1, 3 },
};
const Mount kWeaponMounts[] = {
	{ "entity", "Entity", "entity", nullptr, nullptr, 0, 0 },
	{ "muzzle", "Muzzle", "attachment", nullptr, "muzzle_flash", 0, 0 },
	{ "muzzle_flash", "Muzzle flash", "attachment", nullptr, "muzzle_flash", 0, 0 },
};

const Mount* FindMount(const Mount* mounts, size_t count, const std::string& id) {
	for (size_t i = 0; i < count; ++i)
		if (id == mounts[i].id) return &mounts[i];
	return nullptr;
}

// Where a mount is this frame. A bone's frame is its matrix: x along the bone, y and z across it.
struct MountPose {
	bool valid = false;
	bool oriented = false;
	Filmmaker::FollowVec3 pos;
	Filmmaker::FollowAngles ang;
	int entity = -1;  // whose model it was read from: the player's, or its ragdoll's once dead
	const char* source = "none";
};

Filmmaker::FollowVec3 Axis(const double axes[3][3], int which) {
	const int i = (which < 0 ? -which : which) - 1;
	const double sign = which < 0 ? -1.0 : 1.0;
	return Filmmaker::FollowVec3{ sign * axes[i][0], sign * axes[i][1], sign * axes[i][2] };
}

MountPose BonePose(int entity, const Mount& mount) {
	MountPose m;
	double pos[3], axes[3][3];
	if (!SourceFilmmaker::Entity_Bone(entity, mount.bone, pos, axes)) return m;
	m.valid = true;
	m.oriented = true;
	m.pos = Vec(pos);
	m.ang = Filmmaker::FollowAnglesFromBasis(Axis(axes, mount.forward), Axis(axes, mount.up));
	m.entity = entity;
	m.source = "bone";
	return m;
}

MountPose ResolveMount(int index, Filmmaker::FollowTargetType type, const std::string& id) {
	MountPose m;
	EntityState e;
	if (!SourceFilmmaker::Entity_Get(index, e) || e.dormant) return m;
	if (type == Filmmaker::FollowTargetType::Player && IsPlayer(e)) {
		const Mount* mount = FindMount(kPlayerMounts, sizeof(kPlayerMounts) / sizeof(kPlayerMounts[0]), id.empty() ? "eyes" : id);
		if (!mount) return m;
		// A dead player is its ragdoll: the same skeleton, falling. Never the dead player's eyes.
		int body = index;
		EntityState ragdoll;
		if (!e.alive) {
			if (e.ragdollIndex < 0 || !SourceFilmmaker::Entity_Get(e.ragdollIndex, ragdoll) || ragdoll.dormant) return m;
			body = e.ragdollIndex;
		}
		if (mount->bone) return BonePose(body, *mount);
		const EntityState& b = e.alive ? e : ragdoll;
		if (0 == std::strcmp(mount->id, "eyes")) {
			if (!e.alive) return BonePose(body, kPlayerMounts[2]); // the head
			m.pos = Vec(e.eye);
			m.ang = Filmmaker::FollowAngles{ e.eyeAngles[0], e.eyeAngles[1], 0.0 };
			m.source = "player-eye";
		} else if (0 == std::strcmp(mount->id, "feet")) {
			m.pos = Vec(b.origin);
			m.ang = Filmmaker::FollowAngles{ 0.0, e.alive ? e.eyeAngles[1] : b.angles[1], 0.0 };
			m.source = "player-feet";
		} else {
			m.pos = Vec(b.origin);
			m.ang = Filmmaker::FollowAngles{ b.angles[0], b.angles[1], b.angles[2] };
			m.source = "entity-origin";
		}
		m.valid = Filmmaker::IsFiniteOrigin(m.pos);
		m.oriented = m.valid;
		m.entity = body;
		return m;
	}
	// A weapon, the C4 or a grenade: its own model's attachment, else the entity itself.
	const Mount* mount = FindMount(kWeaponMounts, sizeof(kWeaponMounts) / sizeof(kWeaponMounts[0]), id.empty() ? "entity" : id);
	if (mount && mount->attachment) {
		double pos[3], ang[3];
		if (SourceFilmmaker::Entity_Attachment(index, mount->attachment, pos, ang)) {
			m.valid = m.oriented = true;
			m.pos = Vec(pos);
			m.ang = Filmmaker::FollowAngles{ ang[0], ang[1], ang[2] };
			m.entity = index;
			m.source = "model-attachment";
			return m;
		}
	}
	// No such point on this model (a knife has no muzzle): its own root, as CS2 falls back.
	m.pos = Vec(e.origin);
	m.ang = Filmmaker::FollowAngles{ e.angles[0], e.angles[1], e.angles[2] };
	m.valid = m.oriented = Filmmaker::IsFiniteOrigin(m.pos);
	m.entity = index;
	m.source = mount && mount->attachment ? "fallback-entity-transform" : "entity-transform";
	return m;
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

// Attached on a mount (CS2's AttachmentTargetProvider): the mount's position and orientation.
class AttachmentProvider final : public Filmmaker::IFollowTargetProvider {
public:
	AttachmentProvider(int index, Filmmaker::FollowTargetType type, std::string mount)
		: m_index(index), m_type(type), m_mount(std::move(mount)) {}
	bool IsValid() override { return ResolveMount(m_index, m_type, m_mount).valid; }
	Filmmaker::FollowVec3 GetWorldPosition() override { return ResolveMount(m_index, m_type, m_mount).pos; }
	bool GetWorldTransform(Filmmaker::FollowVec3& pos, Filmmaker::FollowAngles& ang) override {
		const MountPose m = ResolveMount(m_index, m_type, m_mount);
		pos = m.pos;
		ang = m.ang;
		return m.valid && m.oriented;
	}
	std::string GetDisplayName() override {
		EntityState e;
		if (!SourceFilmmaker::Entity_Get(m_index, e)) return "Entity";
		return IsPlayer(e) ? PlayerName(m_index) : (IsWeaponOrBomb(e) ? WeaponName(e.className) : e.className);
	}
	Filmmaker::FollowTargetStatus GetStatus() override {
		EntityState e;
		if (!SourceFilmmaker::Entity_Get(m_index, e)) return Filmmaker::FollowTargetStatus::Missing;
		if (m_type == Filmmaker::FollowTargetType::Player && IsPlayer(e))
			return e.alive ? Filmmaker::FollowTargetStatus::Active : Filmmaker::FollowTargetStatus::Dead;
		return IsValid() ? Filmmaker::FollowTargetStatus::Active : Filmmaker::FollowTargetStatus::Missing;
	}
	int EntityIndex() const override { return m_index; }
private:
	int m_index;
	Filmmaker::FollowTargetType m_type;
	std::string m_mount;
};

// A player's weapon in hand, whichever it is (CS2's HeldWeaponProvider): it re-reads the active
// weapon every frame, so the camera stays on the player's gun through a switch. With a mount, the
// mount on that gun (its muzzle), retried on each gun it switches to.
class HeldWeaponProvider final : public Filmmaker::IFollowTargetProvider {
public:
	HeldWeaponProvider(int playerIndex, std::string mount) : m_player(playerIndex), m_mount(std::move(mount)) {}
	int ActiveWeapon() const {
		EntityState p;
		return SourceFilmmaker::Entity_Get(m_player, p) && IsPlayer(p) ? p.activeWeaponIndex : -1;
	}
	bool IsValid() override {
		const int index = ActiveWeapon();
		return index >= 0 && ResolveMount(index, Filmmaker::FollowTargetType::Weapon, m_mount).valid;
	}
	Filmmaker::FollowVec3 GetWorldPosition() override {
		return ResolveMount(ActiveWeapon(), Filmmaker::FollowTargetType::Weapon, m_mount).pos;
	}
	bool GetWorldTransform(Filmmaker::FollowVec3& pos, Filmmaker::FollowAngles& ang) override {
		const MountPose m = ResolveMount(ActiveWeapon(), Filmmaker::FollowTargetType::Weapon, m_mount);
		pos = m.pos;
		ang = m.ang;
		return m.valid && m.oriented;
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
	std::string m_mount;
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

// The placed camera's marker and the target box, for the path overlay (FollowCamera_Overlay).
struct Aids {
	bool marker = false;
	double markerPose[7] = { 0, 0, 0, 0, 0, 0, 90 }; // x y z, pitch yaw roll, fov
	bool box = false;
	float boxPos[3] = { 0, 0, 0 };
	float boxHalf = 0, boxHeight = 0;
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

// As CS2 chooses (FollowCameraHostSource2.cpp): Attached rides a mount; Lock-on watches a player's
// eyes or an entity; a weapon is the player's gun in hand (Held, or Auto with a player picked).
std::unique_ptr<IFollowTargetProvider> FollowHost_MakeProvider(const FollowCameraState& state, FollowPlayerPose*) {
	const bool attach = state.mode == FollowMode::Attach;
	if (state.targetType == FollowTargetType::Player) {
		if (attach) return std::make_unique<AttachmentProvider>(state.targetEntityIndex, FollowTargetType::Player, state.attachmentName);
		return std::make_unique<PlayerProvider>(state.targetEntityIndex);
	}
	if (state.targetType == FollowTargetType::Weapon) {
		const bool held = state.weaponSource == WeaponSource::Held
			|| (state.weaponSource == WeaponSource::Auto && state.weaponPlayerIndex >= 0);
		if (held && state.weaponPlayerIndex >= 0)
			return std::make_unique<HeldWeaponProvider>(state.weaponPlayerIndex, attach ? state.attachmentName : std::string());
		if (attach && !state.attachmentName.empty())
			return std::make_unique<AttachmentProvider>(state.targetEntityIndex, FollowTargetType::Weapon, state.attachmentName);
	}
	if (attach) return std::make_unique<AttachmentProvider>(state.targetEntityIndex, state.targetType, state.attachmentName);
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
	const bool player = type == FollowTargetType::Player && IsPlayer(e);
	const Mount* mounts = player ? kPlayerMounts : kWeaponMounts;
	const size_t count = player ? sizeof(kPlayerMounts) / sizeof(kPlayerMounts[0])
		: (type == FollowTargetType::Grenade || IsBomb(e) ? 1 : sizeof(kWeaponMounts) / sizeof(kWeaponMounts[0]));
	for (size_t i = 0; i < count; ++i) {
		const Mount& m = mounts[i];
		const MountPose pose = ResolveMount(entityIndex, type, m.id);
		// A point the model lacks is listed unavailable; the entity and virtual points always resolve.
		const bool valid = pose.valid && (m.bone || m.attachment ? 0 != std::strcmp(pose.source, "fallback-entity-transform") : true);
		out.push_back(MakeAttachPoint(m.id, m.label, m.kind, valid, m.bone || m.attachment ? (int)i : -1,
			m.bone ? "bone" : (m.attachment ? "model-attachment" : pose.source)));
	}
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

FollowMountDescription FollowHost_DescribeMount(int entityIndex, FollowTargetType type, const std::string& mount,
	FollowPlayerPose*, bool) {
	FollowMountDescription d;
	EntityState e;
	if (!SourceFilmmaker::Entity_Get(entityIndex, e)) return d;
	d.entityHandle = e.handle != 0xFFFFFFFFu ? e.handle : 0;
	d.className = e.className;
	const bool player = type == FollowTargetType::Player && IsPlayer(e);
	const Mount* m = player ? FindMount(kPlayerMounts, sizeof(kPlayerMounts) / sizeof(kPlayerMounts[0]), mount.empty() ? "eyes" : mount)
		: FindMount(kWeaponMounts, sizeof(kWeaponMounts) / sizeof(kWeaponMounts[0]), mount.empty() ? "entity" : mount);
	d.attachKind = m ? m->kind : "entity";
	const MountPose pose = ResolveMount(entityIndex, type, mount);
	d.valid = pose.valid;
	d.source = pose.source;
	return d;
}

// Every mount of the target: where it resolves, its angles, and a bone's raw axes, against where the
// player looks, so a mount's frame can be checked against the model on screen.
void FollowHost_PrintTargetProbe(int entityIndex) {
	EntityState e;
	if (!SourceFilmmaker::Entity_Get(entityIndex, e)) {
		advancedfx::Message("[followcam][probe] entity=%d: none\n", entityIndex);
		return;
	}
	const bool player = IsPlayer(e);
	advancedfx::Message("[followcam][probe] entity=%d class=%s health=%d alive=%d dormant=%d flags=%d origin=(%.2f %.2f %.2f) "
		"eye=(%.2f %.2f %.2f) eyeAngles=(%.2f %.2f) ragdoll=%d\n", entityIndex, e.className.c_str(), e.health, e.alive, e.dormant,
		e.flags, e.origin[0], e.origin[1], e.origin[2], e.eye[0], e.eye[1], e.eye[2], e.eyeAngles[0], e.eyeAngles[1], e.ragdollIndex);
	const Mount* mounts = player ? kPlayerMounts : kWeaponMounts;
	const size_t count = player ? sizeof(kPlayerMounts) / sizeof(kPlayerMounts[0]) : sizeof(kWeaponMounts) / sizeof(kWeaponMounts[0]);
	for (size_t i = 0; i < count; ++i) {
		const MountPose m = ResolveMount(entityIndex, player ? FollowTargetType::Player : FollowTargetType::Weapon, mounts[i].id);
		advancedfx::Message("[followcam][probe]   %-14s valid=%d source=%s from=%d pos=(%.2f %.2f %.2f) ang=(%.2f %.2f %.2f)\n",
			mounts[i].id, m.valid, m.source, m.entity, m.pos.x, m.pos.y, m.pos.z, m.ang.pitch, m.ang.yaw, m.ang.roll);
		double pos[3], axes[3][3];
		if (mounts[i].bone && SourceFilmmaker::Entity_Bone(m.entity >= 0 ? m.entity : entityIndex, mounts[i].bone, pos, axes))
			advancedfx::Message("[followcam][probe]   %-14s bone x=(%.3f %.3f %.3f) y=(%.3f %.3f %.3f) z=(%.3f %.3f %.3f)\n",
				"", axes[0][0], axes[0][1], axes[0][2], axes[1][0], axes[1][1], axes[1][2], axes[2][0], axes[2][1], axes[2][2]);
	}
}

void CameraBridge_SetFollowCameraMarker(bool enabled, double x, double y, double z, double pitch, double yaw, double roll, double fov) {
	g_aids.marker = enabled;
	const double pose[7] = { x, y, z, pitch, yaw, roll, fov };
	for (int i = 0; i < 7; ++i) g_aids.markerPose[i] = pose[i];
}

void CameraBridge_SetFollowTargetBox(bool enabled, float x, float y, float z, float halfWidth, float height) {
	g_aids.box = enabled;
	g_aids.boxPos[0] = x; g_aids.boxPos[1] = y; g_aids.boxPos[2] = z;
	g_aids.boxHalf = halfWidth;
	g_aids.boxHeight = height;
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

bool FollowCamera_Repositioning() { return Filmmaker::FollowCameraRef().Repositioning(); }

void FollowCamera_PlaceReposition() { Filmmaker::FollowCameraRef().PlaceReposition(); }

bool FollowCamera_OnKey(int vk, bool down) {
	if (vk != VK_ESCAPE || !FollowCamera_Repositioning()) return false;
	if (down) MainThread_Run([] { Filmmaker::FollowCameraRef().CancelReposition(); });
	return true;
}

bool FollowCamera_Overlay(Ui::PathOverlayModel& out, float width, float height) {
	out.followCamera = g_aids.marker;
	if (g_aids.marker) {
		for (int i = 0; i < 3; ++i) {
			out.followMarker.pos[i] = g_aids.markerPose[i];
			out.followMarker.ang[i] = g_aids.markerPose[3 + i];
		}
		out.followMarker.fov = View_ScreenFov(g_aids.markerPose[6], width, height); // the frame it films, as on screen
	}
	out.targetBox = g_aids.box;
	if (g_aids.box) {
		for (int i = 0; i < 3; ++i) out.boxPos[i] = g_aids.boxPos[i];
		out.boxHalf = g_aids.boxHalf;
		out.boxHeight = g_aids.boxHeight;
	}
	return g_aids.marker || g_aids.box;
}

void FollowCamera_PrintHealth() {
	const Filmmaker::FollowCamera& follow = Filmmaker::FollowCameraRef();
	const Filmmaker::FollowCameraState& s = follow.State();
	advancedfx::Message("  follow:     %s, %s %s, target #%d, camera %s, marker %s, box %s\n",
		s.enabled ? "LIVE" : "idle", s.mode == Filmmaker::FollowMode::Attach ? "attached" : "lock-on",
		Filmmaker::FollowTargetTypeName(s.targetType), s.targetEntityIndex, s.hasCamera ? "placed" : "not placed",
		g_aids.marker ? "shown" : "hidden", g_aids.box ? "shown" : "hidden");
}

} // namespace SourceFilmmaker
