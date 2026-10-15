#pragma once

// Shift-to-snap maths for the window drag: turn an arbitrary dragged size into a "true" one.
//
// Three snaps, applied in this order, because they answer different questions:
//   1. RATIO   -- make the shape exactly a named ratio (16:9, not 16.03:9). This is the one that
//                 matters while dragging: you can hold a perfect 2.39:1 at any size you like.
//   2. RESOLUTION -- if the ratio-snapped size is near a standard resolution of that same ratio,
//                 land exactly on it and name it (1920x1080 "1080p", not 1916x1078).
//   3. GRID    -- otherwise land on the nearest whole multiple of the ratio, so the size still
//                 reads like a real resolution. 16:9 quantises to 32x18, so a drag settles on
//                 1888x1062 / 1920x1080 / 1952x1098 and never on 1913x1076.
//
// Steps 2 and 3 together are what "snap to the truest resolution for this ratio" means: with Shift
// held there is no such thing as a size that is only nearly the ratio, and the numbers on the card
// are always numbers a render would be happy to be given.
//
// Header-only and dependency-free, same as AspectRatioMath.h, so both compile into the hook DLL
// and into the standalone WindowSnapMathTests target with no Windows.h in sight.

#include "AspectRatioMath.h"

#include <cmath>
#include <cstddef>

namespace Filmmaker {

// Which axis the user is actually dragging. The dragged axis is kept and the other is derived, so
// the window follows the mouse on the edge under the cursor instead of fighting it.
enum class SnapAxis {
	KeepWidth,  // dragging a left/right edge (or a corner): width leads, height follows
	KeepHeight, // dragging a top/bottom edge: height leads, width follows
};

struct StandardResolution {
	int width;
	int height;
	const char* label; // e.g. "1080p", "DCI flat"
};

// Standard sizes worth landing exactly on, grouped by the ratio they belong to. Only sizes people
// actually name -- a longer list makes the snap feel random rather than helpful.
inline const StandardResolution* WindowSnap_Table(std::size_t& outCount) {
	static const StandardResolution kTable[] = {
		// 16:9
		{ 1280,  720, "720p"        },
		{ 1600,  900, "900p"        },
		{ 1920, 1080, "1080p"       },
		{ 2560, 1440, "1440p"       },
		{ 3840, 2160, "4K UHD"      },
		// 16:10
		{ 1280,  800, "16:10"       },
		{ 1680, 1050, "16:10"       },
		{ 1920, 1200, "16:10"       },
		{ 2560, 1600, "16:10"       },
		// 4:3
		{ 1024,  768, "XGA"         },
		{ 1280,  960, "4:3"         },
		{ 1600, 1200, "UXGA"        },
		// 5:4
		{ 1280, 1024, "SXGA"        },
		// 3:2
		{ 1620, 1080, "3:2"         },
		{ 2160, 1440, "3:2"         },
		// ultrawide
		{ 2560, 1080, "UW-FHD"      },
		{ 3440, 1440, "UW-QHD"      },
		// cinema
		{ 1998, 1080, "DCI flat"    },
		{ 2048,  858, "DCI scope"   },
		{ 1920,  804, "2.39:1 HD"   },
		{ 3840, 1608, "2.39:1 UHD"  },
		{ 1920,  960, "2:1 HD"      },
		{ 2560, 1280, "2:1 QHD"     },
		// square / vertical
		{ 1080, 1080, "1:1"         },
		{ 1080, 1920, "9:16"        },
	};
	outCount = sizeof(kTable) / sizeof(kTable[0]);
	return kTable;
}

// Base pull toward a standard resolution, in pixels of summed width+height error. Scaled with the
// size being dragged (see WindowSnap_ResolutionPull) so 4K snaps as readily as 720p -- a fixed
// threshold is 3% of a 1080p drag but 0.8% of a 4K one, which is why the first version felt like it
// only worked at small sizes.
inline constexpr int kSnapResolutionPixels = 32;

// Quantisation step for ratios that have no small whole pair (1.43:1, 2.39:1). One axis lands on a
// multiple of this and the other is derived, which is as close to the ratio as integers allow.
inline constexpr int kSnapFreeStep = 16;

inline int WindowSnap_ResolutionPull(int width) {
	return kSnapResolutionPixels + width / 24; // 1920 -> 112px, 3840 -> 192px of summed error
}

struct SnapResult {
	bool valid = false;
	int width = 0;                           // snapped client size (always even)
	int height = 0;
	bool snappedRatio = false;
	bool snappedResolution = false;          // landed on a named standard resolution
	bool snappedGrid = false;                // landed on a whole multiple of the ratio instead
	const AspectRatioEntry* entry = nullptr; // ratio that was snapped to
	const char* resolutionLabel = nullptr;   // set only when snappedResolution
};

// Rounds up to an even number: odd render sizes are rejected by yuv420p encoders, and a size the
// tool hands out should never need fixing later.
inline int WindowSnap_MakeEven(int value) {
	return (value % 2) ? value + 1 : value;
}

inline SnapResult WindowSnap_Solve(int width, int height, SnapAxis axis,
	bool snapRatio, bool snapResolution) {
	SnapResult out;
	if (width <= 0 || height <= 0)
		return out;

	out.valid = true;
	out.width = WindowSnap_MakeEven(width);
	out.height = WindowSnap_MakeEven(height);

	const AspectMatch match = AspectRatio_Match(width, height);
	if (!match.valid || !match.entry)
		return out;
	out.entry = match.entry;

	if (snapRatio) {
		const double ratio = match.entry->ratio;
		if (axis == SnapAxis::KeepWidth) {
			out.height = AspectRatio_HeightForWidth(out.width, ratio);
		} else {
			out.width = AspectRatio_WidthForHeight(out.height, ratio);
		}
		out.snappedRatio = !match.exact;
	}

	if (snapResolution) {
		std::size_t count = 0;
		const StandardResolution* table = WindowSnap_Table(count);
		const StandardResolution* best = nullptr;
		int bestDistance = 0;
		const int pull = WindowSnap_ResolutionPull(out.width);
		for (std::size_t i = 0; i < count; ++i) {
			// Only consider standard sizes of the SAME named ratio, so a 2.39:1 drag can never be
			// yanked onto a 16:9 resolution just because the pixel counts happen to be close.
			const AspectMatch entryMatch = AspectRatio_Match(table[i].width, table[i].height);
			if (!entryMatch.valid || entryMatch.entry != match.entry)
				continue;
			const int dw = table[i].width - out.width;
			const int dh = table[i].height - out.height;
			const int distance = (dw < 0 ? -dw : dw) + (dh < 0 ? -dh : dh);
			if (distance > pull)
				continue;
			if (!best || distance < bestDistance) {
				best = &table[i];
				bestDistance = distance;
			}
		}
		if (best) {
			out.width = best->width;
			out.height = best->height;
			out.snappedResolution = true;
			out.resolutionLabel = best->label;
			return out;
		}

		// No named resolution nearby: land on the nearest size that IS exactly this ratio. The
		// grid pass runs on the dragged axis only, same as the ratio pass, so the edge under the
		// cursor still leads.
		//
		// Gated on snapRatio as well, because the grid IS a ratio snap in disguise: in
		// resolution-only mode ("snap resolution") the promise is that a drag is never reshaped,
		// only pulled onto a named size when one is close -- so away from the table the size must
		// pass through untouched.
		if (!snapRatio)
			return out;
		const int beforeWidth = out.width;
		const int beforeHeight = out.height;
		int stepWidth = 0, stepHeight = 0;
		if (AspectRatio_ExactStep(match.entry->ratio, stepWidth, stepHeight)) {
			int multiple = (axis == SnapAxis::KeepWidth)
				? (out.width + stepWidth / 2) / stepWidth
				: (out.height + stepHeight / 2) / stepHeight;
			if (multiple < 1)
				multiple = 1;
			out.width = multiple * stepWidth;
			out.height = multiple * stepHeight;
		} else if (axis == SnapAxis::KeepWidth) {
			int width2 = ((out.width + kSnapFreeStep / 2) / kSnapFreeStep) * kSnapFreeStep;
			if (width2 < kSnapFreeStep) width2 = kSnapFreeStep;
			out.width = width2;
			out.height = AspectRatio_HeightForWidth(width2, match.entry->ratio);
		} else {
			int height2 = ((out.height + kSnapFreeStep / 2) / kSnapFreeStep) * kSnapFreeStep;
			if (height2 < kSnapFreeStep) height2 = kSnapFreeStep;
			out.height = height2;
			out.width = AspectRatio_WidthForHeight(height2, match.entry->ratio);
		}
		out.snappedGrid = (out.width != beforeWidth || out.height != beforeHeight);
	}

	return out;
}

} // namespace Filmmaker
