#define _USE_MATH_DEFINES

#include "DeadlockCameraRuntime.h"

#include "DeadlockEngine.h"
#include "DeadlockUi.h"

#include "../WrpConsole.h"
#include "../../Shared/AfxConsole.h"
#include "../../Shared/MirvInput.h"
#include "../Filmmaker/Movie/CameraBridge.h"
#include "../Filmmaker/Deadlock/DeadlockShot.h"
#include "../Filmmaker/MouseCapture.h"
#include "../Filmmaker/Debug/MvmDebugLog.h"
#include "../Filmmaker/Deadlock/DeadlockGraph.h"
#include "../Filmmaker/Deadlock/DeadlockFollowCamera.h"
#include "DeadlockFrame.h"

#include <atomic>
#include <math.h>
#include <windows.h>

namespace Deadlock {

// THE ONE PLACE THIS PREDICATE IS SPELLED OUT. DeadlockMirvInputHost::GetSuspendMirvInput() below
// (a private IMirvInputDependencies override MirvInput itself calls every frame) and
// DeadlockInput.cpp's own key-edge tracking (which needs to know when MirvInput becomes willing to
// accept keys again) both have to ask the identical question; declared ahead of the class so the
// override can delegate to it instead of restating the three conditions a second time.
bool CameraRuntime_MirvInputSuspended() {
	return Deadlock::Ui_ConsoleVisible()
		|| Filmmaker::MouseCapture_IsFree()
		|| Deadlock::Ui_AnyPanelWantsCursor();
}

namespace {

// One pose slot per view owner, stamped with the main-pump count when pushed. Callers push every
// frame while they drive the camera; a slot older than the current pump is dropped, never applied
// late. See DeadlockViewOwnerMath.h.
constexpr int kOwnerSlots = 6;

class DeadlockMirvInputHost : private IMirvInputDependencies {
public:
	DeadlockMirvInputHost() {
		m_input = new MirvInput(this);
		// MirvInput's shared constructor does not initialize its later-added orbit-mode member.
		// Make the Deadlock adapter deterministic before its first Supply_MouseFrameEnd call.
		m_input->SetThirdPersonOrbitMode(false);
	}
	~DeadlockMirvInputHost() { delete m_input; }

	MirvInput* Input() { return m_input; }

	int width = 1920;
	int height = 1080;

	double gameOrigin[3] = { 0, 0, 0 };
	double gameAngles[3] = { 0, 0, 0 };
	double gameFov = 90.0;

	double lastOrigin[3] = { 0, 0, 0 };
	double lastAngles[3] = { 0, 0, 0 };
	double lastFov = 90.0;

	// The owner the render-thread view hook actually resolved and applied LAST, read by
	// CameraRuntime_OwnerName() -- see that declaration's comment for why this is recorded rather
	// than recomputed. Written on the render thread (ApplyOverride), read from wherever a console
	// command happens to run; atomic because those are not guaranteed to be the same thread.
	std::atomic<int> lastAppliedOwner{ (int)ViewOwner::Game };

	OwnedPose slots[kOwnerSlots];

private:
	// Deadlock's version of the CS2 suspend predicate.
	//
	// Every term here is a case where the OS cursor belongs to a panel rather than to the camera.
	// Missing one does not merely feel wrong, it makes that panel's buttons inert while its
	// sliders keep working, because MirvInput consumes the click before Panorama sees it and only
	// the slider path reads our own cursor probe.
	virtual bool GetSuspendMirvInput() override {
		return CameraRuntime_MirvInputSuspended();
	}

	virtual void GetLastCameraData(double& x, double& y, double& z,
	                               double& rX, double& rY, double& rZ, double& fov) override {
		x = lastOrigin[0]; y = lastOrigin[1]; z = lastOrigin[2];
		rX = lastAngles[0]; rY = lastAngles[1]; rZ = lastAngles[2];
		fov = lastFov;
	}

	virtual void GetGameCameraData(double& x, double& y, double& z,
	                               double& rX, double& rY, double& rZ, double& fov) override {
		x = gameOrigin[0]; y = gameOrigin[1]; z = gameOrigin[2];
		rX = gameAngles[0]; rY = gameAngles[1]; rZ = gameAngles[2];
		fov = gameFov;
	}

	virtual double GetInverseScaledFov(double fov) override {
		if (!height) return fov;
		const double engineAspect = (double)width / (double)height;
		const double defaultAspect = 4.0 / 3.0;
		const double ratio = engineAspect / defaultAspect;
		const double t = tan(0.5 * fov * (2.0 * M_PI / 360.0));
		const double halfAngle = atan(t / ratio);
		return 2.0 * halfAngle / (2.0 * M_PI / 360.0);
	}

	MirvInput* m_input = nullptr;
};

DeadlockMirvInputHost* g_host = nullptr;

// Cursor probe, for the graph editor's continuous dragging. Panorama gives in-game HUD JS no
// mouse-move event at all, so this OS-side probe is the only way a drag can be implemented; the
// sequence number is there so the JS can tell "still moving" from "same coordinates twice".
struct UiCursor {
	int x = 0, y = 0;
	bool lmb = false, rmb = false;
	unsigned seq = 0;
} g_uiCursor;

bool g_pathDrawSuppressed = false;

} // namespace

void CameraRuntime_Initialize() {
	if (!g_host) g_host = new DeadlockMirvInputHost();
}

void CameraRuntime_RunFrame(bool demoLoaded) {
	if (demoLoaded || !g_host) return;
	const auto& follow = Filmmaker::FollowCamera_Get();
	const bool hadFollow = follow.on || follow.placed || follow.hero >= 0;
	const bool hadFreeCam = CameraRuntime_FreeCam();
	if (hadFollow) Filmmaker::FollowCamera_Clear();
	CameraRuntime_SetFreeCam(false, "replay unload");
	g_host->Input()->SetThirdPersonOrbitMode(false);
	for (auto& slot : g_host->slots) slot = OwnedPose();

	// Override normally runs only for a claimed view. Drain its pending setters and
	// reset its internal input-on latch into local values, never into engine memory.
	// Otherwise enabling freecam in the next replay can reuse the previous replay's pose.
	float x = 0, y = 0, z = 0, pitch = 0, yaw = 0, roll = 0, fov = 90;
	g_host->Input()->Override(0, x, y, z, pitch, yaw, roll, fov);
	if ((hadFollow || hadFreeCam) && Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_LinefAlways("deadlock.camera.unload",
			"released follow=%d freecam=%d; cleared owner slots and pending input", hadFollow, hadFreeCam);
}

MirvInput* CameraRuntime_Input() {
	return g_host ? g_host->Input() : nullptr;
}

bool CameraRuntime_FreeCam() {
	MirvInput* in = g_host ? g_host->Input() : nullptr;
	return in && in->GetCameraControlMode();
}

bool CameraRuntime_SetFreeCam(bool enable, const char* src) {
	MirvInput* in = g_host ? g_host->Input() : nullptr;
	if (!in) return false;
	const bool was = in->GetCameraControlMode();
	in->SetCameraControlMode(enable);
	if (was != enable && Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("deadlock.freecam", "%s src=%s", enable ? "on" : "off", src ? src : "?");
	return true;
}

void CameraRuntime_SetViewportSize(int width, int height) {
	if (!g_host || width <= 0 || height <= 0) return;
	g_host->width = width;
	g_host->height = height;
}

void CameraRuntime_GetViewportSize(int& width, int& height) {
	width = g_host ? g_host->width : 0;
	height = g_host ? g_host->height : 0;
}

void CameraRuntime_SetGameCamera(const double origin[3], const double angles[3], double fov) {
	if (!g_host) return;
	for (int i = 0; i < 3; ++i) {
		g_host->gameOrigin[i] = origin[i];
		g_host->gameAngles[i] = angles[i];
	}
	g_host->gameFov = fov;
}

void CameraRuntime_SetLastCamera(const double origin[3], const double angles[3], double fov) {
	if (!g_host) return;
	for (int i = 0; i < 3; ++i) {
		g_host->lastOrigin[i] = origin[i];
		g_host->lastAngles[i] = angles[i];
	}
	g_host->lastFov = fov;
}

void CameraRuntime_GetLastCamera(double outOrigin[3], double outAngles[3], double& outFov) {
	if (!g_host) {
		outOrigin[0] = outOrigin[1] = outOrigin[2] = 0.0;
		outAngles[0] = outAngles[1] = outAngles[2] = 0.0;
		outFov = 90.0;
		return;
	}
	for (int i = 0; i < 3; ++i) {
		outOrigin[i] = g_host->lastOrigin[i];
		outAngles[i] = g_host->lastAngles[i];
	}
	outFov = g_host->lastFov;
}

bool CameraRuntime_Override(float deltaSeconds,
                            float& x, float& y, float& z,
                            float& pitch, float& yaw, float& roll, float& fov) {
	if (!g_host) return false;

	// The RESOLVED owner's fresh pose becomes MirvInput's set-values, so path playback, the graph,
	// the follow camera and free-cam input still go through one code path. Every other slot is
	// ignored this frame, and anything stale is dropped: that is what stops the graph overwriting an
	// owning shot, and a pose pushed while nothing claimed the view from jumping in later.
	const unsigned long long pump = Frame_MainCount();
	const ViewOwner owner = CameraRuntime_ResolveOwner();
	OwnedPose& slot = g_host->slots[(int)owner];
	if (OwnedPoseFresh(slot, pump)) {
		MirvInput* in = g_host->Input();
		in->SetTx((float)slot.x);
		in->SetTy((float)slot.y);
		in->SetTz((float)slot.z);
		in->SetRx((float)slot.pitch);
		in->SetRy((float)slot.yaw);
		in->SetRz((float)slot.roll);
		in->SetFov((float)slot.fov);
		// The free camera's pose is a one-shot placement (go to, reposition); the free camera then
		// flies on from it. The other owners push every frame and may be read twice under `phase both`.
		if (owner == ViewOwner::FreeCam) slot.have = false;
	}
	for (int i = 0; i < kOwnerSlots; ++i)
		if (g_host->slots[i].have && g_host->slots[i].stamp != pump) g_host->slots[i].have = false;

	return g_host->Input()->Override(deltaSeconds, x, y, z, pitch, yaw, roll, fov);
}

void CameraRuntime_OnFocus(bool hasFocus) {
	if (!g_host) return;
	g_host->Input()->Supply_Focus(hasFocus);
}

// The CameraBridge's untagged push: a one-shot placement for the free camera.
void CameraRuntime_PushPose(double x, double y, double z,
                            double pitch, double yaw, double roll, double fov) {
	CameraRuntime_PushOwnedPose(ViewOwner::FreeCam, x, y, z, pitch, yaw, roll, fov);
}

void CameraRuntime_PushOwnedPose(ViewOwner owner, double x, double y, double z,
                                 double pitch, double yaw, double roll, double fov) {
	if (!g_host || (int)owner <= 0 || (int)owner >= kOwnerSlots) return;
	OwnedPose& slot = g_host->slots[(int)owner];
	slot.have = true;
	slot.stamp = Frame_MainCount();
	slot.x = x; slot.y = y; slot.z = z;
	slot.pitch = pitch; slot.yaw = yaw; slot.roll = roll; slot.fov = fov;
}

bool CameraRuntime_HasFreshPose(ViewOwner owner) {
	if (!g_host || (int)owner <= 0 || (int)owner >= kOwnerSlots) return false;
	return OwnedPoseFresh(g_host->slots[(int)owner], Frame_MainCount());
}

ViewOwner CameraRuntime_ResolveOwner() {
	ViewOwnerInputs in;
	in.replayAvailable = Engine_DemoTick() >= 0;
	in.shotActive = Filmmaker::Shot_OwnsView();
	in.graphDriving = Filmmaker::Graph_OwnsView();
	// Follow counts only when it is actually producing poses. `FollowCamera_OwnsView()` alone can be
	// true after `follow place` while nothing runs its solve, and the report then named an owner
	// that wrote nothing.
	in.followLive = Filmmaker::FollowCamera_OwnsView() && CameraRuntime_HasFreshPose(ViewOwner::Follow);
	in.freeCam = CameraRuntime_FreeCam();
	in.shotHold = false; // the paused path hold lands with the dolly-path phase
	return ResolveViewOwner(in);
}

void CameraRuntime_RecordAppliedOwner(ViewOwner owner) {
	if (g_host) g_host->lastAppliedOwner.store((int)owner);
}

ViewOwner CameraRuntime_LastAppliedOwner() {
	return g_host ? (ViewOwner)g_host->lastAppliedOwner.load() : ViewOwner::Game;
}

const char* CameraRuntime_OwnerName() {
	// The RECORDED owner, not a fresh CameraRuntime_ResolveOwner() call -- see this function's
	// declaration for why a recomputation from here can disagree with what the view hook actually
	// applied a moment earlier.
	return ViewOwnerName(CameraRuntime_LastAppliedOwner());
}

void CameraRuntime_GetUiCursor(int& x, int& y, bool& lmb, bool& rmb, bool& shift, unsigned& seq) {
	x = g_uiCursor.x;
	y = g_uiCursor.y;
	lmb = g_uiCursor.lmb;
	rmb = g_uiCursor.rmb;
	// Read live rather than tracked: a Shift press that arrives while the game does not have
	// focus never reaches the WndProc, and axis-lock dragging that silently stops working is a
	// worse failure than one extra GetAsyncKeyState per query.
	shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
	seq = g_uiCursor.seq;
}

void CameraRuntime_SetUiCursorSample(int x, int y, bool lmb, bool rmb) {
	g_uiCursor.x = x;
	g_uiCursor.y = y;
	g_uiCursor.lmb = lmb;
	g_uiCursor.rmb = rmb;
	++g_uiCursor.seq;
}

bool CameraRuntime_PathDrawEnabled() {
	// ONE draw flag: the shot's. `path draw`, `mirv_campath draw enabled` and the CameraBridge
	// setter all write it, so there is no second copy here to disagree with the panel.
	return Filmmaker::Shot_DrawPath() && !g_pathDrawSuppressed;
}

void CameraRuntime_SetPathDraw(bool enabled) { Filmmaker::Shot_SetDrawPath(enabled); }
void CameraRuntime_SetPathDrawSuppressed(bool suppressed) { g_pathDrawSuppressed = suppressed; }

} // namespace Deadlock

// ---------------------------------------------------------------------------
// The CameraBridge contract. Declared in Filmmaker/Movie/CameraBridge.h and defined in main.cpp
// for CS2; defined here for Deadlock, so every shared Filmmaker file that drives the camera links
// against this target without modification.
// ---------------------------------------------------------------------------

namespace Filmmaker {

bool CameraBridge_GetFreeCamEnabled() {
	MirvInput* in = Deadlock::CameraRuntime_Input();
	return in && in->GetCameraControlMode();
}

void CameraBridge_SetFreeCamEnabled(bool enable) {
	Deadlock::CameraRuntime_SetFreeCam(enable, "bridge");
}

bool CameraBridge_GetThirdPersonOrbitEnabled() {
	MirvInput* in = Deadlock::CameraRuntime_Input();
	return in && in->GetThirdPersonOrbitMode();
}

void CameraBridge_SetThirdPersonOrbitEnabled(bool enable) {
	if (MirvInput* in = Deadlock::CameraRuntime_Input())
		in->SetThirdPersonOrbitMode(enable);
}

bool CameraBridge_ConsumeThirdPersonOrbit(double& dYawPixels, double& dPitchPixels) {
	MirvInput* in = Deadlock::CameraRuntime_Input();
	if (!in) return false;
	return in->ConsumeThirdPersonOrbitDelta(dYawPixels, dPitchPixels);
}

float CameraBridge_GetFreeCamSpeed() {
	MirvInput* in = Deadlock::CameraRuntime_Input();
	return in ? (float)in->GetCamSpeed() : 0.0f;
}

void CameraBridge_AdjustFreeCamSpeed(int dir) {
	MirvInput* in = Deadlock::CameraRuntime_Input();
	if (!in) return;
	if (dir > 0) in->IncreaseCamSpeed();
	else if (dir < 0) in->DecreaseCamSpeed();
}

void CameraBridge_AdjustFreeCamFov(int dir) {
	MirvInput* in = Deadlock::CameraRuntime_Input();
	if (!in || dir == 0) return;
	double origin[3], angles[3], fov;
	Deadlock::CameraRuntime_GetLastCamera(origin, angles, fov);
	// Scroll up zooms IN, which means a LOWER field of view. One step is five degrees, clamped so
	// a stuck wheel cannot invert or flatten the projection.
	double next = fov - 5.0 * dir;
	if (next < 1.0) next = 1.0;
	if (next > 179.0) next = 179.0;
	in->SetFov((float)next);
}

void CameraBridge_SetFreeCamSlow(bool slow) {
	MirvInput* in = Deadlock::CameraRuntime_Input();
	if (!in) return;
	// Idempotent by construction: the base sensitivity is captured on the first transition into
	// slow and restored on the way out, so repeated true/true calls cannot compound.
	static bool active = false;
	static double baseKeyboard = 0.0;
	static double baseMouse = 0.0;
	if (slow == active) return;
	if (slow) {
		baseKeyboard = in->GetKeyboardSensitivty();
		baseMouse = in->GetMouseSensitivty();
		in->SetKeyboardSensitivity(baseKeyboard * 0.25);
		in->SetMouseSensitivity(baseMouse * 0.25);
	} else {
		in->SetKeyboardSensitivity(baseKeyboard);
		in->SetMouseSensitivity(baseMouse);
	}
	active = slow;
}

float CameraBridge_GetGameCameraFov() {
	double origin[3], angles[3], fov;
	Deadlock::CameraRuntime_GetLastCamera(origin, angles, fov);
	return (float)fov;
}

void CameraBridge_GetCurrentCamera(double outOrigin[3], double outAngles[3], double& outFov) {
	Deadlock::CameraRuntime_GetLastCamera(outOrigin, outAngles, outFov);
}

void CameraBridge_SetCameraPose(double x, double y, double z,
                                double pitch, double yaw, double roll, double fov) {
	Deadlock::CameraRuntime_PushPose(x, y, z, pitch, yaw, roll, fov);
}

void CameraBridge_GetUiCursor(int& x, int& y, bool& lmb, bool& rmb, bool& shift, unsigned& seq) {
	Deadlock::CameraRuntime_GetUiCursor(x, y, lmb, rmb, shift, seq);
}

void CameraBridge_SetPathDrawEnabled(bool enable) {
	Deadlock::CameraRuntime_SetPathDraw(enable);
}

void CameraBridge_SetPathDrawSuppressed(bool suppressed) {
	Deadlock::CameraRuntime_SetPathDrawSuppressed(suppressed);
}

// The three marker/gizmo styling entry points below drive the in-world drawer. Deadlock does not
// have the follow-camera or entity-box features (both need a spectated CS2 pawn), so they are
// accepted and ignored rather than left undefined -- the shared Filmmaker files that reference
// them still have to link.
void CameraBridge_SetFollowCameraMarker(bool, double, double, double,
                                        double, double, double, double) {}

void CameraBridge_SetFollowTargetBox(bool, float, float, float, float, float) {}

void CameraBridge_SetMarkerStyle(bool, bool, int) {}

} // namespace Filmmaker
