#include "../../AfxHookSource2/Filmmaker/EasterEggs/Osu/OsuMath.h"

#include <cmath>
#include <iostream>
#include <string>

using namespace Filmmaker::Osu;

namespace {

int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED line " << __LINE__ << ": " #cond "\n"; ++g_failed; } } while (0)
bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

const char* kMap =
	"osu file format v14\n"
	"\n"
	"[General]\n"
	"AudioFilename: audio.mp3\n"
	"AudioLeadIn: 0\n"
	"Mode: 0\n"
	"\n"
	"[Metadata]\n"
	"Title:Test Song\n"
	"Artist:Someone\n"
	"Creator:Mapper\n"
	"Version:Insane\n"
	"\n"
	"[Difficulty]\n"
	"HPDrainRate:5\n"
	"CircleSize:4\n"
	"OverallDifficulty:8\n"
	"ApproachRate:9\n"
	"SliderMultiplier:1.4\n"
	"SliderTickRate:1\n"
	"\n"
	"[Events]\n"
	"0,0,\"bg.jpg\",0,0\n"
	"\n"
	"[TimingPoints]\n"
	"0,500,4,2,0,60,1,0\n"
	"2000,-50,4,2,0,60,0,0\n"
	"\n"
	"[Colours]\n"
	"Combo1 : 255,0,0\n"
	"Combo2 : 0,255,0\n"
	"\n"
	"[HitObjects]\n"
	"100,100,1000,1,0,0:0:0:0:\n"
	"200,100,1500,1,0,0:0:0:0:\n"
	"100,200,2000,6,0,L|240:200,1,140\n"         // new combo slider, SV x2 from 2000
	"256,192,4000,12,0,6000,0:0:0:0:\n"         // spinner (new combo)
	"50,50,7000,2,0,P|100:0|150:50,2,100\n";   // perfect-circle slider, one repeat

void TestFormulas() {
	CHECK(Near(Preempt(9), 600.0));
	CHECK(Near(Preempt(5), 1200.0));
	CHECK(Near(Preempt(0), 1800.0));
	CHECK(Near(FadeIn(9), 400.0));
	CHECK(Near(FadeIn(0), 400.0) && Near(FadeIn(10), 400.0));        // osu!: 400 * min(1, preempt/450)
	CHECK(Near(Preempt(9.5), 525.0) && Near(Preempt(8.7), 645.0));    // truncated to whole ms
	CHECK(Near(Radius(4), 36.48 * 1.00041, 1e-9));
	const Windows w = HitWindows(8);
	CHECK(Near(w.w300, 31.5) && Near(w.w100, 75.5) && Near(w.w50, 119.5));
	const Windows w7 = HitWindows(7.5);                                // floor(35)-0.5, floor(80)-0.5
	CHECK(Near(w7.w300, 34.5) && Near(w7.w100, 79.5) && Near(w7.w50, 124.5));
	CHECK(Judge(-20, w) == 300 && Judge(50, w) == 100 && Judge(-110, w) == 50);
	CHECK(Judge(121, w) == 0 && Judge(-399, w) == 0);                 // inside the 400 ms miss window
	CHECK(Judge(-401, w) == -1);                                      // too early: ignored
	CHECK(Near(Accuracy(10, 0, 0, 0), 1.0));
	CHECK(Near(Accuracy(1, 1, 1, 1), (300.0 + 100.0 + 50.0) / 1200.0));
	CHECK(std::string(Grade(10, 0, 0, 0)) == "SS");
	CHECK(std::string(Grade(95, 5, 0, 0)) == "S");
	CHECK(std::string(Grade(95, 4, 0, 1)) == "A");
	CHECK(std::string(Grade(10, 90, 0, 0)) == "D");
	CHECK(SpinsRequired(2000, 10) > SpinsRequired(2000, 0));
}

void TestParse() {
	Beatmap b;
	std::string err;
	CHECK(ParseBeatmap(kMap, b, &err));
	CHECK(b.title == "Test Song" && b.artist == "Someone" && b.version == "Insane" && b.audio == "audio.mp3");
	CHECK(b.background == "bg.jpg");
	CHECK(Near(b.ar, 9) && Near(b.cs, 4) && Near(b.od, 8));
	CHECK(b.colours.size() == 2 && b.colours[0] == 0xFF0000);
	CHECK(b.objects.size() == 5);
	// Combo numbering: 1 2 | 1 (slider, new combo) | 1 (spinner) | 1 (after a spinner)
	CHECK(b.objects[0].comboNumber == 1 && b.objects[1].comboNumber == 2);
	CHECK(b.objects[2].comboNumber == 1 && b.objects[2].comboIndex == 1);
	CHECK(b.objects[3].type == kSpinner && Near(b.objects[3].endTime, 6000));
	CHECK(b.objects[4].comboNumber == 1);

	// Linear slider at 2000: SV = -100/-50 = 2 -> 280 px per beat; 140 px = half a beat = 250 ms.
	const HitObject& s = b.objects[2];
	CHECK(Near(s.span, 250.0));
	CHECK(Near(s.endTime, 2250.0));
	const Vec2 end = PathAt(s.path, 1.0);
	CHECK(Near(end.x, 240.0, 1e-6) && Near(end.y, 200.0, 1e-6));
	const Vec2 mid = SliderBall(s, 2125.0);
	CHECK(Near(mid.x, 170.0, 0.5) && Near(mid.y, 200.0, 0.5));
	CHECK(!s.checkpoints.empty() && s.checkpoints.back() <= s.endTime);

	// Perfect-circle slider with a repeat: 2 passes, ball comes back to the head at the end.
	const HitObject& p = b.objects[4];
	CHECK(p.slides == 2);
	CHECK(Near(p.endTime, p.time + 2 * p.span));
	const Vec2 back = SliderBall(p, p.endTime);
	CHECK(Near(back.x, 50.0, 0.5) && Near(back.y, 50.0, 0.5));
	// Every resampled point lies on the circle through the three control points.
	const double r = Len(Vec2{ 50, 50 } - Vec2{ 100, 50 });
	for (const Vec2& q : p.path) CHECK(Near(Len(q - Vec2{ 100, 50 }), r, 0.6));
}

void TestPathTruncateAndExtend() {
	// A 100 px straight curve asked to be 60 px long ends at 60; asked for 130 it carries on.
	HitObject o;
	o.type = kSlider; o.curve = 'L'; o.pos = { 0, 0 }; o.control = { { 100, 0 } };
	auto shortPath = ResamplePath(RawCurve(o), 60);
	CHECK(Near(shortPath.back().x, 60.0, 1e-6));
	auto longPath = ResamplePath(RawCurve(o), 130);
	CHECK(Near(longPath.back().x, 130.0, 1e-6));
	// Resampling spacing is even.
	CHECK(Near(Len(longPath[1] - longPath[0]), kPathStep, 1e-6));
}

void TestRejectsOtherModes() {
	std::string mania = kMap;
	mania.replace(mania.find("Mode: 0"), 7, "Mode: 3");
	Beatmap b;
	CHECK(!ParseBeatmap(mania, b));
}

void TestMods() {
	// Toggling respects exclusions: HR drops EZ, DT drops HT, AT drops NF.
	uint32_t m = ToggleMod(0, kModEZ);
	m = ToggleMod(m, kModHR);
	CHECK((m & kModHR) && !(m & kModEZ));
	m = ToggleMod(ToggleMod(0, kModHT), kModDT);
	CHECK((m & kModDT) && !(m & kModHT));
	m = ToggleMod(ToggleMod(0, kModNF), kModAT);
	CHECK((m & kModAT) && !(m & kModNF));
	CHECK(ToggleMod(kModHD, kModHD) == 0);
	CHECK(Near(ModRate(kModDT), 1.5) && Near(ModRate(kModNC), 1.5) && Near(ModRate(kModHT), 0.75) && Near(ModRate(0), 1.0));
	CHECK(Near(ModMultiplier(kModHD | kModHR), 1.06 * 1.06));
	double cs = 4, ar = 9, od = 8, hp = 6;
	ApplyModsToDifficulty(kModHR, cs, ar, od, hp);
	CHECK(Near(cs, 5.2) && Near(ar, 10.0) && Near(od, 10.0) && Near(hp, 8.4));
	cs = 4; ar = 9; od = 8; hp = 6;
	ApplyModsToDifficulty(kModEZ, cs, ar, od, hp);
	CHECK(Near(cs, 2) && Near(ar, 4.5) && Near(od, 4) && Near(hp, 3));
	// AR9 + DT feels like AR ~10.33 (the well-known value).
	CHECK(Near(EffectiveAr(9, 1.5), 10.333333, 1e-3));
	CHECK(Near(ArFromPreempt(Preempt(7)), 7.0, 1e-9) && Near(ArFromPreempt(Preempt(3)), 3.0, 1e-9));
	// OD8 + DT: the 300 window 32ms -> 21.33ms -> OD 9.78.
	CHECK(Near(EffectiveOd(8, 1.5), (80.0 - 32.0 / 1.5) / 6.0));
}

void TestHiddenAndDrain() {
	const double pre = 1000;
	CHECK(Near(HiddenAlpha(0, 1000, pre), 0.0));           // appears at t-preempt
	CHECK(Near(HiddenAlpha(400, 1000, pre), 1.0));         // fully in at 40%
	CHECK(Near(HiddenAlpha(550, 1000, pre), 0.5));         // halfway out
	CHECK(Near(HiddenAlpha(700, 1000, pre), 0.0));         // gone before the hit time
	Beatmap b;
	CHECK(ParseBeatmap(kMap, b));
	CHECK(!InDrainTime(b, 500));                            // before the first object: no drain
	CHECK(InDrainTime(b, 1200));
	b.breaks.push_back({ 2500, 3800 });
	CHECK(!InDrainTime(b, 3000));                           // a break: no drain
	CHECK(!InDrainTime(b, 1e9));
}

void TestSummaryAndFastParse() {
	Beatmap full, fast;
	CHECK(ParseBeatmap(kMap, full));
	CHECK(ParseBeatmap(kMap, fast, nullptr, false));
	CHECK(fast.objects[2].path.empty());                    // no paths in the fast parse...
	CHECK(Near(fast.objects[2].endTime, full.objects[2].endTime)); // ...but the same timing
	const Summary s = Summarize(fast);
	CHECK(s.circles == 2 && s.sliders == 2 && s.spinners == 1);
	CHECK(Near(s.bpmMain, 120.0));
	CHECK(Near(s.firstMs, 1000.0));
}

void TestPlayfield() {
	const Playfield p = PlayfieldFor(1920, 1080);
	CHECK(Near(p.scale, 1080 * 0.8 / 384));
	CHECK(Near(p.ox + 512 * p.scale * 0.5, 960.0));
	CHECK(Near(p.oy, (1080 - 384 * p.scale) * 0.5 + 8 * p.scale)); // osu!'s 8 px downward shift
	const Playfield tall = PlayfieldFor(1000, 1600);                 // narrower than 4:3: width-bound
	CHECK(Near(tall.scale, 1000 * 0.8 / 512));
}

void TestStacking() {
	// Three circles on one spot 100 ms apart: osu! fans the EARLIER ones up-left by r/10 per level.
	const char* text =
		"osu file format v14\n[General]\nStackLeniency: 0.7\n[Difficulty]\nCircleSize:4\nApproachRate:9\n"
		"[TimingPoints]\n0,500,4,2,0,100,1,0\n"
		"[HitObjects]\n100,100,1000,1,0\n100,100,1100,1,0\n100,100,1200,1,0\n300,300,5000,1,0\n";
	Beatmap b;
	CHECK(ParseBeatmap(text, b));
	ApplyStacking(b);
	const double step = Radius(4) / 10.0;
	CHECK(b.objects[0].stack == 2 && b.objects[1].stack == 1 && b.objects[2].stack == 0 && b.objects[3].stack == 0);
	CHECK(Near(b.objects[0].pos.x, 100 - 2 * step) && Near(b.objects[0].pos.y, 100 - 2 * step));
	CHECK(Near(b.objects[2].pos.x, 100.0));
	// Far apart in time (beyond preempt * leniency): no stack.
	const char* apart =
		"osu file format v14\n[Difficulty]\nApproachRate:9\n[TimingPoints]\n0,500,4,2,0,100,1,0\n"
		"[HitObjects]\n100,100,1000,1,0\n100,100,2000,1,0\n";
	Beatmap a;
	CHECK(ParseBeatmap(apart, a));
	ApplyStacking(a);
	CHECK(a.objects[0].stack == 0 && a.objects[1].stack == 0);
}

void TestTicksAndHitsounds() {
	// 1 beat = 500 ms, SliderMultiplier 1 -> 100 px per beat, tick rate 2 -> a tick every 50 px.
	const char* text =
		"osu file format v14\n[General]\nSampleSet: Soft\n[Difficulty]\nSliderMultiplier:1\nSliderTickRate:2\n"
		"[TimingPoints]\n0,500,4,0,0,70,1,0\n"
		"[HitObjects]\n0,0,1000,2,2,L|200:0,1,200,4|0,0:0|3:0,0:0:0:0:\n";
	Beatmap b;
	CHECK(ParseBeatmap(text, b));
	const HitObject& s = b.objects[0];
	CHECK(Near(s.span, 1000.0));
	// ticks at 50/100/150 px (250/500/750 ms), then the legacy end 36 ms early
	CHECK(s.checkpoints.size() == 4 && s.checkpointIsTick.size() == 4);
	CHECK(Near(s.checkpoints[0], 1250.0) && Near(s.checkpoints[2], 1750.0) && s.checkpointIsTick[2]);
	CHECK(Near(s.checkpoints[3], 1964.0) && !s.checkpointIsTick[3]);
	CHECK(s.edgeSounds.size() == 2 && s.edgeSounds[0] == 4 && s.edgeSets.size() == 2 && s.edgeSets[1].first == 3);
	std::vector<SampleRef> out;
	HitSamples(b, s, s.time, s.edgeSounds[0], s.edgeSets[0].first, s.edgeSets[0].second, out);
	CHECK(out.size() == 2 && out[0].name == "soft-hitnormal" && out[1].name == "soft-hitfinish" && out[0].volume == 70);
	out.clear();
	HitSamples(b, s, s.endTime, s.edgeSounds[1], s.edgeSets[1].first, s.edgeSets[1].second, out);
	CHECK(out.size() == 1 && out[0].name == "drum-hitnormal");
	CHECK(TickSample(b, s, 1250).name == "soft-slidertick");
}

void TestHealth() {
	// osu!'s numbers: a miss costs 0.03 / 0.125 / 0.2 at HP 0 / 5 / 10; a 300 heals 0.03.
	CHECK(Near(Health::Miss(0), -0.03) && Near(Health::Miss(5), -0.125) && Near(Health::Miss(10), -0.2));
	CHECK(Near(Health::ForResult(300, 5), 0.03) && Near(Health::ForResult(100, 5), 0.011));
	CHECK(Near(Health::TargetMinimum(5), 0.9) && Near(Health::TargetMinimum(10), 0.4));
	// The solved drain takes a perfect play down to the target minimum (within osu!'s 0.01).
	std::string text = "osu file format v14\n[Difficulty]\nHPDrainRate:6\n[TimingPoints]\n0,500,4,2,0,100,1,0\n[HitObjects]\n";
	for (int i = 0; i < 40; ++i) text += std::to_string(100 + (i % 4) * 60) + ",200," + std::to_string(1000 + i * 500) + (i % 4 == 0 ? ",5,0\n" : ",1,0\n");
	Beatmap b;
	CHECK(ParseBeatmap(text, b));
	const double rate = Health::DrainRate(b, 1000);
	CHECK(rate > 0.0);
	double hp = 1.0, lowest = 1.0, last = 1000;
	for (size_t i = 0; i < b.objects.size(); ++i) {
		hp -= (b.objects[i].time - last) * rate; last = b.objects[i].time;
		lowest = std::min(lowest, hp);
		hp = std::min(1.0, hp + Health::kGreat); // osu! solves the drain WITHOUT combo-end bonuses
	}
	CHECK(std::fabs(lowest - Health::TargetMinimum(6)) <= 0.011);
	// A handful of misses at the start of a normal map does not end the run.
	double h = 1.0;
	for (int i = 0; i < 3; ++i) h += Health::Miss(4);
	CHECK(h > 0.6);
	// Spinners (lazer): 150 RPM to clear at OD 5, ticks = required + 2 + bonus spins.
	CHECK(SpinsRequired(2000, 5) == 5 && SpinsRequired(2000, 10) == 7 && SpinsRequired(2000, 0) == 3);
	CHECK(SpinnerTicks(2000, 5) == 5 + 2 + (12 - 5 - 2));
	CHECK(SpinnerJudge(1.0) == 300 && SpinnerJudge(0.95) == 100 && SpinnerJudge(0.8) == 50 && SpinnerJudge(0.7) == 0);
	// No drain from the last object before a break to the first after it, nor before/after the map.
	const char* brk = "osu file format v14\n[Events]\n2,3000,8000\n[TimingPoints]\n0,500,4,2,0,100,1,0\n"
		"[HitObjects]\n100,100,1000,1,0\n100,100,2000,1,0\n100,100,9000,1,0\n100,100,10000,1,0\n";
	Beatmap nb;
	CHECK(ParseBeatmap(brk, nb));
	CHECK(Health::NoDrain(nb, 500) && !Health::NoDrain(nb, 1500) && Health::NoDrain(nb, 2500));
	CHECK(Health::NoDrain(nb, 8500) && !Health::NoDrain(nb, 9500) && Health::NoDrain(nb, 10500));
	// lazer's combo flags: NewCombo is forced after a spinner; a spinner never starts a combo, so the
	// object before it is not LastInCombo; the map's last object never is (no combo-end bonus).
	const char* cmb = "osu file format v14\n[TimingPoints]\n0,500,4,2,0,100,1,0\n[HitObjects]\n"
		"100,100,1000,5,0\n100,100,1500,1,0\n256,192,2000,12,0,3000\n100,100,3500,1,0\n100,100,4000,5,0\n100,100,4500,1,0\n";
	Beatmap cb;
	CHECK(ParseBeatmap(cmb, cb) && cb.objects.size() == 6);
	CHECK(Health::NewCombo(cb, 0) && !Health::NewCombo(cb, 1) && Health::NewCombo(cb, 3) && Health::NewCombo(cb, 4));
	CHECK(!Health::LastInCombo(cb, 1) && Health::LastInCombo(cb, 2) && Health::LastInCombo(cb, 3));
	CHECK(!Health::LastInCombo(cb, 0) && !Health::LastInCombo(cb, 5));
}

} // namespace

int main() {
	TestFormulas();
	TestParse();
	TestPathTruncateAndExtend();
	TestRejectsOtherModes();
	TestPlayfield();
	TestStacking();
	TestTicksAndHitsounds();
	TestHealth();
	TestMods();
	TestHiddenAndDrain();
	TestSummaryAndFastParse();
	if (g_failed) { std::cerr << g_failed << " check(s) failed\n"; return 1; }
	std::cout << "OsuMathTests passed\n";
	return 0;
}
