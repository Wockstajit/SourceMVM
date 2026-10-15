#include "ViewFxVm.h"

#include "ViewFxVmInternal.h"
#include "../Debug/MvmDebugLog.h"
#include "../../ClientEntitySystem.h"
#include "../../MirvTime.h"
#include "../../../Shared/AfxConsole.h"

#include <atomic>
#include <cmath>

namespace Filmmaker {
namespace ViewFxVmInternal {

std::atomic<int> g_site{ 0 };
std::atomic<bool> g_siteExplicit{ false };
std::atomic<int> g_field{ 1 };
std::atomic<int> g_mode{ 0 };
double g_spinDegPerApply = 0.0;
double g_freeAimPitch = 0.0;
double g_freeAimYaw = 0.0;
std::atomic<double> g_cameraCompX{ 0.0 };
std::atomic<double> g_cameraCompY{ 0.0 };
std::atomic<double> g_cameraCompZ{ 0.0 };
std::atomic<double> g_cameraCompPitch{ 0.0 };
std::atomic<double> g_cameraCompYaw{ 0.0 };
std::atomic<double> g_cameraCompRoll{ 0.0 };
std::atomic<double> g_weaponKickX{ 0.0 };
std::atomic<double> g_weaponKickY{ 0.0 };
std::atomic<double> g_weaponKickZ{ 0.0 };
std::atomic<double> g_weaponKickPitch{ 0.0 };
std::atomic<double> g_weaponKickYaw{ 0.0 };
std::atomic<double> g_weaponKickRoll{ 0.0 };
std::atomic<int> g_lastVmCallerFrame{ -1 };
std::atomic<bool> g_povActive{ false };
std::atomic<int> g_povPawnIndex{ -1 };

} // namespace ViewFxVmInternal

using namespace ViewFxVmInternal;

void ViewFxVm_NoteViewmodelHelper(size_t helperAddr) {
	CalcHook_NoteViewmodelHelper(helperAddr);
}

void ViewFxVm_OnViewmodelCalc() {
	if (g_site.load(std::memory_order_relaxed) == 1)
		SceneTransform_Apply("1/helper-post");
}

void ViewFxVm_MainPump() {
	if (g_site.load(std::memory_order_relaxed) == 2)
		SceneTransform_Apply("2/main-pump");
}

void ViewFxVm_RenderPump() {
	if (g_site.load(std::memory_order_relaxed) == 3)
		SceneTransform_Apply("3/render-pump");
}

void ViewFxVm_SetFreeAim(double pitchDeg, double yawDeg) {
	g_freeAimPitch = pitchDeg;
	g_freeAimYaw = yawDeg;
	if ((pitchDeg != 0.0 || yawDeg != 0.0)
		&& g_mode.load(std::memory_order_relaxed) == 0) {
		g_mode.store(1, std::memory_order_relaxed);
		if (!g_siteExplicit.load(std::memory_order_relaxed)
			&& g_site.load(std::memory_order_relaxed) == 0)
			g_site.store(1, std::memory_order_relaxed);
	}
}

void ViewFxVm_SetPovContext(bool active, int pawnIndex) {
	SceneTransform_UpdatePovState(active, pawnIndex);
}

bool ViewFxVm_EnsureCameraCompensationHook() {
	return CalcHook_EnsureInstalled();
}

void ViewFxVm_SetCameraCompensation(double x, double y, double z,
	double pitchDeg, double yawDeg, double rollDeg) {
	// Every decouple consumer (Handheld, View Dynamics) funnels through here per frame, and the
	// toggles ignore the install result — so a failed lazy install used to leave decoupling ON in
	// the UI while doing nothing, forever. Retry here, throttled, while compensation is actually
	// being requested; log the recovery and the sustained failure so "on but inert" is visible.
	const bool wantsComp = x != 0.0 || y != 0.0 || z != 0.0
		|| pitchDeg != 0.0 || yawDeg != 0.0 || rollDeg != 0.0;
	// Joined camera/viewmodel delivery diagnostic: this function runs once per CAMERA frame, so a
	// viewmodel that skipped frames shows up as g_lastVmCallerFrame falling behind. Logged only
	// while compensation is live -- that is exactly when a missed viewmodel frame produces the
	// one-frame "gun lags the camera" artifact the jitter reports describe.
	if (wantsComp && MvmDebugLog_Active() && CalcHook_Installed()) {
		const int camFrame = g_MirvTime.framecount_get();
		const int lastVmFrame = g_lastVmCallerFrame.load(std::memory_order_relaxed);
		static int s_prevCamFrame = -1;
		static unsigned long long s_missedFrames = 0;
		if (s_prevCamFrame >= 0 && camFrame == s_prevCamFrame + 1
			&& lastVmFrame >= 0 && lastVmFrame < s_prevCamFrame) {
			++s_missedFrames;
			static unsigned s_gapThrottle = 0;
			if ((s_gapThrottle++ % 8) == 0)
				MvmDebugLog_LinefAlways("viewfx.vm.gap",
					"viewmodel did NOT run on camera frame %d (lastVmFrame=%d, missedTotal=%llu)",
					s_prevCamFrame, lastVmFrame, s_missedFrames);
		}
		s_prevCamFrame = camFrame;
	}
	if (wantsComp && !CalcHook_Installed()) {
		static unsigned s_retryThrottle = 0;
		if ((s_retryThrottle++ % 128) == 0) {
			if (CalcHook_EnsureInstalled()) {
				advancedfx::Message("[viewfxvm] viewmodel hook installed on retry; "
					"camera compensation is now live.\n");
			} else if (MvmDebugLog_Active()) {
				MvmDebugLog_LinefAlways("viewfx.vm.hook",
					"compensation requested but CalcViewModelView hook NOT installed (retrying)");
			}
		}
	}
	g_cameraCompX.store(x, std::memory_order_relaxed);
	g_cameraCompY.store(y, std::memory_order_relaxed);
	g_cameraCompZ.store(z, std::memory_order_relaxed);
	g_cameraCompPitch.store(pitchDeg, std::memory_order_relaxed);
	g_cameraCompYaw.store(yawDeg, std::memory_order_relaxed);
	g_cameraCompRoll.store(rollDeg, std::memory_order_relaxed);
	if (MvmDebugLog_Active()) {
		static unsigned s_compThrottle = 0;
		const bool large = std::fabs(x) > 4.0 || std::fabs(y) > 4.0 || std::fabs(z) > 4.0
			|| std::fabs(pitchDeg) > 8.0 || std::fabs(yawDeg) > 8.0 || std::fabs(rollDeg) > 8.0;
		const unsigned interval = large ? 5u : 15u;
		if ((s_compThrottle++ % interval) == 0)
			MvmDebugLog_LinefAlways("viewfx.vm.camera_comp",
				"frame=%d requestedPos=(%.3f %.3f %.3f) requestedAng=(%.3f %.3f %.3f) large=%d",
				g_MirvTime.framecount_get(), x, y, z, pitchDeg, yawDeg, rollDeg, large ? 1 : 0);
	}
}

void ViewFxVm_SetWeaponKick(double x, double y, double z,
	double pitchDeg, double yawDeg, double rollDeg) {
	g_weaponKickX.store(x, std::memory_order_relaxed);
	g_weaponKickY.store(y, std::memory_order_relaxed);
	g_weaponKickZ.store(z, std::memory_order_relaxed);
	g_weaponKickPitch.store(pitchDeg, std::memory_order_relaxed);
	g_weaponKickYaw.store(yawDeg, std::memory_order_relaxed);
	g_weaponKickRoll.store(rollDeg, std::memory_order_relaxed);
}

bool ViewFxVm_RotationActive() {
	return g_mode.load(std::memory_order_relaxed) == 1
		&& g_site.load(std::memory_order_relaxed) != 0;
}

} // namespace Filmmaker
