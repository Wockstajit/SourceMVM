#pragma once

// The CS:S demo bar's rules, engine-free and header-only: the speed dial (CS2's DemoBarButtonsJs.h
// mapping, kept number for number), the scrubber's tick and pixel mapping, -5s/+5s, how a seek is
// sent, the LOOP region, and where the next kill or round is. Automation/tests/
// source-demo-bar-math-tests.cpp pins it. The bar's drawing lives in DemoBar.cpp.

#include <cmath>
#include <cstdio>

namespace SourceFilmmaker {
namespace Ui {
namespace DemoBarMath {

// ---- speed dial ---------------------------------------------------------------------------------
// Logarithmic over 0.05x..20x, with the middle snapping to exactly 1x.
constexpr double kMinSpeed = 0.05;
constexpr double kMaxSpeed = 20.0;
constexpr double kSnap = 0.02;
// Five equal click zones. Each speed's handle position falls on its zone's centre (0.116, 0.269,
// 0.5, 0.731, 0.884 against 0.1, 0.3, 0.5, 0.7, 0.9), so a click lands the handle under its label.
constexpr int kZoneCount = 5;
constexpr double kZoneSpeeds[kZoneCount] = { 0.1, 0.25, 1, 4, 10 };
constexpr const char* kZoneLabels[kZoneCount] = { "0.1x", "0.25x", "1x", "4x", "10x" };
// Travel, in pixels, that still reads as a click rather than a drag.
constexpr float kClickSlopPx = 4;
// The dial takes the room the bar's buttons leave, within these bounds (stage px).
constexpr float kDialMinW = 300, kDialMaxW = 420, kDialBarShare = 0.26f, kDialH = 36;
// A new speed is sent only when it differs from the last by more than this fraction.
constexpr double kSendThreshold = 0.008;

inline double Clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

inline double SpeedFor(double v) {
	v = Clamp01(v);
	if (std::fabs(v - 0.5) < kSnap) return 1.0;
	return kMinSpeed * std::pow(kMaxSpeed / kMinSpeed, v);
}

inline double SliderFor(double speed) {
	if (!(speed > 0)) return 0.5;
	return Clamp01(std::log(speed / kMinSpeed) / std::log(kMaxSpeed / kMinSpeed));
}

// "0.050x", "0.25x", "1x", "1.50x", "12.0x", as CS2's readout pill writes them.
inline void FormatSpeed(double speed, char* out, size_t size) {
	if (speed >= 10) std::snprintf(out, size, "%.1fx", speed);
	else if (speed >= 1) {
		if (std::fabs(speed - std::round(speed)) < 0.005) std::snprintf(out, size, "%.0fx", speed);
		else std::snprintf(out, size, "%.2fx", speed);
	} else if (speed >= 0.1) std::snprintf(out, size, "%.2fx", speed);
	else std::snprintf(out, size, "%.3fx", speed);
}

// The zone a strip position (0..1) falls in.
inline int ZoneAt(double v) {
	const int zone = (int)(Clamp01(v) * kZoneCount);
	return zone >= kZoneCount ? kZoneCount - 1 : zone;
}

inline float DialWidth(float barW, float freeW) {
	float w = kDialMaxW;
	if (barW * kDialBarShare < w) w = barW * kDialBarShare;
	if (freeW - 48 < w) w = freeW - 48;
	if (w < kDialMinW) w = kDialMinW;
	return std::round(w);
}

// Whether a dial value is worth sending, given the speed the game runs at.
inline bool SpeedChanged(double wanted, double current) {
	if (!(current > 0)) return true;
	return std::fabs(wanted - current) / current > kSendThreshold;
}

// ---- scrubber -----------------------------------------------------------------------------------

// Tick at x along a scrubber from x0 to x1 that spans firstTick..lastTick.
inline int TickAtX(float x, float x0, float x1, int firstTick, int lastTick) {
	if (!(x1 > x0) || lastTick <= firstTick) return firstTick;
	const double f = Clamp01((x - x0) / (double)(x1 - x0));
	return (int)std::lround(firstTick + f * (lastTick - firstTick));
}

inline float XAtTick(int tick, float x0, float x1, int firstTick, int lastTick) {
	if (lastTick <= firstTick) return x0;
	const double f = Clamp01((tick - firstTick) / (double)(lastTick - firstTick));
	return (float)(x0 + f * (x1 - x0));
}

// "1:06.15", or "1:02:03.40" past an hour.
inline void FormatTime(double seconds, char* out, size_t size) {
	if (!(seconds > 0)) seconds = 0;
	const int whole = (int)seconds;
	const double frac = seconds - whole;
	const int h = whole / 3600, m = (whole / 60) % 60, s = whole % 60;
	if (h > 0) std::snprintf(out, size, "%d:%02d:%05.2f", h, m, s + frac);
	else std::snprintf(out, size, "%d:%05.2f", m, s + frac);
}

// ---- seeking ------------------------------------------------------------------------------------

// Ticks in a whole number of seconds at the demo's tick rate (CS2's -5s/+5s: 5 x round(1/interval)).
inline int TicksForSeconds(double seconds, double intervalPerTick) {
	if (!(intervalPerTick > 0)) return 0;
	return (int)std::lround(seconds) * (int)std::lround(1.0 / intervalPerTick);
}

// The tick a seek goes to: the target, held inside the demo. Both ways are the engine's own seek
// (backward, rewound in place: SourceDemoRewind.h).
inline int SeekTick(int targetTick, int totalTicks) {
	if (targetTick < 0) targetTick = 0;
	if (totalTicks > 0 && targetTick > totalTicks) targetTick = totalTicks;
	return targetTick;
}

// ---- LOOP ---------------------------------------------------------------------------------------

struct LoopRegion {
	int in = 0, out = 0;
	bool Valid() const { return out > in; }
};

// CS2's seed when LOOP turns on with no region set: the playhead and 320 ticks after it, inside
// the demo.
constexpr int kLoopSeedTicks = 320;

inline LoopRegion SeedLoop(int playhead, int totalTicks) {
	LoopRegion r;
	r.in = playhead < 0 ? 0 : playhead;
	r.out = r.in + kLoopSeedTicks;
	if (totalTicks > 0 && r.out > totalTicks) {
		r.out = totalTicks;
		r.in = r.out - kLoopSeedTicks < 0 ? 0 : r.out - kLoopSeedTicks;
	}
	return r;
}

// Playback reached Out: go back to In.
inline bool LoopWraps(const LoopRegion& r, int tick, bool playing) {
	return playing && r.Valid() && tick >= r.out;
}

// The LOOP region's In and Out flags on the marker band (CS2's CameraTimelineRangeJs.h): a line at
// its tick with an I or O tab beside it, dragged from where it was grabbed. Stage px.
constexpr float kLoopFlagTabW = 15, kLoopFlagLineW = 2, kLoopFlagGrabW = 9;
constexpr double kLoopFlagFine = 16; // Shift: the flag moves 1/16 as far as the pointer

// Where a dragged flag is, as a fraction of the bar: its fraction when grabbed, moved by the pointer
// since then (fine: 1/16 of it), held to the bar.
inline double LoopFlagDrag(double anchorFrac, float anchorX, float x, float barWidth, bool fine) {
	if (!(barWidth > 1)) return anchorFrac;
	const double f = anchorFrac + (double)(x - anchorX) / barWidth / (fine ? kLoopFlagFine : 1.0);
	return f < 0 ? 0 : (f > 1 ? 1 : f);
}

// The two flags committed together, in order: dragging In past Out swaps their roles, as in a video
// editor, and the region keeps at least one tick.
inline LoopRegion LoopFromFlags(int a, int b) {
	LoopRegion r;
	r.in = a < b ? a : b;
	r.out = a < b ? b : a;
	if (r.out <= r.in) r.out = r.in + 1;
	return r;
}

// The left edge of a flag's tab: outside the region (In's left of its line, Out's right of it), and
// inside where the outside would leave the bar.
inline float LoopFlagTabX(float lineX, bool isIn, float barX0, float barX1, float tabW, float lineW) {
	const float outer = isIn ? lineX - lineW * 0.5f - tabW : lineX + lineW * 0.5f;
	const float inner = isIn ? lineX + lineW * 0.5f : lineX - lineW * 0.5f - tabW;
	return (outer < barX0 || outer + tabW > barX1) ? inner : outer;
}

// ---- events -------------------------------------------------------------------------------------

// CS2's "Go To Event" lands 40 ticks (at 64 tick) before the event, so the moment plays in.
inline int EventLeadTicks(double intervalPerTick) {
	if (!(intervalPerTick > 0)) return 40;
	return (int)std::lround(0.625 / intervalPerTick);
}

// The first of sortedTicks after tick (plus a small margin, so pressing again moves on), or -1.
template <class Ticks>
int NextTickAfter(const Ticks& sortedTicks, int tick, int margin) {
	for (int t : sortedTicks)
		if (t > tick + margin) return t;
	return -1;
}

// The last of sortedTicks before tick (minus a margin, so pressing again moves back), or -1.
template <class Ticks>
int PreviousTickBefore(const Ticks& sortedTicks, int tick, int margin) {
	int found = -1;
	for (int t : sortedTicks)
		if (t < tick - margin) found = t;
	return found;
}

} // namespace DemoBarMath
} // namespace Ui
} // namespace SourceFilmmaker
