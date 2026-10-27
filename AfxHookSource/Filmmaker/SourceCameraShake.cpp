// CS2's camera shake layers in CS:S, and this hook's answers to their host (CameraShakeHost.h).
//
// The player the view follows in first person is the observed player in eye (SourceTV's camera in
// playback). CS:S's SourceTV demos carry no weapon_fire or player_footstep event: a 10-bot SourceTV
// demo recorded on 2026-10-27 held only hltv_status, player_death, bomb_planted, round_end and
// round_start. So a shot comes from the profile's shot temp entity (TEFireBullets in CS:S), which the
// game sends every client for each shot, for its sounds and tracers: the shooter is its m_iPlayer
// plus one, the gun the shooter's active weapon. Footsteps have no source, so Weapon Bob's step sync
// keeps its own clock. The gun moves through SourceViewModel.cpp's CalcViewModelView hook, as CS2's.
// SourceTV sends no velocity for the watched player, so the body's speed is measured from its origin
// over the demo clock.

#include "stdafx.h"

#include "SourceCameraShake.h"
#include "SourceCameraPath.h"
#include "SourceDirector.h"
#include "SourceEngine.h"
#include "SourceGameProfile.h"
#include "SourceNetvars.h"
#include "SourceSdk2013.h"
#include "SourceViewModel.h"
#include "Ui/DirectorMath.h"
#include "Ui/PathOverlay.h"

#include "AfxStreams.h"
#include "RenderView.h"
#include "WrpGlobals.h"
#include "WrpVEngineClient.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <AfxHookSource2/Filmmaker/Movie/CameraShakeHost.h>
#include <AfxHookSource2/Filmmaker/Movie/CameraShakeStack.h>
#include <AfxHookSource2/Filmmaker/Movie/FollowCamera.h>
#include <AfxHookSource2/Filmmaker/Movie/FollowCameraMath.h>
#include <AfxHookSource2/Filmmaker/Movie/HandheldCam.h>
#include <AfxHookSource2/Filmmaker/Movie/WeaponBob.h>
#include <AfxHookSource2/Filmmaker/Movie/WeaponRecoil.h>
#include <Shared/AfxConsole.h>
#include <Shared/AfxDetours.h>

#include <windows.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern WrpVEngineClient* g_VEngineClient;

namespace {

namespace Sdk2013 = SourceFilmmaker::Sdk2013;
using SourceFilmmaker::EntityState;

// ---- the watched player and its gun ----------------------------------------------------------

int WatchedIndex() {
	return SourceFilmmaker::Players_ObserverMode() == SourceFilmmaker::Ui::DirectorMath::kInEye
		? SourceFilmmaker::Players_ObservedIndex() : -1;
}

int UserIdOf(int index) {
	if (index < 1) return -1;
	std::vector<SourceFilmmaker::PlayerState> players;
	SourceFilmmaker::Players_Snapshot(players);
	for (const SourceFilmmaker::PlayerState& p : players)
		if (p.index == index) return p.userId;
	return -1;
}

// A weapon's class name as CS2's recoil table and gun test read it ("weapon_ak47"), from its network
// class: CS:S names weapons CWeapon<Name> (CWeaponM4A1) or, the older ones, C<Name> (CAK47).
bool WeaponClassName(int weaponIndex, std::string& out) {
	out.clear();
	EntityState w;
	if (weaponIndex < 1 || !SourceFilmmaker::Entity_Get(weaponIndex, w) || w.className.size() < 2) return false;
	std::string token = w.className;
	for (char& c : token) c = (char)std::tolower((unsigned char)c);
	token = token.compare(0, 7, "cweapon") == 0 ? token.substr(7) : token.substr(1);
	if (token.empty()) return false;
	out = "weapon_" + token;
	return true;
}

// ---- the body under the camera ---------------------------------------------------------------
//
// The watched player while its eyes are at the camera, else a player whose eyes are there (an
// attached camera), held across frames as CS2 holds its pawn. Its vertical speed is measured from
// its origin over the demo clock, held over render frames that share a tick.
constexpr double kBodyNearRadius = 96.0;
constexpr double kBodyScanRadius = 24.0;
struct Body {
	int index = -1;
	double lastTime = -1.0;
	double last[3] = { 0, 0, 0 };
	double velocity[3] = { 0, 0, 0 };
} g_body;

bool EyesNear(const EntityState& e, double x, double y, double z, double radius) {
	if (!e.alive || e.dormant) return false;
	const double dx = e.eye[0] - x, dy = e.eye[1] - y, dz = e.eye[2] - z;
	return dx * dx + dy * dy + dz * dz <= radius * radius;
}

bool FindBody(double x, double y, double z, EntityState& out) {
	if (g_body.index >= 1 && SourceFilmmaker::Entity_Get(g_body.index, out) && EyesNear(out, x, y, z, kBodyNearRadius))
		return true;
	const int watched = WatchedIndex();
	if (watched >= 1 && SourceFilmmaker::Entity_Get(watched, out) && EyesNear(out, x, y, z, kBodyNearRadius)) {
		g_body = Body();
		g_body.index = watched;
		return true;
	}
	std::vector<SourceFilmmaker::PlayerState> players;
	SourceFilmmaker::Players_Snapshot(players);
	for (const SourceFilmmaker::PlayerState& p : players) {
		if (p.index == g_body.index) continue;
		if (SourceFilmmaker::Entity_Get(p.index, out) && EyesNear(out, x, y, z, kBodyScanRadius)) {
			g_body = Body();
			g_body.index = p.index;
			return true;
		}
	}
	g_body = Body();
	return false;
}

// ---- shots -------------------------------------------------------------------------------------

typedef void (*PostDataUpdateFn)(void* networkable, int updateType);
PostDataUpdateFn g_truePostDataUpdate = nullptr;
int g_playerOffset = -1;
bool g_done = false;
int g_tries = 0;
std::string g_status = "not installed yet";
unsigned long long g_shots = 0;

// The pivot markers (ShakeHost_DrawPivotOrb), per WeaponPivotOwner.
struct PivotMarker {
	bool shown = false;
	double pos[3] = { 0, 0, 0 };
	double radius = 0;
};
PivotMarker g_pivots[Filmmaker::kWeaponPivotOwnerCount];

// The temp entity's object (its IClientUnknown) and the shooter field in it. Guarded.
int ReadShooter(void* networkable) {
	__try {
		typedef void* (*GetUnknownFn)(void*);
		void* object = ((GetUnknownFn)(*(void***)networkable)[0])(networkable);
		return object ? *(const int*)((const unsigned char*)object + g_playerOffset) + 1 : -1;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return -1;
	}
}

void HookPostDataUpdate(void* networkable, int updateType) {
	g_truePostDataUpdate(networkable, updateType);
	const int shooter = ReadShooter(networkable);
	if (shooter < 1) return;
	++g_shots;
	Filmmaker::CameraShake_OnEvent("weapon_fire", [shooter](Filmmaker::ShakeEventFields& out) {
		out.userId = UserIdOf(shooter);
		EntityState e;
		if (SourceFilmmaker::Entity_Get(shooter, e)) WeaponClassName(e.activeWeaponIndex, out.weapon);
		return out.userId >= 0;
	});
}

void Finish(const std::string& status) {
	g_done = true;
	g_status = status;
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("camera.shake.shots", "%s", status.c_str());
}

// Guarded: the class's create-event function hands out its one temp entity object.
void* TempEntityObject(const Sdk2013::ClientClass* c) {
	__try {
		typedef void* (*CreateEventFn)();
		return c && c->m_pCreateEventFn ? ((CreateEventFn)c->m_pCreateEventFn)() : nullptr;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

const Sdk2013::ClientClass* FindClass(const Sdk2013::ClientClass* classes, const char* name) {
	__try {
		for (const Sdk2013::ClientClass* c = classes; c; c = c->m_pNext)
			if (c->m_pNetworkName && 0 == std::strcmp(c->m_pNetworkName, name)) return c;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return nullptr;
}

} // namespace

// ---- the host -----------------------------------------------------------------------------------

namespace Filmmaker {

bool ShakeHost_DemoLoaded() { return SourceFilmmaker::Demo_State().playing; }

bool ShakeHost_EntitiesLive() { return g_VEngineClient && g_VEngineClient->IsInGame(); }

bool ShakeHost_DemoPaused() { return SourceFilmmaker::Demo_State().paused; }

double ShakeHost_DemoTimescale() {
	const double timescale = SourceFilmmaker::Demo_State().timescale;
	return timescale > 0.0 ? timescale : 1.0;
}

int ShakeHost_DemoTick() {
	const SourceFilmmaker::DemoState demo = SourceFilmmaker::Demo_State();
	return demo.playing ? demo.tick : -1;
}

bool ShakeHost_Recording() { return g_AfxStreams.IsRecording(); }

int ShakeHost_SpectatedUserId() { return UserIdOf(WatchedIndex()); }

bool ShakeHost_SpectatedWeapon(std::string& out) {
	EntityState e;
	const int watched = WatchedIndex();
	return watched >= 1 && SourceFilmmaker::Entity_Get(watched, e) && WeaponClassName(e.activeWeaponIndex, out);
}

// The magazine is not read yet: -1 lets every shot through, as an unreadable count does in CS2.
int ShakeHost_SpectatedClip(int* outWeaponIndex) {
	if (outWeaponIndex) *outWeaponIndex = -1;
	return -1;
}

bool ShakeHost_BodyAt(double x, double y, double z, double outVelocity[3], bool* outOnGround, bool* outDucking) {
	EntityState e;
	if (!FindBody(x, y, z, e)) return false;
	WrpGlobals* globals = g_Hook_VClient_RenderView.GetGlobals();
	const double now = globals ? globals->curtime_get() : 0.0;
	const double dt = now - g_body.lastTime;
	if (g_body.lastTime >= 0.0 && dt > 0.0 && dt < 0.25)
		for (int i = 0; i < 3; ++i) g_body.velocity[i] = (e.origin[i] - g_body.last[i]) / dt;
	else if (!(g_body.lastTime >= 0.0 && dt == 0.0))
		for (int i = 0; i < 3; ++i) g_body.velocity[i] = 0.0;
	if (dt != 0.0 || g_body.lastTime < 0.0) {
		g_body.lastTime = now;
		for (int i = 0; i < 3; ++i) g_body.last[i] = e.origin[i];
	}
	for (int i = 0; i < 3; ++i) outVelocity[i] = g_body.velocity[i];
	if (outOnGround) *outOnGround = (e.flags & 1) != 0;
	if (outDucking) *outDucking = (e.flags & 2) != 0;
	return true;
}

bool ShakeHost_EnsureWeaponChannel(const char** whyNot) {
	if (SourceFilmmaker::ViewModel_Installed()) return true;
	if (whyNot) *whyNot = SourceFilmmaker::ViewModel_Status();
	return false;
}

void ShakeHost_SetWeaponKick(double x, double y, double z, double pitch, double yaw, double roll) {
	SourceFilmmaker::ViewModel_SetWeaponKick(x, y, z, pitch, yaw, roll);
}

void ShakeHost_DrawPivotOrb(WeaponPivotOwner owner, bool enabled, double x, double y, double z, double radius) {
	SourceFilmmaker::CameraShake_SetPivotMarker((int)owner, enabled && radius > 0.0, x, y, z, radius);
}

bool ShakeHost_OverlaysHidden() { return SourceFilmmaker::Director_State().blackout; }

} // namespace Filmmaker

// ---- the hook -----------------------------------------------------------------------------------

namespace SourceFilmmaker {

void CameraShake_Init() {
	if (g_done) return;
	const GameProfile& profile = GameProfile_Current();
	if (!profile.shotTempEntityClass) {
		Finish("off: the game profile names no shot temp entity");
		return;
	}
	const Sdk2013::ClientClass* classes = Engine_ClientClasses();
	const Sdk2013::ClientClass* shot = classes ? FindClass(classes, profile.shotTempEntityClass) : nullptr;
	if (!shot) {
		if (++g_tries > 600) Finish(std::string("NOT installed: no ") + profile.shotTempEntityClass + " client class");
		return;
	}
	g_playerOffset = FindNetvarOffset(classes, profile.shotTempEntityTable, profile.shotTempEntityPlayerProp);
	void* networkable = TempEntityObject(shot);
	if (g_playerOffset < 0 || !networkable) {
		Finish(std::string("NOT installed: ") + profile.shotTempEntityTable + "." + profile.shotTempEntityPlayerProp + " or the temp entity not found");
		return;
	}
	void** slot = &(*(void***)networkable)[Sdk2013::Slot::Networkable_PostDataUpdate];
	if (!AfxDetourPtr((PVOID*)slot, (PVOID)&HookPostDataUpdate, (PVOID*)&g_truePostDataUpdate)) {
		Finish("NOT installed: PostDataUpdate could not be wrapped");
		return;
	}
	char status[160];
	std::snprintf(status, sizeof(status), "shots from %s (%s +0x%x)", profile.shotTempEntityClass,
		profile.shotTempEntityPlayerProp, (unsigned)g_playerOffset);
	Finish(status);
}

void CameraShake_Frame() { Filmmaker::CameraShake_RunFrame(); }

void CameraShake_OnView(float& x, float& y, float& z, float& pitch, float& yaw, float& roll, float& fov,
	bool followOwns, bool cameraOverridden) {
	// CS2's gate (CameraShakeStack.h): the game's own view is a POV, an attached follow camera rides
	// the shake, and a hand-flown free camera, a camera path or a placed Lock-on camera do not.
	Filmmaker::ShakeCamMode mode = Filmmaker::ShakeCamMode::Pov;
	if (followOwns)
		mode = Filmmaker::FollowCameraRef().State().mode == Filmmaker::FollowMode::Attach
			? Filmmaker::ShakeCamMode::Attach : Filmmaker::ShakeCamMode::Other;
	else if (cameraOverridden)
		mode = CameraPath_OwnsView() ? Filmmaker::ShakeCamMode::CameraPath : Filmmaker::ShakeCamMode::FreeCam;
	WrpGlobals* globals = g_Hook_VClient_RenderView.GetGlobals();
	const float x0 = x, y0 = y, z0 = z, pitch0 = pitch, yaw0 = yaw, roll0 = roll;
	Filmmaker::ShakePose decoupled; // Handheld's part, while its viewmodel is decoupled
	Filmmaker::CameraShake_Apply(globals ? globals->curtime_get() : 0.0, mode, x, y, z, pitch, yaw, roll, fov, &decoupled);
	// The gun rides the shake, as in CS2, where the shake moves the eye the viewmodel is built from;
	// a decoupled Handheld's part is taken back out (CS2's camera compensation), in the pre-shake basis.
	const Filmmaker::FollowVec3 comp = Filmmaker::FollowRotateVector(Filmmaker::FollowVec3{ decoupled.fwd, decoupled.right, decoupled.up },
		Filmmaker::FollowAngles{ pitch0, yaw0, roll0 });
	auto turn = [](double d) { return std::remainder(d, 360.0); };
	ViewModel_SetCameraDelta(x - x0 - comp.x, y - y0 - comp.y, z - z0 - comp.z, turn(pitch - pitch0) - decoupled.pitch,
		turn(yaw - yaw0) - decoupled.yaw, turn(roll - roll0) - decoupled.roll);
}

void CameraShake_SetPivotMarker(int owner, bool shown, double x, double y, double z, double radius) {
	if (owner < 0 || owner >= Filmmaker::kWeaponPivotOwnerCount) return;
	PivotMarker& m = g_pivots[owner];
	m.shown = shown;
	m.pos[0] = x;
	m.pos[1] = y;
	m.pos[2] = z;
	m.radius = radius;
}

bool CameraShake_PivotMarkers(Ui::PathOverlayModel& model) {
	bool any = false;
	for (int owner = 0; owner < Filmmaker::kWeaponPivotOwnerCount; ++owner) {
		const PivotMarker& m = g_pivots[owner];
		if (!m.shown) continue;
		Ui::PathOverlayPivot p;
		for (int i = 0; i < 3; ++i) p.pos[i] = m.pos[i];
		p.radius = m.radius;
		const Filmmaker::WeaponPivotColour c = Filmmaker::WeaponPivotOwnerColour((Filmmaker::WeaponPivotOwner)owner);
		p.rgb[0] = c.r;
		p.rgb[1] = c.g;
		p.rgb[2] = c.b;
		model.pivots.push_back(p);
		any = true;
	}
	return any;
}

void CameraShake_PrintHealth() {
	advancedfx::Message("  shakes:     handheld %s, recoil %s, bob %s; %s, %llu shots\n",
		Filmmaker::HandheldCamRef().Enabled() ? "on" : "off", Filmmaker::WeaponRecoilRef().Enabled() ? "on" : "off",
		Filmmaker::WeaponBobRef().Enabled() ? "on" : "off", g_status.c_str(), g_shots);
}

} // namespace SourceFilmmaker
