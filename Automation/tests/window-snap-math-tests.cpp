#include "../../AfxHookSource2/Filmmaker/Window/WindowSnapMath.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

using Filmmaker::SnapAxis;
using Filmmaker::SnapResult;

void Fail(const char* name, const char* detail) {
	std::cerr << name << ": " << detail << "\n";
	std::exit(1);
}

SnapResult Solve(int width, int height, SnapAxis axis, bool ratio = true, bool resolution = true) {
	return Filmmaker::WindowSnap_Solve(width, height, axis, ratio, resolution);
}

void ExpectSize(const char* name, const SnapResult& result, int expectedW, int expectedH) {
	if (!result.valid)
		Fail(name, "expected a valid result");
	if (result.width != expectedW || result.height != expectedH) {
		std::cerr << name << ": expected " << expectedW << "x" << expectedH << ", got "
			<< result.width << "x" << result.height << "\n";
		std::exit(1);
	}
}

void ExpectEven(const char* name, const SnapResult& result) {
	if (result.width % 2 || result.height % 2)
		Fail(name, "expected even dimensions");
}

// Every standard resolution must be reachable from a nearby drag, and must already be a fixed
// point of the snap. A row that snaps to something else is a table bug.
void ExpectTableIsStable() {
	std::size_t count = 0;
	const Filmmaker::StandardResolution* table = Filmmaker::WindowSnap_Table(count);
	if (count == 0)
		Fail("snap table", "empty");
	for (std::size_t i = 0; i < count; ++i) {
		const SnapResult exact = Solve(table[i].width, table[i].height, SnapAxis::KeepWidth);
		if (exact.width != table[i].width || exact.height != table[i].height) {
			std::cerr << "snap table: " << table[i].width << "x" << table[i].height
				<< " (" << table[i].label << ") moved to "
				<< exact.width << "x" << exact.height << "\n";
			std::exit(1);
		}
	}
}

} // namespace

int main() {
	// Invalid input is rejected, not guessed at.
	if (Solve(0, 1080, SnapAxis::KeepWidth).valid)
		Fail("zero width", "expected invalid");
	if (Solve(1920, -1, SnapAxis::KeepWidth).valid)
		Fail("negative height", "expected invalid");

	// Ratio snap: a few pixels off 16:9 becomes exactly 16:9, keeping the dragged axis.
	{
		const SnapResult r = Solve(1920, 1064, SnapAxis::KeepWidth, true, false);
		ExpectSize("ratio keep width", r, 1920, 1080);
		if (!r.snappedRatio)
			Fail("ratio keep width", "expected snappedRatio");
		if (r.snappedResolution)
			Fail("ratio keep width", "resolution snap was disabled");
	}
	{
		// Dragging the bottom edge: height leads, width follows.
		const SnapResult r = Solve(1900, 1080, SnapAxis::KeepHeight, true, false);
		ExpectSize("ratio keep height", r, 1920, 1080);
	}

	// A size that is already an exact ratio is left alone and not reported as snapped.
	{
		const SnapResult r = Solve(1600, 900, SnapAxis::KeepWidth, true, false);
		ExpectSize("already exact", r, 1600, 900);
		if (r.snappedRatio)
			Fail("already exact", "nothing needed snapping");
	}

	// Resolution snap lands exactly on the standard size and names it.
	{
		const SnapResult r = Solve(1908, 1074, SnapAxis::KeepWidth);
		ExpectSize("near 1080p", r, 1920, 1080);
		if (!r.snappedResolution || !r.resolutionLabel || 0 != std::strcmp(r.resolutionLabel, "1080p"))
			Fail("near 1080p", "expected the 1080p label");
	}

	// The pull scales with the size being dragged, so a 4K drag snaps as readily as a 720p one.
	// 3800x2138 is 40px+22px off 4K UHD -- outside the old fixed 32px window, inside this one.
	{
		const SnapResult r = Solve(3800, 2138, SnapAxis::KeepWidth);
		ExpectSize("near 4K", r, 3840, 2160);
		if (!r.snappedResolution || !r.resolutionLabel || 0 != std::strcmp(r.resolutionLabel, "4K UHD"))
			Fail("near 4K", "expected the 4K UHD label");
	}

	// Far from any NAMED size the grid pass takes over: the result is still a whole multiple of the
	// ratio (16:9 quantises to 32x18), never a near miss like 1500x844 (1.7773, not 1.7778).
	{
		const SnapResult r = Solve(1500, 830, SnapAxis::KeepWidth);
		if (r.snappedResolution)
			Fail("far from standard", "no named resolution is that close");
		if (!r.snappedGrid)
			Fail("far from standard", "expected the grid snap to fire");
		ExpectSize("far from standard", r, 1504, 846); // 47 x (32x18)
		if (r.width * 9 != r.height * 16)
			Fail("far from standard", "expected an exact 16:9 multiple");
	}

	// Dragging a bottom edge quantises the HEIGHT and derives the width, so the edge under the
	// cursor still leads.
	{
		const SnapResult r = Solve(1500, 830, SnapAxis::KeepHeight);
		ExpectSize("grid keep height", r, 1472, 828); // 46 x (32x18)
		if (r.width * 9 != r.height * 16)
			Fail("grid keep height", "expected an exact 16:9 multiple");
	}

	// A ratio with no small whole pair (2.39:1 is 239/100) cannot land on an exact multiple, so the
	// dragged axis quantises to a round step and the other is derived.
	{
		const SnapResult r = Solve(3000, 1250, SnapAxis::KeepWidth);
		if (!r.entry || 0 != std::strcmp(r.entry->label, "2.39:1"))
			Fail("grid scope", "expected the 2.39:1 ratio");
		ExpectSize("grid scope", r, 3008, 1258); // 3008 / 2.39 = 1258.58, nearest even
		ExpectEven("grid scope", r);
	}

	// The grid never reports itself as having moved a size that was already on it.
	{
		const SnapResult r = Solve(1504, 846, SnapAxis::KeepWidth);
		ExpectSize("already on the grid", r, 1504, 846);
		if (r.snappedGrid)
			Fail("already on the grid", "nothing needed snapping");
	}

	// Grid quantisation must not override a named resolution that is not on the grid. 1998x1080
	// (DCI flat, 1.85:1) is exactly 27 x (74x40), but the point stands for the table generally:
	// the named size wins and keeps its label.
	{
		const SnapResult r = Solve(1990, 1076, SnapAxis::KeepWidth);
		ExpectSize("DCI flat", r, 1998, 1080);
		if (!r.snappedResolution || !r.resolutionLabel || 0 != std::strcmp(r.resolutionLabel, "DCI flat"))
			Fail("DCI flat", "expected the DCI flat label");
		if (r.snappedGrid)
			Fail("DCI flat", "the table pass should have returned first");
	}

	// Ratio-only mode is unchanged by the grid work: it shapes the window and stops there.
	{
		const SnapResult r = Solve(1500, 830, SnapAxis::KeepWidth, true, false);
		ExpectSize("ratio only", r, 1500, 844);
		if (r.snappedGrid || r.snappedResolution)
			Fail("ratio only", "only the ratio snap was enabled");
	}

	// Resolution-only mode never reshapes a drag: away from the table the size passes through
	// untouched (the grid is a ratio snap in disguise and must not fire) ...
	{
		const SnapResult r = Solve(1500, 830, SnapAxis::KeepWidth, false, true);
		ExpectSize("resolution only, far", r, 1500, 830);
		if (r.snappedGrid || r.snappedRatio || r.snappedResolution)
			Fail("resolution only, far", "nothing should have fired away from the table");
	}
	// ... while a drag near a named size still lands on it.
	{
		const SnapResult r = Solve(1908, 1074, SnapAxis::KeepWidth, false, true);
		ExpectSize("resolution only, near", r, 1920, 1080);
		if (!r.snappedResolution || !r.resolutionLabel || 0 != std::strcmp(r.resolutionLabel, "1080p"))
			Fail("resolution only, near", "expected the 1080p label");
	}

	// A cinema drag must never be yanked onto a monitor resolution of a different ratio.
	{
		const SnapResult r = Solve(1930, 810, SnapAxis::KeepWidth);
		if (r.snappedResolution && r.resolutionLabel && 0 == std::strcmp(r.resolutionLabel, "1080p"))
			Fail("scope drag", "snapped to a 16:9 resolution");
		if (!r.entry || 0 != std::strcmp(r.entry->label, "2.39:1"))
			Fail("scope drag", "expected the 2.39:1 ratio");
		ExpectSize("scope drag", r, 1920, 804);
	}

	// Vertical and square shapes work the same way.
	{
		const SnapResult r = Solve(1074, 1912, SnapAxis::KeepWidth);
		ExpectSize("vertical", r, 1080, 1920);
	}
	{
		const SnapResult r = Solve(1086, 1074, SnapAxis::KeepWidth);
		ExpectSize("square", r, 1080, 1080);
	}

	// Snapping disabled entirely still returns a usable (even) size.
	{
		const SnapResult r = Solve(1919, 1079, SnapAxis::KeepWidth, false, false);
		ExpectEven("no snap", r);
		ExpectSize("no snap", r, 1920, 1080);
		if (r.snappedRatio || r.snappedResolution)
			Fail("no snap", "nothing should have snapped");
	}

	ExpectEven("evenness", Solve(1333, 751, SnapAxis::KeepWidth));
	ExpectTableIsStable();

	return 0;
}
