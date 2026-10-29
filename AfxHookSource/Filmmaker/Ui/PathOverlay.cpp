#include "PathOverlay.h"

#include "../../../AfxHookSource2/Deadlock/DeadlockOverlayMath.h"
#include "../../../AfxHookSource2/SceneMarkerMath.h"

#include <cfloat>
#include <cmath>
#include <string>

namespace SourceFilmmaker {
namespace Ui {

namespace {

using namespace Filmmaker::Marker;
using Deadlock::OverlayVec3;

constexpr double kPi = 3.14159265358979323846;

// CampathDrawer.cpp's sizes.
constexpr double kCameraRadius = 18.0;       // c_CameraRadius
constexpr float kTrajectoryWidth = 5.0f;     // c_MarkerTrajectoryPixelWidth
constexpr float kEdgeWidth = 2.5f;           // c_MarkerEdgePixelWidth
constexpr double kIndexSize = 18.0;          // CameraBridge_SetMarkerStyle's keyframe index size
constexpr float kPivotWidth = 4.0f;          // c_CampathCrossPixelWidth, the pivot marker's circles
const ImU32 kUnderline = IM_COL32(0, 0, 0, 150); // c_MarkerUnderline

ImU32 Col(Rgb c, int alpha) { return IM_COL32(c.r, c.g, c.b, alpha); }
Vec V(const double p[3]) { return Vec{ p[0], p[1], p[2] }; }
Vec V(const OverlayVec3& p) { return Vec{ p.x, p.y, p.z }; }
OverlayVec3 O(Vec v) { return OverlayVec3{ v.x, v.y, v.z }; }

struct Edge { Vec a, b; };

// The view, and the frame the solid markers are turned to and shaded for (SceneMarkerMath.h).
struct Viewer {
	Deadlock::OverlayCameraPose pose;
	Vec eye, forward, right, up, light;
	double focal = 1; // pixels per unit of camera-space slope

	bool Point(Vec p, ImVec2& out) const {
		const Deadlock::OverlayProjection pr = Deadlock::ProjectPoint(O(p), pose);
		if (!pr.visible) return false;
		out = ImVec2((float)pr.pixelX, (float)pr.pixelY);
		return true;
	}
	// Pixels per world unit at p's depth, 0 at or behind the near plane.
	double PixelsPerUnit(Vec p) const {
		const double depth = Dot(p - eye, forward);
		return depth > pose.nearPlane ? focal / depth : 0.0;
	}
	// CampathDrawer.cpp's WhollyInFront: a solid draws only while all of it is ahead of the eye.
	bool WhollyInFront(Vec centre, double radius) const { return Dot(centre - eye, forward) > radius + 2.0; }
};

void Line(ImDrawList& list, const Viewer& v, Vec a, Vec b, ImU32 colour, float width) {
	const Deadlock::OverlayClippedSegment seg = Deadlock::ClipSegmentNearPlane(O(a), O(b), v.pose);
	ImVec2 pa, pb;
	if (seg.visible && v.Point(V(seg.a), pa) && v.Point(V(seg.b), pb)) list.AddLine(pa, pb, colour, width);
}

// DrawMarkerEdges: every underline first, then every colour, so no underline cuts an edge.
void Edges(ImDrawList& list, const Viewer& v, const Edge* edges, int count, ImU32 colour, float width) {
	for (int i = 0; i < count; ++i) Line(list, v, edges[i].a, edges[i].b, kUnderline, width * 2.2f);
	for (int i = 0; i < count; ++i) Line(list, v, edges[i].a, edges[i].b, colour, width);
}

// One solid triangle, its colour per corner. Only called for solids wholly in front of the eye.
void Tri(ImDrawList& list, const Viewer& v, Vec a, ImU32 ca, Vec b, ImU32 cb, Vec c, ImU32 cc) {
	ImVec2 pa, pb, pc;
	if (!v.Point(a, pa) || !v.Point(b, pb) || !v.Point(c, pc)) return;
	const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
	list.PrimReserve(3, 3);
	const ImDrawIdx base = (ImDrawIdx)list._VtxCurrentIdx;
	list.PrimWriteIdx(base);
	list.PrimWriteIdx((ImDrawIdx)(base + 1));
	list.PrimWriteIdx((ImDrawIdx)(base + 2));
	list.PrimWriteVtx(pa, uv, ca);
	list.PrimWriteVtx(pb, uv, cb);
	list.PrimWriteVtx(pc, uv, cc);
}

// DrawMarkerBox: the faces turned to the viewer, each shaded by its facing; alpha below 255 is glass.
void Box(ImDrawList& list, const Viewer& v, Vec centre, const Vec axes[3], const double half[3], Rgb rgb, int alpha = 255) {
	for (int i = 0; i < 3; ++i) {
		const int j = (i + 1) % 3, k = (i + 2) % 3;
		for (int side = -1; side <= 1; side += 2) {
			const Vec n = double(side) * axes[i];
			const Vec c = centre + (side * half[i]) * axes[i];
			if (!FacesViewer(n, c, v.eye)) continue;
			const ImU32 colour = Col(Shade(rgb, n, v.light), alpha);
			const Vec du = half[j] * axes[j], dv = half[k] * axes[k];
			Tri(list, v, c - du - dv, colour, c + du - dv, colour, c + du + dv, colour);
			Tri(list, v, c - du - dv, colour, c + du + dv, colour, c - du + dv, colour);
		}
	}
}

// DrawCameraMarker: a box behind the eye, a glass lens out to the field of view's frame, a solid
// triangle over the frame's top edge for which way is up, and the frame's edges.
void CameraMarker(ImDrawList& list, const Viewer& v, const PathOverlayMarker& m, Rgb rgb, bool selected,
	float screenW, float screenH) {
	const double fov = m.fov < 1 ? 1 : (m.fov > 179 ? 179 : m.fov);
	const Deadlock::OverlayBasis basis = Deadlock::CameraBasisFromAngles(m.ang[0], m.ang[1], m.ang[2]);
	const Vec f = V(basis.forward), r = V(basis.right), u = V(basis.up);
	const Vec eye = V(m.pos);

	const double distance = std::sqrt(Dot(eye - v.eye, eye - v.eye));
	const double radius = kCameraRadius * ScreenScale(distance);
	// The view looking out of this camera (the editor holds the view on a selected camera): its frame
	// would be drawn across the whole screen, so it is left out.
	if (distance < radius) return;
	const double a = std::sin(fov * kPi / 360.0) * radius;
	const double b = a * (screenW > 0 ? screenH / screenW : 1.0);
	const Vec frame = eye + radius * f;
	const Vec lu = frame - a * r + b * u, ru = frame + a * r + b * u;
	const Vec ld = frame - a * r - b * u, rd = frame + a * r - b * u;
	const Vec t0 = lu + 0.22 * (ru - lu) + (0.12 * b) * u, t1 = lu + 0.78 * (ru - lu) + (0.12 * b) * u;
	const Vec tip = 0.5 * (lu + ru) + (0.62 * b) * u;
	const Rgb body = selected ? Mix(rgb, kSelected, 0.45) : rgb;

	if (v.WhollyInFront(eye, 1.4 * radius)) {
		const auto drawBody = [&]() {
			const Vec axes[3] = { f, r, u };
			const double half[3] = { 0.42 * radius, 0.26 * radius, 0.34 * radius };
			Box(list, v, eye - (0.42 * radius) * f, axes, half, body);
		};
		const auto drawLens = [&]() {
			// Bright at the eye, fading toward the frame, each side shaded by its facing.
			const Vec corners[4] = { lu, ru, rd, ld };
			for (int i = 0; i < 4; ++i) {
				const Vec p = corners[i], q = corners[(i + 1) % 4];
				Vec n = Unit(Cross(p - eye, q - eye));
				if (Dot(n, 0.5 * (p + q) - frame) < 0) n = -n;
				const Rgb side = Shade(body, n, v.light);
				Tri(list, v, eye, Col(side, 120), p, Col(side, 40), q, Col(side, 40));
			}
			const ImU32 glass = Col(body, 26);
			Tri(list, v, lu, glass, ru, glass, rd, glass);
			Tri(list, v, lu, glass, rd, glass, ld, glass);
		};
		// The nearer part last: nothing here has depth.
		if (Dot(v.eye - eye, f) > 0) { drawBody(); drawLens(); }
		else { drawLens(); drawBody(); }
		const ImU32 solid = Col(body, 255);
		Tri(list, v, t0, solid, t1, solid, tip, solid);
	}

	const Edge edges[11] = {
		{ lu, ru }, { ru, rd }, { rd, ld }, { ld, lu },
		{ eye, lu }, { eye, ru }, { eye, rd }, { eye, ld },
		{ t0, tip }, { tip, t1 }, { t1, t0 },
	};
	Edges(list, v, edges, 11, Col(selected ? kSelected : rgb, 255), kEdgeWidth);
}

// The trajectory, split where it crosses the near plane so no run joins a point behind the eye;
// each run a dark underline under the colour (DrawPathMarker).
void Trajectory(ImDrawList& list, const Viewer& v, const std::vector<double>& xyz, Rgb rgb) {
	const size_t n = xyz.size() / 3;
	if (n < 2) return;
	std::vector<std::vector<ImVec2>> runs(1);
	for (size_t i = 1; i < n; ++i) {
		const OverlayVec3 a{ xyz[3 * i - 3], xyz[3 * i - 2], xyz[3 * i - 1] }, b{ xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2] };
		const Deadlock::OverlayClippedSegment seg = Deadlock::ClipSegmentNearPlane(a, b, v.pose);
		ImVec2 pa, pb;
		if (!seg.visible || !v.Point(V(seg.a), pa) || !v.Point(V(seg.b), pb)) {
			if (!runs.back().empty()) runs.emplace_back();
			continue;
		}
		const bool aClipped = seg.a.x != a.x || seg.a.y != a.y || seg.a.z != a.z;
		const bool bClipped = seg.b.x != b.x || seg.b.y != b.y || seg.b.z != b.z;
		if (aClipped && !runs.back().empty()) runs.emplace_back();
		if (runs.back().empty()) runs.back().push_back(pa);
		runs.back().push_back(pb);
		if (bClipped) runs.emplace_back();
	}
	for (int pass = 0; pass < 2; ++pass) {
		const ImU32 colour = pass == 0 ? IM_COL32(0, 0, 0, 120) : Col(rgb, 210);
		const float width = pass == 0 ? kTrajectoryWidth * 2.0f : kTrajectoryWidth;
		for (const std::vector<ImVec2>& run : runs)
			if (run.size() >= 2) list.AddPolyline(run.data(), (int)run.size(), colour, ImDrawFlags_None, width);
	}
}

// A disc turned to the viewer (DrawMarkerDisc), `radius` world units.
void Disc(ImDrawList& list, const Viewer& v, Vec at, double radius, ImU32 colour) {
	ImVec2 p;
	const double ppu = v.PixelsPerUnit(at);
	if (ppu > 0 && v.Point(at, p)) list.AddCircleFilled(p, (float)(radius * ppu), colour, 20);
}

// The marker's index: CS2's billboard digits, kIndexSize tall, the last digit's right edge on the
// marker; white over a black outline, as HLAE's hexfont draws them.
void Index(ImDrawList& list, const Viewer& v, Vec at, int index, ImFont* font) {
	ImVec2 p;
	const double height = kIndexSize * v.PixelsPerUnit(at);
	if (!font || height < 4.0 || !v.Point(at, p)) return;
	const float size = (float)(height > 160.0 ? 160.0 : height);
	const std::string text = std::to_string(index);
	const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
	const ImVec2 origin(p.x - extent.x, p.y - extent.y * 0.5f);
	const float o = size > 24.0f ? 2.0f : 1.0f;
	for (int dx = -1; dx <= 1; ++dx)
		for (int dy = -1; dy <= 1; ++dy)
			if (dx || dy) list.AddText(font, size, ImVec2(origin.x + dx * o, origin.y + dy * o), IM_COL32(0, 0, 0, 255), text.c_str());
	list.AddText(font, size, origin, IM_COL32(255, 255, 255, 255), text.c_str());
}

} // namespace

void DrawPathOverlay(ImDrawList& list, const PathOverlayModel& model, const PathOverlayView& view, ImFont* digits) {
	if ((model.markers.empty() && !model.followCamera && !model.targetBox && model.pivots.empty()) || !(view.width > 0)
		|| !(view.height > 0))
		return;
	if (!(view.fov > 1.0 && view.fov < 179.0)) return;

	Viewer v;
	v.pose = Deadlock::MakeCameraPose({ view.origin[0], view.origin[1], view.origin[2] },
		view.angles[0], view.angles[1], view.angles[2], view.fov, (int)view.width, (int)view.height);
	v.eye = V(view.origin);
	v.forward = V(v.pose.basis.forward);
	ViewBasis(v.forward, v.right, v.up);
	v.light = KeyLight(v.forward, v.right, v.up);
	v.focal = 0.5 * view.width / std::tan(view.fov * kPi / 360.0);

	const Rgb rgb = model.freeze ? kCampathFreeze : kCampathLive;

	// CS2's order: the numbers, the line, the cameras, the playhead over all of them.
	for (size_t i = 0; i < model.markers.size(); ++i) Index(list, v, V(model.markers[i].pos), (int)i, digits);
	Trajectory(list, v, model.trajectory, rgb);
	for (size_t i = 0; i < model.markers.size(); ++i)
		CameraMarker(list, v, model.markers[i], rgb, (int)i == model.aimed, view.width, view.height);
	if (model.head) {
		const Vec at = V(model.headPos);
		const double s = ScreenScale(std::sqrt(Dot(at - v.eye, at - v.eye)));
		if (Dot(at - v.eye, v.forward) > 8.0 * s + 2.0) {
			Disc(list, v, at, 7.5 * s, IM_COL32(0, 0, 0, 200));
			Disc(list, v, at, 6.0 * s, Col(rgb, 255));
			Disc(list, v, at, 3.0 * s, IM_COL32(255, 255, 255, 255));
		}
	}
	if (model.followCamera) CameraMarker(list, v, model.followMarker, kFollowCamera, false, view.width, view.height);
	if (model.targetBox && model.boxHalf > 0 && model.boxHeight > 0) {
		// A glass box around the follow target: its near faces faintly filled, its edges crisp. Cyan,
		// so it never reads as a camera marker.
		const double x = model.boxPos[0], y = model.boxPos[1], z = model.boxPos[2], h = model.boxHalf, top = z + model.boxHeight;
		const Vec c[8] = {
			{ x - h, y - h, z }, { x + h, y - h, z }, { x + h, y + h, z }, { x - h, y + h, z },
			{ x - h, y - h, top }, { x + h, y - h, top }, { x + h, y + h, top }, { x - h, y + h, top },
		};
		const Vec centre{ x, y, z + model.boxHeight * 0.5 };
		const Vec axes[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
		const double half[3] = { h, h, model.boxHeight * 0.5 };
		if (v.WhollyInFront(centre, h + model.boxHeight)) Box(list, v, centre, axes, half, kFollowTarget, 30);
		const Edge edges[12] = {
			{ c[0], c[1] }, { c[1], c[2] }, { c[2], c[3] }, { c[3], c[0] },
			{ c[4], c[5] }, { c[5], c[6] }, { c[6], c[7] }, { c[7], c[4] },
			{ c[0], c[4] }, { c[1], c[5] }, { c[2], c[6] }, { c[3], c[7] },
		};
		Edges(list, v, edges, 12, Col(kFollowTarget, 230), kEdgeWidth * 0.8f);
	}
	// The weapon pivots (CampathDrawer.cpp's pivot marker): three world-axis great circles of 24
	// segments, a wide faint pass under a thin bright one, over everything else.
	static const struct { float widthMul; int alpha; } kPivotGlow[] = { { 3.0f, 40 }, { 1.0f, 210 } };
	for (const PathOverlayPivot& p : model.pivots) {
		if (!(p.radius > 0)) continue;
		for (const auto& pass : kPivotGlow) {
			const ImU32 colour = IM_COL32(p.rgb[0], p.rgb[1], p.rgb[2], pass.alpha);
			for (int axis = 0; axis < 3; ++axis)
				for (int seg = 0; seg < 24; ++seg) {
					const double a0 = 2.0 * kPi * seg / 24, a1 = 2.0 * kPi * (seg + 1) / 24;
					const double c0 = std::cos(a0) * p.radius, s0 = std::sin(a0) * p.radius;
					const double c1 = std::cos(a1) * p.radius, s1 = std::sin(a1) * p.radius;
					Vec q0 = V(p.pos), q1 = V(p.pos);
					if (axis == 0) { q0.x += c0; q0.y += s0; q1.x += c1; q1.y += s1; }
					else if (axis == 1) { q0.x += c0; q0.z += s0; q1.x += c1; q1.z += s1; }
					else { q0.y += c0; q0.z += s0; q1.y += c1; q1.z += s1; }
					Line(list, v, q0, q1, colour, kPivotWidth * pass.widthMul);
				}
		}
	}
}

} // namespace Ui
} // namespace SourceFilmmaker
