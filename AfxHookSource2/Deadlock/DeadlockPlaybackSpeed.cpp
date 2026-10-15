#include "DeadlockPlaybackSpeed.h"

#include "DeadlockPlaybackSpeedMath.h"
#include "DeadlockEngine.h"

#include "../../Shared/AfxConsole.h"

#include <cstdio>

namespace Deadlock {

namespace {

double g_speed = 1.0;

void Apply() {
	char cmd[64];
	std::snprintf(cmd, sizeof(cmd), "demo_timescale %.4f", g_speed);
	Engine_ExecuteClientCmd(cmd);
}

} // namespace

double PlaybackSpeed() { return g_speed; }

void PlaybackSpeed_Set(double speed) {
	g_speed = PlaybackSpeed_Clamp(speed);
	Apply();
}

void PlaybackSpeed_Step(int dir) {
	g_speed = PlaybackSpeed_NextStep(g_speed, dir);
	Apply();
}

void PlaybackSpeed_Reassert() { Apply(); }

void PlaybackSpeed_Report() {
	advancedfx::Message("  speed:     %.2fx  (nearest preset %d)\n",
		g_speed, PlaybackSpeed_NearestIndex(g_speed));
}

} // namespace Deadlock
