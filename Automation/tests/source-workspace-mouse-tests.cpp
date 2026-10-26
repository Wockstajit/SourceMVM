// WorkspaceMouse, the CS:S workspace's version of CS2's mouse rules: who gets each click, when a
// right press becomes a look, and how the mouse comes back.

#include "../../AfxHookSource/Filmmaker/Ui/WorkspaceMouse.h"

#include <cstdio>

using namespace SourceFilmmaker::Ui;
using Mode = WorkspaceMouse::Mode;
using Route = WorkspaceMouse::Route;

static int g_failed = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n", __LINE__, #x); ++g_failed; } } while (0)

static const Rect kPreview = MakeRect(0, 50, 570, 370);

static void TestPanelClicksStayInUi() {
	WorkspaceMouse m;
	CHECK(m.ButtonDown(WorkspaceMouse::Left, 700, 20, 0, kPreview) == Route::Ui);
	CHECK(m.ButtonUp(WorkspaceMouse::Left, 10) == Route::Ui);
	CHECK(m.GetMode() == Mode::Ui);
	CHECK(m.ButtonDown(WorkspaceMouse::Right, 700, 20, 0, kPreview) == Route::Ui); // a panel's own menu
	CHECK(!m.RightPending());
}

static void TestLeftClickInPreviewGoesToGame() {
	WorkspaceMouse m;
	CHECK(m.ButtonDown(WorkspaceMouse::Left, 200, 200, 0, kPreview) == Route::Game);
	CHECK(m.GetMode() == Mode::Game);
	CHECK(m.ButtonUp(WorkspaceMouse::Left, 10) == Route::Game);       // the release follows the press
	CHECK(m.Move(300, 300, 20) == Route::Game);
	CHECK(m.ButtonDown(WorkspaceMouse::Left, 700, 20, 30, kPreview) == Route::Game); // no panel clicks now
	m.ToggleGame();                                                    // G
	CHECK(m.GetMode() == Mode::Ui);
	m.ToggleGame();
	CHECK(m.GetMode() == Mode::Game);
}

static void TestQuickRightClickStaysAClick() {
	WorkspaceMouse m;
	CHECK(m.ButtonDown(WorkspaceMouse::Right, 200, 200, 1000, kPreview) == Route::Swallow);
	CHECK(m.RightPending());
	m.Tick(1000 + Filmmaker::UiClick::kRightLookHoldMs - 1);
	CHECK(m.GetMode() == Mode::Ui);
	CHECK(m.ButtonUp(WorkspaceMouse::Right, 1100) == Route::Swallow);
	CHECK(m.GetMode() == Mode::Ui && !m.RightPending());
}

static void TestRightHoldLooks() {
	WorkspaceMouse m;
	m.ButtonDown(WorkspaceMouse::Right, 200, 200, 1000, kPreview);
	m.Tick(1000 + Filmmaker::UiClick::kRightLookHoldMs);
	CHECK(m.GetMode() == Mode::Look);
	CHECK(m.LookX() == 200 && m.LookY() == 200);       // where the cursor returns
	CHECK(m.Move(260, 210, 1300) == Route::Game);       // the game looks around
	CHECK(m.ButtonUp(WorkspaceMouse::Right, 1400) == Route::Swallow);
	CHECK(m.GetMode() == Mode::Ui);
}

static void TestRightDragLooks() {
	WorkspaceMouse m;
	m.ButtonDown(WorkspaceMouse::Right, 200, 200, 1000, kPreview);
	CHECK(m.Move(202, 202, 1010) == Route::Ui);         // inside the 4 px slop
	CHECK(m.GetMode() == Mode::Ui);
	CHECK(m.Move(203, 202, 1020) == Route::Game);       // 5 px: a drag, so a look
	CHECK(m.GetMode() == Mode::Look);
}

static void TestLookIgnoresG() {
	WorkspaceMouse m;
	m.ButtonDown(WorkspaceMouse::Right, 200, 200, 0, kPreview);
	m.Tick(500);
	m.ToggleGame();
	CHECK(m.GetMode() == Mode::Look);
}

static void TestWheel() {
	WorkspaceMouse m;
	CHECK(m.Wheel(200, 200, kPreview) == Route::Game);
	CHECK(m.Wheel(700, 200, kPreview) == Route::Ui);
	m.ToggleGame();
	CHECK(m.Wheel(700, 200, kPreview) == Route::Game);
}

static void TestFocusLostAndReset() {
	WorkspaceMouse m;
	m.ButtonDown(WorkspaceMouse::Right, 200, 200, 0, kPreview);
	m.FocusLost();
	m.Tick(5000);
	CHECK(m.GetMode() == Mode::Ui);                     // a press with no release never looks
	m.ButtonDown(WorkspaceMouse::Right, 200, 200, 0, kPreview);
	m.Tick(500);
	m.FocusLost();
	CHECK(m.GetMode() == Mode::Ui);
	m.ButtonDown(WorkspaceMouse::Left, 200, 200, 0, kPreview);
	m.Reset();
	CHECK(m.GetMode() == Mode::Ui && !m.RightPending());
}

static void TestNoPreview() {
	WorkspaceMouse m;
	CHECK(m.ButtonDown(WorkspaceMouse::Left, 200, 200, 0, Rect()) == Route::Ui); // cannot place it: stay
	CHECK(m.ButtonDown(WorkspaceMouse::Right, 200, 200, 0, Rect()) == Route::Ui);
	CHECK(m.GetMode() == Mode::Ui);
}

int main() {
	TestPanelClicksStayInUi();
	TestLeftClickInPreviewGoesToGame();
	TestQuickRightClickStaysAClick();
	TestRightHoldLooks();
	TestRightDragLooks();
	TestLookIgnoresG();
	TestWheel();
	TestFocusLostAndReset();
	TestNoPreview();
	if (g_failed) { std::printf("%d check(s) failed\n", g_failed); return 1; }
	std::printf("SourceWorkspaceMouseTests passed\n");
	return 0;
}
