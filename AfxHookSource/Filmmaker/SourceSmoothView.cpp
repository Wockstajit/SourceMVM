#include "stdafx.h"

#include "SourceSmoothView.h"
#include "SourceEngine.h"
#include "Ui/DirectorMath.h"

#include "RenderView.h"
#include "WrpGlobals.h"

#include <AfxHookSource2/Filmmaker/Movie/SmoothView.h>
#include <AfxHookSource2/Filmmaker/Movie/SmoothViewHost.h>
#include <Shared/MirvInput.h>

namespace Filmmaker {

// CS:S sends a player's velocity and ground flag only to that player (the local-player table), so a
// SourceTV demo has none for the player it watches: Smooth View measures the camera's own movement,
// its fallback in CS2 too.
bool SmoothViewHost_PawnMotion(double, double, double, double velocity[3], bool* onGround) {
	(void)velocity;
	(void)onGround;
	return false;
}

void SmoothViewHost_RenderFrame(double& frameTime, int& frameCount) {
	frameTime = 0;
	frameCount = 0;
	if (WrpGlobals* globals = g_Hook_VClient_RenderView.GetGlobals()) {
		frameTime = globals->absoluteframetime_get();
		frameCount = globals->framecount_get();
	}
}

} // namespace Filmmaker

namespace SourceFilmmaker {

void SmoothView_OnView(float x, float y, float z, float& pitch, float& yaw, float& roll, float& fov, bool cameraOverridden) {
	Filmmaker::SmoothView& smoothView = Filmmaker::SmoothViewRef();
	if (!smoothView.Enabled() || cameraOverridden) return;
	MirvInput* freeCam = g_Hook_VClient_RenderView.m_MirvInput;
	if (freeCam && freeCam->GetCameraControlMode()) return;

	// A first-person view (a player's own recording, or SourceTV in eye) is a POV; a change of POV
	// starts the motion afresh (CS2's SetPovContext).
	const int mode = Players_ObserverMode();
	const bool pov = mode == 0 || mode == Ui::DirectorMath::kInEye;
	smoothView.SetPovContext(pov, pov ? (mode == 0 ? Players_LocalIndex() : Players_ObservedIndex()) : -1);

	WrpGlobals* globals = g_Hook_VClient_RenderView.GetGlobals();
	const double curTime = globals ? globals->curtime_get() : 0.0; // the demo clock during playback
	const Filmmaker::SmoothView::MotionSample motion = smoothView.SampleMotion(curTime, x, y, z, yaw);
	double p = pitch, yw = yaw, r = roll, f = fov;
	smoothView.ApplyCamera(p, yw, r, f, motion.dt, motion.speed, motion.lateralSpeed, motion.vz, motion.onGround,
		Demo_State().paused);
	pitch = (float)p;
	yaw = (float)yw;
	roll = (float)r;
	fov = (float)f;
}

void SmoothView_Command(int argc, advancedfx::ICommandArgs* args) {
	Filmmaker::SmoothView_RunCommand(argc, args, "mirv_filmmaker smoothview");
}

} // namespace SourceFilmmaker
