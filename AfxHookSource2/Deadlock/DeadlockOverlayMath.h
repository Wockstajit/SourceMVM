#pragma once

// THE POSE THE OVERLAY DRAWS WITH, AND HOW A WORLD POINT BECOMES A SCREEN PIXEL.
//
// The Deadlock editor draws an in-world D3D overlay in the Present hook -- the dolly camera path
// polyline, the marker diamonds, and a box+name over every hero. All three need the same pipeline:
// take the pose the engine actually used THIS frame (position + Source pitch/yaw/roll + the view
// struct's horizontal FOV) and turn a world point into a pixel the overlay can draw a line to. Get
// any one of these wrong -- the pitch sign, which axis is "right", the near plane, the aspect used
// for the vertical extent, or the viewport/back-buffer/preview-rect chain -- and the drawing still
// runs, it just paints in the wrong place or flips through the camera, which is much harder to
// notice than a crash. Hence pulling the whole pipeline into pure, tested functions.
//
// MEASURED CONVENTION (live Deadlock, 2026-09-13, `mirv_input angles` captures at pitch +/-45), not
// re-derived from CS2: pitch/yaw/roll are Source degrees, POSITIVE PITCH LOOKS DOWN, yaw 0 faces
// +X, yaw 90 faces +Y, Z is up, and the view struct's FOV is a HORIZONTAL field of view already
// aspect-widened for the actual viewport (DeadlockViewMath.h ScaleFovForAspect does that widening;
// this header consumes an already-widened value, it does not redo that step).
//
// WHY THE BASIS MATH IS DUPLICATED HERE INSTEAD OF CALLING FollowCameraMath.h. FollowRotateVector
// and FollowInverseRotateVector implement exactly this Source AngleVectors basis already, and their
// formula is what CameraBasisFromAngles below reproduces -- but they are declared, not defined, in
// that header, so using them means also linking Filmmaker/Movie/FollowCameraMath.cpp. This header
// has to stay header-only (a standalone .cpp test compiled against nothing but this file, the same
// contract every other engine-free *Math.h in this repo follows), so the small trig block is
// duplicated here rather than pulling in that link dependency. The formula is the well-tested
// Source AngleVectors identity; FollowCameraMathTests exercises the same math from the Filmmaker
// side, and this header's own tests exercise it from here.

#include <algorithm>
#include <cmath>

namespace Deadlock {

struct OverlayVec3 {
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

// forward/right/up form an orthonormal basis in world space for a given Source pitch/yaw/roll.
struct OverlayBasis {
	OverlayVec3 forward;
	OverlayVec3 right;
	OverlayVec3 up;
};

// Source AngleVectors, in the convention this repo measured for Deadlock: positive pitch tilts
// `forward` toward -Z (looks down); at roll 0, `right` is the horizontal direction at (yaw - 90)
// degrees; roll then rotates `right`/`up` around `forward` without touching `forward` itself.
inline OverlayBasis CameraBasisFromAngles(double pitch, double yaw, double roll) {
	constexpr double kPi = 3.14159265358979323846;
	constexpr double kDeg2Rad = kPi / 180.0;
	const double sp = std::sin(pitch * kDeg2Rad), cp = std::cos(pitch * kDeg2Rad);
	const double sy = std::sin(yaw * kDeg2Rad), cy = std::cos(yaw * kDeg2Rad);
	const double sr = std::sin(roll * kDeg2Rad), cr = std::cos(roll * kDeg2Rad);
	OverlayBasis out;
	out.forward = { cp * cy, cp * sy, -sp };
	out.right = { -sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp };
	out.up = { cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp };
	return out;
}

// A world offset re-expressed along the camera's own axes: how far AHEAD (along `forward`), how
// far RIGHT, and how far UP the point sits relative to the camera. Kept as a distinct type from
// OverlayVec3 -- these three numbers are never a world-space position, and giving them their own
// names (instead of reusing .x/.y/.z for a different meaning) is what caught the sign mix-ups this
// header's tests check for.
struct OverlayCameraSpace {
	double forward = 0.0;
	double right = 0.0;
	double up = 0.0;
};

inline OverlayCameraSpace ToCameraSpace(const OverlayVec3& worldPoint, const OverlayVec3& cameraOrigin,
	const OverlayBasis& basis) {
	const OverlayVec3 d{ worldPoint.x - cameraOrigin.x, worldPoint.y - cameraOrigin.y, worldPoint.z - cameraOrigin.z };
	const auto dot = [&d](const OverlayVec3& v) { return d.x * v.x + d.y * v.y + d.z * v.z; };
	return { dot(basis.forward), dot(basis.right), dot(basis.up) };
}

// Everything the overlay needs to project a point, bundled once per frame: the basis is computed
// once from the engine's pitch/yaw/roll (CameraBasisFromAngles / MakeCameraPose) and then reused
// for every polyline vertex, marker corner, and hero box corner that frame -- not recomputed with
// fresh sin/cos per point.
struct OverlayCameraPose {
	OverlayVec3 origin;
	OverlayBasis basis;
	double horizontalFovDegrees = 90.0; // already aspect-widened for viewportWidth x viewportHeight
	int viewportWidth = 1;
	int viewportHeight = 1;
	double nearPlane = 4.0; // measured default: points at or behind this are not visible
};

inline OverlayCameraPose MakeCameraPose(const OverlayVec3& origin, double pitch, double yaw, double roll,
	double horizontalFovDegrees, int viewportWidth, int viewportHeight, double nearPlane = 4.0) {
	return OverlayCameraPose{ origin, CameraBasisFromAngles(pitch, yaw, roll), horizontalFovDegrees,
		viewportWidth, viewportHeight, nearPlane };
}

inline OverlayCameraSpace ToCameraSpace(const OverlayVec3& worldPoint, const OverlayCameraPose& pose) {
	return ToCameraSpace(worldPoint, pose.origin, pose.basis);
}

inline bool CameraSpaceVisible(const OverlayCameraSpace& cam, double nearPlane) {
	return cam.forward > nearPlane;
}

struct OverlayProjection {
	bool visible = false;
	double ndcX = 0.0;
	double ndcY = 0.0; // +1 = top of screen, -1 = bottom (world "up" projects to a smaller pixelY)
	double pixelX = 0.0;
	double pixelY = 0.0;
};

// The vertical extent comes from the SAME horizontal FOV plus the viewport's own aspect ratio
// (width/height), not a separately stored vertical FOV -- matching the engine, which only stores
// the one (already aspect-widened) horizontal value. See DeadlockViewMath.h ScaleFovForAspect for
// the sibling conversion (re-widening a FOV from one aspect to another); this is the same
// tan-based relationship applied once, for projection rather than for re-widening.
inline OverlayProjection ProjectCameraSpace(const OverlayCameraSpace& cam, double horizontalFovDegrees,
	int viewportWidth, int viewportHeight, double nearPlane) {
	OverlayProjection out;
	if (!CameraSpaceVisible(cam, nearPlane)) return out;
	if (!std::isfinite(horizontalFovDegrees) || horizontalFovDegrees <= 1.0 || horizontalFovDegrees >= 179.0) return out;
	if (viewportWidth <= 0 || viewportHeight <= 0) return out;
	constexpr double kPi = 3.14159265358979323846;
	const double tanHalfH = std::tan(horizontalFovDegrees * kPi / 360.0);
	const double aspect = static_cast<double>(viewportWidth) / static_cast<double>(viewportHeight);
	const double tanHalfV = tanHalfH / aspect;
	if (!(tanHalfH > 0.0) || !std::isfinite(tanHalfV) || !(tanHalfV > 0.0)) return out;
	out.ndcX = (cam.right / cam.forward) / tanHalfH;
	out.ndcY = (cam.up / cam.forward) / tanHalfV;
	out.pixelX = (out.ndcX + 1.0) * 0.5 * viewportWidth;
	out.pixelY = (1.0 - out.ndcY) * 0.5 * viewportHeight;
	out.visible = true;
	return out;
}

inline OverlayProjection ProjectPoint(const OverlayVec3& worldPoint, const OverlayCameraPose& pose) {
	return ProjectCameraSpace(ToCameraSpace(worldPoint, pose), pose.horizontalFovDegrees,
		pose.viewportWidth, pose.viewportHeight, pose.nearPlane);
}

// How far in front of the near plane a clipped endpoint is placed. Visibility is strictly
// `forward > nearPlane`, so an endpoint exactly ON the plane would be thrown away by the projection
// that follows -- which silently undid the clipping for every box straddling the plane.
constexpr double kOverlayClipEpsilon = 1e-7;

struct OverlayClippedSegment {
	OverlayVec3 a;
	OverlayVec3 b;
	bool visible = false; // false only when BOTH endpoints are at or behind the near plane
};

// Clip in WORLD space, before projecting. Projecting an endpoint that sits behind the camera and
// only then trying to fix up the result flips it to the wrong side of the screen instead of
// shortening the segment -- `forward` (the camera-space depth) is an affine function of world
// position for a fixed pose, so the near-plane crossing can be solved directly along the original
// world-space segment and the visible endpoint is returned untouched.
inline OverlayClippedSegment ClipSegmentNearPlane(const OverlayVec3& worldA, const OverlayVec3& worldB,
	const OverlayCameraPose& pose) {
	const OverlayCameraSpace camA = ToCameraSpace(worldA, pose);
	const OverlayCameraSpace camB = ToCameraSpace(worldB, pose);
	const bool aIn = CameraSpaceVisible(camA, pose.nearPlane);
	const bool bIn = CameraSpaceVisible(camB, pose.nearPlane);
	if (!aIn && !bIn) return { worldA, worldB, false };
	if (aIn && bIn) return { worldA, worldB, true };
	const double clipForward = pose.nearPlane + kOverlayClipEpsilon;
	const double denom = camB.forward - camA.forward;
	double t = (std::isfinite(denom) && denom != 0.0) ? (clipForward - camA.forward) / denom : 0.5;
	t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
	const OverlayVec3 crossing{
		worldA.x + (worldB.x - worldA.x) * t,
		worldA.y + (worldB.y - worldA.y) * t,
		worldA.z + (worldB.z - worldA.z) * t
	};
	return aIn ? OverlayClippedSegment{ worldA, crossing, true } : OverlayClippedSegment{ crossing, worldB, true };
}

struct OverlayScreenRect {
	bool visible = false; // false only when every corner is at or behind the near plane
	double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
};

// Bounds an upright box (feet at `feetOrigin`, footprint +/- halfWidth on X and Y, standing
// `height` above the feet) in screen pixels. Each of the 12 edges is near-plane clipped BEFORE
// projecting, so a hero straddling the near plane (walking past the camera) is bounded by its
// in-front corners plus the points where its edges cross that plane: wide, often past the screen
// edges, but finite and never flipped to the wrong side when a corner crosses behind.
inline OverlayScreenRect ProjectBox(const OverlayVec3& feetOrigin, double halfWidth, double height,
	const OverlayCameraPose& pose) {
	const OverlayVec3 corners[8] = {
		{ feetOrigin.x - halfWidth, feetOrigin.y - halfWidth, feetOrigin.z },
		{ feetOrigin.x + halfWidth, feetOrigin.y - halfWidth, feetOrigin.z },
		{ feetOrigin.x - halfWidth, feetOrigin.y + halfWidth, feetOrigin.z },
		{ feetOrigin.x + halfWidth, feetOrigin.y + halfWidth, feetOrigin.z },
		{ feetOrigin.x - halfWidth, feetOrigin.y - halfWidth, feetOrigin.z + height },
		{ feetOrigin.x + halfWidth, feetOrigin.y - halfWidth, feetOrigin.z + height },
		{ feetOrigin.x - halfWidth, feetOrigin.y + halfWidth, feetOrigin.z + height },
		{ feetOrigin.x + halfWidth, feetOrigin.y + halfWidth, feetOrigin.z + height },
	};
	// Each vertex to its three neighbours differing in exactly one of (x, y, z) -- the 12 edges of
	// a rectangular box.
	constexpr int kEdges[12][2] = {
		{0,1}, {0,2}, {0,4}, {1,3}, {1,5}, {2,3}, {2,6}, {3,7}, {4,5}, {4,6}, {5,7}, {6,7}
	};
	OverlayScreenRect rect;
	for (const auto& edge : kEdges) {
		const OverlayClippedSegment seg = ClipSegmentNearPlane(corners[edge[0]], corners[edge[1]], pose);
		if (!seg.visible) continue;
		const OverlayVec3 endpoints[2] = { seg.a, seg.b };
		for (const OverlayVec3& p : endpoints) {
			const OverlayProjection proj = ProjectPoint(p, pose);
			if (!proj.visible) continue; // defensive: ClipSegmentNearPlane keeps its endpoints in front
			if (!rect.visible) {
				rect.minX = rect.maxX = proj.pixelX;
				rect.minY = rect.maxY = proj.pixelY;
				rect.visible = true;
			} else {
				// Parenthesized (std::min) / (std::max): this header is included from files that
				// pull in <windows.h> without NOMINMAX (a deliberate project choice -- see
				// AfxHookSource2/Deadlock/CMakeLists.txt), whose min/max MACROS would otherwise
				// swallow the "::" and fail to compile with an unrelated-looking token error.
				rect.minX = (std::min)(rect.minX, proj.pixelX);
				rect.maxX = (std::max)(rect.maxX, proj.pixelX);
				rect.minY = (std::min)(rect.minY, proj.pixelY);
				rect.maxY = (std::max)(rect.maxY, proj.pixelY);
			}
		}
	}
	return rect;
}

struct OverlayPixel {
	double x = 0.0;
	double y = 0.0;
};

// Normalised fractions of the back buffer: (x0,y0) top-left, (x1,y1) bottom-right. Same convention
// as DeadlockViewportBlit.h ViewportBlit_SetRequest, whose rect this mirrors -- {0,0,1,1} is the
// no-preview, full-frame identity.
struct OverlayRect {
	double x0 = 0.0, y0 = 0.0, x1 = 1.0, y1 = 1.0;
};

// The engine's viewport and the D3D back buffer were both 1920x1080 at last measurement, but
// nothing here may assume they stay equal -- a plain per-axis scale between whatever the two
// actually are this frame.
inline OverlayPixel ScaleToBackBuffer(const OverlayPixel& viewportPixel, int viewportWidth, int viewportHeight,
	int backBufferWidth, int backBufferHeight) {
	if (viewportWidth <= 0 || viewportHeight <= 0 || backBufferWidth <= 0 || backBufferHeight <= 0)
		return viewportPixel; // no sane scale; pass through rather than divide by zero
	return {
		viewportPixel.x * (static_cast<double>(backBufferWidth) / viewportWidth),
		viewportPixel.y * (static_cast<double>(backBufferHeight) / viewportHeight)
	};
}

// The editor scales the full-frame render into `previewRect` (DeadlockViewportBlit.h does this in
// D3D); a point computed against the full back buffer needs the same remap to land where the
// scaled picture actually is. With `previewRect` at its {0,0,1,1} default this is the identity.
inline OverlayPixel RemapToPreviewRect(const OverlayPixel& backBufferPixel, int backBufferWidth, int backBufferHeight,
	const OverlayRect& previewRect) {
	if (backBufferWidth <= 0 || backBufferHeight <= 0) return backBufferPixel;
	return {
		previewRect.x0 * backBufferWidth + backBufferPixel.x * (previewRect.x1 - previewRect.x0),
		previewRect.y0 * backBufferHeight + backBufferPixel.y * (previewRect.y1 - previewRect.y0)
	};
}

struct OverlayScreenPoint {
	bool visible = false;
	double x = 0.0;
	double y = 0.0;
};

// The whole per-point pipeline the Present-hook drawer actually wants: project with this frame's
// pose, scale into the back buffer, then remap into whatever preview rect is currently active.
inline OverlayScreenPoint ProjectToScreen(const OverlayVec3& worldPoint, const OverlayCameraPose& pose,
	int backBufferWidth, int backBufferHeight, const OverlayRect& previewRect = OverlayRect{}) {
	const OverlayProjection proj = ProjectPoint(worldPoint, pose);
	if (!proj.visible) return {};
	const OverlayPixel viewportPixel{ proj.pixelX, proj.pixelY };
	const OverlayPixel backBufferPixel = ScaleToBackBuffer(viewportPixel, pose.viewportWidth, pose.viewportHeight,
		backBufferWidth, backBufferHeight);
	const OverlayPixel screenPixel = RemapToPreviewRect(backBufferPixel, backBufferWidth, backBufferHeight, previewRect);
	return { true, screenPixel.x, screenPixel.y };
}

// ============================================================================================
// CS2'S CAMPATH GIZMO (AfxHookSource2/CampathDrawer.cpp, marker style), as geometry.
//
// The constants are CampathDrawer.cpp's own; the functions are its DrawCamera, its constant-screen
// marker scale, afx_line_vs_5_0's screen-space stroke extrusion and mitre, and
// CameraPathPicking.cpp's crosshair pick. Kept here, engine-free, so the Deadlock drawer draws the
// same shapes CS2 does and the tests can say so.
// ============================================================================================

constexpr double kGizmoCrossRadius = 36.0;                      // c_CampathCrossRadius
constexpr double kGizmoCameraRadius = kGizmoCrossRadius / 2.0;  // c_CameraRadius
constexpr double kGizmoLinePixelWidth = 4.0;                    // c_CampathCrossPixelWidth / c_CameraPixelWidth
constexpr double kGizmoTrajectoryPixelWidth = 8.0;              // c_CameraTrajectoryPixelWidth
constexpr double kGizmoScreenRefDist = 300.0;                   // c_MarkerScreenRefDist
constexpr double kGizmoScreenMinScale = 0.5;                    // c_MarkerScreenMinScale
constexpr double kGizmoScreenMaxScale = 2.2;                    // c_MarkerScreenMaxScale

// A marker's gizmo scale: 1 at 300 units from the viewer, clamped to [0.5, 2.2], so near markers do
// not swell and far ones stay findable (CS2's gscale).
inline double MarkerScreenScale(const OverlayVec3& marker, const OverlayVec3& viewer) {
	const double dx = marker.x - viewer.x, dy = marker.y - viewer.y, dz = marker.z - viewer.z;
	double s = std::sqrt(dx * dx + dy * dy + dz * dz) / kGizmoScreenRefDist;
	if (!std::isfinite(s)) s = kGizmoScreenMinScale;
	return s < kGizmoScreenMinScale ? kGizmoScreenMinScale : (s > kGizmoScreenMaxScale ? kGizmoScreenMaxScale : s);
}

struct OverlaySegment3 {
	OverlayVec3 a;
	OverlayVec3 b;
};

// CCampathDrawer::DrawCamera: the apex at the camera, a rectangle `radius` ahead whose half-width
// follows the marker's FOV and whose half-height follows the screen aspect, and a roof triangle on
// the rectangle's top edge so the reader can see which way is up. Ten edges, in CS2's order.
inline int CameraGizmoSegments(const OverlayVec3& position, double pitch, double yaw, double roll, double fov,
	int screenWidth, int screenHeight, double sizeScale, OverlaySegment3 out[10]) {
	constexpr double kPi = 3.14159265358979323846;
	if (!std::isfinite(fov)) fov = 90.0;
	fov = fov < 1.0 ? 1.0 : (fov > 179.0 ? 179.0 : fov);
	const OverlayBasis basis = CameraBasisFromAngles(pitch, yaw, roll);
	const double radius = kGizmoCameraRadius * sizeScale;
	const double a = std::sin(fov * kPi / 360.0) * radius;
	const double b = a * (screenWidth > 0 ? static_cast<double>(screenHeight) / screenWidth : 1.0);
	const auto at = [&](double f, double r, double u) {
		return OverlayVec3{
			position.x + f * basis.forward.x + r * basis.right.x + u * basis.up.x,
			position.y + f * basis.forward.y + r * basis.right.y + u * basis.up.y,
			position.z + f * basis.forward.z + r * basis.right.z + u * basis.up.z };
	};
	const OverlayVec3 lu = at(radius, -a, b), ru = at(radius, a, b);
	const OverlayVec3 ld = at(radius, -a, -b), rd = at(radius, a, -b);
	const OverlayVec3 roof = at(radius, 0.0, 1.5 * b); // midpoint of the top edge, raised half a b
	out[0] = { position, ld }; out[1] = { position, rd }; out[2] = { position, lu }; out[3] = { position, ru };
	out[4] = { ld, rd };       out[5] = { rd, ru };       out[6] = { ru, lu };       out[7] = { lu, ld };
	out[8] = { lu, roof };     out[9] = { ru, roof };
	return 10;
}

// afx_line_vs_5_0 for a single line: each end pushed out half the width either side, perpendicular
// to the line IN SCREEN SPACE, so the stroke is `width` pixels at any depth. Corners are
// {a+, a-, b+, b-}. False for a zero-length line, which CS2's saveNormalize also draws as nothing.
inline bool ThickLineQuad(const OverlayPixel& a, const OverlayPixel& b, double width, OverlayPixel out[4]) {
	const double dx = b.x - a.x, dy = b.y - a.y;
	const double len = std::sqrt(dx * dx + dy * dy);
	if (!std::isfinite(len) || len < 1e-6) return false;
	const double nx = -dy / len * width * 0.5, ny = dx / len * width * 0.5;
	out[0] = { a.x + nx, a.y + ny }; out[1] = { a.x - nx, a.y - ny };
	out[2] = { b.x + nx, b.y + ny }; out[3] = { b.x - nx, b.y - ny };
	return true;
}

// afx_line_vs_5_0's polyline join, for the "+" side (the "-" side is its negation): the normals of
// the incoming and outgoing segments are summed and clamped to their bounding box -- CS2's own mitre
// limit -- then scaled to half the width. Adjacent quads share these offsets, so a stroke has no
// overlap at its joints (an overlap would double a translucent colour there). `prev` / `next` are
// null at the ends of a run.
inline OverlayPixel PolylineJoinOffset(const OverlayPixel* prev, const OverlayPixel& cur, const OverlayPixel* next,
	double width) {
	const auto unit = [](double x, double y) {
		const double l = std::sqrt(x * x + y * y);
		return (std::isfinite(l) && l >= 1e-6) ? OverlayPixel{ x / l, y / l } : OverlayPixel{ 0.0, 0.0 };
	};
	OverlayPixel p{ 0.0, 0.0 }, n{ 0.0, 0.0 };
	if (prev) p = unit(-(prev->y - cur.y), prev->x - cur.x); // rot90CCW(prev - cur)
	if (next) n = unit(next->y - cur.y, -(next->x - cur.x)); // rot90CW(next - cur)
	double mx = p.x + n.x, my = p.y + n.y;
	const double loX = p.x < n.x ? p.x : n.x, hiX = p.x < n.x ? n.x : p.x;
	const double loY = p.y < n.y ? p.y : n.y, hiY = p.y < n.y ? n.y : p.y;
	mx = mx < loX ? loX : (mx > hiX ? hiX : mx);
	my = my < loY ? loY : (my > hiY ? hiY : my);
	return { mx * width * 0.5, my * width * 0.5 };
}

// CameraPathPicking.cpp's UpdateHover: the marker the crosshair is aiming at, or -1. A narrow ~5
// degree cone, widened up close by the angle a 30-unit marker subtends; the most centred wins, and a
// marker behind the viewer is never picked.
constexpr double kAimConeCos = 0.9962;
constexpr double kAimMarkerRadius = 30.0;

inline int PickAimedMarker(const OverlayVec3& viewer, double pitch, double yaw, const OverlayVec3* markers, int count) {
	constexpr double kPi = 3.14159265358979323846;
	if (!markers || count <= 0) return -1;
	const double pr = pitch * kPi / 180.0, yr = yaw * kPi / 180.0;
	const double fx = std::cos(pr) * std::cos(yr), fy = std::cos(pr) * std::sin(yr), fz = -std::sin(pr);
	int best = -1;
	double bestDot = -2.0;
	for (int i = 0; i < count; ++i) {
		const double dx = markers[i].x - viewer.x, dy = markers[i].y - viewer.y, dz = markers[i].z - viewer.z;
		const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (len < 1e-3) { best = i; bestDot = 1.0; continue; } // standing inside it
		const double inv = 1.0 / len;
		const double dot = (dx * fx + dy * fy + dz * fz) * inv;
		if (dot <= 0.0) continue;
		double coneCos = kAimConeCos;
		double sinHalf = kAimMarkerRadius * inv;
		if (sinHalf > 0.85) sinHalf = 0.85;
		const double nearCos = std::sqrt(1.0 - sinHalf * sinHalf);
		if (nearCos < coneCos) coneCos = nearCos;
		if (dot >= coneCos && dot > bestDot) { bestDot = dot; best = i; }
	}
	return best;
}

// CampathDrawer.cpp's keyframe index (CameraBridge_SetMarkerStyle turns it on at size 18): each
// decimal digit of `index` is a billboard in the view plane, half the size wide and the size tall,
// the least significant digit touching the marker and the rest running LEFT of it, sampled from
// HLAE's hexfont -- a 256x128 atlas of 8 x 2 glyph cells, 0-F. Corners are {left-top, left-bottom,
// right-top, right-bottom}; returns the digit count written.
struct OverlayDigitQuad {
	OverlayVec3 corner[4];
	double u0 = 0.0, v0 = 0.0, u1 = 0.0, v1 = 0.0;
};

inline int KeyframeDigitQuads(int index, const OverlayVec3& at, const OverlayVec3& right, const OverlayVec3& up,
	double size, OverlayDigitQuad* out, int maxOut) {
	if (!out || maxOut <= 0 || index < 0) return 0;
	int digits = 0;
	for (int t = index; t > 0; t /= 10) ++digits;
	if (digits < 1) digits = 1;
	if (digits > maxOut) digits = maxOut;
	const auto point = [&](double x, double y) {
		return OverlayVec3{ at.x + x * right.x + y * up.x, at.y + x * right.y + y * up.y, at.z + x * right.z + y * up.z };
	};
	int value = index;
	for (int i = 0; i < digits; ++i) {
		const int glyph = value % 10;
		value /= 10;
		const double left = -0.5 * size * (i + 1), rightEdge = left + 0.5 * size;
		const double top = 0.5 * size, bottom = -0.5 * size;
		OverlayDigitQuad& q = out[i];
		q.corner[0] = point(left, top);
		q.corner[1] = point(left, bottom);
		q.corner[2] = point(rightEdge, top);
		q.corner[3] = point(rightEdge, bottom);
		q.u0 = (32.0 * (glyph % 8)) / 256.0;
		q.v0 = (64.0 * (glyph / 8)) / 128.0;
		q.u1 = q.u0 + 32.0 / 256.0;
		q.v1 = q.v0 + 64.0 / 128.0;
	}
	return digits;
}

} // namespace Deadlock
