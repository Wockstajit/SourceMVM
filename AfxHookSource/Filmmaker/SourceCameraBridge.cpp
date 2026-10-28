// The CS:S hook's answers to the camera bridge (AfxHookSource2/Filmmaker/Movie/CameraBridge.h) that
// CS2's shared path playback (CamPlayback.cpp) uses: HLAE's free camera (mirv_input) flies the path,
// HLAE's view hook says where the camera is, and HLAE's demo helpers give the demo clock. The scrub
// frame cache is CS2's render-side cache; CS:S has none, so a camera tick is the playhead's own.

#include "stdafx.h"

#include "SourceEngine.h"

#include "RenderView.h"

#include <AfxHookSource2/Filmmaker/Movie/CameraBridge.h>
#include <AfxHookSource2/Filmmaker/Movie/ScrubFrameCache.h>
#include <Shared/MirvInput.h>

// HLAE's demo clock (MirvCommands.cpp). GetCurrentDemoTime reads the demo info without a null check,
// so it is called only while a demo plays.
bool GetCurrentDemoTick(int& outTick);
bool GetCurrentDemoTime(double& outDemoTime);

namespace Filmmaker {

namespace {
MirvInput* FreeCam() { return g_Hook_VClient_RenderView.m_MirvInput; }
} // namespace

bool CameraBridge_GetFreeCamEnabled() { return FreeCam() && FreeCam()->GetCameraControlMode(); }

void CameraBridge_SetFreeCamEnabled(bool enable) {
	if (FreeCam() && FreeCam()->GetCameraControlMode() != enable) FreeCam()->SetCameraControlMode(enable);
}

void CameraBridge_GetCurrentCamera(double outOrigin[3], double outAngles[3], double& outFov) {
	const SourceFilmmaker::ViewPose view = SourceFilmmaker::View_Rendered();
	for (int i = 0; i < 3; ++i) {
		outOrigin[i] = view.origin[i];
		outAngles[i] = view.angles[i];
	}
	outFov = view.fov;
}

void CameraBridge_SetCameraPose(double x, double y, double z, double pitch, double yaw, double roll, double fov) {
	MirvInput* mi = FreeCam();
	if (!mi) return;
	mi->SetTx((float)x); mi->SetTy((float)y); mi->SetTz((float)z);
	mi->SetRx((float)pitch); mi->SetRy((float)yaw); mi->SetRz((float)roll);
	mi->SetFov((float)fov);
}

void CameraBridge_GetDemoClock(double& outDemoTime, int& outDemoTick) {
	outDemoTime = 0.0;
	outDemoTick = 0;
	if (!SourceFilmmaker::Demo_State().playing) return;
	GetCurrentDemoTick(outDemoTick);
	GetCurrentDemoTime(outDemoTime);
}

// The follow camera turns CS2's path gizmos off when it goes live. CS:S's path overlay hides itself
// while the follow camera has the view (CameraPath_Overlay), so there is nothing to switch.
void CameraBridge_SetPathDrawEnabled(bool) {}

} // namespace Filmmaker
