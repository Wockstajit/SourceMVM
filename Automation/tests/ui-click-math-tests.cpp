// UiClickMath.h: when a click in GAME mode switches to UI mode.
#include "../../AfxHookSource2/Filmmaker/Movie/UiClickMath.h"

#include <cstdio>

using namespace Filmmaker::UiClick;

namespace {
int g_failed = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAILED line %d: %s\n", __LINE__, #c); ++g_failed; } } while (0)

// 1600x1200 client, editor preview in the top-left (the layout measured live).
Input Editor(float x, float y) {
	Input in;
	in.uiModeAvailable = true; in.workspace = true;
	in.preview = FromFractions(0.0f, 0.12963f, 0.53889f, 0.66852f, 1600, 1200);
	in.x = x; in.y = y;
	return in;
}

Input Regular(float x, float y) {
	Input in;
	in.uiModeAvailable = true;
	in.demoBar.valid = true; in.demoBar.x0 = 100; in.demoBar.y0 = 1100; in.demoBar.x1 = 1500; in.demoBar.y1 = 1190;
	in.x = x; in.y = y;
	return in;
}

void TestWorkspace() {
	CHECK(ClickSwitchesToUiMode(Editor(1300, 500)));  // inspector
	CHECK(ClickSwitchesToUiMode(Editor(400, 1000)));  // bottom panel under the preview
	CHECK(ClickSwitchesToUiMode(Editor(400, 50)));    // header above the preview
	CHECK(!ClickSwitchesToUiMode(Editor(400, 500)));  // the preview: stays a camera click
	CHECK(!ClickSwitchesToUiMode(Editor(0, 156)));    // preview's top-left corner is inside
	CHECK(ClickSwitchesToUiMode(Editor(862.3f, 500))); // first pixel right of the preview
	// No preview rect yet: the click cannot be placed, so it keeps the old behaviour.
	Input none = Editor(1300, 500); none.preview = Rect();
	CHECK(!ClickSwitchesToUiMode(none));
	// The demo bar does not matter while a workspace owns the screen.
	Input bar = Editor(400, 500); bar.demoBar = Regular(0, 0).demoBar;
	CHECK(!ClickSwitchesToUiMode(bar));
}

void TestRegularViewing() {
	CHECK(ClickSwitchesToUiMode(Regular(800, 1150)));  // on the demo bar
	CHECK(!ClickSwitchesToUiMode(Regular(800, 600)));  // the world
	CHECK(!ClickSwitchesToUiMode(Regular(50, 1150)));  // beside the bar
	Input hidden = Regular(800, 1150); hidden.demoBar = Rect(); // bar hidden / not found
	CHECK(!ClickSwitchesToUiMode(hidden));
}

void TestGates() {
	Input in = Editor(1300, 500);
	in.uiModeOn = true;          // already UI mode: the click is the panel's anyway
	CHECK(!ClickSwitchesToUiMode(in));
	// Mid-look the hidden cursor is pinned to the window centre, which is inside the preview.
	CHECK(!ClickSwitchesToUiMode(Editor(800, 600)));
	CHECK(!ClickSwitchesToUiMode(Regular(800, 600)));
	in = Regular(800, 1150);
	in.uiModeAvailable = false;  // first person: G has no UI mode to give
	CHECK(!ClickSwitchesToUiMode(in));
}

void TestFractions() {
	const Rect r = FromFractions(0.25f, 0.5f, 0.75f, 1.0f, 1920, 1080);
	CHECK(r.valid && r.x0 == 480 && r.y0 == 540 && r.x1 == 1440 && r.y1 == 1080);
	CHECK(!FromFractions(0.5f, 0, 0.5f, 1, 1920, 1080).valid); // zero width
	CHECK(!FromFractions(0, 0, 1, 1, 0, 1080).valid);          // no client size
}
} // namespace

int main() {
	TestWorkspace();
	TestRegularViewing();
	TestGates();
	TestFractions();
	if (g_failed) { std::printf("%d check(s) failed\n", g_failed); return 1; }
	std::printf("ui-click-math-tests: all passed\n");
	return 0;
}
