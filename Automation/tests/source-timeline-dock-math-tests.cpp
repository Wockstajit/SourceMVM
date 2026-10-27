// TimelineDockMath, the CS:S TIMELINE dock's rules from CS2's dock: widths, the time window and its
// navigation, playback paging, time snap, ruler steps, the time format, merged key times, key jumps,
// the key glyph's kinds, a camera channel's display range, and the scrollbars.

#include "../../AfxHookSource/Filmmaker/Ui/TimelineDockMath.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace SourceFilmmaker::Ui::TimelineDockMath;

static int g_failed = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n", __LINE__, #x); ++g_failed; } } while (0)

static bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

static void TestWidths() {
	// 1600x900 with CS2's default inspector: a 1321 px dock.
	const Widths w = WidthsFor(1321);
	CHECK(Near(w.tw, 1013) && Near(w.ow, 1025) && Near(w.cw, 989));
	// A narrow dock keeps a usable track.
	const Widths n = WidthsFor(300);
	CHECK(Near(n.tw, 120) && Near(n.cw, 96));
	CHECK(Near(kRowsY, 112) && Near(kRowsH, 302));
}

static void TestTickMapping() {
	const Span v = MakeSpan(1000, 3000);
	const float cw = 989;
	CHECK(Near(TickToX(v, cw, 1000), kPad));
	CHECK(Near(TickToX(v, cw, 3000), kPad + cw));
	for (double t = 900; t <= 3100; t += 137) CHECK(Near(XToTick(v, cw, TickToX(v, cw, t)), t, 1e-6));
	const Span d = MakeSpan(0, 10000);
	CHECK(Near(DomToX(d, 1025, 5000), 512.5));
	CHECK(Near(XToDom(d, 1025, DomToX(d, 1025, 1234)), 1234, 1e-6));
}

static void TestNormalize() {
	const Span d = MakeSpan(0, 10000);
	// Too narrow grows to 24 ticks about its centre.
	Span v = Normalize(MakeSpan(500, 510), d);
	CHECK(Near(v.Len(), kMinSpan) && Near((v.t0 + v.t1) / 2, 505));
	// Wider than the demo is the whole demo.
	v = Normalize(MakeSpan(-5000, 20000), d);
	CHECK(Near(v.t0, 0) && Near(v.t1, 10000));
	// Past either end is pushed back inside, keeping its length.
	v = Normalize(MakeSpan(-300, 700), d);
	CHECK(Near(v.t0, 0) && Near(v.t1, 1000));
	v = Normalize(MakeSpan(9500, 10500), d);
	CHECK(Near(v.t0, 9000) && Near(v.t1, 10000));
	// A domain shorter than 24 ticks: the window is the domain.
	v = Normalize(MakeSpan(0, 1), MakeSpan(0, 10));
	CHECK(Near(v.t0, 0) && Near(v.t1, 10));
	// No demo: the window and playhead make the domain, else 0..2000.
	const Span nd = DomainFor(0, 0, MakeSpan(100, 300), 900);
	CHECK(Near(nd.t0, 100) && Near(nd.t1, 900));
	CHECK(Near(DomainFor(0, 0, MakeSpan(0, 0), 0).t1, 2000));
	// A seed under half a tick opens on the playhead.
	v = SeedView(500, 500.2, 4000, d);
	CHECK(Near(v.t0, 3000) && Near(v.t1, 5000));
}

static void TestZoomAndPan() {
	const Span d = MakeSpan(0, 10000);
	const Span v = MakeSpan(1000, 3000);
	const float cw = 989;
	// Zooming keeps the anchor under the same x.
	const double anchor = 1600;
	const Span z = ZoomAt(v, d, 0.5, anchor);
	CHECK(Near(z.Len(), 1000));
	CHECK(Near(TickToX(z, cw, anchor), TickToX(v, cw, anchor), 1e-6));
	// The footer's buttons zoom about the playhead when it is in view, else about the centre.
	const Span a = ZoomStep(v, d, 1.5, 1200);
	CHECK(Near(TickToX(a, cw, 1200), TickToX(v, cw, 1200), 1e-6));
	const Span b = ZoomStep(v, d, 1.5, 9000);
	CHECK(Near((b.t0 + b.t1) / 2, 2000));
	// Pan stops at the demo's ends.
	const Span p = Pan(v, d, -5000);
	CHECK(Near(p.t0, 0) && Near(p.t1, 2000));
}

static void TestFrameAndReveal() {
	const Span d = MakeSpan(0, 10000);
	// A range is padded 8 %, at least 32 ticks.
	Span f = FrameTicks(1000, 3000, d);
	CHECK(Near(f.t0, 840) && Near(f.t1, 3160));
	f = FrameTicks(1000, 1100, d);
	CHECK(Near(f.t0, 968) && Near(f.t1, 1132));
	// A single key is framed 96 wide about it.
	f = FrameTicks(500, 500, d);
	CHECK(Near(f.t0, 452) && Near(f.t1, 548));

	// Already in view: nothing moves.
	Span v = MakeSpan(1000, 2000);
	CHECK(!Reveal(v, d, 1500, 1500));
	CHECK(Near(v.t0, 1000));
	// Off the right: the window shifts so the key lands 2 % inside, same width.
	CHECK(Reveal(v, d, 2500, 2500));
	CHECK(Near(v.t1, 2520) && Near(v.Len(), 1000));
	// Off the left: 2 % inside the left edge.
	v = MakeSpan(1000, 2000);
	CHECK(Reveal(v, d, 300, 300));
	CHECK(Near(v.t0, 280) && Near(v.Len(), 1000));
	// Too wide to fit: framed instead.
	v = MakeSpan(1000, 2000);
	CHECK(Reveal(v, d, 1000, 5000));
	CHECK(Near(v.t0, 680) && Near(v.t1, 5320));
}

static void TestPaging() {
	const Span d = MakeSpan(0, 10000);
	Span v = MakeSpan(1000, 2000);
	Paging p;
	// Paused: the window never follows.
	CHECK(!FollowPlayback(v, d, p, 2500, false, false));
	// Playback starts inside, plays past the right edge: the next page starts at the playhead.
	CHECK(!FollowPlayback(v, d, p, 1900, true, false));
	CHECK(FollowPlayback(v, d, p, 2001, true, false));
	CHECK(Near(v.t0, 2001) && Near(v.Len(), 1000));
	// A scrub never pages.
	CHECK(!FollowPlayback(v, d, p, 5000, true, true));
	// A manual change suspends paging while playback continues...
	v = MakeSpan(1000, 2000);
	CHECK(!FollowPlayback(v, d, p, 1500, true, false));
	NoteManualView(p, true);
	CHECK(!FollowPlayback(v, d, p, 2600, true, false));
	CHECK(Near(v.t0, 1000));
	// ...until it restarts: then a playhead outside pages at once.
	CHECK(!FollowPlayback(v, d, p, 2600, false, false));
	CHECK(FollowPlayback(v, d, p, 2600, true, false));
	CHECK(Near(v.t0, 2600));
	// Leaving on the left (a loop wrapping): the page starts 2 % before the playhead.
	v = MakeSpan(1000, 2000);
	Paging q;
	CHECK(!FollowPlayback(v, d, q, 1500, true, false));
	CHECK(FollowPlayback(v, d, q, 400, true, false));
	CHECK(Near(v.t0, 380));
}

static void TestSnap() {
	CHECK(Near(NiceSnap(0.3), 1) && Near(NiceSnap(1.4), 1) && Near(NiceSnap(1.6), 2));
	CHECK(Near(NiceSnap(3.4), 2) && Near(NiceSnap(3.6), 5) && Near(NiceSnap(7.4), 5) && Near(NiceSnap(7.6), 10));
	CHECK(Near(NiceSnap(240), 200) && Near(NiceSnap(400), 500));
	const Span v = MakeSpan(0, 2000);
	CHECK(SnapStep(kSnapOff, v) == 0);
	CHECK(Near(SnapStep(kSnapAuto, v), 50)); // 2000 / 40
	CHECK(Near(SnapStep(100, v), 100));
	CHECK(Near(SnapTick(149, 100, false), 100) && Near(SnapTick(151, 100, false), 200));
	CHECK(Near(SnapTick(151, 100, true), 151)); // Alt places finely
	CHECK(Near(SnapTick(151.4, 0, false), 151.4)); // no snap
	CHECK(Near(SnapTick(-40, 100, false), 0));
}

static void TestRuler() {
	// 2000 ticks at 64 tps across 989 px: 31.6 px a second, so 5 s labels (158 px) with 1 s minors.
	RulerStep r = RulerSteps(MakeSpan(0, 2000), 989, 64);
	CHECK(Near(r.major, 320) && Near(r.minor, 64));
	// Zoomed to 100 ticks: 633 px a second, so 1/8 s labels (79 px) and 1/16 s minors (4 ticks).
	r = RulerSteps(MakeSpan(0, 100), 989, 64);
	CHECK(Near(r.major, 8) && Near(r.minor, 4));
	// 2 s labels split in halves.
	r = RulerSteps(MakeSpan(0, 64 * 26), 989, 64);
	CHECK(Near(r.major, 128) && Near(r.minor, 64));
	// The minor never goes under one tick.
	r = RulerSteps(MakeSpan(0, 30), 989, 10);
	CHECK(r.minor >= 1);
	// A whole match: 20 minute labels at most.
	r = RulerSteps(MakeSpan(0, 64 * 3600 * 10), 989, 64);
	CHECK(Near(r.major, 1200 * 64));
}

static void TestTimeText() {
	char text[32];
	FormatTime(790, 64, text, sizeof(text));
	CHECK(0 == std::strcmp(text, "0:12.34"));
	FormatTime(3840, 64, text, sizeof(text));
	CHECK(0 == std::strcmp(text, "1:00.00"));
	FormatTime(3840, 64, text, sizeof(text), true);
	CHECK(0 == std::strcmp(text, "1:00"));
	FormatTime(3872, 64, text, sizeof(text), true);
	CHECK(0 == std::strcmp(text, "1:00.50"));
	FormatTime(-50, 64, text, sizeof(text));
	CHECK(0 == std::strcmp(text, "0:00.00"));
	FormatTime(66.666, 0, text, sizeof(text)); // no rate yet: 64
	CHECK(0 == std::strcmp(text, "0:01.04"));
	CHECK(CleanValue(-0.04, 1) == 0 && Near(CleanValue(-0.06, 1), -0.06));
	CHECK(CleanValue(0.4, 0) == 0 && Near(CleanValue(0.6, 0), 0.6));
}

static void TestMergeKeys() {
	std::vector<KeyRef> keys;
	auto add = [&](int ch, int id, double t, bool sel) { KeyRef k; k.ch = ch; k.id = id; k.t = t; k.sel = sel; keys.push_back(k); };
	add(0, 1, 300, false);
	add(0, 2, 100, false);
	add(1, 3, 100.2, true);  // same half tick as 100
	add(2, 4, 100.3, false); // the next half tick
	add(1, 5, 300, false);
	const std::vector<MergedKey> m = MergeKeys(keys);
	CHECK(m.size() == 3);
	CHECK(Near(m[0].t, 100) && m[0].members.size() == 2 && m[0].sel);
	CHECK(Near(m[1].t, 100.3) && m[1].members.size() == 1 && !m[1].sel);
	CHECK(Near(m[2].t, 300) && m[2].members.size() == 2 && !m[2].sel);
	CHECK(MergeKeys(std::vector<KeyRef>()).empty());
}

static void TestNeighbourTime() {
	const std::vector<double> times = { 100, 200, 300 };
	double t = 0;
	CHECK(NeighbourTime(times, 200, 1, t) && Near(t, 300));
	CHECK(NeighbourTime(times, 200, -1, t) && Near(t, 100));
	CHECK(NeighbourTime(times, 199.8, 1, t) && Near(t, 300)); // on 200 within a quarter tick
	CHECK(NeighbourTime(times, 199.7, 1, t) && Near(t, 200));
	CHECK(!NeighbourTime(times, 300, 1, t));
	CHECK(!NeighbourTime(times, 100, -1, t));
	CHECK(!NeighbourTime(std::vector<double>(), 5, 1, t));
}

static void TestGlyphKinds() {
	CHECK(SideOf(false, false) == Side::Linear);
	CHECK(SideOf(true, false) == Side::Ease);
	CHECK(SideOf(true, true) == Side::Auto);
	CHECK(SideOf(false, true) == Side::Auto); // an end key's missing side still reads as Smooth
	CHECK(EaseOfSides(Side::Auto, Side::Auto) == EaseKind::Smooth);
	CHECK(EaseOfSides(Side::Linear, Side::Linear) == EaseKind::Linear);
	CHECK(EaseOfSides(Side::Ease, Side::Ease) == EaseKind::InOut);
	CHECK(EaseOfSides(Side::Ease, Side::Auto) == EaseKind::In);  // an eased side names the key first
	CHECK(EaseOfSides(Side::Auto, Side::Ease) == EaseKind::Out);
	CHECK(EaseOfSides(Side::Linear, Side::Hold) == EaseKind::Hold);
}

static void TestChannelRange() {
	double lo = 0, hi = 0;
	ChannelRange(std::vector<double>(), 0, lo, hi);
	CHECK(Near(lo, 0.5 - 32) && Near(hi, 0.5 + 32));
	ChannelRange({ 100 }, 6, lo, hi); // FOV: at least 10 wide
	CHECK(Near(lo, 95) && Near(hi, 105));
	ChannelRange({ 0, 100 }, 0, lo, hi); // spread 100: 150 wide about 50
	CHECK(Near(lo, -25) && Near(hi, 125));
	ChannelRange({ 10, 12 }, 4, lo, hi); // yaw: at least 20 wide
	CHECK(Near(lo, 1) && Near(hi, 21));
}

static void TestScrollbars() {
	const Span d = MakeSpan(0, 10000);
	float x0 = 0, x1 = 0;
	OverviewBox(MakeSpan(5000, 5001), d, 1025, 1013, x0, x1);
	CHECK(Near(x1 - x0, 8, 1e-4)); // never thinner than 8 px
	OverviewBox(MakeSpan(0, 10000), d, 1025, 1013, x0, x1);
	CHECK(Near(x0, 0) && Near(x1, 1013)); // clipped to the track
	float x = 0, w = 0;
	FooterThumb(MakeSpan(0, 10000), d, 220, x, w);
	CHECK(Near(w, 220) && Near(x, 0));
	FooterThumb(MakeSpan(5000, 6000), d, 220, x, w);
	CHECK(Near(w, 22) && Near(x, 110));
	FooterThumb(MakeSpan(9999, 10000), d, 220, x, w);
	CHECK(Near(w, 10) && Near(x, 210)); // at least 10 wide, inside the track
	// Rows: no overflow, no travel; overflow scrolls within the content.
	CHECK(ClampRowScroll(50, 200) == 0);
	CHECK(Near(ClampRowScroll(500, 400), 98) && ClampRowScroll(-5, 400) == 0);
	float y = 0, h = 0;
	RowsThumb(0, 604, y, h);
	CHECK(Near(h, 151) && Near(y, 0));
	RowsThumb(302, 604, y, h);
	CHECK(Near(y, 151));
}

int main() {
	TestWidths();
	TestTickMapping();
	TestNormalize();
	TestZoomAndPan();
	TestFrameAndReveal();
	TestPaging();
	TestSnap();
	TestRuler();
	TestTimeText();
	TestMergeKeys();
	TestNeighbourTime();
	TestGlyphKinds();
	TestChannelRange();
	TestScrollbars();
	if (g_failed) {
		std::printf("%d check(s) failed\n", g_failed);
		return 1;
	}
	std::printf("source timeline dock math: all checks passed\n");
	return 0;
}
