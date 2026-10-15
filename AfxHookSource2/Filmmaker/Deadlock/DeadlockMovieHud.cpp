#include "DeadlockMovieHud.h"

#include "DeadlockRender.h"
#include "DeadlockWorkspaces.h"

#include "../../Deadlock/DeadlockCameraRuntime.h"
#include "../../Deadlock/DeadlockEngine.h"
#include "../../Deadlock/DeadlockHudBlackout.h"

// The free camera's own fly-speed multiplier -- CS2's "Cam speed" row reads exactly this
// (CameraBridge_GetFreeCamSpeed() -> MirvInput::GetCamSpeed()), not the demo/playback speed.
// Deadlock shares the same MirvInput underneath (DeadlockCameraRuntime.h), so this is the same
// getter, not a port.
#include "../../../Shared/MirvInput.h"

#include <cmath>
#include <cstdio>

namespace Filmmaker {

namespace {

// 1-decimal rounding so float jitter in the low digits doesn't push a new string every frame --
// same rounding CS2's MovieHud.cpp uses for the same reason.
double r1(double v) { return std::floor(v * 10.0 + 0.5) / 10.0; }

void AppendNumber(std::string& out, double value, int decimals) {
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
	out += buf;
}

} // namespace

void MovieHud_AppendStateJson(std::string& out) {
	const bool playing = Deadlock::Engine_IsPlayingDemo();
	const bool paused = Deadlock::Engine_IsDemoPaused();
	const bool demoActive = playing || paused;

	// Hidden with the rest of the screen on F8 (CS2: "F8 clears every overlay, this card
	// included"), and while a take is actually writing frames -- a capture must never include
	// director chrome, matching the hero-box overlay's own rule (DeadlockWorkspaces.cpp) and CS2's
	// RenderSessionDriver_WantsChromeHidden gate.
	//
	// ALSO hidden while the workspace itself is open -- the one deliberate difference from CS2's
	// always-on card, explained in DeadlockMovieHudJs.h's file header: Deadlock's workspace docks a
	// full-width band and a full-height inspector column around the preview (unlike CS2's floating
	// editor), and both already report this same pose/owner/speed. This card exists for the moment
	// CS2's does not need to cover: watching a replay with the tool closed.
	const bool visible = demoActive
		&& !Deadlock::HudBlackout_Active()
		&& Render_GetState() == RenderState::Idle
		&& !Workspaces_Open();

	out += ",\"director\":{";
	out += "\"visible\":";
	out += visible ? "true" : "false";
	out += ",\"playing\":";
	out += playing ? "true" : "false";
	out += ",\"paused\":";
	out += paused ? "true" : "false";

	// The view-owner resolver already answers "what is actually driving the camera right now"
	// (Game/ShotHold/FreeCam/Follow/Graph/Shot) for the `camera` console report; the director card
	// shows the same word rather than a second, possibly-disagreeing notion of "mode".
	out += ",\"owner\":\"";
	out += Deadlock::CameraRuntime_OwnerName();
	out += "\"";

	out += ",\"freecam\":";
	out += Deadlock::CameraRuntime_FreeCam() ? "true" : "false";
	out += ",\"speed\":";
	{
		MirvInput* input = Deadlock::CameraRuntime_Input();
		AppendNumber(out, input ? input->GetCamSpeed() : 1.0, 3);
	}

	double origin[3] = { 0, 0, 0 };
	double angles[3] = { 0, 0, 0 };
	double fov = 0.0;
	Deadlock::CameraRuntime_GetLastCamera(origin, angles, fov);
	out += ",\"ox\":"; AppendNumber(out, r1(origin[0]), 1);
	out += ",\"oy\":"; AppendNumber(out, r1(origin[1]), 1);
	out += ",\"oz\":"; AppendNumber(out, r1(origin[2]), 1);
	out += ",\"pitch\":"; AppendNumber(out, r1(angles[0]), 1);
	out += ",\"yaw\":"; AppendNumber(out, r1(angles[1]), 1);
	out += ",\"roll\":"; AppendNumber(out, r1(angles[2]), 1);
	out += ",\"fov\":"; AppendNumber(out, r1(fov), 1);

	const int tick = Deadlock::Engine_DemoTick();
	out += ",\"tick\":"; AppendNumber(out, tick < 0 ? 0 : tick, 0);
	out += ",\"time\":"; AppendNumber(out, Deadlock::Engine_DemoTime(), 3);

	out += "}";
}

} // namespace Filmmaker
