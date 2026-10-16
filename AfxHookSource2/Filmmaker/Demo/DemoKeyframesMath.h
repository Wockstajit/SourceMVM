#pragma once

// Where a keyframed copy gets its DENSE grid (Docs/features/filmmaker_demo_scrubbing.md, "Dense
// keyframes around the shot"). Engine-free, so Automation/tests/demo-keyframes-math-tests.cpp pins
// it; DemoKeyframes.cpp feeds it the demo's camera-marker ticks and hands the result to the helper
// as `-dense a-b,c-d -dense-every N`.
//
// A keyframe costs about 45 KB, so the dense grid only covers the shot: markers closer than
// kMergeGapTicks form one cluster, and each cluster is padded by kPadTicks on both sides. The step
// starts at 2 ticks and doubles until the windows fit kMaxDenseTicks / step keyframes.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace Filmmaker {
namespace KeyframeWindows {

constexpr int kPadTicks = 256;       // 4 s at 64 tick, either side of a cluster
constexpr int kMergeGapTicks = 512;  // markers closer than 8 s share one window
constexpr int kMaxDenseTicks = 6144; // ~3072 keyframes at step 2, ~140 MB
constexpr int kFirstStep = 2;
constexpr int kMaxStep = 8;          // still divides the 32-tick base grid

struct Window { int first = 0, last = 0; }; // inclusive ticks

struct Plan {
	std::vector<Window> windows;
	int step = kFirstStep;
};

inline Plan PlanFor(std::vector<int> markerTicks) {
	Plan plan;
	std::sort(markerTicks.begin(), markerTicks.end());
	markerTicks.erase(std::unique(markerTicks.begin(), markerTicks.end()), markerTicks.end());
	int prev = -1;
	for (int t : markerTicks) {
		if (t < 0) continue;
		if (prev >= 0 && t - prev <= kMergeGapTicks)
			plan.windows.back().last = t + kPadTicks;
		else
			plan.windows.push_back({ (std::max)(0, t - kPadTicks), t + kPadTicks });
		prev = t;
	}
	long long total = 0;
	for (const Window& w : plan.windows) total += (long long)w.last - w.first + 1;
	while (plan.step < kMaxStep && total / plan.step > kMaxDenseTicks / kFirstStep)
		plan.step *= 2;
	return plan;
}

// Every marker tick lies inside one of the windows (a copy built with them needs no rebuild).
inline bool Covers(const std::vector<Window>& windows, const std::vector<int>& markerTicks) {
	for (int t : markerTicks) {
		bool inside = false;
		for (const Window& w : windows)
			if (t >= w.first && t <= w.last) { inside = true; break; }
		if (!inside) return false;
	}
	return true;
}

// "a-b,c-d" -- the helper's -dense argument and the marker file's third field.
inline std::string Format(const std::vector<Window>& windows) {
	std::string out;
	char buf[32];
	for (const Window& w : windows) {
		snprintf(buf, sizeof(buf), "%s%d-%d", out.empty() ? "" : ",", w.first, w.last);
		out += buf;
	}
	return out;
}

inline std::vector<Window> Parse(const std::string& text) {
	std::vector<Window> out;
	const char* p = text.c_str();
	while (*p) {
		char* end = nullptr;
		const long a = strtol(p, &end, 10);
		if (end == p || *end != '-') break;
		const char* q = end + 1;
		const long b = strtol(q, &end, 10);
		if (end == q || b < a) break;
		out.push_back({ (int)a, (int)b });
		p = end;
		if (*p == ',') ++p;
		else break;
	}
	return out;
}

} // namespace KeyframeWindows
} // namespace Filmmaker
