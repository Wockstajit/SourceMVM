// CS:S's answers to the follow camera's host (AfxHookSource2/Filmmaker/Movie/FollowCameraHost.h), and
// where the hook runs it. Targets come from the entity adapter: a player is followed at its eyes
// while it lives and at its ragdoll once dead. CS:S has no skeleton reads yet, so the eyes are the
// origin raised by the profile's standing or ducking height (SourceEngine.h EntityState::eye).
// Players only for now: weapons, the C4, grenades, Attached's body mounts and the demo's recorded
// events come with the rest of step 11.

#include "stdafx.h"

#include "SourceFollowCamera.h"
#include "SourceCameraPath.h"
#include "SourceDemoBar.h"
#include "SourceEngine.h"
#include "SourceGameProfile.h"
#include "SourceUi.h"
#include "Ui/DirectorMath.h"

#include "RenderView.h"
#include "WrpGlobals.h"
#include "WrpVEngineClient.h"

#include <AfxHookSource2/Filmmaker/Movie/CameraBridge.h>
#include <AfxHookSource2/Filmmaker/Movie/FollowCamera.h>
#include <AfxHookSource2/Filmmaker/Movie/FollowCameraHost.h>
#include <Shared/AfxConsole.h>

#include <memory>
#include <string>
#include <vector>

extern WrpVEngineClient* g_VEngineClient;

namespace {

using SourceFilmmaker::EntityState;

bool IsPlayer(const EntityState& e) {
	const SourceFilmmaker::GameProfile& profile = SourceFilmmaker::GameProfile_Current();
	return profile.isPlayerClass && profile.isPlayerClass(e.className.c_str());
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

// Any other entity, at its origin.
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
		return SourceFilmmaker::Entity_Get(m_index, e) ? e.className : std::string("Entity");
	}
	Filmmaker::FollowTargetStatus GetStatus() override {
		return IsValid() ? Filmmaker::FollowTargetStatus::Active : Filmmaker::FollowTargetStatus::Missing;
	}
	int EntityIndex() const override { return m_index; }
private:
	int m_index;
};

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

// No event scan for CS:S demos yet, so the Events list stays empty.
std::string FollowHost_DemoPath() { return std::string(); }

bool FollowHost_ReadDemoEvents(const std::string&, std::atomic<bool>*, std::vector<FollowEventRecord>& events,
	std::string& helperPath, std::string& error) {
	events.clear();
	helperPath.clear();
	error = "no demo event scan for this game yet";
	return false;
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
	return std::make_unique<EntityProvider>(state.targetEntityIndex);
}

std::vector<FollowTargetCandidate> FollowHost_FindTargets(FollowTargetType type, bool attachPoints, const FollowVec3& from) {
	std::vector<FollowTargetCandidate> out;
	if (type != FollowTargetType::Player) return out;
	std::vector<EntityState> entities;
	SourceFilmmaker::Entities_Snapshot(entities);
	std::vector<SourceFilmmaker::PlayerState> players;
	SourceFilmmaker::Players_Snapshot(players);
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

// The C4 a player carries comes with the weapons.
bool FollowHost_LivePlayerItems(int entityIndex, int& activeWeaponIndex, int& bombIndex) {
	EntityState e;
	if (!SourceFilmmaker::Entity_Get(entityIndex, e) || !IsPlayer(e) || !e.alive) return false;
	activeWeaponIndex = e.activeWeaponIndex;
	bombIndex = -1;
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
