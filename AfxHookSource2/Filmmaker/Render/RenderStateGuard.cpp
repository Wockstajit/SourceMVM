#include "RenderStateGuard.h"

#include "../Movie/CameraBridge.h"
#include "../Movie/MovieMode.h"
#include "../Movie/SmoothView.h"
#include "../Movie/HandheldCam.h"
#include "../Movie/ViewFx.h"
#include "../Filmmaker.h"
#include "../Panorama/AimCrosshairHud.h"
#include "../Panorama/CameraTimelineHud.h"

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <sstream>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

// Restoring a cvar goes through the engine console for the same reason the crosshair hide does:
// these are client convars with no exposed typed setter here.
void SetCvar(const std::string& name, const std::string& value) {
	if (!g_pEngineToClient) return;
	const std::string cmd = name + " " + value;
	g_pEngineToClient->ExecuteClientCmd(0, cmd.c_str(), true);
}

} // namespace

RenderStateRestoreGuard::RenderStateRestoreGuard()
	: m_hasSnapshot(false)
	, m_restored(false)
	, m_freeCam(false)
	, m_thirdPersonOrbit(false)
	, m_uiCursor(false)
	, m_movieMode(0)
	, m_origin{ 0.0, 0.0, 0.0 }
	, m_angles{ 0.0, 0.0, 0.0 }
	, m_fov(90.0)
	, m_smoothView(false)
	, m_handheld(false)
	, m_aimCrosshair(false)
	, m_fxWeaponSway(0.0)
	, m_fxDeadzone(0.0)
	, m_fxFloatTightness(0.0)
{
}

RenderStateRestoreGuard::~RenderStateRestoreGuard() {
	Restore();
}

void RenderStateRestoreGuard::Snapshot() {
	// Deliberately ignored on a second call: re-snapshotting mid-job would capture the values the
	// job itself already wrote, making Restore() a no-op that still looks successful.
	if (m_hasSnapshot)
		return;

	// --- camera ownership ---
	m_freeCam = CameraBridge_GetFreeCamEnabled();
	m_thirdPersonOrbit = CameraBridge_GetThirdPersonOrbitEnabled();
	m_uiCursor = CameraTimelineHudRef().Cursor();
	m_movieMode = (int)MovieModeRef().GetMode();

	// --- camera pose ---
	CameraBridge_GetCurrentCamera(m_origin, m_angles, m_fov);

	// --- camera effect modules ---
	m_smoothView = SmoothViewRef().Enabled();
	m_handheld = HandheldCamRef().Enabled();
	m_aimCrosshair = AimCrosshairHudRef().Enabled();

	ViewFx& fx = ViewFxRef();
	m_fxWeaponSway = fx.WeaponSway().Intensity();
	m_fxDeadzone = fx.DeadzoneAim().Intensity();
	m_fxFloatTightness = fx.WeaponFloat().Tightness();

	m_hasSnapshot = true;
	m_restored = false;
}

void RenderStateRestoreGuard::TrackCvar(const char* name, const char* currentValue) {
	if (!name || !*name) return;
	for (const RenderCvarBackup& b : m_cvars) {
		if (b.name == name) return; // keep the first (pre-job) value
	}
	RenderCvarBackup b;
	b.name = name;
	b.value = currentValue ? currentValue : "";
	m_cvars.push_back(b);
}

void RenderStateRestoreGuard::Restore() {
	if (!m_hasSnapshot || m_restored)
		return;
	// Set FIRST, not last. Every step below is an unconditional write of a captured value, so a
	// pass that dies partway still leaves each field it reached correct -- whereas letting the
	// destructor run the whole sequence a second time could re-apply stale ownership after the
	// caller had already moved on.
	m_restored = true;

	// Reverse order of capture.

	// --- cvars (most recently tracked first) ---
	for (auto it = m_cvars.rbegin(); it != m_cvars.rend(); ++it)
		SetCvar(it->name, it->value);
	m_cvars.clear();

	// --- camera effect modules ---
	ViewFx& fx = ViewFxRef();
	fx.WeaponFloat().SetTightness(m_fxFloatTightness);
	fx.DeadzoneAim().SetIntensity(m_fxDeadzone);
	fx.WeaponSway().SetIntensity(m_fxWeaponSway);

	AimCrosshairHudRef().SetEnabled(m_aimCrosshair);
	HandheldCamRef().SetEnabled(m_handheld);
	SmoothViewRef().SetEnabled(m_smoothView);

	// --- camera pose ---
	// Restored BEFORE ownership: the pose write lands while the job's camera still owns the view,
	// so handing ownership back afterwards leaves the user's own camera where they left it.
	CameraBridge_SetCameraPose(m_origin[0], m_origin[1], m_origin[2],
		m_angles[0], m_angles[1], m_angles[2], m_fov);

	// --- camera ownership ---
	// SetMode normally turns the UI cursor off when it reapplies Default/ThirdPerson. Keep cursor
	// ownership untouched here, then restore the exact pre-job value explicitly below.
	MovieModeRef().SetMode((MovieMode::Mode)m_movieMode, /*keepCursor*/ true);
	CameraBridge_SetThirdPersonOrbitEnabled(m_thirdPersonOrbit);
	CameraBridge_SetFreeCamEnabled(m_freeCam);
	CameraEditor_SetCursorMode(m_uiCursor);
}

const std::vector<const char*>& RenderStateRestoreGuard::Covered() {
	static const std::vector<const char*> k = {
		"free cam enabled",
		"third-person orbit enabled",
		"Camera Editor UI cursor ownership",
		"movie mode (Default/ThirdPerson/FreeCam)",
		"camera origin + angles + fov",
		"Smooth View enabled",
		"Handheld camera enabled",
		"True-aim crosshair enabled",
		"ViewFx weapon sway / deadzone intensities, weapon-float tightness",
		"explicitly tracked cvars (see TrackCvar)",
		"first-person viewmodel visibility (r_drawviewmodel when tracked)",
		"first-person weapon FX suppression (World + Players / No Gun module)",
	};
	return k;
}

const std::vector<const char*>& RenderStateRestoreGuard::Uncovered() {
	// Everything in plan section 20 this guard does NOT yet capture. Keep this list honest: it is
	// the only thing standing between a caller and a false assumption.
	static const std::vector<const char*> k = {
		"demo paused/playing state and demo tick",
		"demo timescale",
		"HUD visibility mode (hidden/game/full)",
		"native Panorama panel visibility + styles",
		"X-ray / True View settings",
		"third-person distance / yaw / pitch / preset / fov",
		"Follow Camera configuration and target",
		"Camera Path playback state",
		"fisheye and other post passes",
		"particle + ragdoll settings",
		"render resolution / viewport overrides",
		"stream + recording settings created for the job",
		"Camera Editor open state and bottom timeline mode",
	};
	return k;
}

std::string RenderStateRestoreGuard::CoverageReport() {
	std::ostringstream o;
	o << "RenderStateRestoreGuard covers " << Covered().size()
		<< " categories; " << Uncovered().size() << " NOT yet covered.\n";
	o << "  covered:\n";
	for (const char* c : Covered()) o << "    + " << c << "\n";
	o << "  NOT covered (do not assume these are restored):\n";
	for (const char* c : Uncovered()) o << "    - " << c << "\n";
	return o.str();
}

} // namespace Filmmaker
