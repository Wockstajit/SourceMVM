// EasterEggAudioMath.h: distance falloff and stereo bearing for the easter eggs' world sounds.
#include "../../AfxHookSource2/Filmmaker/EasterEggs/EasterEggAudioMath.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace Filmmaker::EggAudio;

namespace {
int g_failed = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAILED line %d: %s\n", __LINE__, #c); ++g_failed; } } while (0)
bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

void TestDistance() {
	CHECK(Near(DistanceGain(0, 250, 2000), 1.0));
	CHECK(Near(DistanceGain(250, 250, 2000), 1.0));
	CHECK(DistanceGain(500, 250, 2000) < 0.5 && DistanceGain(500, 250, 2000) > 0.3);
	CHECK(DistanceGain(1000, 250, 2000) < DistanceGain(500, 250, 2000));
	CHECK(Near(DistanceGain(2000, 250, 2000), 0.0) && Near(DistanceGain(5000, 250, 2000), 0.0)); // across the map: silent
}

void TestBearing() {
	const double me[3] = { 0, 0, 0 };
	const double right[3] = { 0, -500, 0 }, left[3] = { 0, 500, 0 }, ahead[3] = { 500, 0, 0 }, behindP[3] = { -500, 0, 0 };
	double pan, behind;
	Bearing(me, 0, right, pan, behind);  CHECK(Near(pan, 1.0) && Near(behind, 0.0));   // yaw 0 faces +x; -y is right
	Bearing(me, 0, left, pan, behind);   CHECK(Near(pan, -1.0));
	Bearing(me, 0, ahead, pan, behind);  CHECK(Near(pan, 0.0) && Near(behind, 0.0));
	Bearing(me, 0, behindP, pan, behind); CHECK(Near(pan, 0.0) && Near(behind, 1.0));
	Bearing(me, 90, ahead, pan, behind); CHECK(Near(pan, 1.0));                         // turned left 90: +x is now on the right
	const Spatial s = Spatialize(me, 0, right, 250, 2000);
	CHECK(s.right > 0.3 && s.left < 1e-6);
	const Spatial c = Spatialize(me, 0, me, 250, 2000);                                  // your own shot: loud, centred
	CHECK(Near(c.left, c.right) && Near(c.left * c.left + c.right * c.right, 1.0));
	const Spatial b = Spatialize(me, 0, behindP, 250, 2000), f = Spatialize(me, 0, ahead, 250, 2000);
	CHECK(b.gain < f.gain);                                                              // behind is duller
}
void TestBeat() {
	// 8 s of a 60 Hz kick thump every 0.5 s (120 BPM) over quiet high hiss.
	const int rate = 44100, frames = rate * 8;
	std::vector<int16_t> pcm((size_t)frames * 2);
	uint32_t seed = 1;
	for (int f = 0; f < frames; ++f) {
		const double t = (double)f / rate, phase = std::fmod(t, 0.5);
		double s = phase < 0.12 ? 0.8 * std::sin(2 * 3.14159265 * 60 * t) * std::exp(-phase * 25) : 0.0;
		seed = seed * 1664525u + 1013904223u;
		s += 0.03 * (((seed >> 9) & 0xFFFF) / 32768.0 - 1.0);
		pcm[2 * f] = pcm[2 * f + 1] = (int16_t)(s * 32767);
	}
	const int hop = rate / 100; // 10 ms
	const std::vector<float> env = BeatEnvelope(pcm.data(), (size_t)frames, 2, rate, hop);
	CHECK(env.size() == 800);
	// Strong right after each kick, quiet between kicks.
	CHECK(env[300 + 1] > 0.5f || env[300 + 2] > 0.5f);   // the kick at 3.0 s
	CHECK(env[325] < 0.2f);                               // 3.25 s: between kicks
	const double bpm = EstimateBpm(env, 0.01);
	CHECK(std::fabs(bpm - 120.0) < 3.0 || std::fabs(bpm - 60.0) < 2.0);
}
} // namespace

int main() {
	TestDistance();
	TestBearing();
	TestBeat();
	if (g_failed) { std::printf("%d check(s) failed\n", g_failed); return 1; }
	std::printf("EasterEggAudioMathTests passed\n");
	return 0;
}
