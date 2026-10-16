// VpScaleBreakMath.h: which presents break the scaled workspace preview, and how they are counted.
#include "../../AfxHookSource2/Filmmaker/Panorama/VpScaleBreakMath.h"

#include <cstdio>

using namespace Filmmaker::VpScaleBreak;

namespace {
int g_failed = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAILED line %d: %s\n", __LINE__, #c); ++g_failed; } } while (0)

// A 1768x865 window with the editor's preview in the top-left 70%, blitted where it was asked.
Present Healthy() {
	Present p;
	p.active = true; p.blitRan = true;
	p.bbW = 1768; p.bbH = 865; p.clientW = 1768; p.clientH = 865;
	p.rect[0] = 0.0f; p.rect[1] = 0.054f; p.rect[2] = 0.707f; p.rect[3] = 0.760f;
	p.vp[0] = 0.0f; p.vp[1] = 0.054f * 865; p.vp[2] = 0.707f * 1768; p.vp[3] = (0.760f - 0.054f) * 865;
	return p;
}

void TestClassify() {
	CHECK(Classify(Healthy()) == None);
	Present missed = Healthy(); missed.blitRan = false;
	CHECK(Classify(missed) == MissedBlit);
	Present recording = missed; recording.recording = true;
	CHECK(Classify(recording) == None); // scaling is off on purpose while a capture runs
	Present off = missed; off.active = false;
	CHECK(Classify(off) == None);       // no preview asked for: nothing to miss
	Present stale = Healthy(); stale.vp[2] += 40;
	CHECK(Classify(stale) == StaleRect);
	Present close = Healthy(); close.vp[2] += 1.5f;
	CHECK(Classify(close) == None);     // within a pixel or two is the same rect
	Present stretched = Healthy(); stretched.clientW = 2560; stretched.clientH = 1351;
	CHECK(Classify(stretched) == Stretched);
	stretched.active = false;
	CHECK(Classify(stretched) == Stretched); // a stretched window breaks with or without a preview
}

void TestTracker() {
	Tracker t;
	CHECK(t.Step(None) == Tracker::Event::None);
	// A missed blit opens a break at once and is counted once while it lasts.
	CHECK(t.Step(MissedBlit) == Tracker::Event::Opened);
	CHECK(t.breaks == 1 && t.perReason[0] == 1);
	CHECK(t.Step(MissedBlit) == Tracker::Event::None);
	CHECK(t.perReason[0] == 1 && t.presentsOpen == 2);
	CHECK(t.Step(None) == Tracker::Event::Closed);
	t.ResetLength();
	// A stretch has to last kSettlePresents presents: a resize in flight is not a break yet.
	for (int i = 1; i < kSettlePresents; ++i) CHECK(t.Step(Stretched) == Tracker::Event::None);
	CHECK(t.Step(None) == Tracker::Event::None);
	CHECK(t.breaks == 1);
	for (int i = 1; i < kSettlePresents; ++i) t.Step(Stretched);
	CHECK(t.Step(Stretched) == Tracker::Event::Opened);
	CHECK(t.breaks == 2 && t.perReason[2] == 1);
	// A second reason joining an open break changes it without opening another.
	CHECK(t.Step(Stretched | MissedBlit) == Tracker::Event::Changed);
	CHECK(t.breaks == 2 && t.perReason[0] == 2);
	CHECK(t.Step(None) == Tracker::Event::Closed);
}
} // namespace

int main() {
	TestClassify();
	TestTracker();
	if (g_failed) { std::printf("%d check(s) failed\n", g_failed); return 1; }
	std::printf("vp-scale-break-math-tests: all passed\n");
	return 0;
}
