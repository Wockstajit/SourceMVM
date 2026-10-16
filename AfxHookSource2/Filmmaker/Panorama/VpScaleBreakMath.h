#pragma once

// When does the scaled workspace preview (the "VP scale", ViewportScaler.h) BREAK, and why?
// Engine-free so Automation/tests/vp-scale-break-math-tests.cpp pins it; the render thread feeds it
// one Present per frame (AfxViewportScaler::RenderThread_OnPresent) and logs every break it opens.
//
// Three ways a frame goes out wrong:
//   * MissedBlit -- scaling was requested and no blit ran: the full-size frame sits behind the
//                   panels, which reads as the preview zooming to full screen;
//   * StaleRect  -- the blit ran into a rect that is not the one requested now: the world and the
//                   panel frame around it disagree;
//   * Stretched  -- the backbuffer is not the size of the window, so Windows stretches every frame
//                   to fit (counted whether or not a preview is up: it is what a resize looks like
//                   when the engine has not rebuilt its swapchain).
// StaleRect and Stretched are normal for a frame or two after a rect move or a window resize, so
// they only count once they have lasted kSettlePresents presents. A missed blit is visible at once.

#include <cmath>

namespace Filmmaker {
namespace VpScaleBreak {

enum Reason : unsigned { None = 0, MissedBlit = 1u, StaleRect = 2u, Stretched = 4u };
constexpr int kReasonCount = 3;
constexpr int kSettlePresents = 3;
constexpr float kRectTolerancePx = 2.0f;

struct Present {
	bool active = false;     // a workspace asked for the scaled preview this present
	bool recording = false;  // a capture after the UI is running: scaling is off on purpose
	bool blitRan = false;    // the scale blit ran for this present
	int bbW = 0, bbH = 0;    // the swapchain's backbuffer
	int clientW = 0, clientH = 0; // the game window's client area
	float rect[4] = { 0, 0, 0, 0 }; // requested, as backbuffer fractions x0 y0 x1 y1
	float vp[4] = { 0, 0, 0, 0 };   // the last blit's viewport in px: x y w h
};

inline unsigned Classify(const Present& p) {
	unsigned r = None;
	if (p.active && !p.recording) {
		if (!p.blitRan) r |= MissedBlit;
		else if (p.bbW > 0 && p.bbH > 0) {
			const float want[4] = { p.rect[0] * p.bbW, p.rect[1] * p.bbH,
				(p.rect[2] - p.rect[0]) * p.bbW, (p.rect[3] - p.rect[1]) * p.bbH };
			for (int i = 0; i < 4; ++i)
				if (std::fabs(want[i] - p.vp[i]) > kRectTolerancePx) { r |= StaleRect; break; }
		}
	}
	if (p.bbW > 0 && p.clientW > 0 && (p.bbW != p.clientW || p.bbH != p.clientH)) r |= Stretched;
	return r;
}

inline const char* ReasonName(int bit) {
	switch (bit) {
	case 0: return "missed-blit";
	case 1: return "stale-rect";
	default: return "stretched";
	}
}

// Turns per-present reasons into break EVENTS. Step() returns what to log for this present.
struct Tracker {
	enum class Event { None, Opened, Changed, Closed };
	unsigned open = None;          // reasons of the break that is open now (0 = healthy)
	unsigned breaks = 0;           // every break ever opened -- the number on each log line
	unsigned perReason[kReasonCount] = { 0, 0, 0 };
	int pending[kReasonCount] = { 0, 0, 0 }; // consecutive presents a debounced reason has held
	unsigned presentsOpen = 0;     // length of the open break, in presents

	Event Step(unsigned raw) {
		unsigned settled = 0;
		for (int b = 0; b < kReasonCount; ++b) {
			const unsigned bit = 1u << b;
			if (!(raw & bit)) { pending[b] = 0; continue; }
			if (bit == MissedBlit || ++pending[b] >= kSettlePresents) settled |= bit;
		}
		const unsigned was = open;
		if (settled) ++presentsOpen;
		// Count each reason once per break it joins, and a break once when it opens.
		for (int b = 0; b < kReasonCount; ++b)
			if ((settled & (1u << b)) && !(was & (1u << b))) ++perReason[b];
		open = settled;
		if (!was && settled) { ++breaks; return Event::Opened; }
		if (was && !settled) return Event::Closed;
		if (was != settled) return Event::Changed;
		return Event::None;
	}
	void ResetLength() { presentsOpen = 0; }
};

} // namespace VpScaleBreak
} // namespace Filmmaker
