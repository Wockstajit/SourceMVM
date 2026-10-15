// Release builds define NDEBUG, which would compile every assert below away.
#undef NDEBUG
#include "../../AfxHookSource2/Filmmaker/World/SunRotationMath.h"

#include <cassert>
#include <cmath>

using Filmmaker::SunRotationMath::AnglesToDirection;
using Filmmaker::SunRotationMath::ClampPitch;
using Filmmaker::SunRotationMath::FromYawPitch;
using Filmmaker::SunRotationMath::Length;
using Filmmaker::SunRotationMath::Normalize;
using Filmmaker::SunRotationMath::Quat;
using Filmmaker::SunRotationMath::Rotate;
using Filmmaker::SunRotationMath::RotateAroundPoint;
using Filmmaker::SunRotationMath::BoundsContain;
using Filmmaker::SunRotationMath::Vec3;
using Filmmaker::SunRotationMath::WrapYaw;

static bool Near(float a, float b, float e = 1e-4f) { return std::fabs(a - b) <= e; }

int main() {
	assert(Near(WrapYaw(-90.0f), 270.0f));
	assert(Near(WrapYaw(450.0f), 90.0f));
	assert(Near(WrapYaw(360.0f), 360.0f));
	assert(Near(ClampPitch(-120.0f), -90.0f));
	assert(Near(ClampPitch(120.0f), 90.0f));

	const Vec3 forward = AnglesToDirection(0.0f, 0.0f);
	assert(Near(forward.x, 1.0f) && Near(forward.y, 0.0f) && Near(forward.z, 0.0f));
	const Vec3 yaw90 = Rotate(forward, 90.0f, 0.0f, forward);
	assert(Near(yaw90.x, 0.0f) && Near(yaw90.y, 1.0f) && Near(yaw90.z, 0.0f));
	const Vec3 pitch30 = Rotate(forward, 0.0f, 30.0f, forward); // positive pitch RAISES the sun
	assert(Near(pitch30.x, 0.8660254f, 1e-3f) && Near(pitch30.z, 0.5f, 1e-3f));
	const Vec3 sweepBase = AnglesToDirection(17.0f, 35.0f);
	const Vec3 sweep360 = Rotate(sweepBase, 360.0f, 0.0f, sweepBase);
	assert(Near(sweep360.x, sweepBase.x, 1e-4f)
		&& Near(sweep360.y, sweepBase.y, 1e-4f)
		&& Near(sweep360.z, sweepBase.z, 1e-4f));
	const Vec3 skyHigh = AnglesToDirection(270.0f, 90.0f);
	const Vec3 skyLow = AnglesToDirection(270.0f, -90.0f);
	assert(Near(skyHigh.z, 1.0f) && Near(skyLow.z, -1.0f));
	assert(Near(Length(Rotate({ 0.25f, -0.8f, 0.543139f }, 37.0f, -22.0f, { 0.25f, -0.8f, 0.543139f })),
		Length({ 0.25f, -0.8f, 0.543139f }), 1e-4f));

	// The live sky path is quaternion-authoritative.  These checks deliberately cross the poles,
	// where the old Euler-addition implementation produced a visible gimbal flip.
	const Vec3 qPitch90 = Rotate(FromYawPitch(0.0f, 90.0f), forward);
	assert(Near(qPitch90.x, 0.0f, 1e-4f) && Near(qPitch90.y, 0.0f, 1e-4f)
		&& Near(qPitch90.z, -1.0f, 1e-4f));
	const Quat qBeforePole = FromYawPitch(37.0f, 89.0f);
	const Quat qAtPole = FromYawPitch(37.0f, 90.0f);
	const Quat qAfterPole = FromYawPitch(37.0f, 91.0f);
	assert(Near(Length(Rotate(qBeforePole, forward)), 1.0f, 1e-4f));
	assert(Near(Length(Rotate(qAtPole, forward)), 1.0f, 1e-4f));
	assert(Near(Length(Rotate(qAfterPole, forward)), 1.0f, 1e-4f));
	// Adjacent orientations remain adjacent; a raw Euler projection would jump by 180° here.
	const Vec3 before = Rotate(qBeforePole, forward);
	const Vec3 after = Rotate(qAfterPole, forward);
	assert(before.x * after.x + before.y * after.y + before.z * after.z > 0.999f);
	const Quat normalized = Normalize({ 0.2f, -0.4f, 0.1f, 0.8f });
	assert(Near(Length(Rotate(normalized, { 1.0f, 0.0f, 0.0f })), 1.0f, 1e-4f));

	// A rigid camera-relative rotation preserves both the centre radius and the billboard's radial
	// normal. Translating only the centre (the old implementation) leaves the plane facing the old
	// direction, which becomes edge-on at a 90-degree sweep and makes the sun disappear.
	const Vec3 camera{ 0.0f, 0.0f, 0.0f };
	const Vec3 center{ 100.0f, 0.0f, 0.0f };
	const Vec3 movedCenter = RotateAroundPoint(center, camera, 90.0f, 0.0f, center);
	assert(Near(movedCenter.x, 0.0f, 1e-3f) && Near(movedCenter.y, 100.0f, 1e-3f));
	const Vec3 corner{ 100.0f, 4.0f, 2.0f };
	const Vec3 movedCorner = RotateAroundPoint(corner, camera, 90.0f, 0.0f, center);
	assert(Near(movedCorner.x - movedCenter.x, -4.0f, 1e-3f));
	assert(Near(movedCorner.y - movedCenter.y, 0.0f, 1e-3f));
	assert(Near(movedCorner.z - movedCenter.z, 2.0f, 1e-3f));
	assert(Near(Length(movedCenter), Length(center), 1e-3f));

	// Yaw circles the sun at the same height; pitch changes only its height. The bug case: a sun
	// sitting along world Y (azimuth 90), where the old fixed-Y-axis pitch swung it sideways instead.
	{
		const Vec3 sun = AnglesToDirection(90.0f, 30.0f);
		for (float yaw = 0.0f; yaw <= 360.0f; yaw += 15.0f) {
			const Vec3 circled = Rotate(sun, yaw, 0.0f, sun);
			assert(Near(circled.z, sun.z, 1e-4f)); // same elevation all the way round
			const float azimuth = std::atan2(circled.y, circled.x) * 57.29578f;
			float expected = 90.0f + yaw; while (expected > 180.0f) expected -= 360.0f;
			float delta = azimuth - expected; while (delta > 180.0f) delta -= 360.0f; while (delta < -180.0f) delta += 360.0f;
			assert(Near(delta, 0.0f, 0.05f));
		}
		const Vec3 raised = Rotate(sun, 0.0f, 20.0f, sun);
		assert(Near(std::asin(raised.z) * 57.29578f, 50.0f, 0.05f));        // 30 + 20 degrees high
		assert(Near(std::atan2(raised.y, raised.x) * 57.29578f, 90.0f, 0.05f)); // azimuth unchanged
		const Vec3 lowered = Rotate(sun, 0.0f, -45.0f, sun);
		assert(Near(std::asin(lowered.z) * 57.29578f, -15.0f, 0.05f));
		// Both at once: elevation from pitch, azimuth from yaw.
		const Vec3 both = Rotate(sun, 90.0f, 10.0f, sun);
		assert(Near(std::asin(both.z) * 57.29578f, 40.0f, 0.05f));
		assert(Near(std::atan2(both.y, both.x) * 57.29578f, 180.0f, 0.05f) || Near(std::atan2(both.y, both.x) * 57.29578f, -180.0f, 0.05f));
		// The sun never flips over the zenith: +90 pitch on a 30-degree sun stops at 89 degrees.
		const Vec3 top = Rotate(sun, 0.0f, 90.0f, sun);
		assert(Near(std::asin(top.z) * 57.29578f, 89.0f, 0.05f) && top.y > 0.0f);
		// The light direction points away from the sun and turns with it (one rigid rotation).
		const Vec3 light{ -sun.x, -sun.y, -sun.z };
		const Vec3 turnedLight = Rotate(light, 90.0f, 10.0f, sun);
		assert(Near(turnedLight.x, -both.x, 1e-4f) && Near(turnedLight.y, -both.y, 1e-4f) && Near(turnedLight.z, -both.z, 1e-4f));
		// A sun straight overhead has no azimuth: it falls back without NaNs.
		const Vec3 zenith{ 0.0f, 0.0f, 1.0f };
		const Vec3 fromZenith = Rotate(zenith, 45.0f, -30.0f, zenith);
		assert(std::isfinite(fromZenith.x) && Near(Length(fromZenith), 1.0f, 1e-4f));
	}

	// Mirage's large glow is split into two triangles. Each measured triangle encloses the disc
	// anchor, which is the stable geometry discriminator for the low-boost companion draws.
	const Vec3 discAnchor{ 3147.3f, 728.3f, 5240.9f };
	assert(BoundsContain({ -1801.2f, 374.7f, 3266.6f },
		{ 7581.9f, 5686.5f, 6840.5f }, discAnchor));
	assert(BoundsContain({ -1801.2f, -3767.4f, 3266.6f },
		{ 7581.9f, 1544.5f, 6840.5f }, discAnchor));
	assert(!BoundsContain({ -10.0f, -10.0f, -10.0f }, { 10.0f, 10.0f, 10.0f }, discAnchor));
	return 0;
}
