#pragma once

// osu! star rating, ported 1:1 from osu!lazer 2026.1005.0-lazer (OsuDifficultyCalculator version
// 20260706): OsuDifficultyHitObject preprocessing, the Aim (snap / agility / flow), Speed (+ rhythm),
// Reading and Flashlight skills, and the star rating that combines them. Used to show the star
// rating WITH mods (DT / HT / HR / EZ / HD / FL change it), which lazer's stored value lacks.
// Validated against the star ratings lazer stored in the player's client.realm
// (Automation/tests/osu-difficulty-tests.cpp).
//
// Input: a beatmap already passed through ApplyModsToDifficulty (CS / AR / OD) and ApplyStacking,
// as gameplay builds it. Positions and slider paths are therefore stacked (StackedPosition).

#include "OsuMath.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace Filmmaker {
namespace Osu {
namespace Diff {

// ---- DiffUtils ----------------------------------------------------------------------------------
inline double Logistic(double x, double midpointOffset, double multiplier, double maxValue = 1.0) {
	return maxValue / (1 + std::exp(multiplier * (midpointOffset - x)));
}
inline double LogisticE(double exponent, double maxValue = 1.0) { return maxValue / (1 + std::exp(exponent)); }
inline double Smoothstep(double x, double start, double end) {
	x = std::clamp((x - start) / (end - start), 0.0, 1.0);
	return x * x * (3.0 - 2.0 * x);
}
inline double Smootherstep(double x, double start, double end) {
	x = std::clamp((x - start) / (end - start), 0.0, 1.0);
	return x * x * x * (x * (6.0 * x - 15.0) + 10.0);
}
inline double SmoothstepBellCurve(double x) {
	x = 0.5 - std::fabs(x - 0.5);
	x = std::clamp(x * 2.0, 0.0, 1.0);
	return x * x * (3.0 - 2.0 * x);
}
inline double ReverseLerp(double x, double start, double end) { return std::clamp((x - start) / (end - start), 0.0, 1.0); }
inline double Lerp(double a, double b, double t) { return a + (b - a) * t; }
inline double MsToBpm(double ms, int delimiter = 4) { return 60000.0 / (ms * delimiter); }
inline double BpmToMs(double bpm, int delimiter = 4) { return 60000.0 / delimiter / bpm; }
inline double Norm(double p, std::initializer_list<double> values) {
	double sum = 0;
	for (double x : values) sum += std::pow(x, p);
	return std::pow(sum, 1.0 / p);
}
inline double Deg(double d) { return d * 3.14159265358979323846 / 180.0; }
// C# Math.Round: banker's rounding.
inline double RoundEven(double v) { return std::nearbyint(v); }

constexpr int kNormRadius = 50, kNormDiameter = 100, kMinDelta = 25;
constexpr double kMaxSliderRadius = kNormRadius * 2.4, kAssumedSliderRadius = kNormRadius * 1.8;

struct Mods { double rate = 1.0; bool hidden = false, flashlight = false; };

// A slider's nested object (lazer Slider.NestedHitObjects; the legacy last tick is catch-only).
enum NestedKind { kHead, kTick, kRepeat, kTail };
struct Nested { double time; Vec2 pos; NestedKind kind; };

// ---- OsuDifficultyHitObject ----------------------------------------------------------------------
struct DObj {
	const HitObject* base = nullptr; const HitObject* last = nullptr;
	int index = 0;
	double deltaTime = 0, startTime = 0, endTime = 0, clockRate = 1, hitWindowGreat = 0;
	double adjustedDeltaTime = 0, lastObjectEndDeltaTime = 0;
	double jumpDistance = 0, lazyJumpDistance = 0, minimumJumpDistance = 0, minimumJumpTime = 0;
	double travelDistance = 0, travelTime = 0;
	bool hasLazyEnd = false; Vec2 lazyEnd;
	double lazyTravelDistance = 0, lazyTravelTime = 0;
	bool hasAngle = false; double angle = 0;
	bool hasNva = false; double nva = 0;
	double preempt = 0;      // scaled by rate
	double smallCircleBonus = 1, overallDifficulty = 0;
	bool IsSpinner() const { return (base->type & kSpinner) != 0; }
	bool IsSlider() const { return (base->type & kSlider) != 0; }
};

struct Context {
	const Beatmap* map = nullptr;
	Mods mods;
	double radius = 32, preemptRaw = 600, fadeInRaw = 400, fadeInHiddenRaw = 240;
	std::vector<std::vector<Nested>> nested; // per beatmap object (sliders only)
	std::vector<DObj> objs;
	const DObj* Prev(const DObj& d, int skip = 0) const { const int i = d.index - (skip + 1); return i >= 0 && i < (int)objs.size() ? &objs[i] : nullptr; }
	const DObj* Next(const DObj& d, int skip = 0) const { const int i = d.index + (skip + 1); return i >= 0 && i < (int)objs.size() ? &objs[i] : nullptr; }
};

inline Vec2 EndCursor(const DObj& d) { return d.hasLazyEnd ? d.lazyEnd : d.base->pos; }

inline double OpacityAt(const Context& c, const DObj& d, double time, bool hidden) {
	const double start = d.base->time;
	if (time > start) return 0.0;
	const double fadeInStart = start - c.preemptRaw;
	const double fadeInDuration = 400.0 * std::min(1.0, c.preemptRaw / 450.0);
	if (hidden) {
		const double fadeOutStart = start - c.preemptRaw + c.fadeInHiddenRaw;
		const double fadeOutDuration = c.preemptRaw * 0.3;
		return std::min(std::clamp((time - fadeInStart) / fadeInDuration, 0.0, 1.0),
			1.0 - std::clamp((time - fadeOutStart) / fadeOutDuration, 0.0, 1.0));
	}
	return std::clamp((time - fadeInStart) / fadeInDuration, 0.0, 1.0);
}

inline double DoubleTapFeasibility(const DObj& d, const DObj* next) {
	if (!next) return 0;
	const double currDelta = std::max(1.0, d.deltaTime), nextDelta = std::max(1.0, next->deltaTime);
	const double deltaDifference = std::fabs(nextDelta - currDelta);
	const double speedRatio = currDelta / std::max(currDelta, deltaDifference);
	const double windowRatio = std::pow(std::min(1.0, currDelta / d.hitWindowGreat), 5);
	const double distanceFactor = std::pow(ReverseLerp(d.lazyJumpDistance, kNormDiameter, kNormRadius), 2);
	return 1.0 - std::pow(speedRatio, distanceFactor * (1 - windowRatio));
}

inline double CalcAngle(Vec2 cur, Vec2 last, Vec2 lastLast) {
	const Vec2 v1 = lastLast - last, v2 = cur - last;
	const double dot = v1.x * v2.x + v1.y * v2.y, det = v1.x * v2.y - v1.y * v2.x;
	return std::fabs(std::atan2(det, dot));
}

inline void ComputeSliderCursor(Context& c, DObj& d) {
	if (!d.IsSlider()) return;
	const HitObject& s = *d.base;
	const std::vector<Nested>& orig = c.nested[d.base - c.map->objects.data()];
	const double duration = s.endTime - s.time;
	double trackingEnd = std::max(s.time + duration - 36.0, s.time + duration / 2);
	std::vector<Nested> nestedObjects = orig;
	int lastTick = -1;
	for (int i = 0; i < (int)orig.size(); ++i) if (orig[i].kind == kTick) lastTick = i;
	if (lastTick >= 0 && orig[lastTick].time > trackingEnd) {
		trackingEnd = orig[lastTick].time;
		const Nested t = nestedObjects[lastTick];
		nestedObjects.erase(nestedObjects.begin() + lastTick);
		nestedObjects.push_back(t);
	}
	d.lazyTravelTime = trackingEnd - s.time;
	double endTimeMin = s.span > 0 ? d.lazyTravelTime / s.span : 0;
	if (std::fmod(endTimeMin, 2.0) >= 1) endTimeMin = 1 - std::fmod(endTimeMin, 1.0);
	else endTimeMin = std::fmod(endTimeMin, 1.0);
	d.hasLazyEnd = true;
	d.lazyEnd = PathAt(s.path, endTimeMin);
	Vec2 cursor = s.pos;
	const double scaling = kNormRadius / c.radius;
	for (int i = 1; i < (int)nestedObjects.size(); ++i) {
		const Nested& m = nestedObjects[i];
		Vec2 move = m.pos - cursor;
		double moveLen = scaling * Len(move);
		double required = kAssumedSliderRadius;
		if (i == (int)nestedObjects.size() - 1) {
			const Vec2 lazyMove = d.lazyEnd - cursor;
			if (Len(lazyMove) < Len(move)) move = lazyMove;
			moveLen = scaling * Len(move);
		} else if (m.kind == kRepeat) {
			required = kNormRadius;
		}
		if (moveLen > required) {
			cursor = cursor + move * ((moveLen - required) / moveLen);
			moveLen *= (moveLen - required) / moveLen;
			d.lazyTravelDistance += moveLen;
		}
		if (i == (int)nestedObjects.size() - 1) d.lazyEnd = cursor;
	}
}

inline void SetDistances(Context& c, DObj& d) {
	if (d.IsSlider()) {
		d.travelDistance = d.lazyTravelDistance * std::max(1.0, std::pow((double)(d.base->slides - 1), 0.3));
		d.travelTime = std::max(d.lazyTravelTime / d.clockRate, (double)kMinDelta);
	}
	d.minimumJumpTime = d.adjustedDeltaTime;
	if (d.IsSpinner() || (d.last->type & kSpinner)) return;
	const double sf = kNormRadius / c.radius;
	const DObj* last = c.Prev(d);
	const DObj* lastLast = c.Prev(d, 1);
	Vec2 lastCursor = last ? EndCursor(*last) : d.last->pos;
	d.jumpDistance = Len(d.last->pos - d.base->pos) * sf;
	d.lazyJumpDistance = Len(d.base->pos - lastCursor) * sf;
	d.minimumJumpDistance = d.lazyJumpDistance;
	if ((d.last->type & kSlider) && last) {
		const double lastTravelTime = std::max(last->lazyTravelTime / d.clockRate, (double)kMinDelta);
		d.minimumJumpTime = std::max(d.adjustedDeltaTime - lastTravelTime, (double)kMinDelta);
		const double tailJump = Len(EndPosition(*d.last) - d.base->pos) * sf;
		d.minimumJumpDistance = std::max(0.0, std::min(d.lazyJumpDistance - (kMaxSliderRadius - kAssumedSliderRadius), tailJump - kMaxSliderRadius));
	}
	if (lastLast && !lastLast->IsSpinner()) {
		if (last->IsSlider() && last->travelDistance > 0) lastCursor = last->base->pos;
		const Vec2 lastLastCursor = EndCursor(*lastLast);
		const double angle = CalcAngle(d.base->pos, lastCursor, lastLastCursor);
		// calculateSliderAngle
		Vec2 sliderLastCursor = EndCursor(*last), sliderLastLast = lastLastCursor;
		if (last->IsSlider() && last->travelDistance > 0) {
			const std::vector<Nested>& n = c.nested[last->base - c.map->objects.data()];
			if (n.size() >= 2) sliderLastLast = n[n.size() - 2].pos;
		}
		const double sliderAngle = CalcAngle(d.base->pos, sliderLastCursor, sliderLastLast);
		const Vec2 v = d.base->pos - lastCursor;
		d.hasNva = true; d.nva = std::atan2(std::fabs(v.y), std::fabs(v.x));
		d.hasAngle = true; d.angle = std::min(angle, sliderAngle);
	}
}

// ---- evaluators ---------------------------------------------------------------------------------
inline double CalcAngleAcuteness(double a) { return Smoothstep(a, Deg(140), Deg(40)); }
inline double CalcAngleWideness(double a) { return Smoothstep(a, Deg(40), Deg(140)); }

inline double VectorAngleRepetition(const Context& c, const DObj& cur, const DObj& prev) {
	if (!cur.hasAngle || !prev.hasAngle) return 1;
	double constantAngleCount = 0;
	for (int index = 0; index < 6; ++index) {
		const DObj* p = c.Prev(cur, index);
		if (!p) break;
		if (std::max(cur.adjustedDeltaTime, p->adjustedDeltaTime) > 1.1 * std::min(cur.adjustedDeltaTime, p->adjustedDeltaTime)) break;
		if (p->hasNva && cur.hasNva) {
			const double diff = std::fabs(cur.nva - p->nva);
			constantAngleCount += std::cos(8 * std::min(Deg(11.25), diff));
		}
	}
	const double vectorRepetition = std::pow(std::min(0.5 / constantAngleCount, 1.0), 2);
	const double stackFactor = Smootherstep(cur.lazyJumpDistance, 0, kNormDiameter);
	const double angleDifferenceAdjusted = std::cos(2 * std::min(Deg(45), std::fabs(cur.angle - prev.angle) * stackFactor));
	const double baseNerf = 1 - 0.15 * CalcAngleAcuteness(prev.angle) * angleDifferenceAdjusted;
	return std::pow(baseNerf + (1 - baseNerf) * vectorRepetition * 0.5 * stackFactor, 2);
}

inline double SnapAim(const Context& c, const DObj& cur, bool withSliders) {
	const DObj* last = c.Prev(cur);
	if (cur.IsSpinner() || cur.index <= 1 || last->IsSpinner()) return 0;
	const DObj* last2 = c.Prev(cur, 2);
	const double radius = kNormRadius, diameter = kNormDiameter;
	const double currDistance = withSliders ? cur.lazyJumpDistance : cur.jumpDistance;
	double currVelocity = currDistance / cur.adjustedDeltaTime;
	if (last->IsSlider() && withSliders)
		currVelocity = std::max(currVelocity, (last->lazyTravelDistance + cur.lazyJumpDistance) / cur.adjustedDeltaTime);
	const double prevDistance = withSliders ? last->lazyJumpDistance : last->jumpDistance;
	const double prevVelocity = prevDistance / last->adjustedDeltaTime;
	double snap = currVelocity;
	snap *= VectorAngleRepetition(c, cur, *last);
	if (cur.hasAngle && last->hasAngle) {
		const double currAngle = cur.angle, lastAngle = last->angle;
		const double velocityInfluence = std::min(currVelocity, prevVelocity);
		double acute = 0;
		if (std::max(cur.adjustedDeltaTime, last->adjustedDeltaTime) < 1.25 * std::min(cur.adjustedDeltaTime, last->adjustedDeltaTime)) {
			acute = CalcAngleAcuteness(currAngle);
			acute *= 0.08 + 0.92 * (1 - std::min(acute, std::pow(CalcAngleAcuteness(lastAngle), 3)));
			acute *= velocityInfluence * Smootherstep(MsToBpm(cur.adjustedDeltaTime, 2), 300, 400) * Smootherstep(currDistance, 0, diameter * 2);
		}
		double wide = CalcAngleWideness(currAngle);
		wide *= 0.25 + 0.75 * (1 - std::min(wide, std::pow(CalcAngleWideness(lastAngle), 3)));
		const double ts = 1.45;
		double wideCurr = currDistance / std::pow(cur.adjustedDeltaTime, ts);
		const double widePrev = prevDistance / std::pow(last->adjustedDeltaTime, ts);
		if (last->IsSlider() && withSliders)
			wideCurr = std::max(wideCurr, (last->lazyTravelDistance + cur.lazyJumpDistance) / std::pow(cur.adjustedDeltaTime, ts));
		wide *= std::min(wideCurr, widePrev);
		if (last2) {
			const double distance = Len(last2->base->pos - last->base->pos);
			if (distance < 1) wide *= 1 - 0.55 * (1 - distance);
		}
		snap += std::max(acute * 2.41, wide * 9.67);
		const double wiggle = velocityInfluence
			* Smootherstep(currDistance, radius, diameter)
			* std::pow(ReverseLerp(currDistance, diameter * 3, diameter), 1.8)
			* Smootherstep(currAngle, Deg(110), Deg(60))
			* Smootherstep(prevDistance, radius, diameter)
			* std::pow(ReverseLerp(prevDistance, diameter * 3, diameter), 1.8)
			* Smootherstep(lastAngle, Deg(110), Deg(60));
		snap += wiggle * 1.02;
	}
	if (std::max(prevVelocity, currVelocity) != 0) {
		if (withSliders) currVelocity = currDistance / cur.adjustedDeltaTime;
		const double distRatio = Smoothstep(std::fabs(prevVelocity - currVelocity) / std::max(prevVelocity, currVelocity), 0, 1);
		const double overlap = std::min(diameter * 1.25 / std::min(cur.adjustedDeltaTime, last->adjustedDeltaTime), std::fabs(prevVelocity - currVelocity));
		double bonus = overlap * distRatio;
		bonus *= std::pow(std::min(cur.adjustedDeltaTime, last->adjustedDeltaTime) / std::max(cur.adjustedDeltaTime, last->adjustedDeltaTime), 2);
		snap += bonus * 0.9;
	}
	if (cur.IsSlider() && withSliders) {
		const double sb = cur.travelDistance / cur.travelTime;
		snap += (sb < 1 ? sb : std::pow(sb, 0.75)) * 1.5;
	}
	snap *= cur.smallCircleBonus;
	snap *= 1 / (1 - std::pow(0.03, std::pow(cur.adjustedDeltaTime / 1000, 0.65)));
	return snap;
}

inline double Agility(const Context& c, const DObj& cur) {
	if (cur.IsSpinner()) return 0;
	const double cap = kNormDiameter * 1.2;
	const DObj* prev = cur.index > 0 ? c.Prev(cur) : nullptr;
	const double distance = (prev ? prev->lazyTravelDistance : 0) + cur.lazyJumpDistance;
	double a = std::min(distance, cap) / cap * 1000 / cur.adjustedDeltaTime;
	a *= std::pow(cur.smallCircleBonus, 1.5);
	a *= 1 / (1 - std::pow(0.2, cur.adjustedDeltaTime / 1000));
	return a;
}

inline double OverlapFactor(const Context& c, const DObj& a, const DObj& b) {
	const double r = c.radius;
	const double distance = Len(a.base->pos - b.base->pos);
	return std::clamp(1 - std::pow(std::max(distance - r, 0.0) / r, 2), 0.0, 1.0);
}

inline double FlowAim(const Context& c, const DObj& cur, bool withSliders) {
	const DObj* last = c.Prev(cur);
	if (cur.IsSpinner() || cur.index <= 1 || last->IsSpinner()) return 0;
	const DObj* lastLast = c.Prev(cur, 1);
	const double currDistance = withSliders ? cur.lazyJumpDistance : cur.jumpDistance;
	const double prevDistance = withSliders ? last->lazyJumpDistance : last->jumpDistance;
	double currVelocity = currDistance / cur.adjustedDeltaTime;
	if (last->IsSlider() && withSliders)
		currVelocity = std::max(currVelocity, (last->lazyTravelDistance + cur.lazyJumpDistance) / cur.adjustedDeltaTime);
	const double prevVelocity = prevDistance / last->adjustedDeltaTime;
	double flow = currVelocity;
	flow *= std::sqrt(cur.smallCircleBonus);
	flow *= 1 + std::min(0.25, std::pow((std::max(cur.adjustedDeltaTime, last->adjustedDeltaTime) - std::min(cur.adjustedDeltaTime, last->adjustedDeltaTime)) / 50, 4));
	if (cur.hasAngle && last->hasAngle) {
		const double angleDifference = std::fabs(cur.angle - last->angle);
		const double adjusted = std::sin(angleDifference / 2) * 180.0;
		const double angularVelocity = adjusted / (cur.adjustedDeltaTime * 0.1);
		flow *= 0.8 + std::sqrt(angularVelocity / 270.0);
	}
	double overlappedNotesWeight = 1;
	if (cur.index > 2) {
		const double o1 = OverlapFactor(c, cur, *last), o2 = OverlapFactor(c, cur, *lastLast), o3 = OverlapFactor(c, *last, *lastLast);
		overlappedNotesWeight = 1 - o1 * o2 * o3;
	}
	if (cur.hasAngle) flow += currVelocity * CalcAngleAcuteness(cur.angle) * overlappedNotesWeight;
	if (std::max(prevVelocity, currVelocity) != 0) {
		if (withSliders) currVelocity = currDistance / cur.adjustedDeltaTime;
		const double distRatio = Smoothstep(std::fabs(prevVelocity - currVelocity) / std::max(prevVelocity, currVelocity), 0, 1);
		const double overlap = std::min(kNormDiameter * 1.25 / std::min(cur.adjustedDeltaTime, last->adjustedDeltaTime), std::fabs(prevVelocity - currVelocity));
		flow += overlap * distRatio * overlappedNotesWeight * 0.52;
	}
	if (cur.IsSlider() && withSliders) flow += cur.travelDistance / cur.travelTime;
	flow = std::pow(flow, 1.45);
	return flow * Smootherstep(currDistance, 0, kNormRadius);
}

inline double SpeedEval(const Context& c, const DObj& cur) {
	if (cur.IsSpinner()) return 0;
	double strainTime = cur.adjustedDeltaTime;
	const double doubleTap = 1.0 - DoubleTapFeasibility(cur, c.Next(cur));
	strainTime /= std::clamp((strainTime / cur.hitWindowGreat) / 0.93, 0.92, 1.0);
	double speedBonus = 0.0;
	if (MsToBpm(strainTime) > 200) speedBonus = 0.75 * std::pow((BpmToMs(200) - strainTime) / 40, 2);
	double s = (1 + speedBonus) * 1000 / strainTime;
	s *= 1 / (1 - std::pow(0.3, cur.adjustedDeltaTime / 1000));
	return s * doubleTap;
}

struct Island {
	int delta = std::numeric_limits<int>::max(), deltaCount = 1, occurrences = 1;
	explicit Island(int d) { delta = std::max(d, kMinDelta); }
	void AddDelta(int d) { if (delta == std::numeric_limits<int>::max()) delta = std::max(d, kMinDelta); deltaCount++; }
	bool IsSimilarPolarity(const Island& o, double eps) const {
		if (deltaCount <= 1 || o.deltaCount <= 1) return false;
		return std::fabs((double)delta - o.delta) < eps && deltaCount % 2 == o.deltaCount % 2;
	}
	bool AlmostEquals(const Island& o, double eps) const { return std::fabs((double)delta - o.delta) < eps && deltaCount == o.deltaCount; }
};
inline double EffectiveDifficulty(double ratio) {
	const double frac = ratio - std::trunc(ratio);
	return 1.0 + 26.0 * std::min(0.5, SmoothstepBellCurve(frac));
}

inline double Rhythm(const Context& c, const DObj& cur) {
	if (cur.IsSpinner()) return 0;
	const int historyTimeMax = 5000, historyObjectsMax = 32;
	double rhythmComplexitySum = 0;
	const double eps = cur.hitWindowGreat * 0.3;
	auto island = std::make_shared<Island>(std::numeric_limits<int>::max());
	auto previousIsland = std::make_shared<Island>(std::numeric_limits<int>::max());
	std::vector<std::shared_ptr<Island>> islands;
	double startDifficulty = 0;
	bool firstDeltaSwitch = false;
	const int historicalNoteCount = std::min(cur.index, historyObjectsMax);
	int rhythmStart = 0;
	while (rhythmStart < historicalNoteCount - 2 && cur.startTime - c.Prev(cur, rhythmStart)->startTime < historyTimeMax) rhythmStart++;
	const DObj* prevObj = c.Prev(cur, rhythmStart);
	const DObj* prevPrevObj = c.Prev(cur, rhythmStart + 1);
	for (int i = rhythmStart; i > 0; --i) {
		const DObj* currObj = c.Prev(cur, i - 1);
		if (currObj->IsSpinner()) continue;
		const double timeDecay = (historyTimeMax - (cur.startTime - currObj->startTime)) / historyTimeMax;
		const double noteDecay = (double)(historicalNoteCount - i) / historicalNoteCount;
		const double decay = std::min(noteDecay, timeDecay);
		const double minD = 1e-7;
		const double currDelta = std::max(currObj->deltaTime, minD), prevDelta = std::max(prevObj->deltaTime, minD);
		const double deltaDifference = std::fabs(prevDelta - currDelta);
		if (island->delta == std::numeric_limits<int>::max()) island = std::make_shared<Island>((int)currDelta);
		const double ratio = std::max(prevDelta, currDelta) / std::min(prevDelta, currDelta);
		const double differenceMultiplier = std::clamp(2.0 - ratio / 8.0, 0.0, 1.0);
		const double windowPenalty = std::clamp((deltaDifference - eps) / eps, 0.0, 1.0);
		double eff = EffectiveDifficulty(ratio) * windowPenalty * differenceMultiplier;
		if (prevObj->IsSlider()) {
			const double lazyEnd = currObj->minimumJumpTime;
			const double lazyRatio = std::max(lazyEnd, currDelta) / std::min(lazyEnd, currDelta);
			const double realEnd = currObj->lastObjectEndDeltaTime;
			const double realRatio = std::max(realEnd, currDelta) / std::min(realEnd, currDelta);
			eff = std::min(std::min(EffectiveDifficulty(lazyRatio), EffectiveDifficulty(realRatio)), eff);
		}
		if (deltaDifference < eps) island->AddDelta((int)currDelta);
		if (firstDeltaSwitch) {
			if (deltaDifference > eps) {
				if (currObj->IsSlider()) eff *= 0.5;
				if (island->IsSimilarPolarity(*previousIsland, eps)) eff *= 0.5;
				if (std::max(prevPrevObj->deltaTime, minD) > prevDelta + eps && prevDelta > currDelta + eps) eff *= 0.125;
				if (previousIsland->deltaCount == island->deltaCount) eff *= 0.5;
				if (prevDelta > currDelta + eps) eff *= 0.65;
				bool found = false;
				for (auto& existing : islands) {
					if (existing->AlmostEquals(*island, eps)) {
						if (previousIsland->AlmostEquals(*island, eps)) existing->occurrences++;
						const double power = Logistic(island->delta, 58.33, 0.24, 2.75);
						eff *= std::min(3.0 / existing->occurrences, std::pow(1.0 / existing->occurrences, power));
						found = true;
						break;
					}
				}
				if (!found && island->deltaCount > 0) islands.push_back(island);
				eff *= 1 - DoubleTapFeasibility(*prevObj, currObj) * 0.75;
				if (island->deltaCount > 1) rhythmComplexitySum += std::sqrt(eff * startDifficulty) * decay;
				else rhythmComplexitySum += 0.7 * decay;
				startDifficulty = eff;
				if (prevDelta + eps < currDelta) firstDeltaSwitch = false;
				previousIsland = island;
				island = std::make_shared<Island>((int)currDelta);
			}
		} else if (prevDelta > currDelta + eps) {
			firstDeltaSwitch = true;
			if (currObj->IsSlider()) eff *= 0.6;
			if (prevObj->IsSlider()) eff *= 0.6;
			startDifficulty = eff;
			island = std::make_shared<Island>((int)currDelta);
		}
		prevPrevObj = prevObj;
		prevObj = currObj;
	}
	rhythmComplexitySum *= ReverseLerp(island->deltaCount, 22, 3);
	return std::sqrt(4 + rhythmComplexitySum * 0.95) / 2.0;
}

// ---- reading ------------------------------------------------------------------------------------
inline double TimeNerf(double dt) { return std::clamp(2 - dt / (3000.0 / 2), 0.0, 1.0); }

inline double PastObjectInfluence(const Context& c, const DObj& cur) {
	double sum = 0;
	for (int i = 0; i < cur.index; ++i) {
		const DObj* h = c.Prev(cur, i);
		if (!h || cur.startTime - h->startTime > 3000 || h->startTime < cur.startTime - cur.preempt) break;
		double d = OpacityAt(c, cur, h->base->time, false);
		d *= Smootherstep(h->lazyJumpDistance, 15, kNormDiameter * 1.5);
		d *= TimeNerf(cur.startTime - h->startTime);
		sum += d;
	}
	return sum;
}

inline double VisibleObjectDensity(const Context& c, const DObj& cur) {
	double count = 0;
	const DObj* h = c.Next(cur);
	while (h) {
		if (h->startTime - cur.startTime > 3000 || cur.startTime < h->startTime - h->preempt) break;
		count += OpacityAt(c, *h, cur.base->time, false) * TimeNerf(h->startTime - cur.startTime);
		h = c.Next(*h);
	}
	return count;
}

inline double ConstantAngleNerf(const Context& c, const DObj& cur) {
	double count = 0;
	int index = 0;
	double gap = 0;
	const DObj* p0 = &cur; const DObj* p1 = nullptr; const DObj* p2 = nullptr;
	while (gap < 2000) {
		const DObj* loop = c.Prev(cur, index);
		if (!loop) break;
		const double longInterval = 1 - ReverseLerp(loop->adjustedDeltaTime, 200, 2000);
		if (loop->hasAngle && cur.hasAngle) {
			const double angleDifference = std::fabs(cur.angle - loop->angle);
			double alternating = 3.14159265358979323846;
			if (p0->hasAngle && p1 && p1->hasAngle && p2 && p2->hasAngle) {
				alternating = std::fabs(p1->angle - loop->angle);
				alternating += std::fabs(p2->angle - p0->angle);
				double weight = 1.0;
				weight *= ReverseLerp(std::min(loop->angle, p0->angle) * 180 / 3.14159265358979323846, 20, 5);
				weight *= ReverseLerp(std::max(loop->angle, p0->angle) * 180 / 3.14159265358979323846, 60, 120);
				alternating = Lerp(3.14159265358979323846, 0.1 * alternating, weight);
			}
			const double stackFactor = Smootherstep(loop->lazyJumpDistance, 0, kNormRadius);
			count += std::cos(3 * std::min(Deg(30), std::min(angleDifference, alternating) * stackFactor)) * longInterval;
		}
		gap = cur.startTime - loop->startTime;
		index++;
		p2 = p1; p1 = p0; p0 = loop;
	}
	return std::clamp(2 / count, 0.2, 1.0);
}

inline double ReadingEval(const Context& c, const DObj& cur, bool hidden) {
	if (cur.IsSpinner() || cur.index == 0) return 0;
	const DObj* next = c.Next(cur);
	const double velocity = std::max(1.0, cur.lazyJumpDistance / cur.adjustedDeltaTime);
	const double density = VisibleObjectDensity(c, cur);
	const double past = PastObjectInfluence(c, cur);
	const double angleNerf = ConstantAngleNerf(c, cur);
	// density
	double future = std::sqrt(density);
	if (next) future *= Smootherstep(next->lazyJumpDistance, 15, kNormDiameter * 1.5);
	double noteDensity = std::pow(past + future, 1.7) * 0.4 * angleNerf * velocity;
	noteDensity = std::max(0.0, noteDensity - 2.5);
	noteDensity = std::pow(noteDensity, 0.45) * 2.4;
	// hidden
	double hiddenDifficulty = 0;
	if (hidden) {
		const double preemptFactor = std::pow(cur.preempt, 2.2) * 0.01;
		const double densityFactor = std::pow(density + past, 3.3) * 3;
		hiddenDifficulty = (preemptFactor + densityFactor) * angleNerf * velocity * 0.01;
		hiddenDifficulty = std::pow(hiddenDifficulty, 0.4) * 0.28;
		const DObj* prev = c.Prev(cur);
		if (cur.lazyJumpDistance == 0 && OpacityAt(c, cur, prev->base->time, true) == 0 && prev->startTime > cur.startTime - cur.preempt)
			hiddenDifficulty += 0.28 * 2500 / std::pow(cur.adjustedDeltaTime, 1.5);
	}
	// preempt
	double preemptDifficulty = std::pow((500 - cur.preempt + std::fabs(cur.preempt - 500)) / 2, 2.5) / 140000;
	preemptDifficulty *= angleNerf * velocity;
	double r = Norm(1.5, { preemptDifficulty, hiddenDifficulty, noteDensity });
	r *= 1 / (1 - std::pow(0.8, cur.adjustedDeltaTime / 1000));
	return r;
}

// ---- flashlight ---------------------------------------------------------------------------------
inline double FlashlightEval(const Context& c, const DObj& cur) {
	if (cur.IsSpinner()) return 0;
	const double scaling = 52.0 / c.radius;
	double smallDistNerf = 1.0, cumulative = 0.0, fl = 0.0, angleRepeatCount = 0.0;
	const DObj* lastObj = &cur;
	for (int i = 0; i < std::min(cur.index, 10); ++i) {
		const DObj* o = c.Prev(cur, i);
		cumulative += lastObj->adjustedDeltaTime;
		if (!o->IsSpinner()) {
			const double jump = Len(cur.base->pos - EndPosition(*o->base));
			if (i == 0) smallDistNerf = std::min(1.0, jump / 75.0);
			const double stackNerf = std::min(1.0, (o->lazyJumpDistance / scaling) / 25.0);
			const double opacityBonus = 1.0 + 0.4 * (1.0 - OpacityAt(c, cur, o->base->time, c.mods.hidden));
			fl += stackNerf * opacityBonus * scaling * jump / cumulative;
			if (o->hasAngle && cur.hasAngle && std::fabs(o->angle - cur.angle) < 0.02) angleRepeatCount += std::max(1.0 - 0.1 * i, 0.0);
		}
		lastObj = o;
	}
	fl = std::pow(smallDistNerf * fl, 2);
	if (c.mods.hidden) fl *= 1.2;
	fl *= 0.2 + 0.8 / (angleRepeatCount + 1.0);
	double sliderBonus = 0.0;
	if (cur.IsSlider()) {
		const double px = cur.lazyTravelDistance / scaling;
		sliderBonus = std::pow(std::max(0.0, px / cur.travelTime - 0.5), 0.5);
		sliderBonus *= px;
		if (cur.base->slides - 1 > 0) sliderBonus /= cur.base->slides;
	}
	return fl + sliderBonus * 1.3;
}

// ---- skills -------------------------------------------------------------------------------------
struct Peak { double value, length; };

// VariableLengthStrainSkill (decay weight 0.9, 400 ms max sections); peaks kept sorted high -> low.
struct VarStrain {
	double decayWeight = 0.9; int maxSection = 400;
	double sectionPeak = 0, sectionBegin = 0, sectionEnd = 0, totalLength = 0;
	double maxStoredLength = 11 / (1 - 0.9);
	std::vector<Peak> peaks;
	std::vector<std::pair<double, double>> queued; // (strain, start time)
	void Insert(Peak p) {
		// List.BinarySearch with a reverse comparer: the first position whose value is not above p.
		auto it = std::lower_bound(peaks.begin(), peaks.end(), p, [](const Peak& a, const Peak& b) { return a.value > b.value; });
		peaks.insert(it, p);
	}
	void Save(double length) {
		Insert({ sectionPeak, RoundEven(length) }); // StrainPeak rounds its length...
		totalLength += length;                      // ...the running total does not (lazer)
		while (totalLength > maxStoredLength * maxSection) { totalLength -= peaks.back().length; peaks.pop_back(); }
	}
	template <class InitialFn, class StrainFn>
	double Process(const DObj& cur, InitialFn initial, StrainFn strainAt) {
		if (cur.index == 0) {
			sectionBegin = cur.startTime; sectionEnd = sectionBegin + maxSection;
			sectionPeak = strainAt();
			return sectionPeak;
		}
		while (cur.startTime > sectionEnd) {
			Save(sectionEnd - sectionBegin);
			sectionBegin = sectionEnd;
			if (!queued.empty()) {
				const auto q = queued.front(); queued.erase(queued.begin());
				sectionEnd = q.second + maxSection;
				sectionPeak = initial(sectionBegin);
				sectionPeak = std::max(sectionPeak, q.first);
			} else {
				sectionEnd = sectionBegin + maxSection;
				sectionPeak = initial(sectionBegin);
			}
		}
		const double s = strainAt();
		if (s > sectionPeak) {
			queued.clear();
			Save(cur.startTime - sectionBegin);
			sectionBegin = cur.startTime; sectionEnd = sectionBegin + maxSection;
			sectionPeak = s;
		} else {
			while (!queued.empty() && queued.back().first < s) queued.pop_back();
			queued.push_back({ s, cur.startTime });
		}
		return s;
	}
	std::vector<Peak> Final() { std::vector<Peak> p = peaks; Peak f{ sectionPeak, RoundEven(sectionEnd - sectionBegin) };
		auto it = std::lower_bound(p.begin(), p.end(), f, [](const Peak& a, const Peak& b) { return a.value > b.value; });
		p.insert(it, f); return p; }
};

// Aim.DifficultyValue over the reduced strain peaks.
inline double AimDifficultyValue(VarStrain& s) {
	std::vector<Peak> strains;
	for (const Peak& p : s.Final()) if (p.value > 0) strains.push_back(p);
	double time = 0; size_t skip = 0;
	while (strains.size() > skip && time < 4000) {
		const Peak strain = strains[skip];
		for (double added = 0; added < strain.length; added += 20) {
			const double scale = std::log10(Lerp(1, 10, std::clamp((time + added) / 4000, 0.0, 1.0)));
			strains.push_back({ strain.value * Lerp(0.727, 1.0, scale), RoundEven(std::min(20.0, strain.length - added)) });
		}
		time += strain.length;
		skip++;
	}
	std::vector<Peak> rest(strains.begin() + skip, strains.end());
	std::stable_sort(rest.begin(), rest.end(), [](const Peak& a, const Peak& b) { return a.value > b.value; });
	double difficulty = 0; time = 0;
	for (const Peak& p : rest) {
		const double start = time, end = time + p.length / 400.0;
		difficulty += p.value * (std::pow(0.9, start) - std::pow(0.9, end));
		time = end;
	}
	return difficulty / (1 - 0.9);
}

inline double HarmonicValue(std::vector<double> d, double harmonicScale, double decayExponent) {
	std::stable_sort(d.begin(), d.end(), [](double a, double b) { return a > b; });
	double difficulty = 0; int index = 0;
	for (double v : d) {
		if (!(v > 0)) continue;
		const double w = (1 + (harmonicScale / (1 + index))) / (std::pow((double)index, decayExponent) + 1 + (harmonicScale / (1 + index)));
		difficulty += v * w;
		index++;
	}
	return difficulty;
}

// ---- the calculator -----------------------------------------------------------------------------
// `map` is the PLAYABLE beatmap: difficulty mods applied to cs / ar / od and ApplyStacking run.
inline double StarRating(const Beatmap& map, const Mods& mods) {
	const auto& H = map.objects;
	if (H.size() < 2) return 0.0;
	Context c;
	c.map = &map; c.mods = mods;
	c.radius = Radius(map.cs);
	c.preemptRaw = Preempt(map.ar);
	c.fadeInRaw = 400.0 * std::min(1.0, c.preemptRaw / 450.0);
	c.fadeInHiddenRaw = mods.hidden ? c.preemptRaw * 0.4 : c.fadeInRaw;
	const double great = std::floor(DifficultyRange(map.od, 80, 50, 20)) - 0.5;
	// Slider nested objects: head, ticks + repeats in time order, tail (not the legacy last tick).
	c.nested.assign(H.size(), {});
	for (size_t i = 0; i < H.size(); ++i) {
		const HitObject& o = H[i];
		if (!(o.type & kSlider)) continue;
		auto& n = c.nested[i];
		n.push_back({ o.time, o.pos, kHead });
		for (size_t k = 0; k < o.checkpoints.size() && k < o.checkpointIsTick.size(); ++k) {
			const bool lastEntry = k + 1 == o.checkpoints.size();
			if (o.checkpointIsTick[k]) n.push_back({ o.checkpoints[k], SliderBall(o, o.checkpoints[k]), kTick });
			else if (!lastEntry) n.push_back({ o.checkpoints[k], SliderBall(o, o.checkpoints[k]), kRepeat });
		}
		n.push_back({ o.endTime, EndPosition(o), kTail });
	}
	const double rate = mods.rate;
	c.objs.reserve(H.size() - 1);
	for (size_t i = 1; i < H.size(); ++i) {
		DObj d;
		d.base = &H[i]; d.last = &H[i - 1];
		d.index = (int)c.objs.size();
		d.clockRate = rate;
		d.deltaTime = (H[i].time - H[i - 1].time) / rate;
		d.startTime = H[i].time / rate;
		const double end = (H[i].type & (kSlider | kSpinner)) ? H[i].endTime : H[i].time;
		d.endTime = end / rate;
		d.hitWindowGreat = (H[i].type & kSpinner) ? 0.0 : 2 * great / rate;
		d.preempt = c.preemptRaw / rate;
		d.smallCircleBonus = std::max(1.0, 1.0 + (30 - c.radius) / 70);
		d.overallDifficulty = (79.5 - d.hitWindowGreat / 2) / 6;
		d.adjustedDeltaTime = std::max(d.deltaTime, (double)kMinDelta);
		c.objs.push_back(d);
		DObj& r = c.objs.back();
		const DObj* prev = c.Prev(r);
		r.lastObjectEndDeltaTime = prev ? std::max(r.startTime - prev->endTime, (double)kMinDelta) : r.adjustedDeltaTime;
		ComputeSliderCursor(c, r);
		SetDistances(c, r);
	}
	// Skills.
	VarStrain aim, aimNoSliders;
	double aimStrain = 0, aimNsStrain = 0, speedStrain = 0, readingStrain = 0, flStrain = 0;
	std::vector<double> speedD, readingD;
	double readingReducedCount = 0; bool haveReducedDuration = false; double reducedDuration = 0;
	// Flashlight: a fixed-section StrainSkill.
	std::vector<double> flPeaks; double flPeak = 0, flSectionEnd = 0;
	auto aimAdjusted = [&](const DObj& cur, bool sliders) {
		const double snap = SnapAim(c, cur, sliders) * 70.9;
		const double agility = Agility(c, cur) * 2.35;
		const double flow = FlowAim(c, cur, sliders) * 242.0;
		const double combinedSnap = Norm(1.2, { snap, agility });
		const double ratio = flow / combinedSnap;
		double pSnap;
		if (ratio == 0) pSnap = 0; else if (std::isnan(ratio)) pSnap = 1; else pSnap = LogisticE(-7.27 * std::log(ratio));
		double total = (combinedSnap * pSnap + flow * (1 - pSnap)) * 1.12;
		total *= 0.985 + std::pow(std::max(0.0, cur.overallDifficulty), 2) / 4000;
		return total;
	};
	for (const DObj& cur : c.objs) {
		// Aim (with and without slider travel).
		for (int pass = 0; pass < 2; ++pass) {
			VarStrain& s = pass == 0 ? aim : aimNoSliders;
			double& strain = pass == 0 ? aimStrain : aimNsStrain;
			s.Process(cur,
				[&](double time) { return strain * std::pow(0.2, (time - c.Prev(cur)->startTime) / 1000); },
				[&]() {
					const double decay = std::pow(0.2, cur.adjustedDeltaTime / 1000);
					strain *= decay;
					strain += aimAdjusted(cur, pass == 0) * (1 - decay);
					return strain;
				});
		}
		// Speed.
		{
			const double decay = std::pow(0.3, cur.adjustedDeltaTime / 1000);
			speedStrain *= decay;
			speedStrain += SpeedEval(c, cur) * (1 - decay) * 1.16;
			speedD.push_back(speedStrain * Rhythm(c, cur));
		}
		// Reading.
		{
			const double decay = std::pow(0.8, cur.deltaTime / 1000);
			readingStrain *= decay;
			double r = ReadingEval(c, cur, mods.hidden);
			r *= 0.825 + std::pow(std::max(0.0, cur.overallDifficulty), 2.2) / 1125.0;
			readingStrain += r * (1 - decay) * 2.5;
			if (!haveReducedDuration) { reducedDuration = cur.startTime + 60000; haveReducedDuration = true; }
			if (cur.startTime <= reducedDuration) readingReducedCount++;
			readingD.push_back(readingStrain);
		}
		// Flashlight.
		if (mods.flashlight) {
			if (cur.index == 0) flSectionEnd = std::ceil(cur.startTime / 400) * 400;
			while (cur.startTime > flSectionEnd) {
				flPeaks.push_back(flPeak);
				flPeak = flStrain * std::pow(0.15, (flSectionEnd - c.Prev(cur)->startTime) / 1000);
				flSectionEnd += 400;
			}
			flStrain *= std::pow(0.15, cur.deltaTime / 1000);
			double f = FlashlightEval(c, cur);
			f *= 0.985 + std::pow(std::max(0.0, cur.overallDifficulty), 2) / 4000;
			flStrain += f * 0.058;
			flPeak = std::max(flPeak, flStrain);
		}
	}
	const double aimValue = AimDifficultyValue(aim);
	const double speedValue = HarmonicValue(speedD, 20, 0.9);
	// Reading: the first minute is treated as memorised, ramping in.
	std::vector<double> readingT;
	for (double v : readingD) if (v > 0) readingT.push_back(v);
	for (int i = 0; i < (int)std::min((double)readingT.size(), readingReducedCount); ++i) {
		const double scale = std::log10(Lerp(1, 10, std::clamp(i / readingReducedCount, 0.0, 1.0)));
		readingT[i] *= Lerp(0.0, 1.0, scale);
	}
	const double readingValue = HarmonicValue(readingT, 1.0, 0.9);
	double flValue = 0;
	if (mods.flashlight) {
		flPeaks.push_back(flPeak);
		for (double p : flPeaks) flValue += p;
		const double total = (double)H.size();
		flValue *= 0.7 + 0.1 * std::min(1.0, total / 200.0) + (total > 200 ? 0.2 * std::min(1.0, (total - 200) / 200.0) : 0.0);
	}
	const double aimRating = std::pow(aimValue, 0.63) * 0.02275;
	const double speedRating = std::sqrt(speedValue) * 0.0675;
	const double readingRating = std::sqrt(readingValue) * 0.0675;
	const double flRating = mods.flashlight ? std::sqrt(flValue) * 0.0675 : 0.0;
	const double aimPerf = 4.0 * std::pow(aimRating, 3), speedPerf = 4.0 * std::pow(speedRating, 3);
	const double readingPerf = 4.0 * std::pow(readingRating, 3), flPerf = 25 * std::pow(flRating, 2);
	double cognition;
	if (readingPerf <= 0) cognition = flPerf;
	else if (flPerf <= 0) cognition = readingPerf;
	else cognition = Norm(1.1, { readingPerf, flPerf * std::clamp(flPerf / readingPerf, 0.25, 1.0) });
	const double basePerf = Norm(1.1, { aimPerf, speedPerf, cognition });
	return std::cbrt(basePerf * 1.12);
}

} // namespace Diff
} // namespace Osu
} // namespace Filmmaker
