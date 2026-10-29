#include "../../AfxHookSource2/Filmmaker/Render/RenderTypes.h"
#include "../../AfxHookSource2/Filmmaker/Render/RenderRange.h"
#include "../../AfxHookSource2/Filmmaker/Render/RenderSession.h"
#include "../../AfxHookSource2/Filmmaker/Render/RenderValidation.h"

#include <cmath>
#include <cfloat>
#include <cstdlib>
#include <climits>
#include <cstdint>
#include <iostream>
#include <cstring>

using namespace Filmmaker;

namespace {
void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAIL: " << message << "\n";
		std::exit(1);
	}
}
}

int main() {
	// Picking the Camera Path camera takes over every range except a path piece already chosen.
	Check(PathCameraTakesPathRange(RenderRangeMode::FullDemo), "path camera takes the full-demo range");
	Check(PathCameraTakesPathRange(RenderRangeMode::CurrentRound), "path camera takes the round range");
	Check(PathCameraTakesPathRange(RenderRangeMode::CustomTicks), "path camera takes a custom range");
	Check(!PathCameraTakesPathRange(RenderRangeMode::SelectedCameraSegment), "path camera keeps a chosen segment");
	Check(!PathCameraTakesPathRange(RenderRangeMode::CameraPathBounds), "path camera leaves the path range alone");
	// Default range is invalid.
	{
		RenderRange r;
		Check(!r.Valid(), "default range invalid");
		Check(r.TickCount() == 0, "default tick count zero");
	}

	// Valid range with startTick < endTickExclusive.
	{
		RenderRange r;
		r.startTick = 100;
		r.endTickExclusive = 200;
		Check(r.Valid(), "valid range 100-200");
		Check(r.TickCount() == 100, "tick count 100");
	}

	// Range with startTick == endTickExclusive is invalid.
	{
		RenderRange r;
		r.startTick = 50;
		r.endTickExclusive = 50;
		Check(!r.Valid(), "equal ticks invalid");
		Check(r.TickCount() == 0, "zero tick count for equal ticks");
	}

	// Range with startTick > endTickExclusive is invalid.
	{
		RenderRange r;
		r.startTick = 200;
		r.endTickExclusive = 100;
		Check(!r.Valid(), "inverted range invalid");
	}

	// FrameCount: 100 ticks at 64 tickrate, 60 FPS -> 94 frames.
	{
		RenderRange r;
		r.startTick = 0;
		r.endTickExclusive = 100;
		Check(r.FrameCount(60, 64.0) == 94, "100 ticks 60fps @64tr = 94 frames");
	}

	// FrameCount: 640 ticks at 64 tickrate, 60 FPS -> 600 frames.
	{
		RenderRange r;
		r.startTick = 0;
		r.endTickExclusive = 640;
		Check(r.FrameCount(60, 64.0) == 600, "640 ticks 60fps @64tr = 600 frames");
	}

	// FrameCount: 640 ticks at 64 tickrate, 30 FPS -> 300 frames.
	{
		RenderRange r;
		r.startTick = 0;
		r.endTickExclusive = 640;
		Check(r.FrameCount(30, 64.0) == 300, "640 ticks 30fps @64tr = 300 frames");
	}

	// FrameCount: 128 ticks at 128 tickrate, 60 FPS -> 60 frames.
	{
		RenderRange r;
		r.startTick = 0;
		r.endTickExclusive = 128;
		Check(r.FrameCount(60, 128.0) == 60, "128 ticks 60fps @128tr = 60 frames");
	}

	// FrameCount: 1 tick at 64 tickrate, 60 FPS -> 1 frame.
	{
		RenderRange r;
		r.startTick = 0;
		r.endTickExclusive = 1;
		Check(r.FrameCount(60, 64.0) == 1, "1 tick 60fps = 1 frame");
	}

	// FrameCount: 1 tick at 64 tickrate, 120 FPS -> 2 frames.
	{
		RenderRange r;
		r.startTick = 0;
		r.endTickExclusive = 1;
		Check(r.FrameCount(120, 64.0) == 2, "1 tick 120fps = 2 frames");
	}

	// FrameCount at CS:S's 66.67 tick rate (a 0.015 s tick): ticks * fps / rate, rounded up, with
	// the floating-point error of a whole number of frames kept out.
	{
		const double css = 1.0 / 0.015;
		RenderRange r;
		r.startTick = 1000;
		r.endTickExclusive = 1200; // 3 s
		Check(r.FrameCount(60, css) == 180, "200 ticks 60fps @66.67tr = 180 frames");
		Check(r.FrameCount(30, css) == 90, "200 ticks 30fps @66.67tr = 90 frames");
		r.endTickExclusive = 1001;
		Check(r.FrameCount(60, css) == 1, "1 tick 60fps @66.67tr = 1 frame");
		r.endTickExclusive = 1100; // 1.5 s
		Check(r.FrameCount(24, css) == 36, "100 ticks 24fps @66.67tr = 36 frames");
	}

	// FrameCount: zero fps -> 0.
	{
		RenderRange r;
		r.startTick = 0;
		r.endTickExclusive = 100;
		Check(r.FrameCount(0, 64.0) == 0, "zero fps = zero frames");
	}

	// FrameCount: zero tickrate -> 0.
	{
		RenderRange r;
		r.startTick = 0;
		r.endTickExclusive = 100;
		Check(r.FrameCount(60, 0.0) == 0, "zero tickrate = zero frames");
	}

	// FrameCount: negative tickrate -> 0.
	{
		RenderRange r;
		r.startTick = 0;
		r.endTickExclusive = 100;
		Check(r.FrameCount(60, -1.0) == 0, "negative tickrate = zero frames");
	}

	// FrameCount: NaN tickrate -> 0.
	{
		RenderRange r;
		r.startTick = 0;
		r.endTickExclusive = 100;
		Check(r.FrameCount(60, NAN) == 0, "NaN tickrate = zero frames");
	}

	// FrameCount: Inf tickrate -> 0.
	{
		RenderRange r;
		r.startTick = 0;
		r.endTickExclusive = 100;
		Check(r.FrameCount(60, INFINITY) == 0, "Inf tickrate = zero frames");
	}

	// FrameCount: a tick rate that is not whole counts too (CS:S): ceil(100 * 60 / 63.5) = 95.
	{
		RenderRange r;
		r.startTick = 0;
		r.endTickExclusive = 100;
		Check(r.FrameCount(60, 63.5) == 95, "non-integer tickrate counts");
	}

	// FrameCount: valid range but zero ticks (width zero).
	{
		RenderRange r;
		r.startTick = 50;
		r.endTickExclusive = 50;
		Check(r.FrameCount(60, 64.0) == 0, "zero-width range = zero frames");
	}

	// FrameCount: near-integer tickrate 64.00000001 is accepted.
	{
		RenderRange r;
		r.startTick = 0;
		r.endTickExclusive = 640;
		Check(r.FrameCount(60, 64.00000001) == 600, "near-integer tickrate accepted");
	}

	// FrameCount: huge finite tickrate rejected (exceeds INT_MAX after round).
	{
		RenderRange r;
		r.startTick = 0;
		r.endTickExclusive = 100;
		Check(r.FrameCount(60, DBL_MAX) == 0, "huge finite tickrate rejected");
	}

	// TickCount64: wide range is exact.
	{
		RenderRange r;
		r.startTick = INT_MIN;
		r.endTickExclusive = INT_MAX;
		Check(r.TickCount64() == 4294967295LL, "wide tick count is exact");
	}

	// TickCount: int overflow rejected.
	{
		RenderRange r;
		r.startTick = INT_MIN;
		r.endTickExclusive = INT_MAX;
		Check(r.TickCount() == 0, "tick count rejects int overflow");
	}

	// FrameCount: result overflow rejected.
	{
		RenderRange r;
		r.startTick = INT_MIN;
		r.endTickExclusive = INT_MAX;
		Check(r.FrameCount(1, 1.0) == 0, "frame result rejects int overflow");
	}

	// RenderRangeMode values exist.
	{
		Check((int)RenderRangeMode::FullDemo >= 0, "FullDemo enum");
		Check((int)RenderRangeMode::CurrentRound >= 0, "CurrentRound enum");
		Check((int)RenderRangeMode::CameraPathBounds >= 0, "CameraPathBounds enum");
		Check((int)RenderRangeMode::SelectedCameraSegment >= 0, "SelectedCameraSegment enum");
		Check((int)RenderRangeMode::CustomTicks >= 0, "CustomTicks enum");
	}

	// RenderCameraSource values exist.
	{
		Check((int)RenderCameraSource::CurrentViewer >= 0, "CurrentViewer enum");
		Check((int)RenderCameraSource::Pov >= 0, "Pov enum");
		Check((int)RenderCameraSource::ThirdPerson >= 0, "ThirdPerson enum");
		Check((int)RenderCameraSource::FreeCamera >= 0, "FreeCamera enum");
		Check((int)RenderCameraSource::CameraPath >= 0, "CameraPath enum");
		Check((int)RenderCameraSource::FollowCamera >= 0, "FollowCamera enum");
	}

	// RenderSessionState values exist.
	{
		Check((int)RenderSessionState::Idle >= 0, "Idle state");
		Check((int)RenderSessionState::Completed >= 0, "Completed state");
		Check((int)RenderSessionState::Failed >= 0, "Failed state");
		Check((int)RenderSessionState::Canceled >= 0, "Canceled state");
	}

	// CameraPathExtension values exist and Fail is the zero-value.
	{
		Check((int)CameraPathExtension::Fail == 0, "Fail is zero-value default");
		Check((int)CameraPathExtension::HoldFirst == 1, "HoldFirst enum");
		Check((int)CameraPathExtension::HoldLast == 2, "HoldLast enum");
		Check((int)CameraPathExtension::Clamp == 3, "Clamp enum");
	}

	// NOTE: RenderStateRestoreGuard's RAII tests used to live here. The guard now performs real
	// engine reads/writes (camera bridge, movie mode, effect modules, cvars), so it can no longer
	// link into a standalone test binary. Its lifecycle rules -- snapshot-once, restore-once,
	// destructor-restores -- are unchanged and documented in RenderStateGuard.h; they are not
	// covered by an automated test at present.

	// RenderValidation: a resolved custom range with usable timing is valid.
	{
		RenderRange r;
		r.startTick = 10;
		r.endTickExclusive = 74;
		const RenderValidationResult result = RenderValidate(
			r, RenderCameraSource::CurrentViewer, CameraPathExtension::Fail, 60, 64.0);
		Check(result.Ok(), "resolved custom range validates");
		Check(result.FatalCount() == 0, "valid result has no fatals");
		Check(result.WarningCount() == 0, "valid result has no warnings");
		Check(0 == strcmp(result.Token(), "ok"), "valid result token is ok");
	}

	// RenderValidation: camera-path coverage. Was an unconditional fatal, which made camera-path
	// rendering impossible even though the session driver implements it.
	{
		RenderRange r;
		r.startTick = 100;
		r.endTickExclusive = 200;
		RenderCameraPathCoverage cov;
		cov.resolved = true;
		cov.keyCount = 4;
		cov.firstTick = 100;
		cov.lastTick = 199;
		const RenderValidationResult covered = RenderValidate(
			r, RenderCameraSource::CameraPath, CameraPathExtension::Fail, 60, 64.0, cov);
		Check(covered.Ok(), "a camera-path range inside the keyframes validates");

		const RenderValidationResult unresolved = RenderValidate(
			r, RenderCameraSource::CameraPath, CameraPathExtension::Fail, 60, 64.0);
		Check(!unresolved.Ok(), "unresolved camera-path coverage stays fatal");

		RenderCameraPathCoverage tooFew = cov;
		tooFew.keyCount = 1;
		const RenderValidationResult sparse = RenderValidate(
			r, RenderCameraSource::CameraPath, CameraPathExtension::Fail, 60, 64.0, tooFew);
		Check(!sparse.Ok(), "a camera path with one key cannot render");

		RenderRange over;
		over.startTick = 50;
		over.endTickExclusive = 400;
		const RenderValidationResult failExt = RenderValidate(
			over, RenderCameraSource::CameraPath, CameraPathExtension::Fail, 60, 64.0, cov);
		Check(!failExt.Ok(), "overrunning the path is fatal when the extension is Fail");

		const RenderValidationResult holdExt = RenderValidate(
			over, RenderCameraSource::CameraPath, CameraPathExtension::HoldLast, 60, 64.0, cov);
		Check(holdExt.Ok(), "an extension that holds the boundary key lets the job run");
		Check(holdExt.WarningCount() > 0, "...but warns that those frames are a held camera");
	}

	// RenderValidation: invalid range and timing failures are structured fatal errors.
	{
		RenderRange r;
		const RenderValidationResult result = RenderValidate(
			r, RenderCameraSource::CurrentViewer, CameraPathExtension::Fail, 0, 64.0);
		Check(!result.Ok(), "invalid range is rejected");
		Check(result.FatalCount() == 1, "invalid range reported once");
		Check(!result.FirstFatal().empty(), "fatal message is available");
		Check(0 == strcmp(result.Token(), "invalid"), "fatal result token is invalid");
	}

	{
		RenderRange r;
		r.startTick = 0;
		r.endTickExclusive = 64;
		const RenderValidationResult result = RenderValidate(
			r, RenderCameraSource::CurrentViewer, CameraPathExtension::Fail, 0, 64.0);
		Check(!result.Ok(), "invalid output timing is rejected");
		Check(result.FatalCount() == 1, "invalid timing has one fatal");
	}

	// RenderValidation: range modes resolve their ticks eagerly at selection time
	// (RenderCommand's range handler), so by validation a non-custom mode with concrete,
	// usable ticks is valid -- and one whose data could NOT be resolved (ticks never filled
	// in, still start >= end) is rejected.
	{
		RenderRange r;
		r.mode = RenderRangeMode::FullDemo;
		r.startTick = 0;
		r.endTickExclusive = 64;
		const RenderValidationResult result = RenderValidate(
			r, RenderCameraSource::CurrentViewer, CameraPathExtension::Fail, 60, 64.0);
		Check(result.Ok(), "resolved non-custom range validates");
		Check(result.FatalCount() == 0, "resolved non-custom range has no fatals");
	}
	{
		RenderRange r;
		r.mode = RenderRangeMode::CurrentRound; // never resolved (no round markers pushed)
		const RenderValidationResult result = RenderValidate(
			r, RenderCameraSource::CurrentViewer, CameraPathExtension::Fail, 60, 64.0);
		Check(!result.Ok(), "unresolved range mode is rejected");
		Check(result.FatalCount() == 1, "unresolved range has one fatal");
	}

	// RenderValidation: camera-path jobs cannot pass without live keyframe coverage.
	{
		RenderRange r;
		r.startTick = 0;
		r.endTickExclusive = 64;
		const RenderValidationResult result = RenderValidate(
			r, RenderCameraSource::CameraPath, CameraPathExtension::HoldLast, 60, 64.0);
		Check(!result.Ok(), "unresolved camera-path coverage is rejected");
		Check(result.FatalCount() == 1, "camera-path coverage has one fatal");
	}

	// RenderSession: starts Idle, not busy, not terminal.
	{
		RenderSession s;
		Check(s.State() == RenderSessionState::Idle, "session starts Idle");
		Check(!s.IsBusy(), "Idle is not busy");
		Check(!s.IsTerminalNow(), "Idle is not terminal");
		Check(!s.IsCancelRequested(), "no cancel pending initially");
	}

	// RenderSession: the happy path walks the whole pipeline in order.
	{
		RenderSession s;
		const RenderSessionState path[] = {
			RenderSessionState::Validating,
			RenderSessionState::PreparingOutput,
			RenderSessionState::SavingState,
			RenderSessionState::SeekingPreRoll,
			RenderSessionState::WarmingUp,
			RenderSessionState::Capturing,
			RenderSessionState::FlushingGpu,
			RenderSessionState::FinalizingStreams,
			RenderSessionState::PostProcessing,
			RenderSessionState::RestoringState,
			RenderSessionState::Completed,
		};
		for (RenderSessionState st : path)
			Check(s.TryTransition(st), "happy-path transition legal");
		Check(s.IsTerminalNow(), "Completed is terminal");
		Check(!s.IsBusy(), "terminal is not busy");
	}

	// RenderSession: skipping stages is rejected and leaves the state untouched.
	{
		RenderSession s;
		Check(!s.TryTransition(RenderSessionState::Capturing), "Idle -> Capturing rejected");
		Check(s.State() == RenderSessionState::Idle, "rejected transition does not change state");
	}

	// RenderSession: terminal states are sticky -- no resurrection without Reset().
	{
		RenderSession s;
		Check(s.TryTransition(RenderSessionState::Validating), "to Validating");
		Check(s.TryTransition(RenderSessionState::Failed), "any live state may fail");
		Check(s.IsTerminalNow(), "Failed is terminal");
		Check(!s.TryTransition(RenderSessionState::Validating), "cannot leave Failed");
		Check(!s.TryTransition(RenderSessionState::Capturing), "cannot resurrect into Capturing");
		Check(s.State() == RenderSessionState::Failed, "still Failed");
	}

	// RenderSession: Failed is reachable from every live state.
	{
		const RenderSessionState live[] = {
			RenderSessionState::Validating, RenderSessionState::PreparingOutput,
			RenderSessionState::SavingState, RenderSessionState::SeekingPreRoll,
			RenderSessionState::WarmingUp, RenderSessionState::Capturing,
		};
		for (RenderSessionState st : live) {
			RenderSession s;
			// walk to st
			const RenderSessionState order[] = {
				RenderSessionState::Validating, RenderSessionState::PreparingOutput,
				RenderSessionState::SavingState, RenderSessionState::SeekingPreRoll,
				RenderSessionState::WarmingUp, RenderSessionState::Capturing,
			};
			for (RenderSessionState step : order) {
				s.TryTransition(step);
				if (step == st) break;
			}
			Check(s.TryTransition(RenderSessionState::Failed), "Failed reachable from any live state");
		}
	}

	// RenderSession: cancel is cooperative -- it flags and parks, never forces a terminal state.
	{
		RenderSession s;
		s.TryTransition(RenderSessionState::Validating);
		s.TryTransition(RenderSessionState::PreparingOutput);
		s.RequestCancel();
		Check(s.IsCancelRequested(), "cancel flag raised");
		Check(s.State() == RenderSessionState::CancelRequested, "moved to CancelRequested");
		Check(!s.IsTerminalNow(), "CancelRequested is NOT terminal -- worker must finish the handshake");
		// The worker acknowledges by unwinding, then declaring Canceled.
		Check(!s.TryTransition(RenderSessionState::Capturing), "cancel never resumes work");
		Check(s.TryTransition(RenderSessionState::RestoringState), "cancel unwinds through restore");
		Check(s.TryTransition(RenderSessionState::Canceled), "worker declares Canceled");
		Check(s.IsTerminalNow(), "Canceled is terminal");
	}

	// RenderSession: cancel on an Idle session flags but does not strand the machine.
	{
		RenderSession s;
		s.RequestCancel();
		Check(s.IsCancelRequested(), "flag raised even when idle");
		Check(s.State() == RenderSessionState::Idle, "Idle session is not pushed to CancelRequested");
	}

	// RenderSession: Reset only from terminal/Idle, and it clears the cancel flag.
	{
		RenderSession s;
		s.TryTransition(RenderSessionState::Validating);
		Check(!s.Reset(), "cannot reset a running session");
		Check(s.State() == RenderSessionState::Validating, "failed reset leaves state alone");
		s.TryTransition(RenderSessionState::Failed);
		Check(s.Reset(), "reset from terminal ok");
		Check(s.State() == RenderSessionState::Idle, "reset returns to Idle");
		Check(!s.IsCancelRequested(), "reset clears cancel flag");
	}

	// RenderSession: every state has a name (no "Unknown" leaks into the UI).
	{
		const RenderSessionState all[] = {
			RenderSessionState::Idle, RenderSessionState::Validating,
			RenderSessionState::PreparingOutput, RenderSessionState::SavingState,
			RenderSessionState::SeekingPreRoll, RenderSessionState::WarmingUp,
			RenderSessionState::Capturing, RenderSessionState::FlushingGpu,
			RenderSessionState::FinalizingStreams, RenderSessionState::PostProcessing,
			RenderSessionState::RestoringState, RenderSessionState::Completed,
			RenderSessionState::Failed, RenderSessionState::CancelRequested,
			RenderSessionState::Canceled,
		};
		int n = 0;
		for (RenderSessionState st : all) {
			const char* nm = RenderSession::StateName(st);
			Check(nm && *nm, "state has a name");
			Check(0 != strcmp(nm, "Unknown"), "no state reports Unknown");
			++n;
		}
		Check(n == 15, "all 15 states covered by the name table");
	}

	std::cout << "RenderRangeTests: all checks passed\n";
	return 0;
}
