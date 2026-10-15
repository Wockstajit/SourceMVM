#pragma once

// IN-WORLD CAMPATH GIZMOS -- CS2's look, on Deadlock's own projection.
//
// What CS2 SOURCE:MVM's CampathDrawer.cpp draws in marker style, stroke for stroke: the camera
// trajectory as an 8px line (Live gold / Freeze blue), each camera marker as a wireframe camera
// (apex, FOV rectangle, roof triangle) under five additive glow passes, white while the crosshair is
// on it, and the follow target as a cyan wireframe box under three. Widths, colours, glow passes and
// the constant-on-screen sizing are CS2's own constants (DeadlockOverlayMath.h kGizmo*).
//
// WHY NOT CS2'S SHADER. afx_line_vs_5_0 projects WORLD points with a view-projection matrix Deadlock
// has not been shown to expose. This drawer projects on the CPU from the pose the view hook applied
// this frame and extrudes every stroke into a screen-space quad there (DeadlockOverlayMath.h
// ThickLineQuad / PolylineJoinOffset), so its shader pair only maps pixels to clip space. CS2's line
// pixel shader is a flat colour as well, so the strokes come out the same.
//
// NOT YET: CS2's keyframe number digits, drawn from HLAE's hexfont.tga -- they need a textured
// variant of this shader pair and the font embedded in the DLL.
//
// THREADING. The setters are called from the MAIN thread (Workspaces_RunMainThreadFrame) and copy
// into a mutex-guarded snapshot. `OverlayDrawer_RunPreUi` runs on the RENDER thread at the pre-UI
// moment and only ever reads that snapshot.
//
// DRAW ORDER. Must run BEFORE ViewportBlit_RunPreUi(): the world renders full-screen and the blit
// then scales that image into the preview rect, so the gizmos scale with the world -- the same
// ordering CS2's CampathDrawer uses ahead of its own preview-scaling blit.

#include <d3d11.h>

namespace Deadlock {

void OverlayDrawer_BeginDevice(ID3D11Device* device);
void OverlayDrawer_EndDevice();
bool OverlayDrawer_Ready();

// The kill switch (`mirv_filmmaker overlay on|off|status`). ON by default: CS2 draws its path gizmo
// automatically whenever a path exists (CameraPath::EnsureDrawState), and a marker placed with K
// that appears nowhere reads as K having done nothing.
void OverlayDrawer_SetEnabled(bool enabled);
bool OverlayDrawer_Enabled();

// The follow camera's target, in WORLD space: `feet` is the pawn's scene-node origin, the box is
// `halfWidth` either side on X/Y and rises `height` from the feet. Drawn as CS2's cyan wireframe
// box (CampathDrawer's FollowTargetBox).
struct OverlayHeroBox {
	double feetX = 0.0, feetY = 0.0, feetZ = 0.0;
	double halfWidth = 22.0;
	double height = 76.0;
};
void OverlayDrawer_SetHeroes(const OverlayHeroBox* boxes, int count);

// The trajectory, already sampled along the curve by the shot, and the camera markers with their
// own pose (the gizmo is shaped by each marker's angles and FOV). `hovered` is the one under the
// crosshair -- CS2's white "you are looking at it" state, the cue that F / L act on it.
struct OverlayPathPoint { double x = 0.0, y = 0.0, z = 0.0; };
struct OverlayMarker {
	double x = 0.0, y = 0.0, z = 0.0;
	double pitch = 0.0, yaw = 0.0, roll = 0.0;
	double fov = 90.0;
	bool hovered = false;
};
void OverlayDrawer_SetPath(const OverlayPathPoint* points, int count);
void OverlayDrawer_SetMarkers(const OverlayMarker* markers, int count);

// CS2 colours the whole gizmo by the path's timing: gold for Live, blue for Freeze.
void OverlayDrawer_SetPathStyle(bool freeze);

// Render thread, pre-UI moment. No-ops when disabled, not ready, or nothing is published.
void OverlayDrawer_RunPreUi();

void OverlayDrawer_Report();

} // namespace Deadlock
