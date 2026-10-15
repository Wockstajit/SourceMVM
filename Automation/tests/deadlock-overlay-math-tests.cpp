#include "../../AfxHookSource2/Deadlock/DeadlockOverlayMath.h"
#include <cmath>
#include <cstdio>
#include <utility>

// The Deadlock editor's D3D overlay (campath polyline, marker diamonds, hero boxes) projects world
// points with the pose the engine used THIS frame. See DeadlockOverlayMath.h for why the basis
// math is duplicated rather than linked from FollowCameraMath.h, and for the measured convention
// (positive pitch looks down, right = yaw - 90, roll rotates right/up around forward) these tests
// check against.
int main() {
	int failures = 0;
	auto check = [&](bool value, const char* what) {
		if (!value) { ++failures; std::printf("  FAIL %s\n", what); }
	};
	auto near = [](double a, double b, double epsilon = 1e-6) { return std::fabs(a - b) <= epsilon; };

	using namespace Deadlock;
	constexpr double kPi = 3.14159265358979323846;

	// --- CameraBasisFromAngles: the measured convention itself ---------------------------------

	{
		// Level, facing +X: forward = +X, right = (yaw - 90) = -Y, up = +Z.
		const OverlayBasis b = CameraBasisFromAngles(0.0, 0.0, 0.0);
		check(near(b.forward.x, 1.0) && near(b.forward.y, 0.0) && near(b.forward.z, 0.0), "yaw0 pitch0: forward is +X");
		check(near(b.right.x, 0.0) && near(b.right.y, -1.0) && near(b.right.z, 0.0), "yaw0 pitch0: right is -Y (yaw - 90)");
		check(near(b.up.x, 0.0) && near(b.up.y, 0.0) && near(b.up.z, 1.0), "yaw0 pitch0: up is +Z");
	}
	{
		// Facing +Y: right should be (yaw - 90) = 0 degrees = +X.
		const OverlayBasis b = CameraBasisFromAngles(0.0, 90.0, 0.0);
		check(near(b.forward.x, 0.0) && near(b.forward.y, 1.0), "yaw90: forward is +Y");
		check(near(b.right.x, 1.0) && near(b.right.y, 0.0), "yaw90: right is +X (yaw - 90 = 0)");
	}
	{
		// Roll 90 rotates right/up around forward without touching forward: what was up (+Z)
		// becomes -Y, and what was right (-Y) becomes -Z. Axes swap, they do not vanish.
		const OverlayBasis b = CameraBasisFromAngles(0.0, 0.0, 90.0);
		check(near(b.forward.x, 1.0) && near(b.forward.y, 0.0) && near(b.forward.z, 0.0), "roll90: forward unchanged");
		check(near(b.right.x, 0.0) && near(b.right.y, 0.0) && near(b.right.z, -1.0), "roll90: right becomes -Z");
		check(near(b.up.x, 0.0) && near(b.up.y, -1.0) && near(b.up.z, 0.0), "roll90: up becomes -Y");
	}

	// --- ProjectPoint: center, the horizontal FOV edge, pitch direction, near-plane visibility --

	{
		// Straight ahead, level with the camera: dead center regardless of viewport size.
		const OverlayCameraPose pose = MakeCameraPose({0,0,0}, 0.0, 0.0, 0.0, 90.0, 1920, 1080);
		const OverlayProjection p = ProjectPoint({100.0, 0.0, 0.0}, pose);
		check(p.visible, "straight ahead is visible");
		check(near(p.pixelX, 960.0) && near(p.pixelY, 540.0), "straight ahead projects to the exact center");
	}
	{
		// A point at exactly +half the stored horizontal FOV lands on the right edge (pixelX == W)
		// -- and this must hold independent of viewport aspect, since the FOV is already the
		// aspect-widened horizontal value; aspect only sets the vertical extent.
		const OverlayBasis basis = CameraBasisFromAngles(0.0, 0.0, 0.0);
		const double halfFovRad = 90.0 * kPi / 360.0;
		const double tanHalfH = std::tan(halfFovRad);
		// forward*100 + right*100*tanHalfH puts camera-space (forward=100, right=100*tanHalfH) -> ndcX == 1.
		// 100 units out, not 1: the pose's default near plane is 4, and a point inside it is not visible.
		const OverlayVec3 edgePoint{
			basis.forward.x * 100.0 + basis.right.x * 100.0 * tanHalfH,
			basis.forward.y * 100.0 + basis.right.y * 100.0 * tanHalfH,
			basis.forward.z * 100.0 + basis.right.z * 100.0 * tanHalfH
		};
		const OverlayCameraPose wide = MakeCameraPose({0,0,0}, 0.0, 0.0, 0.0, 90.0, 1920, 1080);
		const OverlayProjection pWide = ProjectPoint(edgePoint, wide);
		check(pWide.visible && near(pWide.pixelX, 1920.0), "FOV edge -> right edge (x=W) at 16:9");

		const OverlayCameraPose narrow = MakeCameraPose({0,0,0}, 0.0, 0.0, 0.0, 90.0, 1280, 1024);
		const OverlayProjection pNarrow = ProjectPoint(edgePoint, narrow);
		check(pNarrow.visible && near(pNarrow.pixelX, 1280.0), "FOV edge -> right edge (x=W) at non-16:9 (5:4)");
	}
	{
		// The vertical extent is the horizontal half-FOV divided by the viewport's aspect: a point at
		// camera-space up == tanHalfH / aspect lands exactly on the top edge at any aspect. A multiplied or
		// swapped aspect would put it somewhere else.
		const double tanHalfH = std::tan(90.0 * kPi / 360.0);
		for (const auto& vp : { std::pair<int, int>{1920, 1080}, std::pair<int, int>{1280, 1024} }) {
			const double aspect = static_cast<double>(vp.first) / vp.second;
			const OverlayProjection top = ProjectCameraSpace(OverlayCameraSpace{1.0, 0.0, tanHalfH / aspect}, 90.0,
				vp.first, vp.second, 0.0);
			check(top.visible && near(top.pixelY, 0.0), "vertical FOV edge -> top edge (y=0) at every aspect");
		}
	}
	{
		// A point level with the camera (same world Z, straight ahead) moves UP on screen (a
		// smaller pixelY) as the camera pitches down -- the horizon rises as you look down.
		const OverlayVec3 origin{0.0, 0.0, 50.0};
		const OverlayVec3 levelPoint{100.0, 0.0, 50.0};
		const OverlayCameraPose level = MakeCameraPose(origin, 0.0, 0.0, 0.0, 90.0, 1920, 1080);
		const OverlayCameraPose pitchedDown = MakeCameraPose(origin, 10.0, 0.0, 0.0, 90.0, 1920, 1080);
		const OverlayProjection pLevel = ProjectPoint(levelPoint, level);
		const OverlayProjection pPitched = ProjectPoint(levelPoint, pitchedDown);
		check(pLevel.visible && near(pLevel.pixelY, 540.0), "unpitched: eye-level point is at vertical center");
		check(pPitched.visible && pPitched.pixelY < pLevel.pixelY - 1.0,
			"positive pitch (looking down) moves an eye-level point up on screen (smaller y)");
	}
	{
		// "At or behind" the near plane is not visible, including exactly on it.
		const OverlayCameraPose pose = MakeCameraPose({0,0,0}, 0.0, 0.0, 0.0, 90.0, 1920, 1080, 4.0);
		check(!ProjectPoint({-100.0, 0.0, 0.0}, pose).visible, "a point behind the camera is not visible");
		check(!ProjectPoint({4.0, 0.0, 0.0}, pose).visible, "a point exactly on the near plane is not visible");
		check(ProjectPoint({4.0001, 0.0, 0.0}, pose).visible, "just past the near plane is visible");
	}

	// --- ClipSegmentNearPlane: a crossing segment is shortened, not flipped --------------------

	{
		const OverlayCameraPose pose = MakeCameraPose({0,0,0}, 0.0, 0.0, 0.0, 90.0, 1920, 1080, 4.0);
		const OverlayVec3 front{10.0, 0.0, 0.0}; // forward = 10, in front of the near plane
		const OverlayVec3 behind{-10.0, 0.0, 0.0}; // forward = -10, behind the camera
		const OverlayClippedSegment seg = ClipSegmentNearPlane(front, behind, pose);
		check(seg.visible, "a segment crossing the near plane still has a visible remainder");
		check(near(seg.a.x, 10.0) && near(seg.a.y, 0.0) && near(seg.a.z, 0.0),
			"the in-front endpoint is returned untouched");
		check(near(ToCameraSpace(seg.b, pose).forward, 4.0),
			"the behind endpoint is replaced by the exact near-plane crossing point");

		const OverlayClippedSegment both = ClipSegmentNearPlane(behind, behind, pose);
		check(!both.visible, "a segment fully behind the near plane has nothing left");
	}

	// --- viewport -> back-buffer scaling, then the preview-rect remap --------------------------

	{
		// 1440x810 -> 1920x1080 is a uniform 4/3 scale.
		const OverlayPixel center = ScaleToBackBuffer({720.0, 405.0}, 1440, 810, 1920, 1080);
		check(near(center.x, 960.0) && near(center.y, 540.0), "viewport->back-buffer scales the center correctly");
		const OverlayPixel corner = ScaleToBackBuffer({1440.0, 810.0}, 1440, 810, 1920, 1080);
		check(near(corner.x, 1920.0) && near(corner.y, 1080.0), "viewport->back-buffer scales the far corner correctly");
	}
	{
		// The default {0,0,1,1} rect (no preview active) is the identity.
		const OverlayPixel same = RemapToPreviewRect({123.0, 456.0}, 1920, 1080, OverlayRect{});
		check(near(same.x, 123.0) && near(same.y, 456.0), "the full-frame rect {0,0,1,1} is an identity remap");

		// A preview rect covering the top-right quadrant of the back buffer.
		const OverlayRect quadrant{0.5, 0.0, 1.0, 0.5};
		const OverlayPixel center = RemapToPreviewRect({960.0, 540.0}, 1920, 1080, quadrant);
		check(near(center.x, 1440.0) && near(center.y, 270.0), "preview-rect remap places the full-frame center correctly");
		const OverlayPixel farCorner = RemapToPreviewRect({1920.0, 1080.0}, 1920, 1080, quadrant);
		check(near(farCorner.x, 1920.0) && near(farCorner.y, 540.0), "preview-rect remap places the far corner correctly");
	}

	// --- ProjectBox: the bounding rect contains every visible corner ---------------------------

	{
		const OverlayCameraPose pose = MakeCameraPose({0,0,0}, 0.0, 0.0, 0.0, 90.0, 1920, 1080, 4.0);
		const OverlayVec3 feet{50.0, 0.0, 0.0};
		const double halfWidth = 10.0, height = 20.0;
		const OverlayScreenRect rect = ProjectBox(feet, halfWidth, height, pose);
		check(rect.visible, "a fully in-front box projects as visible");

		double minX = 0, minY = 0, maxX = 0, maxY = 0;
		bool any = false;
		for (double dz : {0.0, height}) {
			for (double dy : {-halfWidth, halfWidth}) {
				for (double dx : {-halfWidth, halfWidth}) {
					const OverlayProjection p = ProjectPoint({feet.x + dx, feet.y + dy, feet.z + dz}, pose);
					check(p.visible, "every corner of this in-front box is individually visible");
					if (!any) { minX = maxX = p.pixelX; minY = maxY = p.pixelY; any = true; continue; }
					minX = std::fmin(minX, p.pixelX); maxX = std::fmax(maxX, p.pixelX);
					minY = std::fmin(minY, p.pixelY); maxY = std::fmax(maxY, p.pixelY);
				}
			}
		}
		check(near(rect.minX, minX) && near(rect.maxX, maxX) && near(rect.minY, minY) && near(rect.maxY, maxY),
			"box projection bounds equal the min/max of every corner");
		check(rect.minX <= minX + 1e-6 && rect.maxX >= maxX - 1e-6
			&& rect.minY <= minY + 1e-6 && rect.maxY >= maxY - 1e-6,
			"box projection bounds contain every visible corner");

		const OverlayScreenRect behind = ProjectBox({-50.0, 0.0, 0.0}, halfWidth, height, pose);
		check(!behind.visible, "a box fully behind the near plane is not visible");
	}

	{
		// A box STRADDLING the near plane: its back corners are behind the camera. The bounds are the
		// four front corners plus the four points where the box's depth edges cross the near plane.
		const OverlayCameraPose pose = MakeCameraPose({0,0,0}, 0.0, 0.0, 0.0, 90.0, 1920, 1080, 4.0);
		const OverlayVec3 feet{5.0, 0.0, 0.0}; // corners at x = -5 (behind) and x = 15 (in front)
		const double halfWidth = 10.0, height = 20.0;
		const OverlayScreenRect rect = ProjectBox(feet, halfWidth, height, pose);
		check(rect.visible, "a straddling box is visible");

		double minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
		const auto grow = [&](const OverlayProjection& p) {
			check(p.visible, "straddle: every expected bound point projects");
			minX = std::fmin(minX, p.pixelX); maxX = std::fmax(maxX, p.pixelX);
			minY = std::fmin(minY, p.pixelY); maxY = std::fmax(maxY, p.pixelY);
		};
		for (double dz : {0.0, height}) {
			for (double dy : {-halfWidth, halfWidth}) {
				grow(ProjectPoint({feet.x + halfWidth, feet.y + dy, feet.z + dz}, pose));
				// The crossing sits on the near plane (right = -Y at yaw 0); a zero near plane lets it project.
				grow(ProjectCameraSpace(OverlayCameraSpace{pose.nearPlane, -dy, dz}, pose.horizontalFovDegrees,
					pose.viewportWidth, pose.viewportHeight, 0.0));
			}
		}
		check(near(rect.minX, minX, 1e-3) && near(rect.maxX, maxX, 1e-3)
			&& near(rect.minY, minY, 1e-3) && near(rect.maxY, maxY, 1e-3),
			"straddle: bounds are the front corners plus the near-plane crossings");
		check(rect.minX < 0.0 && rect.maxX > 1920.0, "straddle: a box around the camera reaches past both screen edges");
	}

	// --- CS2's campath gizmo, ported: scale, camera shape, stroke, joins, crosshair pick ----------

	{
		// gscale is 1 at 300 units and clamps at both ends, exactly CampathDrawer.cpp's constants.
		check(near(MarkerScreenScale({300.0, 0.0, 0.0}, {0,0,0}), 1.0), "marker scale is 1 at the 300-unit reference");
		check(near(MarkerScreenScale({30.0, 0.0, 0.0}, {0,0,0}), 0.5), "marker scale clamps near markers to 0.5");
		check(near(MarkerScreenScale({30000.0, 0.0, 0.0}, {0,0,0}), 2.2), "marker scale clamps far markers to 2.2");
	}
	{
		// Level camera at the origin facing +X, FOV 90 on 1920x1080, scale 1: radius 18, half-width
		// sin(45)*18, half-height that times 1080/1920. Right is -Y at yaw 0, so "-a*right" is +Y.
		OverlaySegment3 s[10];
		const int n = CameraGizmoSegments({0,0,0}, 0.0, 0.0, 0.0, 90.0, 1920, 1080, 1.0, s);
		const double r = 18.0, a = std::sin(kPi / 4.0) * r, b = a * 1080.0 / 1920.0;
		check(n == 10, "the camera gizmo has CS2's ten edges");
		bool apex = true;
		for (int i = 0; i < 4; ++i) apex = apex && near(s[i].a.x, 0.0) && near(s[i].a.y, 0.0) && near(s[i].a.z, 0.0);
		check(apex, "the first four edges leave the camera position (the frustum apex)");
		check(near(s[2].b.x, r) && near(s[2].b.y, a) && near(s[2].b.z, b), "the left-up corner sits radius ahead, +a left, +b up");
		check(near(s[0].b.z, -b) && near(s[1].b.z, -b), "the down corners sit b below the axis");
		check(near(s[8].b.x, r) && near(s[8].b.y, 0.0) && near(s[8].b.z, 1.5 * b), "the roof peak is centred, half a b above the top edge");
		OverlaySegment3 big[10];
		CameraGizmoSegments({0,0,0}, 0.0, 0.0, 0.0, 90.0, 1920, 1080, 2.0, big);
		check(near(big[2].b.x, 2.0 * r) && near(big[2].b.y, 2.0 * a), "the gizmo scales with the marker scale");
	}
	{
		// Pitched down 90: forward is -Z, so the rectangle is below the camera.
		OverlaySegment3 s[10];
		CameraGizmoSegments({0,0,100.0}, 90.0, 0.0, 0.0, 90.0, 1920, 1080, 1.0, s);
		check(near(s[4].a.z, 100.0 - 18.0, 1e-6), "a camera looking down draws its rectangle below it");
	}
	{
		OverlayPixel q[4];
		check(ThickLineQuad({0,0}, {10,0}, 8.0, q), "a horizontal stroke builds a quad");
		check(near(std::fabs(q[0].y - q[1].y), 8.0) && near(q[0].x, 0.0) && near(q[2].x, 10.0),
			"the stroke is exactly `width` pixels across, perpendicular to the line");
		check(!ThickLineQuad({5,5}, {5,5}, 8.0, q), "a zero-length stroke draws nothing");
	}
	{
		// A straight run: interior and end offsets agree, so the quads meet flush.
		const OverlayPixel p0{0,0}, p1{10,0}, p2{20,0};
		const OverlayPixel end0 = PolylineJoinOffset(nullptr, p0, &p1, 8.0);
		const OverlayPixel mid = PolylineJoinOffset(&p0, p1, &p2, 8.0);
		const OverlayPixel end2 = PolylineJoinOffset(&p1, p2, nullptr, 8.0);
		check(near(mid.x, end0.x) && near(mid.y, end0.y) && near(mid.x, end2.x) && near(mid.y, end2.y),
			"a straight polyline has the same offset at its ends and its joints");
		check(near(std::sqrt(mid.x * mid.x + mid.y * mid.y), 4.0), "a straight join is half the width from the centre line");
		// A right-angle corner: CS2's bbox clamp keeps the full diagonal, sqrt(2) half-widths out.
		const OverlayPixel c0{0,0}, c1{10,0}, c2{10,10};
		const OverlayPixel corner = PolylineJoinOffset(&c0, c1, &c2, 8.0);
		check(near(std::fabs(corner.x), 4.0) && near(std::fabs(corner.y), 4.0), "a right-angle join mitres to the corner diagonal");
	}
	{
		// Crosshair pick (CameraPathPicking.cpp): viewer at the origin facing +X.
		const OverlayVec3 ahead[] = { {1000.0, 0.0, 0.0} };
		check(PickAimedMarker({0,0,0}, 0.0, 0.0, ahead, 1) == 0, "a marker dead ahead is picked");
		const double off10 = 10.0 * kPi / 180.0;
		const OverlayVec3 farOff[] = { {1000.0 * std::cos(off10), 1000.0 * std::sin(off10), 0.0} };
		check(PickAimedMarker({0,0,0}, 0.0, 0.0, farOff, 1) == -1, "a far marker 10 degrees off-axis is outside the 5 degree cone");
		const OverlayVec3 nearOff[] = { {100.0 * std::cos(off10), 100.0 * std::sin(off10), 0.0} };
		check(PickAimedMarker({0,0,0}, 0.0, 0.0, nearOff, 1) == 0, "the same angle up close is picked (the cone widens by the marker's size)");
		const OverlayVec3 behind[] = { {-50.0, 0.0, 0.0} };
		check(PickAimedMarker({0,0,0}, 0.0, 0.0, behind, 1) == -1, "a marker behind the viewer is never picked");
		const OverlayVec3 two[] = { {500.0, 20.0, 0.0}, {500.0, 2.0, 0.0} };
		check(PickAimedMarker({0,0,0}, 0.0, 0.0, two, 2) == 1, "the most centred of two candidates wins");
	}

	{
		// Keyframe index digits (CampathDrawer.cpp). World +X as "right" and +Z as "up" for an easy read.
		OverlayDigitQuad q[4];
		check(KeyframeDigitQuads(0, {0,0,0}, {1,0,0}, {0,0,1}, 18.0, q, 4) == 1, "index 0 draws one digit");
		check(near(q[0].corner[0].x, -9.0) && near(q[0].corner[0].z, 9.0)
			&& near(q[0].corner[3].x, 0.0) && near(q[0].corner[3].z, -9.0),
			"a digit is half the size wide and the size tall, touching the marker on its left");
		check(near(q[0].u0, 0.0) && near(q[0].v0, 0.0) && near(q[0].u1, 0.125) && near(q[0].v1, 0.5),
			"glyph 0 is the atlas's first 32x64 cell");
		check(KeyframeDigitQuads(12, {0,0,0}, {1,0,0}, {0,0,1}, 18.0, q, 4) == 2, "index 12 draws two digits");
		check(near(q[0].u0, 0.25) && near(q[0].corner[0].x, -9.0), "the least significant digit (2) sits against the marker");
		check(near(q[1].u0, 0.125) && near(q[1].corner[0].x, -18.0), "the next digit (1) runs further left");
		KeyframeDigitQuads(9, {0,0,0}, {1,0,0}, {0,0,1}, 18.0, q, 4);
		check(near(q[0].u0, 0.125) && near(q[0].v0, 0.5), "glyph 9 is on the atlas's second row");
	}

	std::printf("Deadlock overlay math: %d failures\n", failures);
	return failures ? 1 : 0;
}
