#pragma once

// The window arithmetic both games' free-resize hosts share: CS2's WindowResize.cpp (an SDL window)
// and CS:S's SourceWindow.cpp (a plain Win32 one). Turning the WINDOW rect that WM_SIZING carries
// into the CLIENT size the user cares about and back, the Shift snap applied to that rect in place,
// and the readout card's two lines. Nothing here knows which game or windowing layer it runs under.

#include "AspectRatioMath.h"
#include "WindowSnapMath.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace Filmmaker {

// What holding Shift while dragging an edge does.
enum class WindowSnapMode {
	Both,       // snap the shape to a named ratio, and land exactly on a standard resolution when close
	Ratio,      // snap the shape only -- a perfect 2.39:1 at whatever size you drag to
	Resolution, // land on standard resolutions only, leave the shape alone
	Off,        // Shift does nothing
};

inline const char* WindowSnapModeName(WindowSnapMode mode) {
	switch (mode) {
	case WindowSnapMode::Both: return "both (ratio + standard resolutions)";
	case WindowSnapMode::Ratio: return "ratio";
	case WindowSnapMode::Resolution: return "resolution";
	default: return "off";
	}
}

// "both" / "ratio" / "resolution" / "off"; false for anything else.
inline bool ParseWindowSnapMode(const char* value, WindowSnapMode& out) {
	if (0 == _stricmp(value, "both") || 0 == _stricmp(value, "on")) out = WindowSnapMode::Both;
	else if (0 == _stricmp(value, "ratio")) out = WindowSnapMode::Ratio;
	else if (0 == _stricmp(value, "resolution") || 0 == _stricmp(value, "res")) out = WindowSnapMode::Resolution;
	else if (0 == _stricmp(value, "off")) out = WindowSnapMode::Off;
	else return false;
	return true;
}

inline bool WindowClientSize(HWND hwnd, int& outW, int& outH) {
	RECT client = {};
	if (!hwnd || !GetClientRect(hwnd, &client))
		return false;
	outW = client.right - client.left;
	outH = client.bottom - client.top;
	return outW > 0 && outH > 0;
}

// The non-client border thickness on each side, measured from the live window. Needed to turn the
// WINDOW rect that WM_SIZING carries into the CLIENT rect the user actually cares about, and back.
struct FrameInsets { int left = 0, top = 0, right = 0, bottom = 0; };

inline bool WindowFrameInsets(HWND hwnd, FrameInsets& out) {
	RECT window = {};
	RECT client = {};
	POINT clientOrigin = { 0, 0 };
	if (!hwnd || !GetWindowRect(hwnd, &window) || !GetClientRect(hwnd, &client)
		|| !ClientToScreen(hwnd, &clientOrigin))
		return false;
	out.left = clientOrigin.x - window.left;
	out.top = clientOrigin.y - window.top;
	out.right = window.right - (clientOrigin.x + (client.right - client.left));
	out.bottom = window.bottom - (clientOrigin.y + (client.bottom - client.top));
	return true;
}

inline bool WindowClientCenter(HWND hwnd, int& outX, int& outY) {
	RECT client = {};
	POINT origin = { 0, 0 };
	if (!hwnd || !GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin))
		return false;
	outX = origin.x + (client.right - client.left) / 2;
	outY = origin.y + (client.bottom - client.top) / 2;
	return true;
}

// Which axis leads while this edge is dragged. The dragged axis is kept and the other is derived,
// so a snap follows the mouse on the edge under the cursor instead of fighting it.
inline SnapAxis WindowSnapAxisForEdge(unsigned long long edge) {
	switch (edge) {
	case WMSZ_TOP:
	case WMSZ_BOTTOM:
		return SnapAxis::KeepHeight;
	default:
		return SnapAxis::KeepWidth;
	}
}

// Writes a client size back into the proposed WINDOW rect, keeping the edges the user is NOT
// dragging pinned. Dragging the left edge has to grow leftwards, not drag the whole window right.
inline void ApplyClientSizeToWindowRect(unsigned long long edge, const FrameInsets& insets,
	int clientWidth, int clientHeight, RECT& rect) {
	const int windowWidth = clientWidth + insets.left + insets.right;
	const int windowHeight = clientHeight + insets.top + insets.bottom;

	switch (edge) {
	case WMSZ_LEFT:
	case WMSZ_TOPLEFT:
	case WMSZ_BOTTOMLEFT:
		rect.left = rect.right - windowWidth;
		break;
	default:
		rect.right = rect.left + windowWidth;
		break;
	}

	switch (edge) {
	case WMSZ_TOP:
	case WMSZ_TOPLEFT:
	case WMSZ_TOPRIGHT:
		rect.top = rect.bottom - windowHeight;
		break;
	default:
		rect.bottom = rect.top + windowHeight;
		break;
	}
}

// One WM_SIZING step: the client size Windows proposes, snapped while Shift is held. Constraining a
// resize on Windows means editing the proposed rect in place, which this does; the message still
// goes on to the game's window procedure and DefWindowProc. `note` is the card's live snap state, or
// the hint that Shift can snap; the centre is that of the rect Windows is ABOUT to apply (dragging a
// left or top edge moves the origin too, so the live rect would leave the card lagging).
struct WindowSizingStep {
	bool valid = false;
	bool shiftHeld = false;
	int width = 0, height = 0;
	int centerX = 0, centerY = 0;
	char note[64] = { 0 };
};

inline WindowSizingStep WindowSizing(HWND hwnd, unsigned long long edge, RECT& proposed, WindowSnapMode mode,
	int minClientW, int minClientH) {
	WindowSizingStep step;
	FrameInsets insets;
	if (!WindowFrameInsets(hwnd, insets))
		return step;
	int width = (proposed.right - proposed.left) - insets.left - insets.right;
	int height = (proposed.bottom - proposed.top) - insets.top - insets.bottom;
	if (width <= 0 || height <= 0)
		return step;

	step.shiftHeld = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
	if (step.shiftHeld && mode != WindowSnapMode::Off) {
		const SnapResult snap = WindowSnap_Solve(width, height, WindowSnapAxisForEdge(edge),
			mode != WindowSnapMode::Resolution, mode != WindowSnapMode::Ratio);
		if (snap.valid && snap.width >= minClientW && snap.height >= minClientH) {
			width = snap.width;
			height = snap.height;
			ApplyClientSizeToWindowRect(edge, insets, width, height, proposed);
			if (snap.snappedResolution && snap.resolutionLabel)
				snprintf(step.note, sizeof(step.note), "SNAP %s", snap.resolutionLabel);
			else
				snprintf(step.note, sizeof(step.note), "SNAP");
		}
	} else if (!step.shiftHeld) {
		snprintf(step.note, sizeof(step.note), "SHIFT to snap");
	}

	step.valid = true;
	step.width = width;
	step.height = height;
	step.centerX = proposed.left + insets.left + width / 2;
	step.centerY = proposed.top + insets.top + height / 2;
	return step;
}

// UTF-8 byte sequences, written out so the file's source charset can never change what ships.
constexpr const char* kWindowTimesSign = "\xC3\x97";      // U+00D7 MULTIPLICATION SIGN
constexpr const char* kWindowMiddleDot = "\xC2\xB7";      // U+00B7 MIDDLE DOT
constexpr const char* kWindowAlmostEqual = "\xE2\x89\x88"; // U+2248 ALMOST EQUAL TO

inline std::string FormatWindowSize(int width, int height) {
	char buf[64];
	snprintf(buf, sizeof(buf), "%d %s %d", width, kWindowTimesSign, height);
	return buf;
}

// The second line of the card. Three shapes, in decreasing order of how satisfying they are:
//   exact table hit          -> "16:9 . Widescreen HD"
//   exact but unnamed ratio  -> "7:5 . closest 1.43:1 (-2.1%)"
//   nothing exact            -> "~ 16:9 . Widescreen HD (+0.4%)"
inline std::string FormatWindowAspect(const AspectMatch& match) {
	if (!match.valid || !match.entry)
		return std::string();

	char buf[192];
	if (match.exact) {
		snprintf(buf, sizeof(buf), "%s %s %s", match.entry->label, kWindowMiddleDot, match.entry->name);
		return buf;
	}
	if (match.reducedWidth > 0 && match.reducedHeight > 0) {
		snprintf(buf, sizeof(buf), "%d:%d %s closest %s (%+.1f%%)",
			match.reducedWidth, match.reducedHeight, kWindowMiddleDot,
			match.entry->label, match.errorPercent);
		return buf;
	}
	snprintf(buf, sizeof(buf), "%s %s %s %s (%+.1f%%)",
		kWindowAlmostEqual, match.entry->label, kWindowMiddleDot, match.entry->name, match.errorPercent);
	return buf;
}

// The aspect line with `note` (the snap state) appended.
inline std::string FormatWindowReadout(int width, int height, const char* note) {
	std::string secondary = FormatWindowAspect(AspectRatio_Match(width, height));
	if (note && *note) {
		secondary += ' ';
		secondary += kWindowMiddleDot;
		secondary += ' ';
		secondary += note;
	}
	return secondary;
}

inline double ParseWindowRatio(const char* text) {
	if (!text || !*text)
		return 0.0;
	// Accept "2.39", "16:9" and "16x9".
	const char* separator = strpbrk(text, ":xX/");
	if (separator) {
		const double numerator = atof(text);
		const double denominator = atof(separator + 1);
		if (numerator > 0.0 && denominator > 0.0)
			return numerator / denominator;
		return 0.0;
	}
	const double value = atof(text);
	return value > 0.0 ? value : 0.0;
}

// Accepts a table label ("16:9", "2.39:1", "2.39") as well as a bare number.
inline double ResolveWindowRatioArgument(const char* text) {
	std::size_t count = 0;
	const AspectRatioEntry* table = AspectRatio_Table(count);
	for (std::size_t i = 0; i < count; ++i) {
		if (0 == _stricmp(text, table[i].label))
			return table[i].ratio;
	}
	return ParseWindowRatio(text);
}

} // namespace Filmmaker
