#pragma once

// World-positioned sound for the easter eggs (UwU's bang and pops): how loud and which side, from
// the camera (the listener) to the player who made the sound. Engine-free so
// Automation/tests/easter-egg-audio-math-tests.cpp can pin it.
//
// Like CS2's own falloff: full volume close by, then an inverse-distance roll-off that is gone by
// `maxDist` -- a death across the map is silent, one around the corner is quiet, one next to you
// is loud. Direction is constant-power stereo panning from the source's bearing in the camera's
// horizontal plane, with sources behind the listener a little duller, like CS2's HRTF.
//
// Coordinates are Source units (x forward at yaw 0, y left, z up); angles are pitch, yaw, roll in
// degrees as the engine stores them.

#include <algorithm>
#include <cstdint>
#include <vector>
#include <cmath>

namespace Filmmaker {
namespace EggAudio {

struct Spatial {
	double gain = 0.0;  // 0..1
	double left = 0.0;  // per-channel gain (gain already applied)
	double right = 0.0;
};

// Loudness at distance d: 1 inside refDist, inverse-distance beyond it, faded to 0 at maxDist.
inline double DistanceGain(double d, double refDist, double maxDist) {
	if (!(d > refDist)) return 1.0;
	if (d >= maxDist) return 0.0;
	const double inv = refDist / d;                                  // inverse distance
	const double edge = 1.0 - (d - refDist) / (maxDist - refDist);   // fades the tail to silence
	return std::clamp(inv * edge, 0.0, 1.0);
}

// Pan -1 (hard left) .. +1 (hard right) and how much the source is behind (0 ahead .. 1 behind).
inline void Bearing(const double listener[3], double yawDeg, const double source[3], double& pan, double& behind) {
	const double kDeg = 3.14159265358979323846 / 180.0;
	const double dx = source[0] - listener[0], dy = source[1] - listener[1];
	const double len = std::sqrt(dx * dx + dy * dy);
	if (len < 1e-3) { pan = 0.0; behind = 0.0; return; } // on top of the listener: centred
	const double fx = std::cos(yawDeg * kDeg), fy = std::sin(yawDeg * kDeg);
	// Source's +y is LEFT, so the listener's right is (fy, -fx).
	const double rx = fy, ry = -fx;
	pan = std::clamp((dx * rx + dy * ry) / len, -1.0, 1.0);
	const double ahead = (dx * fx + dy * fy) / len;
	behind = std::clamp(-ahead, 0.0, 1.0);
}

inline Spatial Spatialize(const double listener[3], double yawDeg, const double source[3], double refDist, double maxDist) {
	Spatial s;
	const double dx = source[0] - listener[0], dy = source[1] - listener[1], dz = source[2] - listener[2];
	const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
	double pan = 0.0, behind = 0.0;
	Bearing(listener, yawDeg, source, pan, behind);
	s.gain = DistanceGain(d, refDist, maxDist) * (1.0 - 0.25 * behind);
	// Constant power: equal loudness as a sound sweeps across.
	const double a = (pan + 1.0) * 0.25 * 3.14159265358979323846;
	s.left = s.gain * std::cos(a);
	s.right = s.gain * std::sin(a);
	return s;
}

// ---- the beat of a song (UwU's vignette pulses with its music) ---------------------------------
// Beat strength every `hop` samples: the energy of the low end (a one-pole low-pass at ~150 Hz,
// where the kick lives) above its own recent average, so a hit stands out from a loud passage;
// normalised so the strong beats of the song reach ~1. `pcm` is interleaved 16-bit, `channels`.
inline std::vector<float> BeatEnvelope(const int16_t* pcm, size_t frames, int channels, int rate, int hop) {
	std::vector<float> energy;
	if (!pcm || frames == 0 || channels <= 0 || hop <= 0) return energy;
	const double a = std::exp(-2.0 * 3.14159265358979323846 * 150.0 / rate);
	double lp = 0.0, acc = 0.0;
	int n = 0;
	for (size_t f = 0; f < frames; ++f) {
		double s = 0.0;
		for (int c = 0; c < channels; ++c) s += pcm[f * channels + c];
		s /= (32768.0 * channels);
		lp = a * lp + (1.0 - a) * s;
		acc += lp * lp;
		if (++n == hop) { energy.push_back((float)std::sqrt(acc / hop)); acc = 0.0; n = 0; }
	}
	// Onset: above the average of the previous ~0.3 s.
	const int win = std::max(1, (int)(0.3 * rate / hop));
	std::vector<float> onset(energy.size(), 0.0f);
	double sum = 0.0;
	for (size_t i = 0; i < energy.size(); ++i) {
		const double avg = i > 0 ? sum / std::min<size_t>(i, (size_t)win) : energy[i];
		onset[i] = (float)std::max(0.0, energy[i] - avg * 1.15);
		sum += energy[i];
		if (i >= (size_t)win) sum -= energy[i - win];
	}
	std::vector<float> sorted(onset);
	std::sort(sorted.begin(), sorted.end());
	const float ref = sorted.empty() ? 0.0f : sorted[(size_t)(sorted.size() * 0.97)];
	if (ref > 0.0f) for (float& v : onset) v = std::min(1.0f, v / ref);
	return onset;
}

// Tempo from a beat envelope: the strongest autocorrelation lag between 60 and 200 BPM.
inline double EstimateBpm(const std::vector<float>& env, double hopSeconds) {
	if (env.size() < 16 || !(hopSeconds > 0.0)) return 0.0;
	const int minLag = std::max(1, (int)std::floor(60.0 / 200.0 / hopSeconds));
	const int maxLag = (int)std::ceil(60.0 / 60.0 / hopSeconds);
	double best = -1.0;
	int bestLag = 0;
	for (int lag = minLag; lag <= maxLag && lag < (int)env.size(); ++lag) {
		double c = 0.0;
		for (size_t i = (size_t)lag; i < env.size(); ++i) c += (double)env[i] * env[i - lag];
		c /= (double)(env.size() - lag);
		if (c > best) { best = c; bestLag = lag; }
	}
	return bestLag > 0 ? 60.0 / (bestLag * hopSeconds) : 0.0;
}

} // namespace EggAudio
} // namespace Filmmaker
