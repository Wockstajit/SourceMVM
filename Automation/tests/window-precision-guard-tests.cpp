// Tests for WindowPrecisionGuard.h -- the answer to "Windows Snap grabbed my Shift-drag resize".
//
// The guard's whole job is to say, at WM_WINDOWPOSCHANGING time, whether the size the OS is about
// to apply is one WE approved. These run without a window, a desktop, or a Shift key.

#include "../../AfxHookSource2/Filmmaker/Window/WindowPrecisionGuard.h"

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
	if (condition)
		return;
	std::printf("FAIL: %s\n", what);
	++g_failures;
}

void CheckInt(int actual, int expected, const char* what) {
	if (actual == expected)
		return;
	std::printf("FAIL: %s (got %d, expected %d)\n", what, actual, expected);
	++g_failures;
}

Filmmaker::PrecisionGuardState ApprovedSession() {
	Filmmaker::PrecisionGuardState state;
	// A Shift-held drag that WM_SIZING settled on a 1920x1080 client inside a 1936x1119 window.
	Filmmaker::PrecisionGuard_Approve(state, /*shiftHeld*/ true, 100, 60, 1936, 1119, 1920, 1080);
	return state;
}

void TestSnapDuringPrecisionIsRefused() {
	const Filmmaker::PrecisionGuardState state = ApprovedSession();
	// Aero Snap: half the screen, pinned to the left edge.
	const Filmmaker::PrecisionGuardDecision d = Filmmaker::PrecisionGuard_Evaluate(
		state, /*inModalLoop*/ true, /*shiftHeld*/ true, /*sizeChanging*/ true, /*moveChanging*/ true,
		0, 0, 960, 1080);
	Check(d.clampSize, "a snap that changes the size during a precision drag is clamped");
	CheckInt(d.width, 1936, "clamped width is the approved window width");
	CheckInt(d.height, 1119, "clamped height is the approved window height");
	Check(d.clampMove, "a snap that also repositions is put back");
	CheckInt(d.x, 100, "clamped x is the approved origin");
	CheckInt(d.y, 60, "clamped y is the approved origin");
}

void TestApprovedSizeIsLeftAlone() {
	const Filmmaker::PrecisionGuardState state = ApprovedSession();
	const Filmmaker::PrecisionGuardDecision d = Filmmaker::PrecisionGuard_Evaluate(
		state, true, true, true, true, 100, 60, 1936, 1119);
	Check(!d.clampSize, "the size we approved is not re-asserted over itself");
	Check(!d.clampMove, "an unchanged origin is not rewritten");
}

void TestPlainDragIsUntouched() {
	Filmmaker::PrecisionGuardState state;
	// No Shift: the guard never arms, and Aero Snap stays a feature.
	Filmmaker::PrecisionGuard_Approve(state, /*shiftHeld*/ false, 100, 60, 1936, 1119, 1920, 1080);
	Check(!state.active, "releasing Shift disarms the guard");
	const Filmmaker::PrecisionGuardDecision d = Filmmaker::PrecisionGuard_Evaluate(
		state, true, false, true, true, 0, 0, 960, 1080);
	Check(!d.clampSize, "a snap during a plain drag is allowed through");
}

void TestShiftReleasedMidDragStopsClamping() {
	const Filmmaker::PrecisionGuardState state = ApprovedSession();
	// The state is still armed from the last WM_SIZING, but Shift is up NOW.
	const Filmmaker::PrecisionGuardDecision d = Filmmaker::PrecisionGuard_Evaluate(
		state, true, /*shiftHeld*/ false, true, true, 0, 0, 960, 1080);
	Check(!d.clampSize, "Shift released between messages hands the window straight back");
}

void TestOutsideModalLoopIsUntouched() {
	const Filmmaker::PrecisionGuardState state = ApprovedSession();
	// Maximise / restore / SDL / the engine's own video-mode change all land here.
	const Filmmaker::PrecisionGuardDecision d = Filmmaker::PrecisionGuard_Evaluate(
		state, /*inModalLoop*/ false, true, true, true, 0, 0, 1280, 720);
	Check(!d.clampSize, "a resize outside the drag loop is never clamped");
}

void TestMoveOnlyMessageIsUntouched() {
	const Filmmaker::PrecisionGuardState state = ApprovedSession();
	const Filmmaker::PrecisionGuardDecision d = Filmmaker::PrecisionGuard_Evaluate(
		state, true, true, /*sizeChanging*/ false, /*moveChanging*/ true, 4000, 900, 0, 0);
	Check(!d.clampSize, "a position-only change carries no size to defend");
	Check(!d.clampMove, "a move without a resize is not fought");
}

void TestSizeClampWithoutMoveFlag() {
	const Filmmaker::PrecisionGuardState state = ApprovedSession();
	const Filmmaker::PrecisionGuardDecision d = Filmmaker::PrecisionGuard_Evaluate(
		state, true, true, /*sizeChanging*/ true, /*moveChanging*/ false, 0, 0, 960, 1080);
	Check(d.clampSize, "a size-only snap is still refused");
	Check(!d.clampMove, "SWP_NOMOVE means there is no origin to write back");
}

void TestFinalSizeHijackDetection() {
	const Filmmaker::PrecisionGuardState state = ApprovedSession();
	Check(Filmmaker::PrecisionGuard_FinalSizeHijacked(state, 944, 1041),
		"a finished drag that does not match the approved client size is a hijack");
	Check(!Filmmaker::PrecisionGuard_FinalSizeHijacked(state, 1920, 1080),
		"a finished drag at the approved client size is not a hijack");

	Filmmaker::PrecisionGuardState idle;
	Check(!Filmmaker::PrecisionGuard_FinalSizeHijacked(idle, 944, 1041),
		"a drag that never used precision mode cannot be hijacked");
}

void TestReleaseDisarms() {
	Filmmaker::PrecisionGuardState state = ApprovedSession();
	Filmmaker::PrecisionGuard_Release(state);
	Check(!state.active, "release disarms the guard");
	const Filmmaker::PrecisionGuardDecision d = Filmmaker::PrecisionGuard_Evaluate(
		state, true, true, true, true, 0, 0, 960, 1080);
	Check(!d.clampSize, "a released guard clamps nothing");
}

void TestDegenerateApprovalIsIgnored() {
	Filmmaker::PrecisionGuardState state;
	Filmmaker::PrecisionGuard_Approve(state, true, 0, 0, 0, 1119, 1920, 1080);
	Check(!state.active, "a zero-width approval is refused rather than stored");
}

} // namespace

int main() {
	TestSnapDuringPrecisionIsRefused();
	TestApprovedSizeIsLeftAlone();
	TestPlainDragIsUntouched();
	TestShiftReleasedMidDragStopsClamping();
	TestOutsideModalLoopIsUntouched();
	TestMoveOnlyMessageIsUntouched();
	TestSizeClampWithoutMoveFlag();
	TestFinalSizeHijackDetection();
	TestReleaseDisarms();
	TestDegenerateApprovalIsIgnored();

	if (g_failures != 0) {
		std::printf("%d check(s) failed.\n", g_failures);
		return EXIT_FAILURE;
	}
	std::printf("WindowPrecisionGuardTests: all checks passed.\n");
	return EXIT_SUCCESS;
}
