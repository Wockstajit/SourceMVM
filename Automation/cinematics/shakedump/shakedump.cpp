// shakedump: the camera-path shake (CamPathShake) evaluated OFFLINE with the game's own RAGE shake
// code, so shotplan.py can preview, measure and check a shaken shot before it is placed in game.
//
// Built from the hook's RageShake.cpp + RageShakeStyles.cpp (pure math, compiled in unchanged).
// The per-keyframe walk below mirrors CamPathShake_EvalSnapshot: owning keyframe, run start over
// consecutive same-style keys, intensity/speed glide to the next same-style key, and the 0.35 s
// handover of a different style ending. Keep it in step with that function.
//
//   shakedump styles                         -> "index<TAB>name<TAB>label<TAB>menuIndex" per style
//   shakedump eval <keys> <t0> <t1> <step>   -> per tick: tick right fwd up wx wy wz pitch yaw roll fov
//     keys = "tick:style:intensity:speed,tick:style:intensity:speed,..."  (style = index or name)
#include "../../../AfxHookSource2/Filmmaker/Movie/RageShake.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace Filmmaker;

namespace {
struct Key { int tick; int style; int intensity; int speed; bool Active() const { return style >= 0 && intensity > 0; } };

double AmplitudeFor(double I) { return 0.024 * I * I + 0.076 * I; }   // CamPathShake.cpp
double TimeScaleFor(double speed) { return speed / 50.0; }             // CamPathShake.cpp
constexpr double kHandoverSec = 0.35;                                  // CamPathShake.cpp
double Lerp(double a, double b, double u) { return a + (b - a) * u; }
double Clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

RageShakePose EvalStyle(int style, double intensity, double speed, double tSec) {
	if (style < 0 || intensity <= 0.0 || tSec < 0.0) return RageShakePose();
	return RageShake_Eval(RageShake_Style(style), tSec, AmplitudeFor(intensity), TimeScaleFor(speed));
}

bool Eval(const std::vector<Key>& keys, double spt, double lookupTick, RageShakePose& sum) {
	const int n = (int)keys.size();
	if (n == 0 || lookupTick < (double)keys[0].tick) return false;
	int seg = 0;
	for (int i = 0; i < n; ++i) { if ((double)keys[i].tick <= lookupTick) seg = i; else break; }
	const double now = lookupTick * spt;
	auto findAt = [&](int i) -> const Key* { if (i < 0 || i >= n) return nullptr; return keys[i].Active() ? &keys[i] : nullptr; };
	auto runStartOf = [&](int i) {
		const Key* c = findAt(i);
		if (!c) return i;
		if (RageShake_Finished(RageShake_Style(c->style), 1.0e6, 1.0)) return i;
		while (i > 0) { const Key* prev = findAt(i - 1); if (!prev || prev->style != c->style) break; --i; }
		return i;
	};
	const Key* cur = findAt(seg);
	if (cur) {
		double intensity = cur->intensity, speed = cur->speed;
		const Key* nxt = findAt(seg + 1);
		if (nxt && nxt->style == cur->style) {
			const double span = (double)(keys[seg + 1].tick - keys[seg].tick);
			const double u = span > 0.0 ? Clamp01((lookupTick - keys[seg].tick) / span) : 0.0;
			intensity = Lerp(intensity, nxt->intensity, u);
			speed = Lerp(speed, nxt->speed, u);
		}
		sum.AddScaled(EvalStyle(cur->style, intensity, speed, now - keys[runStartOf(seg)].tick * spt), 1.0);
	}
	if (seg > 0) {
		const double since = now - keys[seg].tick * spt;
		if (since >= 0.0 && since < kHandoverSec) {
			const Key* prev = findAt(seg - 1);
			if (prev && (!cur || prev->style != cur->style)) {
				const double w = 1.0 - since / kHandoverSec;
				sum.AddScaled(EvalStyle(prev->style, prev->intensity, prev->speed, now - keys[runStartOf(seg - 1)].tick * spt), w);
			}
		}
	}
	return sum.Any();
}

int ParseStyle(const char* s) {
	char* end = nullptr;
	long v = std::strtol(s, &end, 10);
	if (end && *end == 0) return (int)v;
	return RageShake_FindStyle(s);
}
} // namespace

int main(int argc, char** argv) {
	if (argc >= 2 && 0 == std::strcmp(argv[1], "styles")) {
		for (int i = 0; i < RageShake_StyleCount(); ++i)
			std::printf("%d\t%s\t%s\t%d\n", i, RageShake_StyleName(i), RageShake_StyleLabel(i), RageShake_MenuIndexOfStyle(i));
		return 0;
	}
	if (argc >= 6 && 0 == std::strcmp(argv[1], "eval")) {
		std::vector<Key> keys;
		std::string spec = argv[2];
		size_t at = 0;
		while (at < spec.size()) {
			size_t comma = spec.find(',', at);
			std::string item = spec.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
			at = comma == std::string::npos ? spec.size() : comma + 1;
			char style[128] = {0};
			Key k{0, -1, 5, 50};
			if (std::sscanf(item.c_str(), "%d:%127[^:]:%d:%d", &k.tick, style, &k.intensity, &k.speed) >= 2) {
				k.style = ParseStyle(style);
				keys.push_back(k);
			}
		}
		const double t0 = std::atof(argv[3]), t1 = std::atof(argv[4]), step = std::atof(argv[5]);
		for (double t = t0; t <= t1 + 1e-9; t += step) {
			RageShakePose p;
			Eval(keys, 1.0 / 64.0, t, p);
			std::printf("%.4f %.5f %.5f %.5f %.5f %.5f %.5f %.6f %.6f %.6f %.6f\n", t,
				p.right, p.fwd, p.up, p.worldX, p.worldY, p.worldZ, p.pitch, p.yaw, p.roll, p.fov);
		}
		return 0;
	}
	std::fprintf(stderr, "usage: shakedump styles | shakedump eval <tick:style:int:speed,...> <t0> <t1> <step>\n");
	return 2;
}
