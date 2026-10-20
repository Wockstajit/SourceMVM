#include "../../AfxHookSource2/Filmmaker/Heatmap/RoundHeatmapMath.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace Filmmaker::Heatmap;

namespace {

int g_failures = 0;
void Check(bool ok, const char* what) {
	if (ok) return;
	std::cerr << "FAIL: " << what << "\n";
	++g_failures;
}

// ---- a paths file written the way FilmmakerDemoInfoGo/paths.go writes it ----
struct Writer {
	std::vector<uint8_t> b;
	template <class T> void Put(T v) {
		const auto* p = reinterpret_cast<const uint8_t*>(&v);
		b.insert(b.end(), p, p + sizeof(T));
	}
	void Bytes(const char* s, size_t n) { b.insert(b.end(), s, s + n); }
};
struct S { int x, y, z; uint8_t f; };
std::vector<uint8_t> File(const std::vector<std::vector<std::vector<S>>>& rounds, uint32_t version = kPathsVersion) {
	Writer w;
	w.Bytes("FMRP", 4);
	w.Put<uint32_t>(version);
	w.Put<uint16_t>(2);
	w.Put<uint16_t>(10);
	w.Bytes("de_inferno", 10);
	w.Put<uint32_t>(uint32_t(rounds.size()));
	int tick = 100;
	for (const auto& r : rounds) {
		w.Put<int32_t>(tick);
		w.Put<int32_t>(tick + 1000);
		tick += 1000;
		w.Put<uint8_t>(0);
		w.Put<uint16_t>(uint16_t(r.size()));
		for (size_t p = 0; p < r.size(); ++p) {
			w.Put<uint32_t>(uint32_t(1000 + p));
			w.Put<uint8_t>(p % 2 ? 3 : 2);
			w.Put<uint32_t>(uint32_t(r[p].size()));
			for (const auto& s : r[p]) {
				w.Put<int16_t>(int16_t(s.x));
				w.Put<int16_t>(int16_t(s.y));
				w.Put<int16_t>(int16_t(s.z));
				w.Put<uint8_t>(s.f);
			}
		}
	}
	return w.b;
}

PathSample P(float x, float y, float z, uint8_t flags = kOnGround) {
	PathSample s;
	s.x = x; s.y = y; s.z = z; s.flags = flags;
	return s;
}
// A walk from a to b at full run speed (about 8 units per 2-tick sample).
void Walk(std::vector<PathSample>& out, float ax, float ay, float bx, float by, float z, bool startStroke) {
	const float len = std::hypot(bx - ax, by - ay);
	const int n = (std::max)(1, int(std::ceil(len / 8.f)));
	for (int i = 0; i <= n; ++i) {
		const float t = float(i) / float(n);
		out.push_back(P(ax + (bx - ax) * t, ay + (by - ay) * t, z, uint8_t(kOnGround | (startStroke && i == 0 ? kStrokeStart : 0))));
	}
}
void Stand(std::vector<PathSample>& out, float x, float y, float z, int samples) {
	for (int i = 0; i < samples; ++i) out.push_back(P(x, y, z));
}

// Visits and height of the layer at world (x, y) that is closest to z.
struct Texel { float visits = 0, height = 0; };
Texel At(const Grid& g, float x, float y, float z = 0) {
	const auto& w = g.window;
	const int i = int(std::floor((x - w.x0) / w.cell)), j = int(std::floor((y - w.y0) / w.cell));
	if (i < 0 || j < 0 || i >= w.size || j >= w.size || g.texels.empty()) return {};
	const float* t = &g.texels[(size_t(j) * size_t(w.size) + size_t(i)) * 4];
	Texel best;
	float bestGap = 1e30f;
	for (int l = 0; l < 2; ++l) {
		if (t[l * 2] <= 0) continue;
		const float h = t[l * 2 + 1] + g.baseZ, gap = std::fabs(h - z);
		if (gap < bestGap) { bestGap = gap; best = {t[l * 2], h}; }
	}
	return best;
}

RoundPaths Round(std::vector<std::vector<PathSample>> players) {
	RoundPaths r;
	r.startTick = 0;
	r.endTick = 10000;
	for (size_t i = 0; i < players.size(); ++i) {
		PlayerPath p;
		p.accountId = uint32_t(i + 1);
		p.team = i % 2 ? 3 : 2;
		p.samples = std::move(players[i]);
		r.players.push_back(std::move(p));
	}
	return r;
}

void TestParse() {
	const auto bytes = File({ { { { 1, 2, 3, kStrokeStart | kOnGround }, { -4, 5, -6, kOnGround } } }, { {}, { { 7, 8, 9, 0 } } } });
	DemoPaths d;
	std::string error;
	Check(ParsePaths(bytes.data(), bytes.size(), d, error), "a well-formed file parses");
	Check(d.map == "de_inferno" && d.sampleStep == 2, "map and sample step are read");
	Check(d.rounds.size() == 2, "both rounds are read");
	Check(d.rounds[0].startTick == 100 && d.rounds[0].endTick == 1100 && d.rounds[1].startTick == 1100, "round ticks are read");
	Check(d.rounds[0].players.size() == 1 && d.rounds[0].players[0].samples.size() == 2, "samples are read");
	const auto& s = d.rounds[0].players[0].samples[1];
	Check(s.x == -4 && s.y == 5 && s.z == -6 && s.flags == kOnGround, "negative coordinates and flags survive");
	Check(d.rounds[1].players.size() == 2 && d.rounds[1].players[0].samples.empty() && d.rounds[1].players[1].team == 3,
		"an empty path and the team are read");

	auto truncated = bytes;
	truncated.pop_back();
	Check(!ParsePaths(truncated.data(), truncated.size(), d, error), "a truncated file is refused");
	auto trailing = bytes;
	trailing.push_back(0);
	Check(!ParsePaths(trailing.data(), trailing.size(), d, error), "trailing bytes are refused");
	const auto other = File({}, kPathsVersion + 1);
	Check(!ParsePaths(other.data(), other.size(), d, error), "another version is refused");
	const char json[] = "{\"ok\":false,\"error\":\"x\"}";
	Check(!LooksLikePaths(json, sizeof(json) - 1) && !ParsePaths(json, sizeof(json) - 1, d, error),
		"the helper's error JSON is not a paths file");
}

void TestRoundAt() {
	DemoPaths d;
	d.rounds.resize(3);
	d.rounds[0].startTick = 0; d.rounds[0].endTick = 500;
	d.rounds[1].startTick = 500; d.rounds[1].endTick = 900;
	d.rounds[2].startTick = 900; d.rounds[2].endTick = 1200;
	Check(RoundAt(d, 0) == 0 && RoundAt(d, 499) == 0, "a tick inside the first round");
	Check(RoundAt(d, 500) == 1, "a round starts at its own start tick");
	Check(RoundAt(d, 5000) == 2, "past the last round reads the last round");
	d.rounds[0].startTick = 50;
	Check(RoundAt(d, 10) == -1, "before the first round there is none");
	Check(RoundAt(DemoPaths{}, 10) == -1, "a demo without rounds has none");
}

void TestWindow() {
	const Window w = WindowAround(1234.5f, -987.25f, 1500);
	Check(w.Valid() && w.size <= kGridMax && w.cell >= kCellMin, "window is bounded");
	Check(w.x0 <= 1234.5f - 1500 && w.y0 <= -987.25f - 1500 && w.x0 + w.Extent() >= 1234.5f + 1500
		&& w.y0 + w.Extent() >= -987.25f + 1500, "window covers the shown disc");
	Check(std::fabs(std::remainder(w.x0, w.cell)) < 1e-3f && std::fabs(std::remainder(w.y0, w.cell)) < 1e-3f,
		"window corner sits on the cell lattice");
	const Window small = WindowAround(0, 0, 300);
	Check(small.cell == kCellMin, "a small radius uses the finest cell");
	Check(!NeedsRebuild(w, 1234.5f + 100, -987.25f, 1500), "a short move keeps the grid");
	Check(NeedsRebuild(w, 1234.5f + 800, -987.25f, 1500), "a long move rebuilds");
	Check(NeedsRebuild(w, 1234.5f, -987.25f, 2000), "a new radius rebuilds");
	Check(NeedsRebuild(Window{}, 0, 0, 1500), "no grid yet rebuilds");
	Check(ClampRadius(1e9f) == kRadiusMax && ClampRadius(1) == kRadiusMin && ClampRadius(NAN) == kRadiusDefault,
		"radius is clamped");
}

void TestPreciseStrip() {
	std::vector<PathSample> a;
	Walk(a, 0, 0, 400, 0, 0, true);
	Grid g;
	BuildGrid(Round({ a }), 2, WindowAround(200, 0, 600), g);
	Check(At(g, 200, 0).visits == 1, "the walked line has one visit");
	Check(At(g, 200, kBodyRadius - 2.5f).visits == 1, "inside the body's width is painted");
	Check(At(g, 200, kBodyRadius + 3).visits == 0, "just outside the body's width is not (no blob)");
	Check(At(g, 200, 200).visits == 0 && At(g, -100, 0).visits == 0, "far away stays empty");
	Check(std::fabs(At(g, 200, 0).height) < 1e-3f, "height is the floor walked on");
	Check(g.players == 1 && g.maxVisits == 1, "grid summary");
}

void TestVisits() {
	// There and back: the far end (near the start) is crossed twice, seconds apart.
	std::vector<PathSample> a;
	Walk(a, 0, 0, 800, 0, 0, true);
	Walk(a, 800, 0, 0, 0, 0, false);
	// Standing on one spot for 20 seconds is one visit, not 320.
	std::vector<PathSample> b;
	Stand(b, 0, 300, 0, 320);
	b[0].flags |= kStrokeStart;
	// A second player on the same line adds a visit.
	std::vector<PathSample> c;
	Walk(c, 0, 0, 800, 0, 0, true);
	Grid g;
	BuildGrid(Round({ a, b, c }), 2, WindowAround(400, 0, 1000), g);
	Check(At(g, 40, 0).visits == 3, "there and back plus another player is three visits");
	Check(At(g, 0, 300).visits == 1, "standing still is one visit");
	Check(Intensity(1) > .3f && Intensity(1) < Intensity(3) && Intensity(kFullVisits) == 1 && Intensity(0) == 0,
		"one visit shows, more is stronger, the full count saturates");
	Grid onlyCt;
	BuildGrid(Round({ a, b, c }), 2, WindowAround(400, 0, 1000), onlyCt, 3);
	Check(At(onlyCt, 40, 0).visits == 0 && At(onlyCt, 0, 300).visits == 1, "a team filter keeps that team only");
}

void TestFloors() {
	// One player on the ground floor, one on a floor 200 units above, over the same ground.
	std::vector<PathSample> low, high;
	Walk(low, 0, 0, 300, 0, 0, true);
	Walk(high, 0, 0, 300, 0, 200, true);
	Grid g;
	BuildGrid(Round({ low, high }), 2, WindowAround(150, 0, 500), g);
	const Texel a = At(g, 150, 0, 0), b = At(g, 150, 0, 200);
	Check(a.visits == 1 && std::fabs(a.height) < 1, "the lower floor keeps its own visit");
	Check(b.visits == 1 && std::fabs(b.height - 200) < 1, "the upper floor keeps its own visit");
}

void TestStrokesAndJumps() {
	// A respawn across the map starts a new stroke: nothing is painted between.
	std::vector<PathSample> a;
	a.push_back(P(0, 0, 0, kOnGround | kStrokeStart));
	a.push_back(P(600, 0, 0, kOnGround | kStrokeStart));
	// A teleport without the flag is not joined either.
	a.push_back(P(600, 500, 0, kOnGround));
	Grid g;
	BuildGrid(Round({ a }), 2, WindowAround(300, 200, 800), g);
	Check(At(g, 0, 0).visits == 1 && At(g, 600, 0).visits == 1 && At(g, 600, 500).visits == 1, "each spot is a disc");
	Check(At(g, 300, 0).visits == 0 && At(g, 600, 250).visits == 0, "no line across a respawn or teleport");

	// A jump: the airborne samples stay on the floor the player left.
	std::vector<PathSample> j;
	j.push_back(P(0, 0, 0, kOnGround | kStrokeStart));
	j.push_back(P(8, 0, 30, 0));
	j.push_back(P(16, 0, 50, 0));
	j.push_back(P(24, 0, 30, 0));
	j.push_back(P(32, 0, 0, kOnGround));
	Grid jg;
	BuildGrid(Round({ j }), 2, WindowAround(16, 0, 300), jg);
	Check(At(jg, 16, 0).visits == 1 && std::fabs(At(jg, 16, 0).height) < 1e-3f, "a jump keeps the floor height");

	// A drop off a 200-unit ledge: the path follows the player down and carries on below.
	std::vector<PathSample> f;
	Walk(f, -120, 0, 0, 0, 200, true);
	f.push_back(P(8, 0, 150, 0));
	f.push_back(P(16, 0, 60, 0));
	Walk(f, 24, 0, 160, 0, 0, false);
	Grid fg;
	BuildGrid(Round({ f }), 2, WindowAround(20, 0, 300), fg);
	Check(At(fg, -80, 0, 200).visits == 1 && std::fabs(At(fg, -80, 0, 200).height - 200) < 1, "the ledge keeps its height");
	Check(At(fg, 100, 0, 0).visits == 1 && std::fabs(At(fg, 100, 0, 0).height) < 1, "below the drop is the lower floor");
}

// Review of #62: a player back on the same spot a few samples after a gap (a respawn, a reconnect)
// is a new visit, however short the gap looks in samples.
void TestVisitAfterAGap() {
	std::vector<PathSample> a;
	Stand(a, 0, 0, 0, 3);
	a[0].flags |= kStrokeStart;
	a.push_back(P(0, 0, 0, kOnGround | kStrokeStart));
	Stand(a, 0, 0, 0, 2);
	Grid g;
	BuildGrid(Round({ a }), 2, WindowAround(0, 0, 300), g);
	Check(At(g, 0, 0).visits == 2, "a new stroke on the same spot is a new visit");
	std::vector<PathSample> b;
	Stand(b, 0, 0, 0, 3);
	b[0].flags |= kStrokeStart;
	b.push_back(P(500, 0, 0)); // a teleport, no flag
	b.push_back(P(0, 0, 0));   // and straight back
	Grid h;
	BuildGrid(Round({ b }), 2, WindowAround(0, 0, 700), h);
	Check(At(h, 0, 0).visits == 2, "a teleport breaks the visit too");
}

// Review of #62: warmup and the rounds before the match went live are not match rounds.
void TestMatchRounds() {
	DemoPaths d;
	d.rounds.resize(4);
	d.rounds[0].warmup = true;
	d.rounds[1].warmup = true;
	Check(MatchRoundNumber(d, 0) == 0 && MatchRoundNumber(d, 1) == 0, "warmup has no match number");
	Check(MatchRoundNumber(d, 2) == 1 && MatchRoundNumber(d, 3) == 2, "the match counts from its own first round");
	Check(MatchRoundCount(d) == 2, "the count leaves warmup out");
	Check(MatchRoundNumber(d, -1) == 0 && MatchRoundNumber(d, 4) == 0, "no round outside the list");
}

void TestOutsideWindow() {
	std::vector<PathSample> a;
	Walk(a, 5000, 5000, 5400, 5000, 0, true);
	Grid g;
	BuildGrid(Round({ a }), 2, WindowAround(0, 0, 500), g);
	Check(g.texels.empty() && g.players == 0 && g.segments == 0, "a path outside the window adds nothing");
}

// Grey where the grid is empty, red by Intensity where it is painted: what the floor pass shows.
void WritePicture(const Grid& g, const char* path) {
	FILE* f = std::fopen(path, "wb");
	if (!f) return;
	const int n = g.window.size;
	std::fprintf(f, "P6 %d %d 255\n", n, n);
	std::vector<uint8_t> row(size_t(n) * 3);
	for (int j = n - 1; j >= 0; --j) { // +y up, as on a radar
		for (int i = 0; i < n; ++i) {
			const float* t = &g.texels[(size_t(j) * size_t(n) + size_t(i)) * 4];
			const float k = Intensity((std::max)(t[0], t[2]));
			const float grey = 40;
			row[size_t(i) * 3 + 0] = uint8_t(k > 0 ? 90 + 165 * k : grey);
			row[size_t(i) * 3 + 1] = uint8_t(k > 0 ? 10 + 20 * k : grey);
			row[size_t(i) * 3 + 2] = uint8_t(k > 0 ? 10 + 10 * k : grey);
		}
		std::fwrite(row.data(), 1, row.size(), f);
	}
	std::fclose(f);
}

// With a roundpaths.fmbin path (the helper's -paths output): each round's size and how long a grid
// around its centre takes to build, to check the maths on a real demo. With a round number and an
// output path it also writes that round's grid as a .ppm picture.
int Inspect(const char* path, int pictureRound, const char* picture) {
	FILE* f = std::fopen(path, "rb");
	if (!f) { std::cerr << "cannot open " << path << "\n"; return 1; }
	std::vector<uint8_t> bytes;
	uint8_t buf[65536];
	for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) bytes.insert(bytes.end(), buf, buf + n);
	std::fclose(f);
	DemoPaths d;
	std::string error;
	if (!ParsePaths(bytes.data(), bytes.size(), d, error)) { std::cerr << error << "\n"; return 1; }
	std::printf("%s: %zu rounds, step %d\n", d.map.c_str(), d.rounds.size(), d.sampleStep);
	for (size_t r = 0; r < d.rounds.size(); ++r) {
		const auto& round = d.rounds[r];
		size_t samples = 0;
		double sx = 0, sy = 0;
		for (const auto& pl : round.players)
			for (const auto& s : pl.samples) { ++samples; sx += s.x; sy += s.y; }
		if (!samples) continue;
		const auto start = std::chrono::steady_clock::now();
		Grid g;
		BuildGrid(round, d.sampleStep, WindowAround(float(sx / samples), float(sy / samples), kRadiusDefault), g);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		size_t painted = 0;
		for (size_t i = 0; i < g.texels.size(); i += 4) painted += (g.texels[i] > 0 || g.texels[i + 2] > 0);
		std::printf("round %2zu ticks %6d-%6d players %2zu samples %6zu | grid %dx%d cell %.2f: %zu segments, %.1f%% painted, max %g visits, %.1f ms\n",
			r, round.startTick, round.endTick, round.players.size(), samples, g.window.size, g.window.size, g.window.cell,
			g.segments, 100.0 * double(painted) / double(g.texels.size() / 4 ? g.texels.size() / 4 : 1), g.maxVisits, ms);
		if (picture && int(r) == pictureRound && !g.texels.empty()) WritePicture(g, picture);
	}
	return 0;
}

} // namespace

int main(int argc, char** argv) {
	if (argc > 1) return Inspect(argv[1], argc > 3 ? std::atoi(argv[2]) : -1, argc > 3 ? argv[3] : nullptr);
	TestParse();
	TestRoundAt();
	TestWindow();
	TestPreciseStrip();
	TestVisits();
	TestFloors();
	TestStrokesAndJumps();
	TestVisitAfterAGap();
	TestMatchRounds();
	TestOutsideWindow();
	if (g_failures) {
		std::cerr << g_failures << " round heat map check(s) failed\n";
		return 1;
	}
	std::cout << "round heat map math tests passed\n";
	return 0;
}
