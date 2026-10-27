#pragma once

// The TIMELINE dock's rules: CS2's dock (GraphEditorJs.h) in C++, so the CS:S dock lays out,
// navigates and snaps as CS2's does. The bands and widths of the dock, the time window (the
// navigator, zoom, FIT, reveal and playback paging), time snap, ruler steps, the time format, key
// times merged per row, the halves of a key's glyph, and a camera channel's display range.
//
// Everything is in CS2's stage px (1080-tall stage). Engine-free;
// Automation/tests/source-timeline-dock-math-tests.cpp pins it.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

namespace SourceFilmmaker {
namespace Ui {
namespace TimelineDockMath {

// The dock's bands, from its top (GraphEditorJs.h): toolbar, overview, ruler, rows, footer.
constexpr float kDockH = 440, kHeaderH = 72, kOverviewH = 14, kRulerH = 26, kFooterH = 26;
constexpr float kRowsY = kHeaderH + kOverviewH + kRulerH; // 112
constexpr float kRowsH = kDockH - kRowsY - kFooterH;       // 302
constexpr float kLeftW = 296, kPad = 12, kScrollW = 12, kRowH = 22, kValueW = 120, kNavW = 54;
constexpr float kGlyph = 14, kKeyHit = 8, kHandleR = 7, kDragPx = 3, kGraphTop = 30, kGraphBottom = 26;
constexpr float kFooterTrackW = 220;
constexpr float kMinDockW = 560;

// Widths right of the layer list (computeGeom): the track (tw), the overview, ruler and GRAPH panel
// that also span the rows' scrollbar column (ow), and the time span's pixels (cw, inside 12 px
// margins).
struct Widths {
	float tw = 0, ow = 0, cw = 0;
};

inline Widths WidthsFor(float dockW) {
	Widths w;
	w.tw = (std::max)(120.0f, dockW - kLeftW - kScrollW);
	w.ow = w.tw + kScrollW;
	w.cw = (std::max)(60.0f, w.tw - 2 * kPad);
	return w;
}

// A tick range: the visible window or the navigable domain.
struct Span {
	double t0 = 0, t1 = 1;
	double Len() const { return t1 - t0; }
	bool Contains(double t) const { return t >= t0 && t <= t1; }
};

inline Span MakeSpan(double t0, double t1) {
	Span s;
	s.t0 = t0;
	s.t1 = t1;
	return s;
}

// Track-local x of a tick, and back; the window fills x = 12 .. 12 + cw.
inline double TickToX(const Span& view, float cw, double t) { return kPad + (t - view.t0) / view.Len() * cw; }
inline double XToTick(const Span& view, float cw, double x) { return view.t0 + (x - kPad) / cw * view.Len(); }
// The overview maps the whole domain across ow with no margins.
inline double DomToX(const Span& domain, float ow, double t) { return (t - domain.t0) / (std::max)(1.0, domain.Len()) * ow; }
inline double XToDom(const Span& domain, float ow, double x) { return domain.t0 + x / ow * (std::max)(1.0, domain.Len()); }

constexpr double kMinSpan = 24;

// The navigable domain: the demo's ticks, else what the window and playhead cover, else 0..2000.
inline Span DomainFor(double d0, double d1, const Span& view, double playhead) {
	if (d1 > d0) return MakeSpan(d0, d1);
	Span d = MakeSpan((std::min)(view.t0, playhead), (std::max)(view.t1, playhead));
	if (!(d.t1 > d.t0)) d = MakeSpan(0, 2000);
	return d;
}

// The window kept between 24 ticks (or the whole domain) and the whole domain, about its centre,
// then pushed inside the domain (normalizeView).
inline Span Normalize(Span v, const Span& d) {
	const double domSpan = (std::max)(1.0, d.Len());
	const double minS = (std::min)(kMinSpan, domSpan), maxS = domSpan;
	double span = v.Len();
	if (!(span > 0)) span = minS;
	span = (std::min)(maxS, (std::max)(minS, span));
	const double c = (v.t0 + v.t1) * 0.5;
	v.t0 = c - span / 2;
	v.t1 = v.t0 + span;
	if (v.t0 < d.t0) { v.t1 += d.t0 - v.t0; v.t0 = d.t0; }
	if (v.t1 > d.t1) { v.t0 -= v.t1 - d.t1; v.t1 = d.t1; }
	if (v.t0 < d.t0) v.t0 = d.t0;
	return v;
}

// The window from C++'s suggestion (seedView): a range under half a tick opens on the playhead.
inline Span SeedView(double s0, double s1, double playhead, const Span& d) {
	if (!(s1 > s0 + 0.5)) { s0 = playhead - 1000; s1 = playhead + 1000; }
	return Normalize(MakeSpan(s0, s1), d);
}

// Zoom by f keeping anchor at the same x (zoomAt).
inline Span ZoomAt(const Span& v, const Span& d, double f, double anchor) {
	const double span = v.Len(), ns = span * f;
	Span z;
	z.t0 = anchor - (anchor - v.t0) * (ns / span);
	z.t1 = z.t0 + ns;
	return Normalize(z, d);
}

// The footer's - and + : about the playhead when it is in the window, else the centre (zoomStep).
inline Span ZoomStep(const Span& v, const Span& d, double f, double playhead) {
	return ZoomAt(v, d, f, v.Contains(playhead) ? playhead : (v.t0 + v.t1) * 0.5);
}

inline Span Pan(const Span& v, const Span& d, double dt) { return Normalize(MakeSpan(v.t0 + dt, v.t1 + dt), d); }

// Frame a..b: padded 8 % (at least 32 ticks); under 96 ticks, 96 wide about the middle (frameTicks).
inline Span FrameTicks(double a, double b, const Span& d) {
	const double pad = (std::max)(32.0, (b - a) * 0.08);
	Span v = MakeSpan(a - pad, b + pad);
	if (v.Len() < kMinSpan * 4) {
		const double c = (a + b) / 2;
		v = MakeSpan(c - kMinSpan * 2, c + kMinSpan * 2);
	}
	return Normalize(v, d);
}

// A jump to a..b off screen shifts the window so it lands 2 % inside; never a zoom, unless the range
// does not fit, which frames it (revealTicks). False when it was already in view.
inline bool Reveal(Span& v, const Span& d, double a, double b) {
	if (!(b >= a)) return false;
	const double span = v.Len(), m = span * 0.02;
	if (a >= v.t0 && b <= v.t1) return false;
	if (b - a > span - 2 * m) { v = FrameTicks(a, b, d); return true; }
	if (a < v.t0) { v.t0 = a - m; v.t1 = v.t0 + span; }
	else { v.t1 = b + m; v.t0 = v.t1 - span; }
	v = Normalize(v, d);
	return true;
}

// Playback paging (followPlayback): while the demo plays, a playhead that leaves the window starts the
// next page at itself (2 % before it when it left on the left). A manual change of the window stops
// paging until playback restarts.
struct Paging {
	bool playWasInView = false;
	bool wasPlaying = false;
	bool manualView = false;
};

inline void NoteManualView(Paging& p, bool playing) {
	p.manualView = true;
	p.wasPlaying = playing;
}

// True when it moved the window.
inline bool FollowPlayback(Span& v, const Span& d, Paging& p, double playhead, bool playing, bool scrubbing) {
	const bool inside = v.Contains(playhead);
	if (!playing || scrubbing) {
		p.playWasInView = inside;
		p.wasPlaying = false;
		return false;
	}
	const bool started = !p.wasPlaying;
	p.wasPlaying = true;
	if (started) p.manualView = false;
	if (p.manualView) { p.playWasInView = inside; return false; }
	if (inside) { p.playWasInView = true; return false; }
	if (!started && !p.playWasInView) return false;
	const double span = v.Len();
	v.t0 = playhead > v.t1 ? playhead : playhead - span * 0.02;
	v.t1 = v.t0 + span;
	v = Normalize(v, d);
	p.playWasInView = true;
	return true;
}

// Time snap (snapStep, snapTick): one step for the playhead, key placement and key drags; Alt bypasses it.
constexpr int kSnapOff = 0;
constexpr int kSnapAuto = -1;

// 1, 2 or 5 times a power of ten, at least 1.
inline double NiceSnap(double v) {
	const double p = std::pow(10.0, std::floor(std::log10((std::max)(1.0, v))));
	const double n = v / p;
	const double m = n < 1.5 ? 1 : n < 3.5 ? 2 : n < 7.5 ? 5 : 10;
	return (std::max)(1.0, std::round(m * p));
}

// The step in ticks for a setting (kSnapOff, kSnapAuto or a tick count); 0 is no snap.
inline double SnapStep(int setting, const Span& view) {
	if (setting == kSnapOff) return 0;
	if (setting == kSnapAuto) return NiceSnap(view.Len() / 40);
	return setting >= 1 ? (double)setting : 0;
}

inline double SnapTick(double t, double step, bool alt) {
	if (step <= 0 || alt) return t;
	return (std::max)(0.0, std::round(t / step) * step);
}

// Ruler steps in ticks (rulerSteps): the first label step of 1/16 s .. 20 min that is at least 70 px
// wide, and its minor step.
struct RulerStep {
	double major = 0, minor = 0;
};

inline RulerStep RulerSteps(const Span& view, float cw, double tickRate) {
	static const double kSeconds[] = { 0.0625, 0.125, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200 };
	const int n = (int)(sizeof(kSeconds) / sizeof(kSeconds[0]));
	const double pxPerSec = cw / (std::max)(1e-9, view.Len() / tickRate);
	double major = kSeconds[n - 1];
	for (int i = 0; i < n; ++i)
		if (kSeconds[i] * pxPerSec >= 70) { major = kSeconds[i]; break; }
	const double minor = major >= 10 ? major / 5 : (major >= 1 ? major / (major == 2 ? 2 : 5) : major / 2);
	RulerStep r;
	r.major = major * tickRate;
	r.minor = (std::max)(1.0, minor * tickRate);
	return r;
}

// "m:ss.cc" counted from tick 0 (fmtTime); the short form drops a trailing ".00".
inline void FormatTime(double tick, double tickRate, char* out, size_t size, bool shortForm = false) {
	double s = (std::max)(0.0, tick) / (tickRate > 1 ? tickRate : 64);
	const int m = (int)std::floor(s / 60);
	s -= m * 60;
	std::snprintf(out, size, "%d:%05.2f", m, s);
	if (shortForm) {
		const size_t len = std::strlen(out);
		if (len > 3 && out[len - 3] == '.' && out[len - 2] == '0' && out[len - 1] == '0') out[len - 3] = 0;
	}
}

// A value as CS2's fmtNum shows it: smaller than half the last decimal is 0, never "-0.0".
inline double CleanValue(double v, int decimals) {
	if (!std::isfinite(v)) return 0;
	return std::fabs(v) < std::pow(10.0, -decimals) / 2 ? 0 : v;
}

// One key of a channel, as a row lists it.
struct KeyRef {
	int ch = 0, id = 0;
	double t = 0;
	bool sel = false;
};

// Keys of several channels merged by time, one entry per half tick (unionKeys): what a
// Position row (X, Y and Z) and a collapsed layer show. Sorted by time; the time is the first member's.
struct MergedKey {
	double t = 0;
	bool sel = false;
	std::vector<KeyRef> members;
};

inline std::vector<MergedKey> MergeKeys(const std::vector<KeyRef>& keys) {
	std::vector<MergedKey> out;
	std::vector<double> slots; // the half-tick slot of each entry
	for (const KeyRef& k : keys) {
		const double slot = std::round(k.t * 2) / 2;
		size_t i = 0;
		while (i < slots.size() && slots[i] != slot) ++i;
		if (i == slots.size()) {
			slots.push_back(slot);
			MergedKey e;
			e.t = k.t;
			out.push_back(e);
		}
		out[i].members.push_back(k);
		if (k.sel) out[i].sel = true;
	}
	std::stable_sort(out.begin(), out.end(), [](const MergedKey& a, const MergedKey& b) { return a.t < b.t; });
	return out;
}

// The nearest key time strictly before (dir < 0) or after the playhead, a quarter tick counting as
// on it (nearestTime). False when there is none.
inline bool NeighbourTime(const std::vector<double>& times, double t, int dir, double& out) {
	bool found = false;
	for (double kt : times) {
		if (dir < 0 ? kt < t - 0.25 && (!found || kt > out) : kt > t + 0.25 && (!found || kt < out)) {
			out = kt;
			found = true;
		}
	}
	return found;
}

// A key glyph is two halves, each saying how the curve meets the key on that side (glyphHalf):
// Linear a half diamond, Auto (Smooth) a half disc, Ease (Easy Ease or a hand-dragged handle) a half
// square with a notch, Hold a half square. The kind is the stored one, never read off the slope.
enum class Side { Linear, Auto, Ease, Hold };

inline Side SideOf(bool active, bool autoTan) {
	if (autoTan) return Side::Auto;
	return active ? Side::Ease : Side::Linear;
}

// What the toolbar's interpolation strip shows as current for a key (easeOfSides): an eased side
// names the key before an auto one does.
enum class EaseKind { Smooth = 0, Linear, In, Out, InOut, Hold, Count };

inline EaseKind EaseOfSides(Side left, Side right) {
	if (right == Side::Hold) return EaseKind::Hold;
	if (left == Side::Ease && right == Side::Ease) return EaseKind::InOut;
	if (left == Side::Ease) return EaseKind::In;
	if (right == Side::Ease) return EaseKind::Out;
	if (left == Side::Auto || right == Side::Auto) return EaseKind::Smooth;
	return EaseKind::Linear;
}

// A camera channel's display range (GraphEditorExperimentHud.cpp): the keys' centre plus and
// minus half of 1.5 times their spread, at least the channel's floor (64 position, 20 angles, 10 FOV).
// Handles never widen it. No keys: around 0.5.
inline void ChannelRange(const std::vector<double>& values, int ch, double& lo, double& hi) {
	static const double kMinSpanOf[7] = { 64, 64, 64, 20, 20, 20, 10 };
	double a = 0, b = 1;
	if (!values.empty()) {
		a = b = values[0];
		for (double v : values) { a = (std::min)(a, v); b = (std::max)(b, v); }
	}
	const double c = (a + b) / 2;
	const double span = (std::max)((b - a) * 1.5, kMinSpanOf[ch < 0 || ch > 6 ? 0 : ch]);
	lo = c - span / 2;
	hi = c + span / 2;
}

// The overview's window box, in overview px, at least 8 wide (ovViewRect).
inline void OverviewBox(const Span& view, const Span& domain, float ow, float tw, float& x0, float& x1) {
	x0 = (float)DomToX(domain, ow, view.t0);
	x1 = (float)DomToX(domain, ow, view.t1);
	if (x0 < 0) x0 = 0;
	if (x1 > tw) x1 = tw;
	if (x1 < x0 + 8) x1 = (std::min)(tw, x0 + 8);
}

// The footer scrollbar's thumb on a trackW track (layoutNav).
inline void FooterThumb(const Span& view, const Span& domain, float trackW, float& x, float& w) {
	const double domSpan = (std::max)(1.0, domain.Len());
	w = (float)(std::max)(10.0, (std::min)((double)trackW, view.Len() / domSpan * trackW));
	const double frac = (std::max)(0.0, (std::min)(1.0, (view.t0 - domain.t0) / domSpan));
	x = (float)(std::min)((double)trackW - w, frac * trackW);
}

// The rows' scroll: kept inside the content and its thumb (layoutRowsScroll).
inline float ClampRowScroll(float scroll, float contentH) {
	const float maxScroll = (std::max)(0.0f, contentH - kRowsH);
	return (std::max)(0.0f, (std::min)(maxScroll, scroll));
}

inline void RowsThumb(float scroll, float contentH, float& y, float& h) {
	h = (std::max)(24.0f, kRowsH * kRowsH / (std::max)(contentH, kRowsH));
	const float travel = contentH - kRowsH;
	y = travel > 0 ? (kRowsH - h) * (scroll / travel) : 0;
}

} // namespace TimelineDockMath
} // namespace Ui
} // namespace SourceFilmmaker
