// CS2's answers to SmoothViewHost.h: the spectated pawn's motion from the camera shake stack, the
// render frame from MirvTime.

#include "SmoothViewHost.h"

#include "CameraShakeStack.h"
#include "../../MirvTime.h"

namespace Filmmaker {

bool SmoothViewHost_PawnMotion(double x, double y, double z, double velocity[3], bool* onGround) {
	return CameraShake_ViewedPawnMotion(x, y, z, velocity, onGround);
}

void SmoothViewHost_RenderFrame(double& frameTime, int& frameCount) {
	frameTime = g_MirvTime.absoluteframetime_get();
	frameCount = g_MirvTime.framecount_get();
}

} // namespace Filmmaker
