#include "stdafx.h"

#include "SourceDirector.h"
#include "SourceCameraPath.h"
#include "SourceConCommand.h"
#include "SourceDemoBar.h"
#include "SourceDemoIndex.h"
#include "SourceEngine.h"

#include "RenderView.h"
#include "WrpVEngineClient.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <Shared/AfxConsole.h>
#include <Shared/MirvInput.h>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern WrpVEngineClient* g_VEngineClient;

namespace SourceFilmmaker {

namespace {

using namespace Ui;

// CS:S's default sv_specspeed: the director's free cam speed 1x.
const double kDefaultSpecSpeed = 3;
// After a camera change the camera reads back its old mode until the command has run; the director
// steps from the mode it asked for until then (CS2 holds it 30 frames too).
const int kExpectFrames = 30;

// SourceTV's camera, between frames: a demo start resets it, a seek back may, and old demos (and
// some new ones) start it at the map origin following nobody.
struct Follow {
	int mode = 0;
	int userId = -1; // the player it follows
};

struct State {
	DirectorState ui;
	bool hudHidden = false;     // cl_drawhud 0 is the blackout's
	double speed = 1;           // the free cam's speed, x
	bool speedSet = false;      // sv_specspeed is ours
	double fov = 0;             // Ctrl+wheel's FOV (the view's FOV override), 0 while off
	int expectMode = 0;
	int expectFrames = 0;
	Follow follow;              // what SourceTV's camera did last
	std::string followFile;     // in which demo (a seek back plays on in the same file)
	bool restore = false;       // a start or seek back whose camera is still to be put right
	int restoreFrames = 0;
	bool wasPlaying = false;
	int lastTick = 0;
	unsigned frame = 0;
	bool rollRight = false, rollLeft = false; // X, Z held in the game's free cam
	double roll = 0;                          // its roll, degrees
};
State g;

void Command(const char* command) {
	if (!g_VEngineClient) return;
	g_VEngineClient->ClientCmd_Unrestricted(command);
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("director", "command %s", command);
}

void ApplyBlackout() {
	if (g.ui.blackout == g.hudHidden) return;
	g.hudHidden = g.ui.blackout;
	Command(g.hudHidden ? "cl_drawhud 0" : "cl_drawhud 1");
}

void DropFov() {
	if (g.fov <= 0) return;
	g.fov = 0;
	g_Hook_VClient_RenderView.FovDefault();
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("director", "fov default");
}

int UserIdOf(const std::vector<PlayerState>& players, int index) {
	for (const PlayerState& p : players)
		if (p.index == index) return p.userId;
	return -1;
}

// A demo starts, or a seek went back: once a player is alive, the camera goes back to
// the mode and the player it had (CS2 keeps the watched player across a seek). A demo that starts
// with the camera at the map origin following nobody goes into first person on the first player,
// as a fresh CS2 demo opens. Gives up after 1200 frames.
void RestoreCamera() {
	if (++g.restoreFrames > 1200) {
		g.restore = false;
		return;
	}
	std::vector<PlayerState> players;
	Players_Snapshot(players);
	bool someoneAlive = false;
	for (const PlayerState& p : players) someoneAlive |= p.alive && !p.hltv;
	if (!someoneAlive) return;
	g.restore = false;
	char command[96];
	if (g.follow.userId >= 0) {
		const int mode = g.follow.mode == DirectorMath::kChase || g.follow.mode == DirectorMath::kRoaming ? g.follow.mode : DirectorMath::kInEye;
		std::snprintf(command, sizeof(command), "spec_mode %d; spec_player %d", mode, g.follow.userId);
	} else {
		const ViewPose view = View_Game();
		if (view.origin[0] != 0 || view.origin[1] != 0 || view.origin[2] != 0 || Players_ObservedIndex() > 0) return; // placed already
		std::snprintf(command, sizeof(command), "spec_mode %d; spec_next", DirectorMath::kInEye);
	}
	Command(command);
	g.expectFrames = 0;
}

// Once a frame during a demo: notices starts and seeks back, and twice a second remembers what
// SourceTV's camera follows.
void TrackCamera() {
	const DemoState demo = Demo_State();
	if (!g.wasPlaying) {
		if (DemoIndex_Get().file != g.followFile) g.follow = Follow(); // another demo: nothing carries over
		g.restore = true;
		g.restoreFrames = 0;
	} else if (demo.tick + 10 < g.lastTick) {
		g.restore = true; // went back
		g.restoreFrames = 0;
	}
	g.wasPlaying = true;
	g.lastTick = demo.tick;
	if (!Engine_SourceTvPlayback()) {
		g.restore = false;
		return;
	}
	if (g.restore) {
		RestoreCamera();
		return;
	}
	if (++g.frame % 30) return;
	const int target = Players_ObservedIndex();
	if (target <= 0) return;
	std::vector<PlayerState> players;
	Players_Snapshot(players);
	const int userId = UserIdOf(players, target);
	if (userId < 0) return;
	g.follow.mode = Players_ObserverMode();
	g.follow.userId = userId;
	g.followFile = DemoIndex_Get().file;
}

int CameraMode() { return g.expectFrames > 0 ? g.expectMode : Players_ObserverMode(); }

// HLAE's free camera: mirv_input's camera control mode, which the view override chain applies
// (RenderView.cpp Override_Input).
MirvInput* FreeCam() { return g_Hook_VClient_RenderView.m_MirvInput; }

// The free camera turns with the game's own aim sensitivity (sensitivity * m_yaw / m_pitch, read once
// a second), as CS2's does: the same hand movement turns the camera as far as it turns the aim.
void FeedSensitivity() {
	static ULONGLONG lastRead = 0;
	const ULONGLONG now = GetTickCount64();
	if (now - lastRead < 1000 || !FreeCam()) return;
	lastRead = now;
	float sensitivity = 0, yaw = 0, pitch = 0;
	if (!ConVar_GetFloat("sensitivity", sensitivity) || !ConVar_GetFloat("m_yaw", yaw) || !ConVar_GetFloat("m_pitch", pitch)) return;
	if (sensitivity <= 0 || yaw <= 0 || pitch <= 0) return;
	FreeCam()->SetGameMouseDegreesPerCount((double)sensitivity * yaw, (double)sensitivity * pitch);
	static float logged = -1;
	if (logged != sensitivity && Filmmaker::MvmDebugLog_Active()) {
		logged = sensitivity;
		Filmmaker::MvmDebugLog_Linef("director", "free cam mouse: sensitivity %.3f m_yaw %.4f m_pitch %.4f", sensitivity, yaw, pitch);
	}
}

bool DemoPlaying() { return Demo_State().playing && g_VEngineClient && g_VEngineClient->IsInGame(); }

} // namespace

DirectorState& Director_State() { return g.ui; }

void Director_Frame() {
	FeedSensitivity();
	if (g.expectFrames > 0 && (--g.expectFrames == 0 || Players_ObserverMode() == g.expectMode)) g.expectFrames = 0;
	if (DemoPlaying()) {
		ApplyBlackout();
		TrackCamera();
		return;
	}
	// The demo is over: no blackout, FOV or speed of the director's stays behind.
	g.wasPlaying = false;
	g.ui.blackout = false;
	ApplyBlackout();
	DropFov();
	if (g.speedSet) {
		g.speedSet = false;
		g.speed = 1;
		char command[64];
		std::snprintf(command, sizeof(command), "sv_specspeed %g", kDefaultSpecSpeed);
		Command(command);
	}
}

bool Director_Wanted() { return DemoPlaying() && !Engine_GameUiVisible(); }

void Director_BuildCards(DirectorCardsModel& cards) {
	cards = DirectorCardsModel();
	const DemoState demo = Demo_State();
	cards.freeCam = Director_FreeCamOn();
	cards.mode = cards.freeCam ? "Free cam" : DirectorMath::CameraCardName(CameraMode());
	cards.playing = demo.playing;
	cards.paused = demo.paused;
	cards.speed = cards.freeCam ? FreeCam()->GetCamSpeed() : g.speed;
	const ViewPose view = View_Rendered(); // what is on screen, the director's FOV included
	for (int i = 0; i < 3; ++i) {
		cards.pos[i] = view.origin[i];
		cards.ang[i] = view.angles[i];
	}
	cards.fov = view.fov;
	cards.haveTick = demo.playing;
	cards.tick = demo.tick;
	cards.time = demo.tick * demo.intervalPerTick;
}

float Director_SpectatorBarHeight(float clientH) {
	if (g.ui.blackout || Players_ObserverMode() == 0) return 0;
	return clientH * DirectorMath::kSpectatorBarShare;
}

void Director_Click(DirectorMouse::Click click) {
	if (click == DirectorMouse::Click::NextPlayer) Command("spec_next");
	else if (click == DirectorMouse::Click::PreviousPlayer) Command("spec_prev");
}

bool Director_FreeCamOn() { return FreeCam() && FreeCam()->GetCameraControlMode(); }

bool Director_RollKey(int vk, bool down, bool ctrl) {
	if (vk != 'X' && vk != 'Z') return false;
	bool& held = vk == 'X' ? g.rollRight : g.rollLeft;
	if (!down) { // a release always clears, so a camera change mid-hold cannot leave it rolling
		const bool was = held;
		held = false;
		return was;
	}
	if (!DirectorMath::RollKeysApply(CameraMode(), Director_FreeCamOn(), ctrl)) return false;
	held = true;
	return true;
}

void Director_ApplyRoll(float& roll, double frameTime) {
	if (CameraMode() != DirectorMath::kRoaming || Director_FreeCamOn()) {
		g.roll = 0;
		return;
	}
	const int direction = (g.rollRight ? 1 : 0) - (g.rollLeft ? 1 : 0);
	if (direction && frameTime > 0 && frameTime <= 0.5) g.roll = DirectorMath::StepRoll(g.roll, direction, frameTime);
	roll += (float)g.roll;
}

void Director_SetSlow(bool slow) {
	if (FreeCam()) FreeCam()->SetSlowFactor(slow ? DirectorMath::kFreeCamSlowFactor : 1.0);
}

void Director_StepCamera(int notches) {
	if (CameraPath_OwnsView()) CameraPath_Stop(); // one of the game's cameras takes the view from the path
	if (Director_FreeCamOn()) {
		FreeCam()->SetCameraControlMode(false); // one of the game's cameras takes the view back
		if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("director", "free cam off");
	}
	const int mode = DirectorMath::StepCamera(CameraMode(), notches);
	char command[64];
	std::snprintf(command, sizeof(command), "spec_mode %d", mode);
	Command(command);
	g.expectMode = mode;
	g.expectFrames = kExpectFrames;
	g.roll = 0;
	DropFov(); // the FOV belongs to the camera it was set in (CS2)
}

void Director_Wheel(int notches, bool ctrl, bool shift) {
	if (!notches) return;
	char command[64];
	switch (DirectorMath::WheelFor(ctrl, shift, CameraMode(), Director_FreeCamOn())) {
	case DirectorMath::Wheel::Camera:
		Director_StepCamera(notches);
		break;
	case DirectorMath::Wheel::Fov: {
		// The view's FOV override (mirv_fov's): demo_fov_override does not reach SourceTV's camera.
		const double from = g.fov > 0 ? g.fov : View_Rendered().fov;
		g.fov = DirectorMath::StepFov(from > 0 ? from : 90, notches);
		g_Hook_VClient_RenderView.FovOverride(g.fov);
		if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("director", "fov %.0f", g.fov);
		break;
	}
	case DirectorMath::Wheel::Speed:
		g.speed = DirectorMath::StepSpeed(g.speed, notches);
		g.speedSet = true;
		std::snprintf(command, sizeof(command), "sv_specspeed %g", g.speed * kDefaultSpecSpeed);
		Command(command);
		break;
	case DirectorMath::Wheel::FreeCamFov:
		for (int i = 0; i < (notches < 0 ? -notches : notches); ++i)
			FreeCam()->NudgeFov(notches > 0 ? -Filmmaker::kFreeCamFovStepDeg : Filmmaker::kFreeCamFovStepDeg); // up zooms in
		break;
	case DirectorMath::Wheel::FreeCamSpeed:
		FreeCam()->SetCamSpeed(DirectorMath::StepSpeed(FreeCam()->GetCamSpeed(), notches));
		break;
	case DirectorMath::Wheel::Nothing:
		break;
	}
}

void Director_Key(DirectorMath::Key key) {
	switch (key) {
	case DirectorMath::Key::TogglePause:
		Command("demo_togglepause");
		break;
	case DirectorMath::Key::SkipBack:
	case DirectorMath::Key::SkipForward: {
		const DemoState demo = Demo_State();
		if (!demo.playing) break;
		DemoBar_SeekTo(DirectorMath::SkipTarget(demo.tick, key == DirectorMath::Key::SkipBack ? -1 : 1,
			demo.intervalPerTick > 0 ? demo.intervalPerTick : 0.015, demo.totalTicks));
		break;
	}
	case DirectorMath::Key::Blackout: {
		DirectorMath::HudState s;
		s.cards = g.ui.cards;
		s.blackout = g.ui.blackout;
		s = DirectorMath::PressF8(s);
		g.ui.cards = s.cards;
		g.ui.blackout = s.blackout;
		ApplyBlackout();
		break;
	}
	case DirectorMath::Key::None:
		break;
	}
}

namespace {

bool Switch(const char* action, bool& flag, const char* stateWord) {
	if (0 == _stricmp(action, "on")) flag = true;
	else if (0 == _stricmp(action, "off")) flag = false;
	else if (0 == _stricmp(action, "toggle")) flag = !flag;
	else if (0 != _stricmp(action, stateWord)) return false;
	return true;
}

} // namespace

void Director_HudCommand(const char* action) {
	if (!Switch(action, g.ui.cards, "state")) {
		advancedfx::Message("mirv_filmmaker hud on|off|toggle|state\n");
		return;
	}
	advancedfx::Message("hud: the director's cards are %s\n", g.ui.cards ? "on" : "off");
}

void Director_BlackoutCommand(const char* action) {
	if (!Switch(action, g.ui.blackout, "status")) {
		advancedfx::Message("mirv_filmmaker blackout on|off|toggle|status\n");
		return;
	}
	ApplyBlackout();
	advancedfx::Message("blackout: %s\n", g.ui.blackout ? "on (no HUD, no bar, no cards)" : "off");
}

void Director_PrintHealth() {
	advancedfx::Message("  director:   %s, cards %s, blackout %s, camera %s, free cam speed %.3gx, fov %.0f\n",
		Director_Wanted() ? "demo playing" : (DemoPlaying() ? "standing aside (game menu or console)" : "no demo"),
		g.ui.cards ? "on" : "off", g.ui.blackout ? "on" : "off", DirectorMath::CameraCardName(CameraMode()), g.speed, g.fov);
	advancedfx::Message("  spectator:  %s\n", Engine_SpectatorCameraText().c_str());
}

} // namespace SourceFilmmaker
