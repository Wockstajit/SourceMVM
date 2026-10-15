#pragma once

// Where the workspace puts things: the scaled world preview, the inspector column, the timeline
// dock.
//
// This is the arithmetic the Deadlock workspace was missing entirely. The panel it replaces drew a
// rail down the left and an opaque body across everything else, so the game rendered FULL SCREEN
// behind it and there was no preview at all -- which is what "there's no preview window and stuff
// like that, the UI is weird, everything's on the left" describes.
//
// The layout is the CS2 camera editor's, one-to-one:
//
//     +------------------------------------------+------------------+
//     |  letterbox                               |                  |
//     |      +--------------------------+        |  INSPECTOR       |
//     |      |   scaled world preview   |        |  COLUMN          |
//     |      +--------------------------+        |  (right)         |
//     |  letterbox                               |                  |
//     +------------------------------------------+------------------+
//     |  TIMELINE DOCK                                              |
//     +-------------------------------------------------------------+
//
// TWO CONSUMERS, ONE ANSWER. The Panorama chrome positions itself in virtual pixels, and the D3D
// blit that shrinks the world wants normalised backbuffer fractions. Both come out of one Solve()
// so they cannot drift: a chrome frame that disagrees with the blit by two pixels shows a seam of
// un-shrunk world along one edge, and chasing that from two separate copies of the maths is how
// the CS2 side lost an afternoon.
//
// VIRTUAL PIXELS. Panorama reports `actuallayoutwidth` in device pixels and `actualuiscale_x` as
// the factor between those and the units `style.width` accepts. Everything here is in the latter,
// so the caller divides once before calling and never again.
//
// Engine-free and header-only so it can be tested without the game:
//   Automation/tests/deadlock-workspace-layout-math-tests.cpp

#include <algorithm>
#include <cmath>

namespace Filmmaker {

struct WorkspaceLayoutInput {
	// Screen, in virtual pixels.
	double screenW = 0;
	double screenH = 0;

	// The inspector column's requested width. Clamped below; 0 means "no column" (the render
	// workspace runs full width).
	double inspectorW = 0;

	// A fixed column pinned to the LEFT of the band, 0 when there is none. Only the render
	// workspace has one: its JOB column, which states what is about to be captured and why it
	// cannot start. It insets the preview exactly the way the inspector column does on the right,
	// so the world is never drawn underneath it.
	double leftW = 0;

	// The timeline dock's height at the bottom, 0 when it is closed.
	double dockH = 0;

	// The header strip across the top of the workspace. It is drawn ABOVE areaY, in 0..headerH,
	// so the area the picture is letterboxed into begins where the header ends.
	double headerH = 0;

	// The accent band the chrome draws across the BOTTOM of the area (a translucent strip plus the
	// 2px rule that closes it). Reserved here so the picture is never letterboxed underneath it.
	//
	// It has to be a solver input rather than a constant in the panel. The chrome used to place
	// that band at a fixed offset from the dock while the solver centred the picture in the whole
	// area, so on every dock tab the band and its rule landed ON the render -- measured at 1920x1080
	// on the native tab as a gold rule across the world at y=904 and 44px of translucent band over
	// the world's bottom edge, with the world running on to 955 underneath the dock.
	double bottomBandH = 0;

	// The world's aspect ratio (width / height). The preview is letterboxed to this so the shrunk
	// image is the same picture as the full-screen one, not a differently framed crop.
	double worldAspect = 16.0 / 9.0;
};

struct WorkspaceLayout {
	bool valid = false;

	// The columns, in virtual px, after clamping.
	double inspectorW = 0;
	double leftW = 0;

	// The band the world may occupy: everything left of the column and above the dock.
	double areaX = 0, areaY = 0, areaW = 0, areaH = 0;

	// The letterboxed world inside that band.
	double previewX = 0, previewY = 0, previewW = 0, previewH = 0;

	// Where the chrome draws its two accent rules and its lower band, in the same virtual pixels.
	// Solved here so the frame and the picture cannot disagree: the invariant the tests hold is
	// frameTopY + 2 <= previewY and previewY + previewH <= frameBottomY.
	double frameTopY = 0;     // the 2px rule that closes the header band; sits just above areaY
	double frameBottomY = 0;  // the 2px rule that opens the lower band
	double bandBottomY = 0;   // the lower band itself, running to areaY + areaH

	// The same rect as normalised backbuffer fractions, for the D3D blit. Top-left / bottom-right.
	double blitX0 = 0, blitY0 = 0, blitX1 = 0, blitY1 = 0;
};

// A column narrower than this cannot hold two value tiles side by side, which is the unit the card
// grammar is built from; wider than 62% of the screen and the preview stops being a preview.
inline double WorkspaceLayout_ClampInspector(double requested, double screenW) {
	if (!(requested > 0)) return 0;
	double w = std::max(320.0, requested);
	if (screenW > 1) w = std::min(w, std::max(320.0, screenW * 0.62));
	return std::floor(w);
}

inline bool WorkspaceLayout_FinitePositive(double v) {
	return std::isfinite(v) && v > 0;
}

inline WorkspaceLayout WorkspaceLayout_Solve(const WorkspaceLayoutInput& in) {
	WorkspaceLayout out;

	if (!WorkspaceLayout_FinitePositive(in.screenW) || !WorkspaceLayout_FinitePositive(in.screenH))
		return out;

	out.inspectorW = WorkspaceLayout_ClampInspector(in.inspectorW, in.screenW);

	const double dockH = std::isfinite(in.dockH) ? std::max(0.0, in.dockH) : 0.0;
	const double headerH = std::isfinite(in.headerH) ? std::max(0.0, in.headerH) : 0.0;

	// The left column is clamped to what is actually left after the inspector, so a narrow screen
	// loses the JOB column rather than producing a negative band and an invalid layout.
	double leftW = std::isfinite(in.leftW) ? std::max(0.0, in.leftW) : 0.0;
	leftW = std::min(leftW, std::max(0.0, in.screenW - out.inspectorW - 64.0));
	out.leftW = std::floor(leftW);

	out.areaX = out.leftW;
	out.areaY = headerH;
	out.areaW = in.screenW - out.inspectorW - out.leftW;
	out.areaH = in.screenH - headerH - dockH;

	// A band this small is not a preview, it is a smear. Report invalid and let the caller hide the
	// chrome and release the blit rather than asking the render layer to scale into four pixels.
	if (out.areaW < 64 || out.areaH < 48)
		return out;

	double aspect = in.worldAspect;
	if (!WorkspaceLayout_FinitePositive(aspect)) aspect = 16.0 / 9.0;

	// THE PICTURE GOES IN THE AREA MINUS THE CHROME'S OWN BAND, not in the whole area.
	//
	// The chrome paints a band across the bottom of the area; letterboxing into the full height and
	// then drawing that band is how the frame ended up over the render on every dock tab.
	const double bandH = std::isfinite(in.bottomBandH)
		? std::max(0.0, std::min(in.bottomBandH, out.areaH * 0.5))
		: 0.0;
	const double innerH = out.areaH - bandH;

	// Letterbox: fit the world's aspect inside the inner band, centred.
	double pw = out.areaW;
	double ph = pw / aspect;
	if (ph > innerH) {
		ph = innerH;
		pw = ph * aspect;
	}

	out.previewW = std::floor(pw);
	out.previewH = std::floor(ph);
	out.previewX = std::floor(out.areaX + (out.areaW - out.previewW) * 0.5);
	out.previewY = std::floor(out.areaY + (innerH - out.previewH) * 0.5);

	// The chrome's own rects, derived from the same numbers. The top rule closes the header band
	// immediately above the area; the bottom rule opens the band that runs to the area's foot.
	out.frameTopY = out.areaY - 2;
	out.frameBottomY = std::floor(out.areaY + out.areaH - bandH);
	out.bandBottomY = out.frameBottomY + 2;

	out.blitX0 = out.previewX / in.screenW;
	out.blitY0 = out.previewY / in.screenH;
	out.blitX1 = (out.previewX + out.previewW) / in.screenW;
	out.blitY1 = (out.previewY + out.previewH) / in.screenH;

	// The render layer rejects anything outside [0,1]; clamping here means a rounding artefact at
	// the edge never costs the whole blit.
	out.blitX0 = std::max(0.0, std::min(1.0, out.blitX0));
	out.blitY0 = std::max(0.0, std::min(1.0, out.blitY0));
	out.blitX1 = std::max(0.0, std::min(1.0, out.blitX1));
	out.blitY1 = std::max(0.0, std::min(1.0, out.blitY1));

	out.valid = (out.blitX1 - out.blitX0) > 0.01 && (out.blitY1 - out.blitY0) > 0.01;
	return out;
}

// Where a tick sits along a timeline strip, and back again. The timeline and the graph are two
// views of one shot, so both map ticks to pixels with this rather than each rolling its own and
// disagreeing by a pixel at the ends.
inline double WorkspaceLayout_TickToX(double tick, double firstTick, double lastTick,
                                      double stripX, double stripW) {
	if (!(lastTick > firstTick) || !WorkspaceLayout_FinitePositive(stripW)) return stripX;
	double t = (tick - firstTick) / (lastTick - firstTick);
	t = std::max(0.0, std::min(1.0, t));
	return stripX + t * stripW;
}

inline double WorkspaceLayout_XToTick(double x, double firstTick, double lastTick,
                                      double stripX, double stripW) {
	if (!(lastTick > firstTick) || !WorkspaceLayout_FinitePositive(stripW)) return firstTick;
	double t = (x - stripX) / stripW;
	t = std::max(0.0, std::min(1.0, t));
	return firstTick + t * (lastTick - firstTick);
}

} // namespace Filmmaker
