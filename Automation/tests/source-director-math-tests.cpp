// The CS:S movie director outside the workspaces (DirectorMath.h, DirectorMouse.h): CS2's wheel ring
// with CS:S's camera numbers, its keys, the 5 s skip, the FOV and speed steps, F8, where the cards
// go, and who gets each click.

#include "../../AfxHookSource/Filmmaker/Ui/DirectorMath.h"
#include "../../AfxHookSource/Filmmaker/Ui/DirectorMouse.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace SourceFilmmaker::Ui;
using namespace SourceFilmmaker::Ui::DirectorMath;
using Mode = DirectorMouse::Mode;
using Route = DirectorMouse::Route;
using Click = DirectorMouse::Click;

static int g_failed = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n", __LINE__, #x); ++g_failed; } } while (0)

static void TestCameraRing() {
	CHECK(StepCamera(kInEye, 1) == kChase);
	CHECK(StepCamera(kChase, 1) == kRoaming); // 6 is the objective camera in this SDK; the ring skips it
	CHECK(StepCamera(kRoaming, 1) == kInEye);
	CHECK(StepCamera(kInEye, -1) == kRoaming);
	CHECK(StepCamera(kChase, -1) == kInEye);
	CHECK(StepCamera(kChase, 0) == kChase);
	CHECK(StepCamera(kInEye, 3) == kInEye);    // a whole turn
	CHECK(StepCamera(kInEye, -4) == kRoaming);
	CHECK(StepCamera(kFixed, 1) == kInEye);    // an old SourceTV demo's fixed camera: first person next
	CHECK(StepCamera(kFixed, -1) == kChase);
	CHECK(std::string(CameraButtonName(kRoaming)) == "FREE CAM");
	CHECK(std::string(CameraCardName(kChase)) == "Chase cam");
	CHECK(std::string(CameraButtonName(0)) == "FIRST PERSON"); // a player's own recording
	CHECK(std::string(CameraButtonName(6)) == "CAMERA");       // the objective camera has no name of ours
}

static void TestKeys() {
	CHECK(KeyFor(0x20) == Key::TogglePause);
	CHECK(KeyFor(0x25) == Key::SkipBack);
	CHECK(KeyFor(0x27) == Key::SkipForward);
	CHECK(KeyFor(0x77) == Key::Blackout);
	CHECK(KeyFor('X') == Key::None);       // no X-ray in CS:S
	CHECK(KeyFor(0x1B) == Key::None);      // ESC is the game's menu outside the workspaces
}

static void TestSkip() {
	CHECK(SkipTarget(1000, 1, 0.015, 4410) == 1000 + 335); // 66.7 tick: 5 x round(66.67)
	CHECK(SkipTarget(100, -1, 0.01, 4410) == 0);           // kept at the start
	CHECK(SkipTarget(4300, 1, 0.01, 4410) == 4410);        // and at the end
	CHECK(SkipTarget(2000, -1, 0.01, 0) == 1500);          // 100 tick, length unknown
}

static void TestFovAndSpeed() {
	CHECK(StepFov(90, 1) == 89);       // wheel up zooms in
	CHECK(StepFov(90, -3) == 93);
	CHECK(StepFov(1.4, 2) == 1);       // clamped, whole degrees
	CHECK(StepFov(169.6, -5) == 170);
	CHECK(std::fabs(StepSpeed(1.0, 1) - 1.5) < 1e-9);
	CHECK(std::fabs(StepSpeed(1.0, -2) - 0.25) < 1e-9);
	CHECK(WheelFor(true, true, kInEye) == Wheel::Fov);
	CHECK(WheelFor(false, true, kRoaming) == Wheel::Speed);
	CHECK(WheelFor(false, true, kChase) == Wheel::Nothing); // used up, as in CS2
	CHECK(WheelFor(false, false, kChase) == Wheel::Camera);
}

static void TestF8() {
	HudState s;
	s = PressF8(s);
	CHECK(s.cards && s.blackout);
	s = PressF8(s);
	CHECK(s.cards && !s.blackout);
	HudState hidden;
	hidden.cards = false;
	hidden.blackout = true;
	hidden = PressF8(hidden);              // with the cards hidden F8 brings them back
	CHECK(hidden.cards && !hidden.blackout);
}

static void TestPlacement() {
	// 1080p: CS2's 278 px card at 0.8, 20 px in from the right, 4 px under the top chrome.
	const CardPlacement d = DirectorCard(1920, 1080, 50);
	CHECK(std::fabs(d.w - 222.4f) < 0.01f);
	CHECK(std::fabs(d.x - (1920 - 20 - 222.4f)) < 0.01f);
	CHECK(std::fabs(d.y - 54) < 0.01f);
	const CardPlacement c = ControlsCard(1920, 1080);
	CHECK(std::fabs(c.w - 201.6f) < 0.01f && c.x == 20 && c.y == 367);
	// 480p scales by height.
	const CardPlacement small = DirectorCard(854, 480, 0);
	CHECK(std::fabs(small.w - 278 * 0.8f * 480 / 1080) < 0.01f);
	const Rect bar = BarRect(854, 480);
	CHECK(bar.x0 == 0 && bar.x1 == 854 && bar.y1 == 480 && std::fabs(bar.H() - 96 * 480.0f / 1080) < 0.01f);
}

static const Rect kBar = MakeRect(0, 437, 854, 480);

static void TestClickStepsPlayers() {
	DirectorMouse m;
	Click click = Click::None;
	CHECK(m.ButtonDown(DirectorMouse::Left, 400, 200, 0, kBar) == Route::Swallow);
	CHECK(m.Pending());
	CHECK(m.ButtonUp(DirectorMouse::Left, 50, click) == Route::Swallow);
	CHECK(click == Click::NextPlayer && m.GetMode() == Mode::Free);
	CHECK(m.ButtonDown(DirectorMouse::Right, 400, 200, 100, kBar) == Route::Swallow);
	CHECK(m.Move(402, 201, 120) == Route::Ui);   // within the slop: still a click
	CHECK(m.ButtonUp(DirectorMouse::Right, 150, click) == Route::Swallow);
	CHECK(click == Click::PreviousPlayer);
}

static void TestHoldLooks() {
	DirectorMouse m;
	Click click = Click::None;
	m.ButtonDown(DirectorMouse::Left, 400, 200, 0, kBar);
	m.Tick(150);
	CHECK(m.GetMode() == Mode::Free);
	m.Tick(200);                                  // held 200 ms: a look
	CHECK(m.GetMode() == Mode::Look);
	CHECK(m.Move(500, 260, 220) == Route::Game);
	CHECK(m.ButtonDown(DirectorMouse::Right, 0, 0, 230, kBar) == Route::Game); // the game has the mouse
	CHECK(m.ButtonUp(DirectorMouse::Right, 240, click) == Route::Game && click == Click::None);
	CHECK(m.ButtonUp(DirectorMouse::Left, 300, click) == Route::Swallow);
	CHECK(click == Click::None && m.GetMode() == Mode::Free);
	CHECK(m.LookX() == 400 && m.LookY() == 200); // the cursor goes back here

	DirectorMouse drag;
	drag.ButtonDown(DirectorMouse::Right, 400, 200, 0, kBar);
	CHECK(drag.Move(406, 200, 10) == Route::Game); // more than 4 px: a look at once
	CHECK(drag.GetMode() == Mode::Look);
	CHECK(drag.ButtonUp(DirectorMouse::Right, 40, click) == Route::Swallow && click == Click::None);
}

static void TestBarKeepsItsClicks() {
	DirectorMouse m;
	Click click = Click::None;
	CHECK(m.ButtonDown(DirectorMouse::Left, 300, 460, 0, kBar) == Route::Ui);
	CHECK(!m.Pending());
	m.Tick(1000);                                 // holding on the bar (a scrub) never looks
	CHECK(m.GetMode() == Mode::Free);
	CHECK(m.Move(500, 300, 1010) == Route::Ui);   // dragged off the bar: still the bar's drag
	CHECK(m.ButtonUp(DirectorMouse::Left, 1020, click) == Route::Ui && click == Click::None);
	CHECK(m.ButtonDown(DirectorMouse::Middle, 300, 200, 0, kBar) == Route::Swallow);
}

static void TestFocusLost() {
	DirectorMouse m;
	Click click = Click::None;
	m.ButtonDown(DirectorMouse::Left, 400, 200, 0, kBar);
	m.FocusLost();
	CHECK(!m.Pending());
	CHECK(m.ButtonUp(DirectorMouse::Left, 10, click) == Route::Swallow && click == Click::None);
	m.ButtonDown(DirectorMouse::Left, 400, 200, 0, kBar);
	m.Tick(500);
	CHECK(m.GetMode() == Mode::Look);
	m.FocusLost();
	CHECK(m.GetMode() == Mode::Free);
}

int main() {
	TestCameraRing();
	TestKeys();
	TestSkip();
	TestFovAndSpeed();
	TestF8();
	TestPlacement();
	TestClickStepsPlayers();
	TestHoldLooks();
	TestBarKeepsItsClicks();
	TestFocusLost();
	if (g_failed) { std::printf("%d check(s) failed\n", g_failed); return 1; }
	std::printf("SourceDirectorMathTests passed\n");
	return 0;
}
