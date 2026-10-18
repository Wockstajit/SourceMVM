#include "../../AfxHookSource2/Filmmaker/Props/PropEditMath.h"

#include <cstdlib>
#include <iostream>

using namespace Filmmaker::Props;
namespace Environment = Filmmaker::Environment;

namespace {

int g_failed = 0;

#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED line " << __LINE__ << ": " #cond "\n"; ++g_failed; } } while (0)

bool Near(float a, float b, float tolerance = .01f) { return std::abs(a - b) <= tolerance; }

// A camera at the origin looking straight down -Z at the ground plane z = -10 (screen right = +X,
// screen up = +Y): the overhead view a placement edit is most often made from.
const Matrix kDown = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1.001f, -.1001f, 0, 0, -1, 0};
const Vec3 kEye{0, 0, 0}, kGround{0, 0, -10};

} // namespace

int main() {
	// The cursor ray passes through the point the cursor sits over.
	Vec3 d;
	CHECK(CursorRay(kDown, kEye, .5f, .5f, d) && Near(d.x, 0) && Near(d.y, 0) && Near(d.z, -1));
	CHECK(CursorRay(kDown, kEye, .75f, .5f, d) && d.x > 0 && Near(d.y, 0));
	CHECK(CursorRay(kDown, kEye, .5f, .25f, d) && d.y > 0); // up the screen is +Y here
	CHECK(!CursorRay(kDown, kEye, NAN, .5f, d));

	// Turning: the angle of the cursor around the prop on the turn ring's (ground) plane. From +X to
	// +Y is a quarter turn to the left, as yaw counts.
	const Vec3 up{0, 0, 1};
	float a0 = 0, a1 = 0, a2 = 0;
	CHECK(CursorRay(kDown, kEye, .6f, .5f, d) && RingAngle(kEye, d, kGround, up, a0));
	CHECK(CursorRay(kDown, kEye, .5f, .4f, d) && RingAngle(kEye, d, kGround, up, a1) && Near(WrapDegrees(a1 - a0), 90));
	CHECK(CursorRay(kDown, kEye, .4f, .5f, d) && RingAngle(kEye, d, kGround, up, a2) && Near(std::abs(WrapDegrees(a2 - a0)), 180));
	CHECK(CursorRay(kDown, kEye, .5f, .5f, d) && !RingAngle(kEye, d, kGround, up, a0)); // the centre has no angle
	CHECK(!RingAngle(kEye, {1, 0, 0}, kGround, up, a0));                                  // ray along the plane
	CHECK(!RingAngle(kEye, {0, 0, 1}, kGround, up, a0));                                  // ray away from it

	CHECK(Near(WrapDegrees(190), -170) && Near(WrapDegrees(-190), 170) && Near(WrapDegrees(180), 180) && Near(WrapDegrees(720), 0));

	// Snapping: 15-degree steps, wrapped; a model centred on its origin rests its bottom on the floor.
	CHECK(Near(SnapDegrees(22, 15), 15) && Near(SnapDegrees(23, 15), 30) && Near(SnapDegrees(-7, 15), 0));
	CHECK(Near(SnapDegrees(178, 15), 180) && Near(SnapDegrees(-173, 15), 180));
	CHECK(Near(SnapDegrees(33, 0), 33)); // no step: unchanged
	CHECK(Near(RestingZ(100, 0, 2), 100));     // origin at the base
	CHECK(Near(RestingZ(100, -16, 2), 132));   // centred model, scaled x2: lifted by its half-height

	// Bounds follow position, turn and scale.
	Vec3 c[8];
	BoxCorners({100, 0, 0}, {0, 90, 0}, 2, {-1, -2, 0}, {1, 2, 3}, c);
	float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f, maxZ = -1e9f;
	for (const Vec3& p : c) { minX = std::min(minX, p.x); maxX = std::max(maxX, p.x); minY = std::min(minY, p.y); maxY = std::max(maxY, p.y); maxZ = std::max(maxZ, p.z); }
	CHECK(Near(minX, 96) && Near(maxX, 104)); // the 4-unit-deep Y extent now lies along X, doubled
	CHECK(Near(minY, -2) && Near(maxY, 2));
	CHECK(Near(maxZ, 6));

	// Picking: the nearer of two overlapping props wins; a miss picks nothing.
	Vec3 nearBox[8], farBox[8];
	BoxCorners({0, 0, -10}, {}, 1, {-1, -1, -1}, {1, 1, 1}, nearBox);
	BoxCorners({0, 0, -20}, {}, 1, {-3, -3, -1}, {3, 3, 1}, farBox);
	std::vector<PickCandidate> candidates = {
		{1, ProjectRect(kDown, farBox, 8), 20},
		{2, ProjectRect(kDown, nearBox, 8), 10},
	};
	CHECK(candidates[0].rect.valid && candidates[1].rect.valid);
	CHECK(Pick(candidates, .5f, .5f) == 2);
	CHECK(Pick(candidates, .565f, .5f) == 1); // outside the near box, inside the far one
	CHECK(Pick(candidates, .99f, .99f) == 0);
	Vec3 behind[8];
	BoxCorners({0, 0, 5}, {}, 1, {-1, -1, -1}, {1, 1, 1}, behind);
	CHECK(!ProjectRect(kDown, behind, 8).valid);

	// Each turn ring changes one angle, and the model really turns about that ring's axis (checked
	// against an independent right-handed rotation of a model point about the axis).
	{
		auto rotate = [](Vec3 v, Vec3 k, float deg) { // Rodrigues
			const float t = deg * 0.017453292519943295f, c = std::cos(t), s = std::sin(t);
			k = Environment::Normalize(k);
			return v * c + Environment::Cross(k, v) * s + k * (Environment::Dot(k, v) * (1 - c));
		};
		const Vec3 start{20, 35, 10}, center{100, -50, 8}, local{30, 12, -7};
		for (int r = 0; r < 3; ++r) {
			const Ring ring = Ring(r);
			const Vec3 axis = RingAxis(start, ring);
			const Vec3 before = Environment::Transform(Environment::ModelMatrix(center, start, {1, 1, 1}), local);
			const Vec3 after = Environment::Transform(Environment::ModelMatrix(center, TurnOnRing(start, ring, 25), {1, 1, 1}), local);
			const Vec3 expect = center + rotate(before - center, axis, 25);
			CHECK(Near(after.x, expect.x, .05f) && Near(after.y, expect.y, .05f) && Near(after.z, expect.z, .05f));
		}
		CHECK(TurnOnRing({0, 170, 0}, Ring::Turn, 20).y < -169); // wraps
		// The ring and the angle under the cursor agree: a ring point reads back its own angle.
		const Vec3 axis = RingAxis(start, Ring::Tilt), eye{300, 300, 300};
		for (int i = 0; i < 8; ++i) {
			const Vec3 point = AxisRingPoint(center, axis, 40, i, 8);
			float deg = 0;
			CHECK(RingAngle(eye, Environment::Normalize(point - eye), center, axis, deg));
			CHECK(Near(WrapDegrees(deg - 45.0f * i), 0, .05f));
		}
		float deg = 0;
		CHECK(!RingAngle(eye, Environment::Normalize(center - eye), center, axis, deg)); // the centre
	}

	// The paint brush: inside the circle, never closer than the spacing, and a full spot takes no more.
	{
		uint32_t seed = 7;
		const Vec3 c{100, 200, 30};
		auto first = Scatter(c, 80, 20, 50, seed, {});
		CHECK(first.size() > 5);
		for (size_t i = 0; i < first.size(); ++i) {
			const float dx = first[i].x - c.x, dy = first[i].y - c.y;
			CHECK(dx * dx + dy * dy <= 80.0f * 80.0f + .01f && Near(first[i].z, 30));
			for (size_t j = i + 1; j < first.size(); ++j) {
				const float ex = first[i].x - first[j].x, ey = first[i].y - first[j].y;
				CHECK(ex * ex + ey * ey >= 20.0f * 20.0f - .01f);
			}
		}
		// Painting the same spot again fills only the gaps, and a packed circle takes nothing.
		std::vector<Vec3> all = first;
		for (int pass = 0; pass < 40; ++pass) {
			auto more = Scatter(c, 80, 20, 50, seed, all, 200);
			for (const Vec3& p : more)
				for (const Vec3& e : all) {
					const float dx = p.x - e.x, dy = p.y - e.y;
					CHECK(dx * dx + dy * dy >= 20.0f * 20.0f - .01f);
				}
			all.insert(all.end(), more.begin(), more.end());
		}
		const size_t packed = all.size();
		CHECK(packed < 80);                                      // a 80-unit disc holds well under 80 at 20 apart
		CHECK(Scatter(c, 80, 20, 50, seed, all, 64).size() <= 2); // nearly full
		CHECK(Scatter(c, 80, 20, 0, seed, {}).empty());
		// Density: the capacity estimate is close to what a real random fill packed in, a full
		// density lets a stroke keep filling, and lower ones stop at their share.
		const int capacity = PaintCapacity(80, 20);
		CHECK(capacity >= int(packed * 0.6) && capacity <= int(packed * 1.5));
		CHECK(PaintBudget(80, 20, 1.0f, 0, 6) == 6);                       // empty circle: a full batch
		CHECK(PaintBudget(80, 20, 1.0f, capacity - 2, 6) == 2);            // nearly full: the rest
		CHECK(PaintBudget(80, 20, 0.25f, int(capacity * 0.25f) + 1, 6) == 0); // a quarter reached
		CHECK(PaintBudget(80, 20, 0.25f, 0, 6) > 0);
		CHECK(PaintBudget(80, 20, 0.0f, 0, 6) >= 1);                       // clamped to 5%, never nothing
		CHECK(PaintBudget(80, 20, 2.0f, 0, 100) == 2 * capacity);          // past 100%: more than the spacing fits
		CHECK(PaintBudget(80, 20, 9.0f, 0, 1000) == int(std::ceil(kPaintMaxDensity * capacity))); // clamped to the most
		CHECK(PaintSpacing(20, 1.0f) == 20 && PaintSpacing(20, 0.5f) == 20); // up to 100% the spacing is as set
		CHECK(Near(PaintSpacing(20, 4.0f), 10));                            // 4x the copies: half the spacing
		// Painting over a FULL patch at 200% adds to it (at 100% a full patch took nothing more).
		{
			std::vector<Vec3> full;
			uint32_t s1 = 21;
			for (int tick = 0; tick < 200; ++tick) {
				const int b = PaintBudget(80, 20, 1.0f, int(full.size()), 8);
				if (!b) break;
				auto more = Scatter(c, 80, 20, b, s1, full, kPaintScatterAttempts);
				if (more.empty()) break;
				full.insert(full.end(), more.begin(), more.end());
			}
			CHECK(PaintBudget(80, 20, 1.0f, int(full.size()), 8) == 0 || Scatter(c, 80, 20, 8, s1, full, kPaintScatterAttempts).size() <= 1);
			const size_t before = full.size();
			for (int tick = 0; tick < 200; ++tick) {
				const int b = PaintBudget(80, 20, 2.0f, int(full.size()), 8);
				if (!b) break;
				auto more = Scatter(c, 80, PaintSpacing(20, 2.0f), b, s1, full, kPaintScatterAttempts);
				if (more.empty()) break;
				full.insert(full.end(), more.begin(), more.end());
			}
			CHECK(full.size() >= before + before / 2);
		}
		// A spray of a six-model brush is a full deck, and it deals every model once before repeats.
		CHECK(PaintBatch(1) == kPaintBatchMin && PaintBatch(12) == 12 && PaintBatch(40) == 16);
		{
			PaintBag bag;
			uint32_t s2 = 99;
			for (int round = 0; round < 4; ++round) {
				bool dealt[6] = {};
				for (int i = 0; i < 6; ++i) { const int k = PaintBagNext(bag, 6, s2); CHECK(k >= 0 && k < 6); dealt[k] = true; }
				for (bool d : dealt) CHECK(d);
			}
			CHECK(PaintBagNext(bag, 1, s2) == 0);
		}
		CHECK(PaintCapacity(0, 20) == 0 && PaintCapacity(80, 0) == 0);
		// A HELD brush (the spray Props.cpp runs every 45 ms) reaches what Density asks for: 100%
		// fills the circle to its capacity, half fills half -- the top of the slider is not dead.
		// The circle is 160 at the default 48 spacing, as the brush opens.
		for (float density : {1.0f, 0.5f}) {
			std::vector<Vec3> held;
			uint32_t spray = 11;
			for (int tick = 0; tick < 200; ++tick) {
				const int budget = PaintBudget(160, 48, density, int(held.size()), 6);
				if (!budget) break;
				auto more = Scatter(c, 160, 48, budget, spray, held, kPaintScatterAttempts);
				held.insert(held.end(), more.begin(), more.end());
			}
			const int want = int(std::ceil(density * PaintCapacity(160, 48)));
			CHECK(int(held.size()) >= want - 1);
		}
		CHECK(PaintCapacity(160, 48) >= 15); // the default brush paints a real patch, not three
		// Placing against a surface. A floor keeps the turn and rests the bottom on it; a ceiling
		// hangs it by its top; a wall turns the thin side flat against it, front to the room.
		{
			const auto floor = PoseOnSurface({10, 20, 30}, {0, 0, 1}, {-8, -8, -5}, {8, 8, 40}, 2, 35);
			CHECK(!floor.wall && Near(floor.angles.y, 35) && Near(floor.position.z, 40)); // 30 - (-5 * 2)
			const auto ceiling = PoseOnSurface({10, 20, 300}, {0, 0, -1}, {-8, -8, -5}, {8, 8, 40}, 1, 35);
			CHECK(!ceiling.wall && Near(ceiling.position.z, 260));
			// An ivy card thin along X that grows out along +X, on a wall facing +X: no turn, its back
			// (x = 0) on the wall, centred on the cursor in height.
			const auto ivy = PoseOnSurface({100, 0, 50}, {1, 0, 0}, {0, -50, -60}, {4, 50, 60}, 1, 70);
			CHECK(ivy.wall && Near(ivy.angles.y, 0) && Near(ivy.position.x, 100) && Near(ivy.position.z, 50));
			// A card thin along Y that grows toward -Y, on a wall facing +Y: turned 180 so -Y faces
			// the room, and its back plane (y = 0) on the wall.
			const auto card = PoseOnSurface({0, 200, 0}, {0, 1, 0}, {-50, -4, 0}, {50, 0, 100}, 1, 0);
			CHECK(card.wall && Near(std::fabs(card.angles.y), 180) && Near(card.position.y, 200) && Near(card.position.z, -50));
			// A thick box against a wall facing -X still sits outside it: its far side on the wall.
			const auto box = PoseOnSurface({0, 0, 0}, {-1, 0, 0}, {-10, -20, 0}, {10, 20, 30}, 1, 0);
			CHECK(box.wall && box.position.x < 0);
		}
		CHECK(PaintProbeLift(160) >= 160 && PaintProbeLift(10) >= kFloorProbeLift);
		{
			uint32_t pick = 5;
			int seen[3] = {0, 0, 0};
			for (int i = 0; i < 300; ++i) { const int k = PaintPick(3, pick); CHECK(k >= 0 && k < 3); seen[k]++; }
			CHECK(seen[0] > 50 && seen[1] > 50 && seen[2] > 50); // a mixed brush uses every model
			CHECK(PaintPick(1, pick) == 0);
		}
		uint32_t a = 3, b = 3;
		CHECK(Scatter(c, 50, 10, 5, a, {}).size() == Scatter(c, 50, 10, 5, b, {}).size()); // deterministic
		for (int i = 0; i < 50; ++i) {
			const float s = PaintScale(2, .25f, seed), y = PaintYaw(30, seed);
			CHECK(s >= 1.5f - .001f && s <= 2.5f + .001f);
			CHECK(y >= -30.001f && y <= 30.001f);
		}
	}

	if (g_failed) return 1;
	std::cout << "Prop edit math passed\n";
	return 0;
}
