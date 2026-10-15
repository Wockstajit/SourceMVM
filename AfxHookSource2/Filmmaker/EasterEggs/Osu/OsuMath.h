#pragma once

// osu! (standard mode) beatmap parsing and gameplay math for the "osu" easter egg.
//
// Engine-free and header-only so Automation/tests/osu-math-tests.cpp can pin it. Gameplay values
// match osu!'s own rules (osu.Game.Rulesets.Osu: OsuHitObject, OsuHitWindows, SliderEventGenerator,
// OsuBeatmapProcessor stacking, OsuPlayfieldAdjustmentContainer) and the published .osu format:
//   approach preempt  AR<5: 1200+600*(5-AR)/5   AR=5: 1200   AR>5: 1200-750*(AR-5)/5  (ms, truncated)
//   fade-in           400 * min(1, preempt / 450)                                    (ms)
//   circle radius     64 * (1 - 0.7*(CS-5)/5) / 2 * 1.00041                          (osu px)
//   hit windows       floor(80-6*OD)-0.5  floor(140-8*OD)-0.5  floor(200-10*OD)-0.5  (+-ms)
//                     an earlier press within 400 ms is a miss, earlier still is ignored
//   slider span       length / (100 * SliderMultiplier * SV) * beatLength            (ms)
//                     SV = -100 / beatLength of the active inherited timing point
// Coordinates are osu! pixels on the 512x384 playfield.

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace Filmmaker {
namespace Osu {

struct Vec2 { double x = 0.0, y = 0.0; };
inline Vec2 operator+(Vec2 a, Vec2 b) { return { a.x + b.x, a.y + b.y }; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return { a.x - b.x, a.y - b.y }; }
inline Vec2 operator*(Vec2 a, double s) { return { a.x * s, a.y * s }; }
inline double Len(Vec2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }

enum : int { kCircle = 1, kSlider = 2, kNewCombo = 4, kSpinner = 8 };

struct TimingPoint {
	double time = 0.0, beatLength = 500.0; bool uninherited = true;
	int sampleSet = 0, sampleIndex = 0, volume = 100; // 0 = the beatmap's default set
};

struct HitObject {
	int type = kCircle;
	Vec2 pos;
	double time = 0.0, endTime = 0.0;
	bool newCombo = false;
	int comboSkip = 0;
	int comboNumber = 1, comboIndex = 0;
	// slider
	char curve = 'L';
	std::vector<Vec2> control; // control points AFTER the head (the head is `pos`)
	int slides = 1;
	double length = 0.0;
	double span = 0.0;         // ms for one pass
	double velocity = 0.0;     // osu px per ms
	std::vector<Vec2> path;    // resampled every kPathStep px along the (truncated) curve
	std::vector<double> checkpoints; // slider tick / repeat / end times (ms)
	std::vector<bool> checkpointIsTick; // parallel: a plain tick (else a repeat or the end)
	int stack = 0;               // osu! stack height (positions already include the offset)
	// hitsounds: additions bitmask (2 whistle, 4 finish, 8 clap) and per-object overrides
	int sound = 0, normalSet = 0, addSet = 0, index = 0, volume = 0;
	std::string file;
	std::vector<int> edgeSounds;                 // slider: one per node (head, repeats, tail)
	std::vector<std::pair<int, int>> edgeSets;   // slider: (normal set, addition set) per node
};

struct Beatmap {
	std::string audio, title, artist, creator, version, background, source, tags;
	double leadIn = 0.0, preview = -1.0;
	int mode = 0;
	long long beatmapId = -1, setId = -1;
	std::vector<std::pair<double, double>> breaks; // [start, end] ms
	double hp = 5.0, cs = 5.0, od = 5.0, ar = -1.0, sliderMult = 1.4, tickRate = 1.0;
	double stackLeniency = 0.7;
	int formatVersion = 14, sampleSet = 1; // 1 normal, 2 soft, 3 drum
	std::vector<TimingPoint> timing;
	std::vector<HitObject> objects;
	std::vector<int> colours; // 0xRRGGBB, combo colours
};

constexpr double kPathStep = 6.0; // resample spacing of slider paths, osu px

// osu!'s DifficultyRange(value, min, mid, max) for a 0..10 difficulty value.
inline double DifficultyRange(double v, double at0, double at5, double at10) {
	if (v > 5.0) return at5 + (at10 - at5) * (v - 5.0) / 5.0;
	if (v < 5.0) return at5 + (at0 - at5) * (5.0 - v) / 5.0;
	return at5;
}
inline double Preempt(double ar) { return std::trunc(DifficultyRange(ar, 1800.0, 1200.0, 450.0)); }
inline double FadeIn(double ar) { return 400.0 * std::min(1.0, Preempt(ar) / 450.0); }
inline double Radius(double cs) { return 64.0 * (1.0 - 0.7 * (cs - 5.0) / 5.0) / 2.0 * 1.00041; }

struct Windows { double w300, w100, w50; };
constexpr double kMissWindow = 400.0;
inline Windows HitWindows(double od) {
	return { std::floor(DifficultyRange(od, 80, 50, 20)) - 0.5, std::floor(DifficultyRange(od, 140, 100, 60)) - 0.5,
		std::floor(DifficultyRange(od, 200, 150, 100)) - 0.5 };
}

// 300 / 100 / 50 for a press `delta` ms from the object time; 0 = a miss (within 400 ms but
// outside the 50 window); -1 = too early to count at all (the circle just shakes).
inline int Judge(double delta, const Windows& w) {
	const double d = std::fabs(delta);
	if (d <= w.w300) return 300;
	if (d <= w.w100) return 100;
	if (d <= w.w50) return 50;
	if (d <= kMissWindow) return 0;
	return -1;
}

inline double Accuracy(int n300, int n100, int n50, int miss) {
	const int total = n300 + n100 + n50 + miss;
	if (total <= 0) return 1.0;
	return (300.0 * n300 + 100.0 * n100 + 50.0 * n50) / (300.0 * total);
}

// The familiar ranking: SS perfect; S >90% 300s, <=1% 50s, no misses; A >80% 300s and no misses
// or >90%; B >70% and no misses or >80%; C >60%; D below.
inline const char* Grade(int n300, int n100, int n50, int miss) {
	const int total = n300 + n100 + n50 + miss;
	if (total <= 0) return "D";
	const double p300 = (double)n300 / total, p50 = (double)n50 / total;
	if (n300 == total) return "SS";
	if (p300 > 0.9 && p50 <= 0.01 && miss == 0) return "S";
	if ((p300 > 0.8 && miss == 0) || p300 > 0.9) return "A";
	if ((p300 > 0.7 && miss == 0) || p300 > 0.8) return "B";
	if (p300 > 0.6) return "C";
	return "D";
}

// Spinners (osu.Game.Rulesets.Osu Spinner): full spins needed to clear (90 / 150 / 225 RPM at OD
// 0 / 5 / 10) and bonus spins on top (up to 250 / 380 / 430 RPM, after a 2-spin gap). Each full
// spin judges one tick: the first required+2 heal 0.0085, the rest 0.01.
inline int SpinsRequired(double durationMs, double od) {
	return (int)(DifficultyRange(od, 90.0, 150.0, 225.0) / 60.0 * durationMs / 1000.0 + 0.0001);
}
inline int SpinnerTicks(double durationMs, double od) {
	const int req = SpinsRequired(durationMs, od);
	const int bonus = std::max(0, (int)(DifficultyRange(od, 250.0, 380.0, 430.0) / 60.0 * durationMs / 1000.0 + 0.0001) - req - 2);
	return req + 2 + bonus;
}
// Spinner result from progress = spins / required (1 when none are required).
inline int SpinnerJudge(double progress) {
	return progress >= 1.0 ? 300 : progress > 0.9 ? 100 : progress > 0.75 ? 50 : 0;
}

// ---- parsing ------------------------------------------------------------------------------------

inline std::string Trim(const std::string& s) {
	size_t a = 0, b = s.size();
	while (a < b && (unsigned char)s[a] <= ' ') ++a;
	while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
	return s.substr(a, b - a);
}
inline std::vector<std::string> Split(const std::string& s, char sep) {
	std::vector<std::string> out;
	size_t start = 0;
	for (size_t i = 0; i <= s.size(); ++i)
		if (i == s.size() || s[i] == sep) { out.push_back(s.substr(start, i - start)); start = i + 1; }
	return out;
}
inline double Num(const std::string& s, double d = 0.0) {
	char* end = nullptr;
	const std::string t = Trim(s);
	const double v = std::strtod(t.c_str(), &end);
	return end && end != t.c_str() && std::isfinite(v) ? v : d;
}

// The active uninherited beat length and inherited slider-velocity multiplier at `time`.
inline void TimingAt(const Beatmap& b, double time, double& beatLength, double& sv) {
	beatLength = 500.0; sv = 1.0;
	bool haveBeat = false;
	for (const TimingPoint& tp : b.timing) {
		if (tp.time > time + 0.5) {
			if (!haveBeat && tp.uninherited) { beatLength = tp.beatLength; haveBeat = true; }
			break;
		}
		if (tp.uninherited) { beatLength = tp.beatLength; sv = 1.0; haveBeat = true; }
		else if (tp.beatLength < 0.0) sv = std::clamp(-100.0 / tp.beatLength, 0.1, 10.0);
	}
}

// ---- slider curves ------------------------------------------------------------------------------

inline void AppendBezier(const std::vector<Vec2>& pts, std::vector<Vec2>& out) {
	if (pts.size() < 2) { if (!pts.empty()) out.push_back(pts[0]); return; }
	double approx = 0.0;
	for (size_t i = 1; i < pts.size(); ++i) approx += Len(pts[i] - pts[i - 1]);
	const int n = std::clamp((int)(approx / 3.0), 8, 400);
	std::vector<Vec2> tmp(pts.size());
	for (int s = 0; s <= n; ++s) {
		const double t = (double)s / n;
		tmp = pts;
		for (size_t k = tmp.size() - 1; k > 0; --k)
			for (size_t i = 0; i < k; ++i) tmp[i] = tmp[i] * (1.0 - t) + tmp[i + 1] * t;
		out.push_back(tmp[0]);
	}
}

inline bool AppendCircle(Vec2 a, Vec2 b, Vec2 c, std::vector<Vec2>& out) {
	const double d = 2.0 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
	if (std::fabs(d) < 1e-6) return false; // collinear: not a circle
	const double a2 = a.x * a.x + a.y * a.y, b2 = b.x * b.x + b.y * b.y, c2 = c.x * c.x + c.y * c.y;
	const Vec2 center{ (a2 * (b.y - c.y) + b2 * (c.y - a.y) + c2 * (a.y - b.y)) / d,
		(a2 * (c.x - b.x) + b2 * (a.x - c.x) + c2 * (b.x - a.x)) / d };
	const double r = Len(a - center);
	const double t0 = std::atan2(a.y - center.y, a.x - center.x);
	double t1 = std::atan2(b.y - center.y, b.x - center.x);
	double t2 = std::atan2(c.y - center.y, c.x - center.x);
	const double kTwoPi = 6.283185307179586;
	// Go from a to c THROUGH b: pick the direction in which b lies between them.
	auto norm = [&](double v) { while (v < 0) v += kTwoPi; while (v >= kTwoPi) v -= kTwoPi; return v; };
	double sweep = norm(t2 - t0);
	if (norm(t1 - t0) > sweep) sweep -= kTwoPi;
	const int n = std::clamp((int)(std::fabs(sweep) * r / 3.0), 8, 400);
	for (int s = 0; s <= n; ++s) {
		const double t = t0 + sweep * s / n;
		out.push_back({ center.x + r * std::cos(t), center.y + r * std::sin(t) });
	}
	return true;
}

inline void AppendCatmull(const std::vector<Vec2>& p, std::vector<Vec2>& out) {
	for (size_t i = 0; i + 1 < p.size(); ++i) {
		const Vec2 p0 = i > 0 ? p[i - 1] : p[i], p1 = p[i], p2 = p[i + 1];
		const Vec2 p3 = i + 2 < p.size() ? p[i + 2] : p2 + (p2 - p1);
		for (int s = 0; s <= 24; ++s) {
			const double t = s / 24.0, t2 = t * t, t3 = t2 * t;
			out.push_back((p1 * 2.0 + (p2 - p0) * t + (p0 * 2.0 - p1 * 5.0 + p2 * 4.0 - p3) * t2
				+ (p1 * 3.0 - p0 - p2 * 3.0 + p3) * t3) * 0.5);
		}
	}
}

// Raw polyline through the slider's control points, by curve type.
inline std::vector<Vec2> RawCurve(const HitObject& o) {
	std::vector<Vec2> pts;
	pts.push_back(o.pos);
	for (const Vec2& c : o.control) pts.push_back(c);
	std::vector<Vec2> out;
	if (pts.size() < 2) { out = pts; return out; }
	if (o.curve == 'P' && pts.size() == 3 && AppendCircle(pts[0], pts[1], pts[2], out)) return out;
	if (o.curve == 'C') { AppendCatmull(pts, out); return out; }
	if (o.curve == 'L') return pts;
	// Bezier (and P fallback): a repeated point ends one segment and starts the next.
	std::vector<Vec2> seg;
	for (size_t i = 0; i < pts.size(); ++i) {
		seg.push_back(pts[i]);
		const bool split = i + 1 < pts.size() && std::fabs(pts[i + 1].x - pts[i].x) < 1e-9 && std::fabs(pts[i + 1].y - pts[i].y) < 1e-9;
		if (split || i + 1 == pts.size()) { AppendBezier(seg, out); seg.clear(); if (split) ++i, seg.push_back(pts[i]); }
	}
	return out;
}

// The curve resampled every kPathStep px, cut (or extended in its last direction) to `length`.
inline std::vector<Vec2> ResamplePath(const std::vector<Vec2>& raw, double length) {
	std::vector<Vec2> out;
	if (raw.empty()) return out;
	out.push_back(raw[0]);
	if (!(length > 0.0)) return out;
	double travelled = 0.0, nextAt = kPathStep;
	Vec2 lastDir{ 1.0, 0.0 };
	for (size_t i = 1; i < raw.size() && travelled < length; ++i) {
		const Vec2 a = raw[i - 1], b = raw[i];
		const double seg = Len(b - a);
		if (seg < 1e-9) continue;
		lastDir = (b - a) * (1.0 / seg);
		while (nextAt <= travelled + seg && nextAt <= length) {
			out.push_back(a + lastDir * (nextAt - travelled));
			nextAt += kPathStep;
		}
		travelled += seg;
	}
	// Shorter than the declared length: carry on straight (osu! does the same).
	const Vec2 end = raw.back();
	while (nextAt <= length) { out.push_back(end + lastDir * (nextAt - travelled)); nextAt += kPathStep; }
	// The exact end point.
	const double finalLen = (out.size() - 1) * kPathStep;
	if (length - finalLen > 1e-6) {
		const Vec2 from = out.back();
		const Vec2 dir = out.size() >= 2 ? (out.back() - out[out.size() - 2]) * (1.0 / kPathStep) : lastDir;
		out.push_back(from + dir * (length - finalLen));
	}
	return out;
}

// Point at `frac` (0..1) along a resampled path. Every segment is kPathStep long except the last,
// which lands exactly on the slider's length -- so the lookup is by distance, not by index.
inline Vec2 PathAt(const std::vector<Vec2>& path, double frac) {
	if (path.empty()) return {};
	if (path.size() == 1) return path[0];
	frac = std::clamp(frac, 0.0, 1.0);
	const size_t n = path.size();
	const double last = Len(path[n - 1] - path[n - 2]);
	const double total = (n - 2) * kPathStep + last;
	const double d = frac * total;
	size_t i = (size_t)(d / kPathStep);
	if (i >= n - 1) i = n - 2;
	const double segLen = i == n - 2 ? last : kPathStep;
	const double u = segLen > 1e-9 ? std::clamp((d - i * kPathStep) / segLen, 0.0, 1.0) : 0.0;
	return path[i] * (1.0 - u) + path[i + 1] * u;
}

// Fraction along the path of the slider ball at time t (ping-pongs on repeats).
inline double SliderProgress(const HitObject& o, double t) {
	if (o.span <= 0.0) return 0.0;
	double p = (t - o.time) / o.span;
	p = std::clamp(p, 0.0, (double)o.slides);
	int pass = (int)p;
	if (pass >= o.slides) pass = o.slides - 1;
	double f = p - pass;
	if (pass % 2 == 1) f = 1.0 - f;
	return f;
}
inline Vec2 SliderBall(const HitObject& o, double t) { return PathAt(o.path, SliderProgress(o, t)); }

// ---- the whole file -----------------------------------------------------------------------------

inline void Finalize(Beatmap& b, bool paths = true) {
	std::stable_sort(b.timing.begin(), b.timing.end(), [](const TimingPoint& x, const TimingPoint& y) { return x.time < y.time; });
	int combo = 0, colour = 0;
	const int nColours = b.colours.empty() ? 4 : (int)b.colours.size();
	for (size_t i = 0; i < b.objects.size(); ++i) {
		HitObject& o = b.objects[i];
		const bool fresh = i == 0 || o.newCombo || (o.type & kSpinner) || (b.objects[i - 1].type & kSpinner);
		if (fresh) { combo = 0; if (i > 0) colour = (colour + 1 + o.comboSkip) % nColours; }
		o.comboNumber = ++combo;
		o.comboIndex = colour;
		if (o.type & kSlider) {
			double beat = 500.0, sv = 1.0;
			TimingAt(b, o.time, beat, sv);
			const double pxPerBeat = 100.0 * b.sliderMult * sv;
			o.velocity = pxPerBeat / beat;
			o.span = o.length > 0.0 && pxPerBeat > 0.0 ? o.length / pxPerBeat * beat : 0.0;
			o.endTime = o.time + o.span * o.slides;
			if (!paths) continue; // song select only needs times and counts
			o.path = ResamplePath(RawCurve(o), o.length);
			// Checkpoints: ticks along each pass, every repeat, and the end.
			o.checkpoints.clear();
			// osu!'s SliderEventGenerator: ticks every tick distance but not within 10 ms of a span
			// end; the end is judged at the LEGACY last tick, 36 ms early (or the middle).
			o.checkpointIsTick.clear();
			const double tickDist = b.tickRate > 0.0 ? pxPerBeat / b.tickRate : 0.0;
			const double minFromEnd = o.velocity * 10.0;
			const double total = o.span * o.slides;
			for (int pass = 0; pass < o.slides; ++pass) {
				const double base = o.time + pass * o.span;
				std::vector<double> ticks;
				if (tickDist > 0.0 && o.length > 0.0)
					for (double d = tickDist; d <= o.length; d += tickDist) {
						if (d >= o.length - minFromEnd) break;
						ticks.push_back(d / o.length * o.span);
					}
				if (pass % 2 == 1) { std::reverse(ticks.begin(), ticks.end()); for (double& tk : ticks) tk = o.span - tk; }
				for (double tk : ticks) { o.checkpoints.push_back(base + tk); o.checkpointIsTick.push_back(true); }
				const bool last = pass + 1 == o.slides;
				o.checkpoints.push_back(last ? std::max(o.time + total / 2.0, o.time + total - 36.0) : base + o.span);
				o.checkpointIsTick.push_back(false);
			}
		} else if (!(o.type & kSpinner)) {
			o.endTime = o.time;
		}
	}
}

// Parses a .osu file. Standard mode only (Mode: 0); returns false for other modes.
// `paths` = false skips slider curve resampling: what a library scan needs is ~10x cheaper.
inline bool ParseBeatmap(const std::string& text, Beatmap& b, std::string* err = nullptr, bool paths = true) {
	b = Beatmap();
	std::string section;
	size_t pos = 0;
	bool any = false;
	while (pos <= text.size()) {
		size_t nl = text.find('\n', pos);
		if (nl == std::string::npos) nl = text.size();
		std::string line = Trim(text.substr(pos, nl - pos));
		pos = nl + 1;
		if (line.empty() || line.rfind("//", 0) == 0) continue;
		if (section.empty() && line.find("file format v") != std::string::npos) {
			b.formatVersion = (int)Num(line.substr(line.find("file format v") + 13), 14.0);
			continue;
		}
		if (line.front() == '[' && line.back() == ']') { section = line.substr(1, line.size() - 2); continue; }
		if (section == "General" || section == "Metadata" || section == "Difficulty" || section == "Colours") {
			const size_t c = line.find(':');
			if (c == std::string::npos) continue;
			const std::string key = Trim(line.substr(0, c)), val = Trim(line.substr(c + 1));
			if (key == "AudioFilename") b.audio = val;
			else if (key == "AudioLeadIn") b.leadIn = Num(val);
			else if (key == "PreviewTime") b.preview = Num(val, -1.0);
			else if (key == "Mode") b.mode = (int)Num(val);
			else if (key == "StackLeniency") b.stackLeniency = Num(val, 0.7);
			else if (key == "SampleSet") b.sampleSet = val == "Soft" ? 2 : (val == "Drum" ? 3 : 1);
			else if (key == "Title") b.title = val;
			else if (key == "Artist") b.artist = val;
			else if (key == "Creator") b.creator = val;
			else if (key == "Version") b.version = val;
			else if (key == "Source") b.source = val;
			else if (key == "Tags") b.tags = val;
			else if (key == "BeatmapID") b.beatmapId = (long long)Num(val, -1);
			else if (key == "BeatmapSetID") b.setId = (long long)Num(val, -1);
			else if (key == "HPDrainRate") b.hp = Num(val, 5.0);
			else if (key == "CircleSize") b.cs = Num(val, 5.0);
			else if (key == "OverallDifficulty") b.od = Num(val, 5.0);
			else if (key == "ApproachRate") b.ar = Num(val, -1.0);
			else if (key == "SliderMultiplier") b.sliderMult = Num(val, 1.4);
			else if (key == "SliderTickRate") b.tickRate = Num(val, 1.0);
			else if (key.rfind("Combo", 0) == 0) {
				const auto rgb = Split(val, ',');
				if (rgb.size() >= 3)
					b.colours.push_back(((int)Num(rgb[0]) & 255) << 16 | ((int)Num(rgb[1]) & 255) << 8 | ((int)Num(rgb[2]) & 255));
			}
		} else if (section == "Events") {
			const auto f = Split(line, ',');
			if (f.size() >= 3 && (Trim(f[0]) == "2" || Trim(f[0]) == "Break")) {
				const double a = Num(f[1]), e = Num(f[2]);
				if (e > a) b.breaks.push_back({ a, e });
			}
			if (f.size() >= 3 && (Trim(f[0]) == "0" || Trim(f[0]) == "Background") && b.background.empty()) {
				std::string name = Trim(f[2]);
				if (name.size() >= 2 && name.front() == '"' && name.back() == '"') name = name.substr(1, name.size() - 2);
				b.background = name;
			}
		} else if (section == "TimingPoints") {
			const auto f = Split(line, ',');
			if (f.size() < 2) continue;
			TimingPoint tp;
			tp.time = Num(f[0]);
			tp.beatLength = Num(f[1], 500.0);
			tp.uninherited = f.size() >= 7 ? Trim(f[6]) != "0" : tp.beatLength > 0.0;
			if (f.size() >= 4) tp.sampleSet = (int)Num(f[3]);
			if (f.size() >= 5) tp.sampleIndex = (int)Num(f[4]);
			if (f.size() >= 6) tp.volume = (int)Num(f[5], 100.0);
			if (tp.uninherited && !(tp.beatLength > 0.0)) continue;
			b.timing.push_back(tp);
		} else if (section == "HitObjects") {
			const auto f = Split(line, ',');
			if (f.size() < 4) continue;
			HitObject o;
			o.pos = { Num(f[0]), Num(f[1]) };
			o.time = Num(f[2]);
			const int type = (int)Num(f[3]);
			o.newCombo = (type & kNewCombo) != 0;
			o.comboSkip = (type >> 4) & 7;
			o.sound = f.size() >= 5 ? (int)Num(f[4]) : 0;
			auto hitSample = [&o](const std::string& hs) {
				const auto q = Split(hs, ':');
				if (q.size() >= 1) o.normalSet = (int)Num(q[0]);
				if (q.size() >= 2) o.addSet = (int)Num(q[1]);
				if (q.size() >= 3) o.index = (int)Num(q[2]);
				if (q.size() >= 4) o.volume = (int)Num(q[3]);
				if (q.size() >= 5) o.file = Trim(q[4]);
			};
			if (type & kSlider) {
				o.type = kSlider;
				if (f.size() < 8) continue;
				const auto curve = Split(f[5], '|');
				o.curve = curve.empty() || curve[0].empty() ? 'B' : curve[0][0];
				for (size_t k = 1; k < curve.size(); ++k) {
					const auto xy = Split(curve[k], ':');
					if (xy.size() == 2) o.control.push_back({ Num(xy[0]), Num(xy[1]) });
				}
				o.slides = std::max(1, (int)Num(f[6], 1));
				o.length = std::max(0.0, Num(f[7]));
				if (f.size() >= 9) for (const auto& e : Split(f[8], '|')) o.edgeSounds.push_back((int)Num(e));
				if (f.size() >= 10) for (const auto& e : Split(f[9], '|')) {
					const auto q = Split(e, ':');
					o.edgeSets.push_back({ q.size() >= 1 ? (int)Num(q[0]) : 0, q.size() >= 2 ? (int)Num(q[1]) : 0 });
				}
				if (f.size() >= 11) hitSample(f[10]);
			} else if (type & kSpinner) {
				o.type = kSpinner;
				o.pos = { 256.0, 192.0 };
				o.endTime = f.size() >= 6 ? Num(f[5], o.time) : o.time;
				if (f.size() >= 7) hitSample(f[6]);
			} else if (type & kCircle) {
				o.type = kCircle;
				if (f.size() >= 6) hitSample(f[5]);
			} else continue; // mania hold notes etc.
			b.objects.push_back(o);
			any = true;
		}
	}
	if (b.ar < 0.0) b.ar = b.od; // old maps: AR follows OD
	if (b.mode != 0) { if (err) *err = "not an osu!standard map"; return false; }
	if (!any) { if (err) *err = "no hit objects"; return false; }
	std::stable_sort(b.objects.begin(), b.objects.end(), [](const HitObject& x, const HitObject& y) { return x.time < y.time; });
	Finalize(b, paths);
	return true;
}

// ---- library summary ------------------------------------------------------------------------------

struct Summary {
	double bpmMin = 0, bpmMax = 0, bpmMain = 0;
	double firstMs = 0, lastMs = 0, drainMs = 0;
	int circles = 0, sliders = 0, spinners = 0;
};
inline Summary Summarize(const Beatmap& b) {
	Summary s;
	double bestLen = -1.0, prevT = 0.0, prevBeat = 0.0;
	const double end = b.objects.empty() ? 0.0 : b.objects.back().endTime;
	std::vector<std::pair<double, double>> spans; // (beat length, duration) of each uninherited section
	bool first = true;
	for (const TimingPoint& tp : b.timing) {
		if (!tp.uninherited || tp.beatLength <= 0.0) continue;
		const double bpm = 60000.0 / tp.beatLength;
		if (first) { s.bpmMin = s.bpmMax = bpm; first = false; }
		s.bpmMin = std::min(s.bpmMin, bpm); s.bpmMax = std::max(s.bpmMax, bpm);
		if (prevBeat > 0.0) spans.push_back({ prevBeat, std::max(0.0, std::min(tp.time, end) - prevT) });
		prevT = tp.time; prevBeat = tp.beatLength;
	}
	if (prevBeat > 0.0) spans.push_back({ prevBeat, std::max(0.0, end - prevT) });
	for (auto& sp : spans) if (sp.second > bestLen) { bestLen = sp.second; s.bpmMain = 60000.0 / sp.first; }
	if (s.bpmMain <= 0.0) s.bpmMain = s.bpmMax;
	for (const HitObject& o : b.objects) {
		if (o.type & kSlider) s.sliders++; else if (o.type & kSpinner) s.spinners++; else s.circles++;
	}
	if (!b.objects.empty()) {
		s.firstMs = b.objects.front().time;
		s.lastMs = std::max(b.objects.back().endTime, b.objects.back().time);
		double brk = 0.0;
		for (auto& p : b.breaks) brk += std::max(0.0, std::min(p.second, s.lastMs) - std::max(p.first, s.firstMs));
		s.drainMs = std::max(0.0, s.lastMs - s.firstMs - brk);
	}
	return s;
}

// HP drains only while there is something to play: from the first object to the last, outside
// breaks (osu! does the same -- no drain during the intro or a break).
inline bool InDrainTime(const Beatmap& b, double t) {
	if (b.objects.empty()) return false;
	if (t < b.objects.front().time || t > std::max(b.objects.back().endTime, b.objects.back().time)) return false;
	for (auto& p : b.breaks) if (t > p.first && t < p.second) return false;
	return true;
}

// ---- health (osu.Game.Rulesets.Osu OsuHealthProcessor, osu.Game DrainingHealthProcessor) --------
// lazer (non-classic) judgements and what each heals:
//   circle, slider HEAD, spinner   300 +0.03  100 +0.011  50 +0.002  miss -0.03/-0.125/-0.2 (HP 0/5/10)
//   slider tick                    hit +0.015, miss -0.02/-0.075/-0.14
//   slider repeat                  hit +0.02,  miss -0.02/-0.075/-0.14
//   slider tail                    hit +0.02,  miss 0
//   slider itself                  0 (it only carries the combo-end bonus)
//   spinner spin ticks             +0.0085 (bonus spins +0.01); never in the drain simulation
// Finishing a combo (last object of it, if hit; a slider always counts as hit) adds 0.07 if the
// combo was clean, 0.05 after a 100 / slider break / missed repeat or tail, 0.03 after a 50 or miss.
// The drain rate is solved per map from a perfect play WITHOUT those bonuses so it bottoms out at
// a target set by HP (0.99 at HP 0, 0.9 at 5, 0.4 at 10). No drain before the first object, after
// the last, or from the last object before a break to the first one after it.
namespace Health {
constexpr double kGreat = 0.03, kOk = 0.011, kMeh = 0.002;
constexpr double kTick = 0.015, kNode = 0.02; // slider tick; slider repeat / tail
constexpr double kSpinTick = 0.0085, kSpinBonus = 0.01;
inline double Miss(double hp) { return DifficultyRange(hp, -0.03, -0.125, -0.2); }
inline double TickMiss(double hp) { return DifficultyRange(hp, -0.02, -0.075, -0.14); }
inline double ForResult(int value, double hp) {
	return value >= 300 ? kGreat : value >= 100 ? kOk : value >= 50 ? kMeh : Miss(hp);
}
// Combo result: 2 perfect (only 300s), 1 good (a 100 or a broken slider), 0 none (a 50 or a miss).
inline double ComboEndBonus(int comboResult) { return comboResult >= 2 ? 0.07 : comboResult == 1 ? 0.05 : 0.03; }
// lazer's NewCombo as decoded: the flag, forced on the first object and on the one after a spinner.
inline bool NewCombo(const Beatmap& b, size_t i) {
	return i == 0 || b.objects[i].newCombo || (b.objects[i - 1].type & kSpinner);
}
// lazer's LastInCombo (OsuHitObject.UpdateComboInformation): set on an object only when the next one
// starts a combo -- spinners never do, and the map's last object is never marked.
inline bool LastInCombo(const Beatmap& b, size_t i) {
	return i + 1 < b.objects.size() && !(b.objects[i + 1].type & kSpinner) && NewCombo(b, i + 1);
}
inline double TargetMinimum(double hp) { return std::clamp(DifficultyRange(hp, 0.99, 0.9, 0.4), 0.0, 1.0); }

// No-drain windows around breaks: from the last object to end before the break to the first one
// to start after it.
inline bool NoDrain(const Beatmap& b, double t) {
	if (b.objects.empty()) return true;
	const auto endOf = [](const HitObject& o) { return (o.type & (kSlider | kSpinner)) ? o.endTime : o.time; };
	if (t < b.objects.front().time || t > endOf(b.objects.back())) return true;
	for (const auto& br : b.breaks) {
		double from = -1e300, to = 1e300;
		for (const HitObject& o : b.objects) if (endOf(o) <= br.first) from = std::max(from, endOf(o));
		for (const HitObject& o : b.objects) if (o.time >= br.second) { to = o.time; break; }
		if (t >= from && t <= to) return true;
	}
	return false;
}

// Health lost per ms of song time, from a simulated perfect play (binary search, as osu!).
inline double DrainRate(const Beatmap& b, double drainStart) {
	std::vector<std::pair<double, double>> inc; // (time, amount): non-bonus max results only
	for (size_t i = 0; i < b.objects.size(); ++i) {
		const HitObject& o = b.objects[i];
		if (o.type & kSlider) {
			inc.push_back({ o.time, kGreat }); // the head is judged like a circle
			for (size_t k = 0; k + 1 < o.checkpoints.size(); ++k) {
				const bool tick = k < o.checkpointIsTick.size() && o.checkpointIsTick[k];
				inc.push_back({ o.checkpoints[k], tick ? kTick : kNode });
			}
			inc.push_back({ o.endTime, kNode }); // tail
			inc.push_back({ o.endTime, 0.0 });   // the slider itself
		} else if (o.type & kSpinner) {
			inc.push_back({ o.endTime, kGreat });
		} else {
			inc.push_back({ o.time, kGreat });
		}
	}
	std::stable_sort(inc.begin(), inc.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
	if (inc.size() <= 1) return 0.0;
	const double target = TargetMinimum(b.hp);
	int adjustment = 1;
	double result = 1.0;
	while (adjustment > 0 && adjustment < (1 << 30)) {
		double current = 1.0, lowest = 1.0;
		size_t brk = 0;
		for (size_t i = 0; i < inc.size(); ++i) {
			const double now = inc[i].first;
			double last = i > 0 ? inc[i - 1].first : drainStart;
			while (brk < b.breaks.size() && b.breaks[brk].second <= now) { last = now; ++brk; }
			current -= (now - last) * result;
			lowest = std::min(lowest, current);
			current = std::min(1.0, current + inc[i].second);
			if (lowest < 0.0) break;
		}
		if (std::fabs(lowest - target) <= 0.01) break;
		adjustment *= 2;
		result += 1.0 / adjustment * (lowest > target ? 1.0 : (lowest < target ? -1.0 : 0.0));
	}
	return result;
}
} // namespace Health

// ---- mods ---------------------------------------------------------------------------------------
// Values from osu!'s own mod definitions (osu.Game / osu.Game.Rulesets.Osu): EZ x0.5, HR x1.4
// (CS x1.3), capped at 10; DT/NC 1.5x, HT 0.75x; HD fades from 40% of the approach over 30%.
// Score multipliers are stable's.
enum : uint32_t {
	kModEZ = 1u << 0, kModNF = 1u << 1, kModHT = 1u << 2, kModHR = 1u << 3, kModSD = 1u << 4,
	kModPF = 1u << 5, kModDT = 1u << 6, kModNC = 1u << 7, kModHD = 1u << 8, kModFL = 1u << 9,
	kModAT = 1u << 10,
};
struct ModInfo { uint32_t bit; const char* acronym; const char* name; double multiplier; char key; int group; uint32_t excludes; };
inline const ModInfo* ModTable(size_t& n) {
	static const ModInfo t[] = {
		{ kModEZ, "EZ", "Easy", 0.50, 'Q', 0, kModHR },
		{ kModNF, "NF", "No Fail", 0.50, 'W', 0, kModSD | kModPF | kModAT },
		{ kModHT, "HT", "Half Time", 0.30, 'E', 0, kModDT | kModNC },
		{ kModHR, "HR", "Hard Rock", 1.06, 'A', 1, kModEZ },
		{ kModSD, "SD", "Sudden Death", 1.00, 'S', 1, kModNF | kModPF | kModAT },
		{ kModPF, "PF", "Perfect", 1.00, 'X', 1, kModNF | kModSD | kModAT },
		{ kModDT, "DT", "Double Time", 1.12, 'D', 1, kModHT | kModNC },
		{ kModNC, "NC", "Nightcore", 1.12, 'C', 1, kModHT | kModDT },
		{ kModHD, "HD", "Hidden", 1.06, 'F', 1, 0 },
		{ kModFL, "FL", "Flashlight", 1.12, 'G', 1, 0 },
		{ kModAT, "AT", "Autoplay", 1.00, 'Z', 2, kModNF | kModSD | kModPF },
	};
	n = sizeof(t) / sizeof(t[0]);
	return t;
}
inline uint32_t ToggleMod(uint32_t mods, uint32_t bit) {
	if (mods & bit) return mods & ~bit;
	size_t n; const ModInfo* t = ModTable(n);
	for (size_t i = 0; i < n; ++i) if (t[i].bit == bit) mods &= ~t[i].excludes;
	return mods | bit;
}
inline double ModRate(uint32_t mods) {
	if (mods & (kModDT | kModNC)) return 1.5;
	if (mods & kModHT) return 0.75;
	return 1.0;
}
inline double ModMultiplier(uint32_t mods) {
	size_t n; const ModInfo* t = ModTable(n);
	double m = 1.0;
	for (size_t i = 0; i < n; ++i) if (mods & t[i].bit) m *= t[i].multiplier;
	return m;
}
inline void ApplyModsToDifficulty(uint32_t mods, double& cs, double& ar, double& od, double& hp) {
	if (mods & kModEZ) { cs *= 0.5; ar *= 0.5; od *= 0.5; hp *= 0.5; }
	if (mods & kModHR) {
		cs = std::min(cs * 1.3, 10.0); ar = std::min(ar * 1.4, 10.0);
		od = std::min(od * 1.4, 10.0); hp = std::min(hp * 1.4, 10.0);
	}
}
// The AR / OD a player FEELS at a rate (approach time and hit window shrink in real time).
inline double ArFromPreempt(double ms) {
	return ms > 1200.0 ? 5.0 - (ms - 1200.0) / 120.0 : 5.0 + (1200.0 - ms) / 150.0;
}
inline double EffectiveAr(double ar, double rate) { return ArFromPreempt(Preempt(ar) / rate); }
inline double EffectiveOd(double od, double rate) { return (80.0 - (80.0 - 6.0 * od) / rate) / 6.0; }

// Hard Rock flips the playfield vertically (paths included).
inline void FlipY(Beatmap& b) {
	for (HitObject& o : b.objects) {
		o.pos.y = 384.0 - o.pos.y;
		for (Vec2& c : o.control) c.y = 384.0 - c.y;
		for (Vec2& p : o.path) p.y = 384.0 - p.y;
	}
}

// Hidden: the object's opacity at song time t (approach circles are not drawn at all).
inline double HiddenAlpha(double t, double objTime, double preempt) {
	const double fadeInEnd = objTime - preempt + preempt * 0.4;
	if (t < objTime - preempt) return 0.0;
	if (t < fadeInEnd) return (t - (objTime - preempt)) / (preempt * 0.4);
	return std::clamp(1.0 - (t - fadeInEnd) / (preempt * 0.3), 0.0, 1.0);
}

// Playfield placement inside a W x H screen (OsuPlayfieldAdjustmentContainer): the field fills
// 80% of the largest 4:3 area, centred, then shifted down 8 osu px.
struct Playfield { double scale, ox, oy; };
inline Playfield PlayfieldFor(double w, double h) {
	const double s = std::min(w, h * 4.0 / 3.0) * 0.8 / 512.0;
	return { s, (w - 512.0 * s) * 0.5, (h - 384.0 * s) * 0.5 + 8.0 * s };
}

// ---- stacking (osu.Game.Rulesets.Osu OsuBeatmapProcessor) ----------------------------------------
// Objects close in time at (almost) the same spot are fanned up-left by radius/10 per level so
// each stays readable. Call after mods (preempt from the final AR, after the Hard Rock flip);
// positions, paths and control points are moved by the offset (osu! hit-tests StackedPosition).
inline Vec2 EndPosition(const HitObject& o, bool alwaysPathEnd = false) {
	if (!(o.type & kSlider) || o.path.empty()) return o.pos;
	if (alwaysPathEnd || o.slides % 2 == 1) return o.path.back();
	return o.path.front();
}
inline void ApplyStacking(Beatmap& b) {
	auto& H = b.objects;
	const int n = (int)H.size();
	for (auto& h : H) h.stack = 0;
	if (n == 0) return;
	const double kStackDistance = 3.0;
	const double threshold = std::trunc(Preempt(b.ar)) * b.stackLeniency;
	auto dist = [](Vec2 a, Vec2 c) { return Len(a - c); };
	auto isSpinner = [](const HitObject& h) { return (h.type & kSpinner) != 0; };
	auto isSlider = [](const HitObject& h) { return (h.type & kSlider) != 0; };
	auto endTime = [](const HitObject& h) { return (h.type & (kSlider | kSpinner)) ? h.endTime : h.time; };
	if (b.formatVersion >= 6) {
		// applyStacking(beatmap, objects, 0, n - 1): the range is everything, so no forward pass.
		for (int i = n - 1; i > 0; --i) {
			int ni = i;
			int io = i; // objectI
			if (H[io].stack != 0 || isSpinner(H[io])) continue;
			if (H[io].type & kCircle) {
				while (--ni >= 0) {
					const HitObject& N = H[ni];
					if (isSpinner(N)) continue;
					if ((int)H[io].time - (int)endTime(N) > threshold) break;
					if (isSlider(N) && dist(EndPosition(N), H[io].pos) < kStackDistance) {
						const int offset = H[io].stack - N.stack + 1;
						for (int j = ni + 1; j <= i; ++j)
							if (dist(EndPosition(N), H[j].pos) < kStackDistance) H[j].stack -= offset;
						break;
					}
					if (dist(N.pos, H[io].pos) < kStackDistance) {
						H[ni].stack = H[io].stack + 1;
						io = ni;
					}
				}
			} else if (isSlider(H[io])) {
				while (--ni >= 0) {
					const HitObject& N = H[ni];
					if (isSpinner(N)) continue;
					if (H[io].time - N.time > threshold) break;
					if (dist(EndPosition(N), H[io].pos) < kStackDistance) {
						H[ni].stack = H[io].stack + 1;
						io = ni;
					}
				}
			}
		}
	} else {
		for (int i = 0; i < n; ++i) {
			HitObject& cur = H[i];
			if (cur.stack != 0 && !isSlider(cur)) continue;
			double start = endTime(cur);
			int sliderStack = 0;
			for (int j = i + 1; j < n; ++j) {
				if (H[j].time - threshold > start) break;
				const Vec2 p2 = isSlider(cur) ? EndPosition(cur, true) : cur.pos;
				if (dist(H[j].pos, cur.pos) < kStackDistance) { cur.stack++; start = H[j].time; }
				else if (dist(H[j].pos, p2) < kStackDistance) { sliderStack++; H[j].stack -= sliderStack; start = H[j].time; }
			}
		}
	}
	const double step = -Radius(b.cs) / 10.0; // osu!: StackHeight * Scale * -6.4
	for (auto& h : H) {
		if (h.stack == 0 || isSpinner(h)) continue;
		const Vec2 off{ h.stack * step, h.stack * step };
		h.pos = h.pos + off;
		for (Vec2& c : h.control) c = c + off;
		for (Vec2& p : h.path) p = p + off;
	}
}

// ---- hitsounds -------------------------------------------------------------------------------------
// osu!'s legacy sample naming: "<set>-hit<normal|whistle|finish|clap>[index]" and
// "<set>-slidertick[index]"; set 1 normal, 2 soft, 3 drum. Index 0 means the skin's sample,
// index 1 the beatmap's unnumbered file, N>1 the beatmap's file numbered N.
struct SampleRef { std::string name; int index = 0; int volume = 100; std::string file; };
inline const char* SampleSetName(int set) { return set == 2 ? "soft" : (set == 3 ? "drum" : "normal"); }
inline const TimingPoint* TimingPointAt(const Beatmap& b, double t) {
	const TimingPoint* best = nullptr;
	for (const TimingPoint& tp : b.timing) { if (tp.time > t + 5.0) break; best = &tp; } // osu!: +5 ms leniency
	if (!best && !b.timing.empty()) best = &b.timing.front();
	return best;
}
// Samples for one hit: a node of an object (circle, slider head/repeat/tail, spinner end).
inline void HitSamples(const Beatmap& b, const HitObject& o, double t, int sound, int nodeNormal, int nodeAdd,
	std::vector<SampleRef>& out) {
	const TimingPoint* tp = TimingPointAt(b, t);
	const int normal = nodeNormal ? nodeNormal : (o.normalSet ? o.normalSet : (tp && tp->sampleSet ? tp->sampleSet : b.sampleSet));
	const int add = nodeAdd ? nodeAdd : (o.addSet ? o.addSet : normal);
	const int index = o.index ? o.index : (tp ? tp->sampleIndex : 0);
	const int volume = o.volume ? o.volume : (tp ? tp->volume : 100);
	if (!o.file.empty()) { out.push_back({ "", 0, volume, o.file }); return; }
	out.push_back({ std::string(SampleSetName(normal)) + "-hitnormal", index, volume, "" });
	if (sound & 2) out.push_back({ std::string(SampleSetName(add)) + "-hitwhistle", index, volume, "" });
	if (sound & 4) out.push_back({ std::string(SampleSetName(add)) + "-hitfinish", index, volume, "" });
	if (sound & 8) out.push_back({ std::string(SampleSetName(add)) + "-hitclap", index, volume, "" });
}
inline SampleRef TickSample(const Beatmap& b, const HitObject& o, double t) {
	const TimingPoint* tp = TimingPointAt(b, t);
	const int normal = o.normalSet ? o.normalSet : (tp && tp->sampleSet ? tp->sampleSet : b.sampleSet);
	return { std::string(SampleSetName(normal)) + "-slidertick", o.index ? o.index : (tp ? tp->sampleIndex : 0),
		o.volume ? o.volume : (tp ? tp->volume : 100), "" };
}

} // namespace Osu
} // namespace Filmmaker
