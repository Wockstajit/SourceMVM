// Tests for the Deadlock workspace layout solver.
//
// The layout is the thing that was wrong on screen, so it is the thing that gets a test. Each case
// below is a shape the panel actually has to survive: a first frame with no measurement yet, a
// closed dock, a column wider than the screen, an ultrawide monitor.

#include "../../AfxHookSource2/Filmmaker/Deadlock/DeadlockWorkspaceLayoutMath.h"

#include <cmath>
#include <cstdio>
#include <limits>

using Filmmaker::WorkspaceLayout;
using Filmmaker::WorkspaceLayoutInput;
using Filmmaker::WorkspaceLayout_Solve;
using Filmmaker::WorkspaceLayout_ClampInspector;
using Filmmaker::WorkspaceLayout_TickToX;
using Filmmaker::WorkspaceLayout_XToTick;

int main() {
	int failures = 0;
	auto check = [&](bool value, const char* what) {
		if (!value) { ++failures; std::printf("  FAIL %s\n", what); }
	};
	auto near = [](double a, double b) { return std::fabs(a - b) < 1e-6; };

	// --- a 1920x1080 screen, 520px column, 200px dock, 44px header ---------------------------
	{
		WorkspaceLayoutInput in;
		in.screenW = 1920; in.screenH = 1080;
		in.inspectorW = 520; in.dockH = 200; in.headerH = 44;
		const WorkspaceLayout out = WorkspaceLayout_Solve(in);
		check(out.valid, "1920x1080 solves");
		check(near(out.inspectorW, 520), "column keeps its requested width");
		check(near(out.areaW, 1400), "area is screen minus column");
		check(near(out.areaH, 836), "area is screen minus header and dock");
		check(near(out.areaY, 44), "area starts below the header");
		// 1400 wide at 16:9 wants 787.5 high, which fits in 836, so width is the binding edge.
		check(near(out.previewW, 1400), "preview fills the band's width");
		check(near(out.previewH, 787), "preview height follows the world aspect");
		check(out.previewY > 44, "preview is centred vertically in the band");
		check(out.blitX0 >= 0 && out.blitX1 <= 1, "blit rect stays normalised");
		check(near(out.blitX1 - out.blitX0, 1400.0 / 1920.0), "blit width matches the preview");
	}

	// --- height-bound: a tall narrow band letterboxes on the sides ---------------------------
	{
		WorkspaceLayoutInput in;
		in.screenW = 1920; in.screenH = 1080;
		in.inspectorW = 1000; in.dockH = 0; in.headerH = 0;
		const WorkspaceLayout out = WorkspaceLayout_Solve(in);
		check(out.valid, "narrow band solves");
		check(near(out.areaW, 920), "area narrows with a wide column");
		// 920 wide would want 517.5 high; that fits, so still width-bound. Force height-bound:
		WorkspaceLayoutInput tall = in;
		tall.dockH = 700;
		const WorkspaceLayout t = WorkspaceLayout_Solve(tall);
		check(t.valid, "short band solves");
		check(near(t.areaH, 380), "area shortens with a tall dock");
		check(near(t.previewH, 380), "preview fills the band's height when height-bound");
		check(t.previewW < t.areaW, "preview letterboxes at the sides when height-bound");
		check(t.previewX > 0, "preview is centred horizontally when height-bound");
	}

	// --- THE FRAME NEVER COVERS THE RENDER ----------------------------------------------------
	//
	// The chrome draws a band across the foot of the area, closed by a 2px rule, and another rule
	// above the area. Both used to be placed in JavaScript at literal offsets from areaY and from
	// screenH - dockH while the solver letterboxed into the FULL area -- so on a 1920x1080 native
	// tab the top rule landed at y=88 with the picture starting at 168, and the bottom rule plus a
	// 44px translucent band were drawn over live world that ran on to 955, underneath the dock.
	// One case per real dock height, and the invariant is the one that was being violated.
	{
		const double docks[] = { 130, 208, 422 };  // native, camera timeline, graph
		for (int i = 0; i < 3; ++i) {
			WorkspaceLayoutInput in;
			in.screenW = 1920; in.screenH = 1080;
			in.inspectorW = 520; in.headerH = 44; in.dockH = docks[i];
			in.bottomBandH = 46;
			const WorkspaceLayout out = WorkspaceLayout_Solve(in);
			check(out.valid, "banded layout solves");
			check(out.previewY >= out.frameTopY + 2, "picture starts below the top rule");
			check(out.previewY + out.previewH <= out.frameBottomY,
			      "picture ends above the bottom rule");
			check(out.previewY + out.previewH <= in.screenH - in.dockH,
			      "picture never runs under the dock");
			check(near(out.bandBottomY, out.frameBottomY + 2), "the band opens under its rule");
			check(out.bandBottomY <= out.areaY + out.areaH, "the band stays inside the area");
		}
	}

	// A zero band is the old behaviour exactly, so nothing that does not draw a band pays for one.
	{
		WorkspaceLayoutInput in;
		in.screenW = 1920; in.screenH = 1080;
		in.inspectorW = 520; in.dockH = 200; in.headerH = 44; in.bottomBandH = 0;
		const WorkspaceLayout out = WorkspaceLayout_Solve(in);
		check(near(out.previewY, std::floor(44 + (836 - 787) * 0.5)),
		      "no band means the picture is centred in the whole area");
		check(near(out.frameTopY, 42), "the top rule closes the header band");
	}

	// --- the column is clamped, never trusted -------------------------------------------------
	{
		check(near(WorkspaceLayout_ClampInspector(100, 1920), 320), "column floors at 320");
		check(near(WorkspaceLayout_ClampInspector(5000, 1920), std::floor(1920 * 0.62)),
		      "column caps at 62% of the screen");
		check(near(WorkspaceLayout_ClampInspector(0, 1920), 0), "no column means no column");
		// A tiny screen must not produce a column wider than it: the floor wins, and the caller
		// then sees an invalid layout rather than a negative band.
		check(near(WorkspaceLayout_ClampInspector(520, 400), 320), "column floor survives a tiny screen");
	}

	// --- degenerate inputs are rejected, not clamped into nonsense ---------------------------
	{
		WorkspaceLayoutInput in;
		in.screenW = 0; in.screenH = 1080;
		check(!WorkspaceLayout_Solve(in).valid, "zero width is invalid");

		in.screenW = std::numeric_limits<double>::quiet_NaN(); in.screenH = 1080;
		check(!WorkspaceLayout_Solve(in).valid, "NaN width is invalid");

		// The first frames after a build report 0 before layout settles. That must read as
		// "hide the chrome", never as a blit into a degenerate rect.
		in.screenW = 1920; in.screenH = 1080; in.inspectorW = 520; in.dockH = 1060; in.headerH = 44;
		check(!WorkspaceLayout_Solve(in).valid, "a dock taller than the screen is invalid");
	}

	// --- ultrawide ----------------------------------------------------------------------------
	{
		WorkspaceLayoutInput in;
		in.screenW = 3440; in.screenH = 1440;
		in.inspectorW = 520; in.dockH = 200; in.headerH = 44;
		const WorkspaceLayout out = WorkspaceLayout_Solve(in);
		check(out.valid, "ultrawide solves");
		check(near(out.areaW, 2920), "ultrawide band is wide");
		// 2920 at 16:9 wants 1642.5 high but only 1196 is available, so this one is height-bound.
		check(near(out.previewH, 1196), "ultrawide preview is height-bound");
		check(out.previewX > 0, "ultrawide preview letterboxes at the sides");
	}

	// --- a non-16:9 world aspect is honoured --------------------------------------------------
	{
		WorkspaceLayoutInput in;
		in.screenW = 1600; in.screenH = 1200;
		in.inspectorW = 400; in.dockH = 0; in.headerH = 0;
		in.worldAspect = 4.0 / 3.0;
		const WorkspaceLayout out = WorkspaceLayout_Solve(in);
		check(out.valid, "4:3 solves");
		check(near(out.previewW, 1200), "4:3 preview width follows the band height");
		check(near(out.previewH, 900), "4:3 preview height follows its aspect");
	}

	// --- the render workspace's JOB column insets the band from the LEFT ----------------------
	{
		WorkspaceLayoutInput in;
		in.screenW = 1920; in.screenH = 1080;
		in.inspectorW = 520; in.leftW = 340; in.dockH = 130; in.headerH = 44;
		const WorkspaceLayout out = WorkspaceLayout_Solve(in);
		check(out.valid, "a left column still solves");
		check(near(out.leftW, 340), "the left column keeps its width");
		// The band is what is left BETWEEN the two columns, and it starts at the left one's edge.
		check(near(out.areaX, 340), "the band starts at the left column");
		check(near(out.areaW, 1060), "the band is inset by both columns");
		check(out.previewX >= 340, "the preview never starts inside the left column");
		check(near(out.previewX + out.previewW, 1400) || out.previewX + out.previewW <= 1400,
		      "the preview never runs into the inspector");
	}

	// A left column wider than the room left over is clamped rather than producing a negative
	// band: the column is lost, which is visible, instead of the whole layout going invalid.
	{
		WorkspaceLayoutInput in;
		in.screenW = 800; in.screenH = 600;
		in.inspectorW = 500; in.leftW = 340; in.dockH = 0; in.headerH = 0;
		const WorkspaceLayout out = WorkspaceLayout_Solve(in);
		// 500 clamps to 496 (62% of 800), leaving 304; the left column may take at most 304-64.
		check(out.leftW <= 240, "the left column is clamped to what is left");
		check(out.areaW >= 64, "the band never goes below the minimum");
	}

	// No left column is the default, and it must leave the band exactly where it was.
	{
		WorkspaceLayoutInput in;
		in.screenW = 1920; in.screenH = 1080;
		in.inspectorW = 520; in.dockH = 130; in.headerH = 44;
		const WorkspaceLayout out = WorkspaceLayout_Solve(in);
		check(near(out.leftW, 0), "no left column by default");
		check(near(out.areaX, 0), "the band starts at the screen edge");
		check(near(out.areaW, 1400), "the band is inset by the inspector alone");
	}

	// --- tick <-> x round-trips ----------------------------------------------------------------
	{
		check(near(WorkspaceLayout_TickToX(0, 0, 100, 10, 200), 10), "first tick maps to strip start");
		check(near(WorkspaceLayout_TickToX(100, 0, 100, 10, 200), 210), "last tick maps to strip end");
		check(near(WorkspaceLayout_TickToX(50, 0, 100, 10, 200), 110), "mid tick maps to strip middle");
		// Out of range clamps rather than running off the strip: a playhead past the end must sit
		// ON the end, not thirty pixels past the panel it lives in.
		check(near(WorkspaceLayout_TickToX(500, 0, 100, 10, 200), 210), "a tick past the end clamps");
		check(near(WorkspaceLayout_TickToX(-500, 0, 100, 10, 200), 10), "a tick before the start clamps");
		check(near(WorkspaceLayout_XToTick(110, 0, 100, 10, 200), 50), "x maps back to its tick");
		check(near(WorkspaceLayout_XToTick(-99, 0, 100, 10, 200), 0), "x before the strip clamps");
		check(near(WorkspaceLayout_XToTick(9999, 0, 100, 10, 200), 100), "x past the strip clamps");
		// A degenerate range must not divide by zero.
		check(near(WorkspaceLayout_TickToX(5, 100, 100, 10, 200), 10), "a zero-length range is safe");
		check(near(WorkspaceLayout_XToTick(150, 100, 100, 10, 200), 100), "a zero-length range maps back safely");
	}

	std::printf("Deadlock workspace layout math: %d failures\n", failures);
	return failures ? 1 : 0;
}
