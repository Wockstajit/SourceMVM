#include "../../AfxHookSource2/Filmmaker/Movie/RagdollWeaponMath.h"
#include "../../AfxHookSource2/Filmmaker/Movie/RagdollWeaponRates.h"
#include "../../AfxHookSource2/Filmmaker/Movie/RagdollWeaponArmPhysicsMath.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <limits>
#include <cstdlib>
#include <initializer_list>

int main() {
	using namespace Filmmaker;
	float startPose[8]={0,0,50,1,0,0,0,1}, endPose[8]={2,-1,50,1,0,0,0,1}, velocity[3], angular[3];
	RagdollWeaponReleaseVelocity(startPose,endPose,0.01,velocity,angular);
	assert(std::abs(velocity[0]-200)<0.001 && std::abs(velocity[1]+100)<0.001 && angular[0]==0);
	const auto pitchStep=RagdollWeaponPitchQuat(5);
	endPose[5]=(float)pitchStep.y; endPose[7]=(float)pitchStep.w;
	RagdollWeaponReleaseVelocity(startPose,endPose,0.01,velocity,angular);
	assert(std::abs(angular[1]-500)<0.01);
	endPose[5]*=-1; endPose[7]*=-1;
	RagdollWeaponReleaseVelocity(startPose,endPose,0.01,velocity,angular);
	assert(std::abs(angular[1]-500)<0.01); // quaternion sign cannot reverse the release spin
	RagdollWeaponReleaseVelocity(startPose,endPose,0,velocity,angular);
	assert(velocity[0]==0 && angular[1]==0); // paused/repeated frame
	RagdollWeaponReleaseVelocity(startPose,endPose,1,velocity,angular);
	assert(velocity[0]==0 && angular[1]==0); // seek gap
	assert(RagdollWeaponHash(1234u) == RagdollWeaponHash(1234u));
	unsigned firing = 0;
	for (uint32_t i = 0; i < 1000; ++i) firing += RagdollWeaponWillFire(i,45) ? 1u : 0u;
	assert(firing > 390 && firing < 510);
	for(uint32_t seed=0;seed<10000;++seed) {
		assert(RagdollWeaponWillFire(seed,100));
		assert(!RagdollWeaponWillFire(seed,0));
	}
	assert(RagdollWeaponShotDue(0,0.001,0,10,10));
	assert(!RagdollWeaponShotDue(0,0.001,1,10,10.13));
	assert(!RagdollWeaponShotDue(.13,.001,1,10.13,10.13));
	assert(RagdollWeaponShotDue(.13,.5,1,10.13,10.13));
	assert(!RagdollWeaponShotDue(.5,.5,2,10.5,10.5));
	assert(RagdollWeaponFallZ(100,0,0)==100);
	assert(RagdollWeaponFallZ(100,0,.25)==75);
	assert(RagdollWeaponFallZ(100,0,.5)==0);
	assert(RagdollWeaponFallZ(100,0,100)==0);
	assert(RagdollWeaponFallZ(100,0,-1)==100);
	for(double t:{0.0,.1,.5,1.0,2.0}) {
		auto blend=RagdollWeaponBlend({0,0,0,1},{0,0,0,-1},t);
		assert(std::abs(blend.w-1)<1e-12);
	}
	assert(RagdollWeaponProfile(7,nullptr)->cycle==0.1);
	assert(RagdollWeaponProfile(1,nullptr)->cycle==0.225);
	assert(RagdollWeaponProfile(60,"weapon_m4a1")->cycle==0.1);
	assert(RagdollWeaponProfile(16,nullptr)->cycle==0.09);
	assert(RagdollWeaponProfile(0,"weapon_hkp2000")->cycle==0.17);
	assert(!RagdollWeaponProfile(0,"unknown"));
	for(unsigned item:{9u,11u,38u,40u}) {
		auto profile=RagdollWeaponProfile(item,nullptr);
		assert(profile && profile->singleShot);
		assert(RagdollWeaponShotDue(0,5,0,10,10,profile->singleShot));
		assert(!RagdollWeaponShotDue(2,5,1,12,11,profile->singleShot));
	}
	// Repeated callback and paused-time scheduling: never double-fire or catch up in a burst.
	for(unsigned item:{1u,7u,16u,32u,60u}) {
		auto profile=RagdollWeaponProfile(item,nullptr);
		double next=0,last=-10; unsigned shots=0;
		for(int frame=0;frame<1000;++frame) {
			double now=frame/1000.0;
			for(int callback=0;callback<3;++callback) {
				if(!RagdollWeaponShotDue(now,.5,shots,now,next)) continue;
				assert(now-last+1e-9>=profile->cycle);
				last=now; next=now+profile->cycle; ++shots;
			}
		}
		assert(shots>=2 && shots<=6);
	}
	assert(RagdollWeaponFallTime(100,0)==.5);
	auto arm = RagdollWeaponMakeArmImpulse({0,0,0,1}, 1800);
	assert(arm.force[0] < -1700 && arm.force[2] > 300);
	assert(std::abs(std::sqrt(arm.force[0]*arm.force[0] + arm.force[1]*arm.force[1]
		+ arm.force[2]*arm.force[2]) - 1800) < 0.1);
	auto rotatedArm = RagdollWeaponMakeArmImpulse(
		{0,0,std::sqrt(.5),std::sqrt(.5)}, 1800);
	assert(rotatedArm.force[1] < -1700 && rotatedArm.force[2] > 300);
	auto noArm = RagdollWeaponMakeArmImpulse({0,0,0,0}, 1800);
	assert(noArm.force[0] == 0 && noArm.force[1] == 0 && noArm.force[2] == 0);
	for(auto normal:{RagdollWeaponQuat{0,0,1,0},RagdollWeaponQuat{.3,0,std::sqrt(.91),0}}) {
		auto resting=RagdollWeaponGroundRotation({0,0,0,1},normal.x,normal.y,normal.z);
		assert(std::abs(resting.x*resting.x+resting.y*resting.y+resting.z*resting.z+resting.w*resting.w-1)<1e-12);
	}

	assert(RagdollWeaponKick(-0.01) == 0.0);
	assert(std::abs(RagdollWeaponKick(0.0) - 8.0) < 1e-9);
	assert(RagdollWeaponKick(0.055) > 0.0);
	assert(RagdollWeaponKick(0.11) == 0.0);
	RagdollWeaponQuat q = RagdollWeaponQuatMul({0,0,0,1}, RagdollWeaponPitchQuat(8.0));
	assert(std::abs(q.y) > 0.01 && std::abs(q.w) > 0.9);
	// Exercise float rounding over a long paused shot. The old unnormalized conjugate
	// multiply squared the previous norm until it became zero or NaN.
	for (int frame = 0; frame < 100000; ++frame) {
		q = {(float)q.x, (float)q.y, (float)q.z, (float)q.w};
		if (!RagdollWeaponNormalize(q)) std::abort();
		const double n = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
		if (std::abs(n-1.0) > 1e-12) std::abort();
	}
	q = {0,0,0,0};
	// Round-trip the interpolated scene rotation, including vertical and upside-down
	// poses. Compare orientations, not Euler values, because singular poses are nonunique.
	for (double pitch : {-90.0,-89.99,-45.0,0.0,45.0,89.99,90.0,135.0}) {
		for (double yaw : {-170.0,0.0,78.0}) for (double roll : {-125.0,0.0,175.0}) {
			const double h=0.008726646259971648;
			auto original=RagdollWeaponQuatMul(RagdollWeaponQuatMul(
				{0,0,std::sin(yaw*h),std::cos(yaw*h)},RagdollWeaponPitchQuat(pitch)),
				{std::sin(roll*h),0,0,std::cos(roll*h)});
			float angles[3]; RagdollWeaponQuatToAngles(original,angles);
			auto rebuilt=RagdollWeaponQuatMul(RagdollWeaponQuatMul(
				{0,0,std::sin(angles[1]*h),std::cos(angles[1]*h)},RagdollWeaponPitchQuat(angles[0])),
				{std::sin(angles[2]*h),0,0,std::cos(angles[2]*h)});
			const double dot=original.x*rebuilt.x+original.y*rebuilt.y+original.z*rebuilt.z+original.w*rebuilt.w;
			if (std::abs(dot)<0.999999) std::abort();
		}
	}
	if (RagdollWeaponNormalize(q)) std::abort();
	q = {0,0,0,std::numeric_limits<double>::quiet_NaN()};
	if (RagdollWeaponNormalize(q)) std::abort();
	return 0;
}
