#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace Filmmaker {

struct RagdollWeaponQuat {
	double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
};

inline bool RagdollWeaponNormalize(RagdollWeaponQuat& q) {
	const double n = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
	if (!std::isfinite(n) || n < 1e-12) return false;
	const double inv = 1.0 / std::sqrt(n);
	q.x *= inv; q.y *= inv; q.z *= inv; q.w *= inv;
	return true;
}

inline uint32_t RagdollWeaponHash(uint32_t value) {
	value ^= value >> 16;
	value *= 0x7feb352du;
	value ^= value >> 15;
	value *= 0x846ca68bu;
	return value ^ (value >> 16);
}

// Source QAngle order is pitch (Y axis), yaw (Z axis), roll (X axis).
// Choose roll=0 at a vertical forward axis, where yaw and roll are not unique.
inline void RagdollWeaponQuatToAngles(const RagdollWeaponQuat& q, float out[3]) {
	const double fx = 1.0 - 2.0*(q.y*q.y + q.z*q.z);
	const double fy = 2.0*(q.x*q.y + q.w*q.z);
	const double fz = 2.0*(q.x*q.z - q.w*q.y);
	const double leftX = 2.0*(q.x*q.y - q.w*q.z);
	const double leftY = 1.0 - 2.0*(q.x*q.x + q.z*q.z);
	const double leftZ = 2.0*(q.y*q.z + q.w*q.x);
	const double upZ = 1.0 - 2.0*(q.x*q.x + q.y*q.y);
	const double horizontal = std::sqrt(fx*fx + fy*fy);
	constexpr double degrees = 57.29577951308232;
	out[0] = (float)(std::atan2(-fz, horizontal)*degrees);
	out[1] = (float)((horizontal > 1e-6 ? std::atan2(fy,fx) : std::atan2(-leftX,leftY))*degrees);
	out[2] = horizontal > 1e-6 ? (float)(std::atan2(leftZ,upZ)*degrees) : 0.0f;
}

// Fixed per recorded death: seeking reproduces the same chance result.
inline bool RagdollWeaponWillFire(uint32_t deathSeed, double percent = 100.0) {
	return std::isfinite(percent) && (RagdollWeaponHash(deathSeed) % 100u) < percent;
}

// The first resolved frame gets a shot even for a 1 ms window. Subsequent shots
// must fall strictly within the window; two late callbacks cannot duplicate it.
inline bool RagdollWeaponShotDue(double age, double duration, unsigned shots,
	double now, double next, bool singleShot = false) {
	return duration > 0 && age >= 0 && age < duration && now >= next && (!singleShot || shots == 0);
}

inline double RagdollWeaponFallZ(double start, double ground, double elapsed) {
	const double t = (std::max)(0.0, elapsed);
	return (std::max)(ground, start - 400.0*t*t);
}

inline RagdollWeaponQuat RagdollWeaponBlend(RagdollWeaponQuat a, RagdollWeaponQuat b, double t) {
	t = std::clamp(t, 0.0, 1.0);
	if (a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w < 0) b={-b.x,-b.y,-b.z,-b.w};
	RagdollWeaponQuat q{a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t,a.w+(b.w-a.w)*t};
	RagdollWeaponNormalize(q);
	return q;
}

inline RagdollWeaponQuat RagdollWeaponQuatMul(const RagdollWeaponQuat& a,
	const RagdollWeaponQuat& b) {
	return {
		a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
		a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
		a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
		a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
	};
}

inline RagdollWeaponQuat RagdollWeaponPitchQuat(double degrees) {
	const double half = degrees * 0.008726646259971648;
	return { 0.0, std::sin(half), 0.0, std::cos(half) };
}

inline void RagdollWeaponReleaseVelocity(const float before[8], const float after[8], double dt,
	float velocity[3], float angularDegrees[3]) {
	for(int i=0;i<3;++i) {velocity[i]=0; angularDegrees[i]=0;}
	if(!std::isfinite(dt) || dt<0.001 || dt>0.1) return;
	RagdollWeaponQuat a{before[4],before[5],before[6],before[7]}, b{after[4],after[5],after[6],after[7]};
	if(!RagdollWeaponNormalize(a) || !RagdollWeaponNormalize(b)) return;
	for(int i=0;i<3;++i) velocity[i]=(float)((after[i]-before[i])/dt);
	auto q=RagdollWeaponQuatMul(b,{-a.x,-a.y,-a.z,a.w});
	if(q.w<0) q={-q.x,-q.y,-q.z,-q.w};
	const double n=std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z);
	if(n>1e-9) {
		const double scale=2*std::atan2(n,q.w)*57.29577951308232/(n*dt);
		angularDegrees[0]=(float)(q.x*scale); angularDegrees[1]=(float)(q.y*scale); angularDegrees[2]=(float)(q.z*scale);
	}
	// Bound discontinuous animation samples; ordinary hand momentum is left intact.
	for(auto pair : {std::pair<float*,double>{velocity,600.0}, {angularDegrees,1440.0}}) {
		double speed=std::sqrt(pair.first[0]*pair.first[0]+pair.first[1]*pair.first[1]+pair.first[2]*pair.first[2]);
		if(!std::isfinite(speed)) {for(int i=0;i<3;++i)pair.first[i]=0;}
		else if(speed>pair.second) {for(int i=0;i<3;++i)pair.first[i]=(float)(pair.first[i]*pair.second/speed);}
	}
}

inline double RagdollWeaponFallTime(double start, double ground) {
	return std::sqrt((std::max)(0.0,start-ground)/400.0);
}
inline RagdollWeaponQuat RagdollWeaponGroundRotation(const RagdollWeaponQuat& source,
	double nx, double ny, double nz) {
	float angles[3]; RagdollWeaponQuatToAngles(source,angles);
	const double yaw=angles[1]*0.008726646259971648;
	auto flat=RagdollWeaponQuatMul({0,0,std::sin(yaw),std::cos(yaw)},
		{std::sqrt(0.5),0,0,std::sqrt(0.5)});
	RagdollWeaponQuat tilt{-ny,nx,0,1+nz};
	RagdollWeaponNormalize(tilt);
	return RagdollWeaponQuatMul(tilt,flat);
}

inline double RagdollWeaponKick(double sinceShotSeconds) {
	if (sinceShotSeconds < 0.0 || sinceShotSeconds >= 0.11) return 0.0;
	const double t = sinceShotSeconds / 0.11;
	return 8.0 * (1.0 - t) * (1.0 - t);
}

} // namespace Filmmaker
