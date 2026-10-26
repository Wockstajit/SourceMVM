// UiClickMath.h: inside a workspace, where you click or scroll decides UI vs GAME.
#include "../../AfxHookSource2/Filmmaker/Movie/UiClickMath.h"

#include <cstdio>

using namespace Filmmaker::UiClick;

namespace {
int g_failed = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAILED line %d: %s\n", __LINE__, #c); ++g_failed; } } while (0)

// 1600x1200 client, editor preview in the top-left (the layout measured live).
Input Editor(float x, float y, bool uiOn) {
	Input in;
	in.workspace = true; in.uiModeOn = uiOn;
	in.preview = FromFractions(0.0f, 0.12963f, 0.53889f, 0.66852f, 1600, 1200);
	in.x = x; in.y = y;
	return in;
}

Input Regular(float x, float y) {
	Input in;
	in.x = x; in.y = y;
	return in;
}

void TestClickToUi() {
	CHECK(ClickSwitchesToUiMode(Editor(1300, 500, false)));   // inspector
	CHECK(ClickSwitchesToUiMode(Editor(400, 1000, false)));   // bottom panel under the preview
	CHECK(ClickSwitchesToUiMode(Editor(400, 50, false)));     // header above the preview
	CHECK(!ClickSwitchesToUiMode(Editor(400, 500, false)));   // the preview: stays a camera click
	CHECK(!ClickSwitchesToUiMode(Editor(0, 156, false)));     // preview's top-left corner is inside
	CHECK(ClickSwitchesToUiMode(Editor(862.3f, 500, false))); // first pixel right of the preview
	CHECK(!ClickSwitchesToUiMode(Editor(1300, 500, true)));   // already UI: the panel has it anyway
	// No preview rect yet: the click cannot be placed, so the mode stays as it is.
	Input none = Editor(1300, 500, false); none.preview = Rect();
	CHECK(!ClickSwitchesToUiMode(none));
	// Mid-look the hidden cursor is pinned to the window centre, which is inside the preview.
	CHECK(!ClickSwitchesToUiMode(Editor(800, 600, false)));
}

void TestClickToGame() {
	CHECK(ClickSwitchesToGameMode(Editor(400, 500, true)));   // the preview
	CHECK(ClickSwitchesToGameMode(Editor(0, 156, true)));     // its top-left corner
	CHECK(!ClickSwitchesToGameMode(Editor(1300, 500, true))); // a panel stays UI
	CHECK(!ClickSwitchesToGameMode(Editor(400, 500, false))); // already GAME
	Input none = Editor(400, 500, true); none.preview = Rect();
	CHECK(!ClickSwitchesToGameMode(none));
}

void TestNoWorkspace() {
	// Plain demo viewing has no UI mode: CS2 hands the cursor back itself after a look.
	CHECK(!ClickSwitchesToUiMode(Regular(800, 1150)));
	CHECK(!ClickSwitchesToGameMode(Regular(800, 600)));
	CHECK(WheelCyclesCamera(Regular(800, 600)));
	CHECK(WheelCyclesCamera(Regular(800, 1150)));
}

void TestWheel() {
	// Over the preview the wheel steps the camera in either mode; over a panel it scrolls the panel.
	CHECK(WheelCyclesCamera(Editor(400, 500, true)));
	CHECK(WheelCyclesCamera(Editor(400, 500, false)));
	CHECK(!WheelCyclesCamera(Editor(1300, 500, true)));
	CHECK(!WheelCyclesCamera(Editor(1300, 500, false)));
	Input none = Editor(400, 500, true); none.preview = Rect();
	CHECK(!WheelCyclesCamera(none)); // cannot place it: leave the panel scroll alone
}

void TestPopupOverPreview() {
	// Editor Settings opens over the preview: a click on its X-Ray toggle there is the popup's.
	Input game = Editor(400, 500, false); game.popupOpen = true;
	CHECK(ClickSwitchesToUiMode(game));
	Input ui = Editor(400, 500, true); ui.popupOpen = true;
	CHECK(!ClickSwitchesToGameMode(ui)); // stays UI, the click reaches the toggle
	CHECK(!WheelCyclesCamera(ui));
	Input outside = Editor(1300, 500, false); outside.popupOpen = true;
	CHECK(ClickSwitchesToUiMode(outside));
	// No workspace, no UI mode to switch to.
	Input regular = Regular(400, 500); regular.popupOpen = true;
	CHECK(!ClickSwitchesToUiMode(regular));
}

void TestPathPlayback() {
	// A playing path owns the view: presses in the preview go nowhere, in either mouse mode, so
	// neither free-look nor CS2's own look-around can take the pointer mid-playback.
	CHECK(PathPlaybackSwallowsPress(Editor(400, 500, true), true));
	CHECK(PathPlaybackSwallowsPress(Editor(400, 500, false), true));
	CHECK(!PathPlaybackSwallowsPress(Editor(400, 500, true), false));  // not playing: a normal click
	CHECK(!PathPlaybackSwallowsPress(Editor(1300, 500, true), true));  // the panels keep working (pause, stop)
	Input popup = Editor(400, 500, true); popup.popupOpen = true;
	CHECK(!PathPlaybackSwallowsPress(popup, true));                    // a popup over the preview keeps its clicks
	Input none = Editor(400, 500, true); none.preview = Rect();
	CHECK(!PathPlaybackSwallowsPress(none, true));                     // cannot place it: leave it alone
	CHECK(!PathPlaybackSwallowsPress(Regular(800, 600), true));        // no workspace: the free-cam swallow covers it
}

void TestViewEdit() {
	// Config's ENVIRONMENT tab edits in the preview: a left click there picks and drags scene
	// objects, so it must not hand the mouse to the camera; a held right click looks around.
	Input left = Editor(400, 500, true); left.viewEdit = true;
	CHECK(!ClickSwitchesToGameMode(left));
	CHECK(RightPressLooks(left));
	CHECK(!RightPressLooks(Editor(400, 500, true)));   // other tabs keep the left click for looking
	Input panel = Editor(1300, 500, true); panel.viewEdit = true;
	CHECK(!RightPressLooks(panel));                    // the outliner's right-click menu stays
	Input game = Editor(400, 500, false); game.viewEdit = true;
	CHECK(!RightPressLooks(game));                     // already looking: the game has the press
	Input popup = Editor(400, 500, true); popup.viewEdit = true; popup.popupOpen = true;
	CHECK(!RightPressLooks(popup));                    // a popup over the preview keeps its clicks
	Input none = Editor(400, 500, true); none.viewEdit = true; none.preview = Rect();
	CHECK(!RightPressLooks(none));                     // cannot place it: leave it alone
	// Leaving GAME mode by clicking a panel still works on this tab.
	Input back = Editor(1300, 500, false); back.viewEdit = true;
	CHECK(ClickSwitchesToUiMode(back));
}

void TestRightHold() {
	CHECK(!RightPressBecomesLook(0, 0, 0));                        // a fresh press is still a click
	CHECK(!RightPressBecomesLook(kRightLookHoldMs - 1, 2, -2));    // short and inside the slop
	CHECK(RightPressBecomesLook(kRightLookHoldMs, 0, 0));          // held long enough
	CHECK(!RightPressBecomesLook(10, kRightLookSlopPx, 0));        // the slop itself is still a click
	CHECK(RightPressBecomesLook(10, kRightLookSlopPx + 1, 0));     // one pixel past it is a drag
	CHECK(RightPressBecomesLook(10, -3, 2));                       // Manhattan distance, either sign
}

void TestFractions() {
	const Rect r = FromFractions(0.25f, 0.5f, 0.75f, 1.0f, 1920, 1080);
	CHECK(r.valid && r.x0 == 480 && r.y0 == 540 && r.x1 == 1440 && r.y1 == 1080);
	CHECK(!FromFractions(0.5f, 0, 0.5f, 1, 1920, 1080).valid); // zero width
	CHECK(!FromFractions(0, 0, 1, 1, 0, 1080).valid);          // no client size
}
} // namespace

int main() {
	TestClickToUi();
	TestClickToGame();
	TestNoWorkspace();
	TestWheel();
	TestPopupOverPreview();
	TestPathPlayback();
	TestViewEdit();
	TestRightHold();
	TestFractions();
	if (g_failed) { std::printf("%d check(s) failed\n", g_failed); return 1; }
	std::printf("ui-click-math-tests: all passed\n");
	return 0;
}
