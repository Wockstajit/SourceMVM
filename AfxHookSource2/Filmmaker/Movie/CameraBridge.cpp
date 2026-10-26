#include "CameraBridge.h"

#include "../../stdafx.h"
#include "../../CampathDrawer.h"
#include "../../ClientEntitySystem.h"
#include "../../MirvTime.h"
#include "CameraRuntimeInternal.h"
#include "FreeCamStepMath.h"
#include "../../WindowInputHooks.h"
#include "../../../Shared/MirvInput.h"
#include "../Filmmaker.h"
#include "../../../Deps/release/prop/cs2/Source2Client.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

extern CCampathDrawer g_CampathDrawer;
extern SOURCESDK::CS2::ISource2EngineToClient * g_pEngineToClient;
extern MirvInputEx g_MirvInputEx;

namespace Filmmaker {
	void CameraBridge_GetUiCursor(int& x, int& y, bool& lmb, bool& rmb, bool& shift, unsigned& seq) {
		x = g_UiCursorProbe.x; y = g_UiCursorProbe.y;
		POINT pt;
		if (g_UiCursorProbe.hwnd && GetCursorPos(&pt) && ScreenToClient(g_UiCursorProbe.hwnd, &pt)) {
			x = pt.x; y = pt.y;
		}
		lmb = g_UiCursorProbe.lmb;
		rmb = g_UiCursorProbe.rmb;
		shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
		seq = g_UiCursorProbe.seq;
        }
        bool CameraBridge_GetCtrlDown() { return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0; }
        bool CameraBridge_GetAltDown() { return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0; }
        bool  CameraBridge_GetFreeCamEnabled() { return g_MirvInputEx.m_MirvInput->GetCameraControlMode(); }
        void  CameraBridge_SetFreeCamEnabled(bool enable) { g_MirvInputEx.m_MirvInput->SetCameraControlMode(enable); }
        bool  CameraBridge_GetThirdPersonOrbitEnabled() { return g_MirvInputEx.m_MirvInput->GetThirdPersonOrbitMode(); }
        void  CameraBridge_SetThirdPersonOrbitEnabled(bool enable) { g_MirvInputEx.m_MirvInput->SetThirdPersonOrbitMode(enable); }
        bool  CameraBridge_ConsumeThirdPersonOrbit(double& dYaw, double& dPitch) { return g_MirvInputEx.m_MirvInput->ConsumeThirdPersonOrbitDelta(dYaw, dPitch); }
        float CameraBridge_GetFreeCamSpeed() { return (float)g_MirvInputEx.m_MirvInput->GetCamSpeed(); }
	// The last wheel adjustment, for the Config/Camera Editor speed and FOV toast (MovieHud).
	// Main thread only: the wheel handler and the HUD state both run there.
	static int s_adjustSeq = 0, s_adjustKind = 0, s_adjustDir = 0;
	void CameraBridge_NoteFreeCamAdjust(int kind, int dir) { ++s_adjustSeq; s_adjustKind = kind; s_adjustDir = dir; }

	void  CameraBridge_AdjustFreeCamSpeed(int dir) {
		if (dir == 0) return;
		auto* in = g_MirvInputEx.m_MirvInput;
		in->SetCamSpeed(NextFreeCamSpeed(in->GetCamSpeed(), dir));
		CameraBridge_NoteFreeCamAdjust(1, dir);
	}
	void  CameraBridge_AdjustFreeCamFov(int dir) {
		if (dir == 0) return;
		g_MirvInputEx.m_MirvInput->NudgeFov(dir > 0 ? -kFreeCamFovStepDeg : kFreeCamFovStepDeg);
		CameraBridge_NoteFreeCamAdjust(2, dir);
	}
	void  CameraBridge_LastFreeCamAdjust(int& seq, int& kind, int& dir) {
		seq = s_adjustSeq; kind = s_adjustKind; dir = s_adjustDir;
	}
	void  CameraBridge_SetFreeCamSlow(bool slow) {
		constexpr double kSlowFactor = 0.25;
		g_MirvInputEx.m_MirvInput->SetSlowFactor(slow ? kSlowFactor : 1.0);
	}
	float CameraBridge_GetGameCameraFov() { return (float)g_MirvInputEx.LastCameraFov; }

	void CameraBridge_GetCurrentCamera(double outOrigin[3], double outAngles[3], double& outFov) {
		outOrigin[0] = g_MirvInputEx.LastCameraOrigin[0];
		outOrigin[1] = g_MirvInputEx.LastCameraOrigin[1];
		outOrigin[2] = g_MirvInputEx.LastCameraOrigin[2];
		outAngles[0] = g_MirvInputEx.LastCameraAngles[0];
		outAngles[1] = g_MirvInputEx.LastCameraAngles[1];
		outAngles[2] = g_MirvInputEx.LastCameraAngles[2];
		outFov = g_MirvInputEx.LastCameraFov;
	}

	void CameraBridge_GetDemoClock(double& outDemoTime, int& outDemoTick) {
		outDemoTime = 0.0;
		outDemoTick = 0;
		g_MirvTime.GetCurrentDemoTime(outDemoTime);
		g_MirvTime.GetCurrentDemoTick(outDemoTick);
	}

	void CameraBridge_SetCameraPose(double x, double y, double z,
		double pitch, double yaw, double roll, double fov) {
		MirvInput* mi = g_MirvInputEx.m_MirvInput;
		mi->SetTx((float)x); mi->SetTy((float)y); mi->SetTz((float)z);
		mi->SetRx((float)pitch); mi->SetRy((float)yaw); mi->SetRz((float)roll);
		mi->SetFov((float)fov);
		if (CampathDebug() && CameraPathOwnsView()) {
			static unsigned s_bridgePoseLogThrottle = 0;
			if ((s_bridgePoseLogThrottle++ % 15) == 0) {
				advancedfx::Message(
					"[campath][bridge] SetCameraPose queued pos=(%.1f %.1f %.1f) ang=(%.1f %.1f %.1f) fov=%.1f freecam=%d\n",
					x, y, z, pitch, yaw, roll, fov, mi->GetCameraControlMode() ? 1 : 0);
			}
		}
	}

	void CameraBridge_SetPathDrawEnabled(bool enable) { g_CampathDrawer.Draw_set(enable); }

	void CameraBridge_SetPathDrawSuppressed(bool suppressed) {
		g_CampathDrawer.SuppressForCapture_set(suppressed);
	}

	void CameraBridge_SetFollowCameraMarker(bool enabled,
		double x, double y, double z,
		double pitch, double yaw, double roll, double fov) {
		g_CampathDrawer.FollowCameraMarker_set(enabled, CamPathValue(x, y, z, pitch, yaw, roll, fov));
		if (enabled) g_CampathDrawer.Draw_set(true);
	}

	void CameraBridge_SetPathHead(bool enabled, double x, double y, double z) {
		g_CampathDrawer.PathHead_set(enabled, x, y, z);
	}

	void CameraBridge_SetFollowTargetBox(bool enabled,
		float x, float y, float z, float halfWidth, float height) {
		CCampathDrawer::FollowTargetBox box;
		box.enabled = enabled;
		box.x = x; box.y = y; box.z = z;
		box.half = halfWidth; box.height = height;
		// Deliberately does NOT call Draw_set. CameraPath::EnsureDrawState is the single per-frame
		// owner of that master flag and only writes it on a CHANGE, so forcing it true from here
		// would latch the path gizmos on behind its back -- including during a Follow live preview,
		// which StartPreview turns them off for on purpose. EnsureDrawState asks for the highlight
		// instead, via FollowCamera::WantsTargetHighlight().
		g_CampathDrawer.FollowTargetBox_set(box);
	}

	void CameraBridge_SetSceneOverlay(std::shared_ptr<const SceneOverlay> overlay) {
		g_CampathDrawer.SceneOverlay_set(std::move(overlay));
	}

	void CameraBridge_SetMarkerStyle(bool enabled, bool freeze, int highlightIndex) {
		g_CampathDrawer.MarkerStyle_set(enabled, freeze, highlightIndex);
		if (enabled) {
			g_CampathDrawer.SetDrawKeyframeCam(true);
			if (g_CampathDrawer.GetDrawKeyframeIndex() <= 0.0f)
				g_CampathDrawer.SetDrawKeyframeIndex(18.0f);
		}
	}
}
