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

// What the keyframes helper may take beside a running game. Measured 2026-11-01: a 445 MB demo
// peaks at about 1.05 GB in the helper and writes a 1.02 GB copy. Four crash reports ran out of
// system commit with CS2 at 10-17 GB on 16 GB PCs, three of them with the helper running.
namespace KeyframeHelperBudget {

constexpr unsigned long long kMB = 1024ull * 1024ull;
// Commit that must stay free for CS2 itself after the helper takes its whole cap.
constexpr unsigned long long kGameCommitReserve = 2048 * kMB;

// The helper's hard cap (a job object limit): about three times the demo, 1 to 6 GB.
inline unsigned long long HardCapBytes(unsigned long long demoBytes) {
	return std::clamp(demoBytes * 3 + 512 * kMB, 1024 * kMB, 6144 * kMB);
}

// The soft heap limit the helper's garbage collector works toward (GOMEMLIMIT).
inline unsigned long long SoftLimitMB(unsigned long long demoBytes) {
	return HardCapBytes(demoBytes) / 4 * 3 / kMB;
}

// Free disk the copy needs (about 2.3 times the demo) with some slack.
inline unsigned long long DiskNeedBytes(unsigned long long demoBytes) {
	return demoBytes / 2 * 5 + 256 * kMB;
}

// "" when the helper may start now, otherwise why not, for the status line.
inline std::string WhyNotEnough(unsigned long long demoBytes, unsigned long long freeDiskBytes,
	unsigned long long availCommitBytes) {
	char text[160];
	if (freeDiskBytes < DiskNeedBytes(demoBytes)) {
		std::snprintf(text, sizeof(text), "not enough free disk (%llu MB free, %llu MB needed)",
			freeDiskBytes / kMB, DiskNeedBytes(demoBytes) / kMB);
		return text;
	}
	const unsigned long long need = HardCapBytes(demoBytes) + kGameCommitReserve;
	if (availCommitBytes < need) {
		std::snprintf(text, sizeof(text), "not enough free memory (%llu MB free, %llu MB needed)",
			availCommitBytes / kMB, need / kMB);
		return text;
	}
	return std::string();
}

} // namespace KeyframeHelperBudget
} // namespace Filmmaker
