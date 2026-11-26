#pragma once

// Engine-free path sampling, screen picking, and curve-graph math for Scene Editor.

#include "../Takes/ActorAnimationsMath.h"
#include "../Takes/TakesMath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Filmmaker::SceneEditor {

struct SampledPathPoint {
	int sceneTick = 0;
	float pos[3] = {0.f, 0.f, 0.f};
	float yaw = 0.f;
};

struct TrackVisualPath {
	std::vector<SampledPathPoint> points;
	std::vector<SampledPathPoint> groundPoints; // floor shadow under airborne jump/drop arcs
	std::vector<std::array<float, 3>> cueMarkers;
	float startPos[3] = {0.f, 0.f, 0.f};
	float startYaw = 0.f;
	float endPos[3] = {0.f, 0.f, 0.f};
	float scrubPos[3] = {0.f, 0.f, 0.f};
	float scrubYaw = 0.f;
	bool valid = false;
	bool hasAirborneArc = false;
	float minZ = 0.f;
	float maxZ = 0.f;
};

enum class GraphChannel : uint8_t {
	HeightZ = 0,
	Speed = 1,
	Yaw = 2,
	AnimWeight = 3,
};

struct GraphPoint {
	int sceneTick = 0;
	float value = 0.f;
};

struct GraphCurve {
	GraphChannel channel = GraphChannel::HeightZ;
	int startTick = 0;
	int endTick = 0;
	float minVal = 0.f;
	float maxVal = 1.f;
	std::vector<GraphPoint> samples;
};

inline void SampleTrackPoseAtSceneTick(const Takes::Track &t, float tickRate, const ActorAnim::Library *animLib,
                                       int sceneTick, float outPos[3], float &outYaw, float *outSpeed = nullptr) {
	if (t.ticks.empty()) {
		outPos[0] = t.offsetPos[0]; outPos[1] = t.offsetPos[1]; outPos[2] = t.offsetPos[2];
		outYaw = Takes::WrapAngleDeg(t.offsetYaw);
		if (outSpeed) *outSpeed = 0.f;
		return;
	}
	const float rate = tickRate > 0.f ? tickRate : 64.f;
	const int32_t maxIdx = int32_t(t.ticks.size()) - 1;
	const int32_t local = std::clamp(sceneTick - t.delayTicks, int32_t(0), maxIdx);

	// For a placed actor whose animation cue has root motion / vertical jump displacement, evaluate
	// directly from the clip + cue timing so swapping cues updates the trajectory arc immediately.
	if (t.placedActor && animLib && !t.animCues.empty()) {
		const auto &cue = t.animCues[0];
		if (const auto *clip = animLib->Find(cue.clip)) {
			const Takes::TickSample &s0 = t.ticks[0];
			float basePos[3] = {}, baseVel[3] = {}, baseYaw = s0.yaw;
			Takes::TransformTrackSample(t, s0, basePos, baseVel, baseYaw);
			const double localSec = std::max(0.0, double(local - cue.startTick) / double(rate));
			float offXY[2] = {0.f, 0.f};
			ActorAnim::RootMotionOffset(*clip, cue, localSec, offXY);
			float pel[3] = {}, rot[ActorAnim::kCoreBones][4] = {}, dir[ActorAnim::kCoreBones][3] = {};
			float lift = 0.f;
			if (!clip->frames.empty())
				ActorAnim::SampleClip(*clip, float(localSec * cue.speed + cue.startTime), cue.loop, pel, rot, dir, &lift);
			const float yawRad = baseYaw * 0.017453292519943295f;
			const float cs = std::cos(yawRad), sn = std::sin(yawRad);
			outPos[0] = basePos[0] + offXY[0] * cs - offXY[1] * sn;
			outPos[1] = basePos[1] + offXY[0] * sn + offXY[1] * cs;
			outPos[2] = basePos[2] + std::max(-1.5f, lift);
			outYaw = baseYaw;
			if (outSpeed) {
				float nextXY[2] = {0.f, 0.f};
				ActorAnim::RootMotionOffset(*clip, cue, localSec + 1.0 / double(rate), nextXY);
				*outSpeed = std::hypot(nextXY[0] - offXY[0], nextXY[1] - offXY[1]) * rate;
			}
			return;
		}
	}

	const Takes::TickSample &s = t.ticks[local];
	float vel[3] = {};
	Takes::TransformTrackSample(t, s, outPos, vel, outYaw);
	if (outSpeed) *outSpeed = std::hypot(vel[0], vel[1]);
}

inline TrackVisualPath BuildTrackVisualPath(const Takes::Track &t, float tickRate, const ActorAnim::Library *animLib,
                                            int scrubTick, int maxPoints = 160) {
	TrackVisualPath path;
	if (t.ticks.empty()) return path;
	path.valid = true;
	const int n = int(t.ticks.size());
	const int step = std::max(1, (n + std::max(2, maxPoints) - 2) / std::max(2, maxPoints - 1));

	float minZ = 1e9f, maxZ = -1e9f;
	for (int i = 0; i < n; i += step) {
		SampledPathPoint pt;
		pt.sceneTick = t.delayTicks + i;
		SampleTrackPoseAtSceneTick(t, tickRate, animLib, pt.sceneTick, pt.pos, pt.yaw);
		minZ = std::min(minZ, pt.pos[2]);
		maxZ = std::max(maxZ, pt.pos[2]);
		path.points.push_back(pt);
	}
	if ((n - 1) % step != 0) {
		SampledPathPoint pt;
		pt.sceneTick = t.delayTicks + (n - 1);
		SampleTrackPoseAtSceneTick(t, tickRate, animLib, pt.sceneTick, pt.pos, pt.yaw);
		minZ = std::min(minZ, pt.pos[2]);
		maxZ = std::max(maxZ, pt.pos[2]);
		path.points.push_back(pt);
	}

	for (int k = 0; k < 3; ++k) {
		path.startPos[k] = path.points.front().pos[k];
		path.endPos[k] = path.points.back().pos[k];
	}
	path.startYaw = path.points.front().yaw;
	path.minZ = minZ;
	path.maxZ = maxZ;
	path.hasAirborneArc = (maxZ - minZ) > 8.0f;

	if (path.hasAirborneArc) {
		path.groundPoints.reserve(path.points.size());
		for (const auto &pt : path.points) {
			SampledPathPoint gp = pt;
			gp.pos[2] = minZ;
			path.groundPoints.push_back(gp);
		}
	}

	for (const auto &c : t.animCues) {
		float cp[3] = {}, cy = 0.f;
		SampleTrackPoseAtSceneTick(t, tickRate, animLib, t.delayTicks + c.startTick, cp, cy);
		path.cueMarkers.push_back({cp[0], cp[1], cp[2]});
	}

	SampleTrackPoseAtSceneTick(t, tickRate, animLib, scrubTick, path.scrubPos, path.scrubYaw);
	return path;
}

inline float EvaluateCueWeightAtSceneTick(const ActorAnim::Cue &c, const ActorAnim::Library *animLib,
                                          int delayTicks, float tickRate, int sceneTick) {
	const float rate = tickRate > 0.f ? tickRate : 64.f;
	const double localSec = double(sceneTick - (delayTicks + c.startTick)) / double(rate);
	if (localSec < 0.0) return 0.f;
	const ActorAnim::Clip *clip = animLib ? animLib->Find(c.clip) : nullptr;
	float effDur = c.duration;
	if (effDur <= 0.f && clip) effDur = ActorAnim::CueEffectiveDuration(c, *clip);
	if (c.loop && c.duration <= 0.f) effDur = 1e6f;
	if (effDur <= 1e-4f) effDur = 1.f;
	if (localSec >= double(effDur)) return 0.f;
	float w = 1.f;
	if (c.blendIn > 1e-4f && localSec < double(c.blendIn))
		w = std::min(w, float(localSec / double(c.blendIn)));
	if (c.blendOut > 1e-4f && (!c.loop || c.duration > 0.f)) {
		const double rem = double(effDur) - localSec;
		if (rem < double(c.blendOut)) w = std::min(w, float(std::max(0.0, rem) / double(c.blendOut)));
	}
	return std::clamp(w, 0.f, 1.f);
}

inline GraphCurve BuildTrackGraphCurve(const Takes::Track &t, float tickRate, const ActorAnim::Library *animLib,
                                       GraphChannel channel, int maxSamples = 128) {
	GraphCurve curve;
	curve.channel = channel;
	const int n = int(t.ticks.size());
	if (n <= 0) return curve;
	curve.startTick = t.delayTicks;
	curve.endTick = t.delayTicks + std::max(1, n - 1);
	const int step = std::max(1, (n + std::max(2, maxSamples) - 2) / std::max(2, maxSamples - 1));

	float minV = 1e9f, maxV = -1e9f;
	auto addSample = [&](int i) {
		const int st = t.delayTicks + i;
		float pos[3] = {}, yaw = 0.f, spd = 0.f;
		SampleTrackPoseAtSceneTick(t, tickRate, animLib, st, pos, yaw, &spd);
		float val = 0.f;
		switch (channel) {
		case GraphChannel::HeightZ: val = pos[2]; break;
		case GraphChannel::Speed: val = spd; break;
		case GraphChannel::Yaw: val = yaw; break;
		case GraphChannel::AnimWeight: {
			for (const auto &c : t.animCues)
				val = std::max(val, EvaluateCueWeightAtSceneTick(c, animLib, t.delayTicks, tickRate, st));
			break;
		}
		}
		minV = std::min(minV, val);
		maxV = std::max(maxV, val);
		curve.samples.push_back({st, val});
	};

	for (int i = 0; i < n; i += step) addSample(i);
	if ((n - 1) % step != 0) addSample(n - 1);

	if (channel == GraphChannel::AnimWeight) {
		curve.minVal = 0.f;
		curve.maxVal = 1.f;
	} else {
		if (maxV - minV < 1e-2f) {
			const float mid = 0.5f * (minV + maxV);
			minV = mid - 1.f;
			maxV = mid + 1.f;
		}
		curve.minVal = minV;
		curve.maxVal = maxV;
	}
	return curve;
}

inline int PickNearestProjected(float u, float v, const float (*uvVis)[3], int count, float maxRadiusNorm = 0.045f) {
	int best = -1;
	float bestD2 = maxRadiusNorm * maxRadiusNorm;
	for (int i = 0; i < count; ++i) {
		if (uvVis[i][2] <= 0.5f) continue;
		const float dx = u - uvVis[i][0];
		const float dy = v - uvVis[i][1];
		const float d2 = dx * dx + dy * dy;
		if (d2 < bestD2) {
			bestD2 = d2;
			best = i;
		}
	}
	return best;
}

} // namespace Filmmaker::SceneEditor
