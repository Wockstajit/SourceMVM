// CS2's answers to the camera shake stack's host (CameraShakeHost.h). The spectated pawn and its
// gun come from the entity system and schema offsets, the demo clock from the engine, the gun's
// transform channel from ViewFxVm's viewmodel hook, the pivot orbs from the camera path's gizmo
// drawer, and the stack's game events through tier0's KV3 to JSON conversion.

#include "CameraShakeHost.h"
#include "CameraShakeStack.h"
#include "DemoTimescale.h"
#include "HudBlackout.h"
#include "ViewFxVm.h"
#include "../MvmTest.h"  // offline live-match harness: a loaded map with no demo
#include "../Platform/JsonParser.h"
#include "../../CampathDrawer.h" // the world-space gizmo drawer the pivot orbs ride on
#include "../../ClientEntitySystem.h" // AfxGetSpectatedUserId
#include "../Cosmetics/CosmeticEntityAccess.h" // CosmeticEntFromIndex: reach the firing weapon
#include "../../SchemaSystem.h" // C_BasePlayerWeapon::m_iClip1 -- did a bullet actually leave?
#include "../../MirvTime.h"     // GetCurrentDemoTick: the shot de-dup discriminator

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/igameevents.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/tier1/utlstring.h"

#include <string>

// Recorder state, from RenderSystemDX11Hooks.cpp. While a capture runs the oscillator clock is
// locked to the demo dt so the output never depends on wall time.
bool AfxStreams_IsRcording();

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

typedef bool (*SaveKV3AsJSON_t)(const SOURCESDK::CS2::KeyValues3* kv,
	SOURCESDK::CS2::CUtlString* error, SOURCESDK::CS2::CUtlString* output);

SaveKV3AsJSON_t GetSaveKV3AsJSON() {
	static bool s_tried = false;
	static SaveKV3AsJSON_t s_fn = nullptr;
	if (!s_tried) {
		s_tried = true;
		if (HMODULE tier0 = GetModuleHandleA("tier0.dll"))
			s_fn = (SaveKV3AsJSON_t)GetProcAddress(tier0, "?SaveKV3AsJSON@@YA_NPEBVKeyValues3@@PEAVCUtlString@@1@Z");
	}
	return s_fn;
}

// One parse for both fields we care about. `weapon_fire` carries the firing player AND the
// weapon; the KV3->JSON conversion is the expensive part, so reading them separately would pay
// for it twice per shot.
bool ParseFireEvent(SOURCESDK::CS2::CGameEvent* event, ShakeEventFields& out) {
	SaveKV3AsJSON_t fn = GetSaveKV3AsJSON();
	if (!fn) return false;
	SOURCESDK::CS2::CUtlString error;
	SOURCESDK::CS2::CUtlString text;
	if (!fn(event->GetDataKeys(), &error, &text) || !text.Get()) return false;
	JsonValue root;
	if (!JsonParse(text.Get(), root) || root.type != JsonValue::Type::Object) return false;
	if (const JsonValue* v = root.Find("userid")) out.userId = v->AsInt(-1);
	if (const JsonValue* w = root.Find("weapon")) out.weapon = w->AsString("");
	return true;
}

bool DemoIsLoaded() {
	if (!g_pEngineToClient) return false;
	// A PAUSED demo is still an active demo -- IsPlayingDemo() alone flips false on pause, which
	// would drop every spring the moment the user hits pause to compose a shot.
	if (auto pDemo = g_pEngineToClient->GetDemoFile())
		return pDemo->IsPlayingDemo() || pDemo->IsDemoPaused();
	return false;
}

// Is there a map loaded whose entity system can be walked? Demo playback OR the offline
// live-match harness -- and NOT the main menu or a level load, where the reads below are
// undefined. Broader than DemoIsLoaded() on purpose; see the note in CameraShake_RunFrame.
bool EntityContextLive() {
	if (DemoIsLoaded()) return true;
	return MvmTest_Active() && MvmTest_LevelLoaded();
}

// ROUNDS IN THE SPECTATED PLAYER'S MAGAZINE, or -1 when it cannot be read.
//
// This is what makes "a bullet actually left the gun" answerable. CS2 raises weapon_fire off the
// attack input, so a dry click on an empty magazine -- and the trailing clicks after a fast pistol
// burst empties it -- raise the event exactly like a real shot does. The event alone therefore
// cannot tell a shot from a click, and the camera kicked on both.
//
// -1 means "unknown", never "empty": the offset is optional (a CS2 rename resolves it to 0) and a
// missing schema field must not silently swallow every real shot. Callers treat -1 as "allow".
// outWeaponIndex receives the active weapon's entity index (-1 if unresolved) so the caller can
// tell "same gun as the previous shot" from a weapon switch, which must never look like a dry fire.
int SpectatedClipCount(int* outWeaponIndex = nullptr) {
	if (outWeaponIndex) *outWeaponIndex = -1;
	const ptrdiff_t clipOff = g_clientDllOffsets.C_BasePlayerWeapon.m_iClip1;
	if (0 == clipOff) return -1;

	const int pawnIndex = AfxGetSpectatedPawnIndex();
	if (pawnIndex < 0) return -1;
	CEntityInstance* pawn = CosmeticEntFromIndex(pawnIndex);
	if (!pawn) return -1;

	SOURCESDK::CS2::CBaseHandle wh = pawn->GetActiveWeaponHandle();
	if (!wh.IsValid()) return -1;
	const int weaponIndex = wh.GetEntryIndex();
	CEntityInstance* weapon = CosmeticEntFromIndex(weaponIndex);
	if (!weapon) return -1;
	if (outWeaponIndex) *outWeaponIndex = weaponIndex;

	// SEH: a stale entity pointer or a moved offset faults HERE and reads as "unknown" rather
	// than taking the render thread down mid-shot.
	int clip = -1;
	__try {
		clip = *(int*)((unsigned char*)weapon + clipOff);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return -1;
	}
	// Weapons with no magazine (knife) report a negative sentinel; treat as unknown, not empty.
	return clip < 0 ? -1 : clip;
}

// The classname of the gun the spectated player is HOLDING, e.g. "weapon_ak47".
//
// This is a second, independent answer to "which gun is this shot", and it exists because the
// first one is not guaranteed. `weapon_fire`'s `weapon` field is optional in the KV3 payload:
// when it is absent the recoil scope resolver gets an empty token, which matches no slot and no
// type, so every Slot/Type override silently falls through to All. That reads exactly like the
// scopes not working -- only "All weapons" ever appears to do anything.
//
// SEH-guarded for the same reason SpectatedClipCount is: a stale entity pointer or a moved
// offset faults HERE and reads as "unknown" rather than taking the render thread down mid-shot.
// Split in two so the __try half touches nothing that needs unwinding.
bool SpectatedWeaponNameRaw(char* buf, size_t bufSize) {
	buf[0] = 0;
	const int pawnIndex = AfxGetSpectatedPawnIndex();
	if (pawnIndex < 0) return false;
	CEntityInstance* pawn = CosmeticEntFromIndex(pawnIndex);
	if (!pawn) return false;
	SOURCESDK::CS2::CBaseHandle wh = pawn->GetActiveWeaponHandle();
	if (!wh.IsValid()) return false;
	CEntityInstance* weapon = CosmeticEntFromIndex(wh.GetEntryIndex());
	if (!weapon) return false;
	__try {
		const char* name = weapon->GetClassName();
		if (name) {
			size_t n = 0;
			while (n + 1 < bufSize && name[n]) { buf[n] = name[n]; ++n; }
			buf[n] = 0;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		buf[0] = 0;
		return false;
	}
	return 0 != buf[0];
}

bool SpectatedWeaponName(std::string& out) {
	char buf[64];
	if (!SpectatedWeaponNameRaw(buf, sizeof(buf))) { out.clear(); return false; }
	out = buf;
	return true;
}

// ---- the viewed pawn's own movement state -------------------------------------------------
//
// AfxGetSpectatedPawnIndex matches a pawn's render eye to the game camera within 0.2 units, so it
// only answers on frames where the eye has barely moved -- measured, it resolved on about half the
// frames and almost only while standing, which is useless for movement state. The pawn being
// watched does not change from one frame to the next, so this keeps hold of it: the previous pawn
// stays the answer for as long as its eye is still near the camera, and the engine's resolver (or,
// failing that, a scan for the pawn under the camera) is consulted only when it stops being so.
constexpr float kBodyNearRadius = 96.f;    // eye-to-camera distance that still counts as "this pawn"
constexpr float kBodyScanRadius = 24.f;    // a scan must be much stricter: any pawn could be in range
int g_bodyPawn = -1;
unsigned g_bodyScanCooldown = 0;

bool PawnEyeNear(CEntityInstance* pawn, double x, double y, double z, float radius) {
	if (!pawn || !pawn->IsPlayerPawn()) return false;
	float o[3] = {};
	pawn->GetRenderEyeOrigin(o);
	const double dx = o[0] - x, dy = o[1] - y, dz = o[2] - z;
	return dx * dx + dy * dy + dz * dz <= (double)radius * radius;
}

CEntityInstance* ResolveBodyPawn(double x, double y, double z) {
	if (g_bodyPawn >= 0) {
		CEntityInstance* held = CosmeticEntFromIndex(g_bodyPawn);
		if (PawnEyeNear(held, x, y, z, kBodyNearRadius)) return held;
		g_bodyPawn = -1;
	}
	int idx = AfxGetSpectatedPawnIndex();
	CEntityInstance* pawn = idx >= 0 ? CosmeticEntFromIndex(idx) : nullptr;
	if (!PawnEyeNear(pawn, x, y, z, kBodyNearRadius)) {
		pawn = nullptr;
		// A failed scan costs a walk over every player, so a camera that is not on any pawn (an
		// attach shot) only pays for it a few times a second.
		if (g_bodyScanCooldown > 0) { --g_bodyScanCooldown; return nullptr; }
		g_bodyScanCooldown = 12;
		const int highest = GetHighestEntityIndex();
		for (int i = 0; i <= highest && !pawn; ++i) {
			CEntityInstance* ent = CosmeticEntFromIndex(i);
			if (PawnEyeNear(ent, x, y, z, kBodyScanRadius)) { pawn = ent; idx = i; }
		}
		if (!pawn) return nullptr;
	}
	g_bodyPawn = idx;
	return pawn;
}

} // namespace

bool ShakeHost_DemoLoaded() { return DemoIsLoaded(); }

bool ShakeHost_EntitiesLive() { return EntityContextLive(); }

bool ShakeHost_DemoPaused() {
	auto* demo = g_pEngineToClient ? g_pEngineToClient->GetDemoFile() : nullptr;
	return demo && demo->IsDemoPaused();
}

double ShakeHost_DemoTimescale() { return DemoTimescale_Get(); }

int ShakeHost_DemoTick() {
	int tick = -1;
	g_MirvTime.GetCurrentDemoTick(tick);
	return tick;
}

bool ShakeHost_Recording() { return AfxStreams_IsRcording(); }

int ShakeHost_SpectatedUserId() { return AfxGetSpectatedUserId(); }

bool ShakeHost_SpectatedWeapon(std::string& out) { return SpectatedWeaponName(out); }

int ShakeHost_SpectatedClip(int* outWeaponIndex) { return SpectatedClipCount(outWeaponIndex); }

// FL_ONGROUND (bit 0) and FL_DUCKING (bit 1) of m_fFlags, plus m_vecVelocity. SEH-guarded like
// every other raw pawn read: a schema offset that moved in an update faults here and reads as
// "no body", not as a crash.
bool ShakeHost_BodyAt(double x, double y, double z, double outVelocity[3], bool* outOnGround, bool* outDucking) {
	const ptrdiff_t offVel = g_clientDllOffsets.C_BaseEntity.m_vecVelocity;
	const ptrdiff_t offFlags = g_clientDllOffsets.C_BaseEntity.m_fFlags;
	if (!offVel || !offFlags) return false;
	CEntityInstance* pawn = ResolveBodyPawn(x, y, z);
	if (!pawn) return false;
	__try {
		const unsigned char* base = (const unsigned char*)pawn;
		const unsigned flags = *(const unsigned*)(base + offFlags);
		// A living pawn always carries some flag bits (a pawn in the air still reads 0x...80). All
		// zero is a dead or not-yet-spawned pawn -- measured: 632 consecutive frames of flags 0,
		// velocity 0 -- and "not on the ground" would silence the bob for it, so it counts as no body.
		if (flags == 0) return false;
		const float* v = (const float*)(base + offVel);
		outVelocity[0] = v[0]; outVelocity[1] = v[1]; outVelocity[2] = v[2];
		if (outOnGround) *outOnGround = (flags & 1u) != 0;
		if (outDucking) *outDucking = (flags & 2u) != 0;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool ShakeHost_EnsureWeaponChannel(const char** whyNot) {
	if (ViewFxVm_EnsureCameraCompensationHook()) return true;
	if (whyNot) *whyNot = "the viewmodel hook could not install (CalcViewModelView caller not resolved); it retries while enabled";
	return false;
}

void ShakeHost_SetWeaponKick(double x, double y, double z, double pitch, double yaw, double roll) {
	ViewFxVm_SetWeaponKick(x, y, z, pitch, yaw, roll);
}

void ShakeHost_DrawPivotOrb(WeaponPivotOwner owner, bool enabled, double x, double y, double z, double radius) {
	CCampathDrawer::PivotMarker marker;
	marker.enabled = enabled && radius > 0.0;
	marker.x = (float)x;
	marker.y = (float)y;
	marker.z = (float)z;
	marker.radius = marker.enabled ? (float)radius : 0.0f;
	// The colour is the whole point of the split: the drawer paints whatever it is handed, so
	// the owner-to-colour mapping lives in one place (WeaponPivot.h) and both orbs get it here.
	const WeaponPivotColour c = WeaponPivotOwnerColour(owner);
	marker.r = c.r; marker.g = c.g; marker.b = c.b;
	g_CampathDrawer.PivotMarker_set((int)owner, marker);
}

bool ShakeHost_OverlaysHidden() { return HudBlackout_Active(); }

void CameraShake_OnGameEvent(SOURCESDK::CS2::CGameEvent* event) {
	if (!event) return;
	CameraShake_OnEvent(event->GetName(), [event](ShakeEventFields& out) { return ParseFireEvent(event, out); });
}

} // namespace Filmmaker
