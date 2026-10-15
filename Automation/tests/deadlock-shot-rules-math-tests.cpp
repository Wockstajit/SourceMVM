#include "../../AfxHookSource2/Filmmaker/Deadlock/DeadlockShotRulesMath.h"

#include <cstdio>
#include <cstring>

// Camera-path ("shot") rules: CanPlay/CanScrub + messages, the arm->play->stop Space transport,
// marker placement, retiming, select prev/next, scrub-release settle and the parked-hold
// predicate. See DeadlockShotRulesMath.h for why each rule exists and which CS2 line it copies.

using namespace Deadlock::ShotRules;

int g_failures = 0;

void Fail(const char* name, const char* detail) {
	std::printf("FAIL %s: %s\n", name, detail);
	++g_failures;
}

void Check(bool value, const char* name, const char* detail) {
	if (!value) Fail(name, detail);
}

// ---------------------------------------------------------------------------------------------
// CanPlay / CanScrub / messages (r1, r3)
// ---------------------------------------------------------------------------------------------

void TestCanPlayThreshold() {
	Check(!CanPlay(0), "CanPlay(0)", "0 markers must refuse");
	Check(!CanPlay(1), "CanPlay(1)", "1 marker must refuse");
	Check(CanPlay(2), "CanPlay(2)", "2 markers is the exact threshold and must allow");
	Check(CanPlay(3), "CanPlay(3)", "more than 2 must still allow");
}

void TestCanScrubThreshold() {
	Check(!CanScrub(0), "CanScrub(0)", "0 markers must refuse");
	Check(!CanScrub(1), "CanScrub(1)", "1 marker must refuse");
	Check(CanScrub(2), "CanScrub(2)", "2 markers is the exact threshold and must allow");
}

void TestMinMarkerMessagesAreDistinct() {
	// r3: the play-refusal and scrub-refusal strings are DIFFERENT (scrub's omits "camera").
	Check(0 != std::strcmp(kNeedMarkersToPlayMessage, kNeedMarkersToScrubMessage),
		"distinct messages", "play and scrub refusal text must not be the same shared string");
	Check(0 == std::strcmp(kNeedMarkersToPlayMessage, "Need at least 2 camera markers to play path."),
		"play message text", "must match CameraPath.cpp:471 verbatim");
	Check(0 == std::strcmp(kNeedMarkersToScrubMessage, "Need at least 2 markers to scrub the path."),
		"scrub message text", "must match CameraPath.cpp:694 verbatim");
	Check(0 == std::strcmp(kPathNotEvaluableMessage, "Camera path could not be built."),
		"path-not-evaluable text", "must match CameraPath.cpp:479 verbatim");
	Check(0 == std::strcmp(kLastMarkerNoSegmentMessage, "Last marker has no outgoing segment."),
		"last-marker text", "must match CameraPath.cpp:382 verbatim");
	Check(0 == std::strcmp(kNothingToUndoMessage, "Nothing to undo."), "undo-empty text", "must match CameraPath.cpp:797");
	Check(0 == std::strcmp(kNothingToRedoMessage, "Nothing to redo."), "redo-empty text", "must match CameraPath.cpp:814");
}

// ---------------------------------------------------------------------------------------------
// ShotTransport: the arm -> play -> stop Space state machine (r12-r19)
// ---------------------------------------------------------------------------------------------

void TestSpace_EditorClosed_IsPlainToggleRegardlessOfMarkers() {
	ShotTransport t;
	Check(t.Mode() == ShotMode::Editing, "initial mode", "starts Editing");
	SpaceAction a = t.DecideSpace(/*editorOpen*/ false, /*playbackPending*/ false);
	Check(a == SpaceAction::PlainDemoToggle, "editor closed -> plain toggle", "MovieMode.cpp:451-463");
	t.Apply(a, /*canPlay*/ false);
	Check(t.Mode() == ShotMode::Editing, "plain toggle doesn't change mode", "a demo pause/resume never touches the shot mode");
}

void TestSpace_EditorOpen_Editing_ArmsAndPlaysWhenAllowed() {
	ShotTransport t;
	SpaceAction a = t.DecideSpace(/*editorOpen*/ true, /*playbackPending*/ false);
	Check(a == SpaceAction::ArmAndPlay, "editor open, nothing pending -> ArmAndPlay", "collapsed J+Space, see file banner");
	t.Apply(a, /*canPlay*/ true);
	Check(t.Mode() == ShotMode::Playing, "ArmAndPlay succeeds -> Playing", "canPlay true must start playback");
	Check(t.StartGuardFramesRemaining() == 30, "start guard opens", "CameraPath.cpp:514/572/1004 -- 30 frames");
}

void TestSpace_EditorOpen_Editing_RefusedArmLeavesModeUntouched() {
	// r1: ArmPreview's refusal is Notify + return with NO other side effect -- not a bounce
	// through Armed and back.
	ShotTransport t;
	SpaceAction a = t.DecideSpace(/*editorOpen*/ true, /*playbackPending*/ false);
	Check(a == SpaceAction::ArmAndPlay, "decision unaffected by canPlay", "the decision doesn't know about marker count yet");
	t.Apply(a, /*canPlay*/ false); // e.g. <2 markers
	Check(t.Mode() == ShotMode::Editing, "refused arm stays Editing", "r1: refusal has no side effect");
	Check(t.StartGuardFramesRemaining() == 0, "no guard opened on refusal", "nothing started, nothing to guard");
}

void TestSpace_EditorOpen_Editing_PendingSwallowsSpace() {
	// MovieMode.cpp:446's !PlaybackPending() guard: a repeated Space while a start is already in
	// flight must not re-issue the command.
	ShotTransport t;
	SpaceAction a = t.DecideSpace(/*editorOpen*/ true, /*playbackPending*/ true);
	Check(a == SpaceAction::Swallow, "pending start swallows Space", "MovieMode.cpp:446");
	t.Apply(a, /*canPlay*/ true);
	Check(t.Mode() == ShotMode::Editing, "swallow never changes mode", "a no-op action must be a true no-op");
}

void TestSpace_Armed_AlwaysPlays_RegardlessOfEditor() {
	// r19: once Armed/Playing, Space's meaning is fixed regardless of whether the editor is open.
	for (bool editorOpen : {false, true}) {
		ShotTransport t;
		t.Arm(/*canPlay*/ true);
		Check(t.Mode() == ShotMode::Armed, "Arm() enters Armed", "explicit arm outside the Space cycle");
		SpaceAction a = t.DecideSpace(editorOpen, /*playbackPending*/ false);
		Check(a == SpaceAction::Play, "Armed -> Play", "r13, unconditional on editor visibility (r19)");
		t.Apply(a, /*canPlay*/ true);
		Check(t.Mode() == ShotMode::Playing, "Play succeeds -> Playing", "");
	}
}

void TestSpace_Armed_FailedPlayStaysArmed() {
	// StartPreviewPlay's failure path (CameraPath.cpp:510) leaves mode at Armed, not Editing.
	ShotTransport t;
	t.Arm(true);
	SpaceAction a = t.DecideSpace(true, false);
	t.Apply(a, /*canPlay*/ false); // e.g. path became unevaluable between arm and play
	Check(t.Mode() == ShotMode::Armed, "failed play stays Armed", "CameraPath.cpp:510 does not revert to Editing");
}

void TestSpace_Playing_StopsAfterGuardElapses() {
	ShotTransport t;
	t.Arm(true);
	t.Apply(t.DecideSpace(true, false), true); // -> Playing, guard = 30
	Check(t.Mode() == ShotMode::Playing, "setup", "must be Playing before this test");

	// While the guard is open, Space is swallowed (r14) regardless of editor visibility.
	SpaceAction guarded = t.DecideSpace(/*editorOpen*/ true, false);
	Check(guarded == SpaceAction::Swallow, "guarded Space is swallowed", "r14: PausePreview no-ops under the guard");
	t.Apply(guarded, true);
	Check(t.Mode() == ShotMode::Playing, "swallow doesn't stop playback", "");

	for (int i = 0; i < 30; ++i) t.Tick();
	Check(t.StartGuardFramesRemaining() == 0, "guard elapses after 30 ticks", "CameraPath.cpp:514, 30-frame window");

	SpaceAction stop = t.DecideSpace(true, false);
	Check(stop == SpaceAction::Stop, "Space stops once guard elapses", "r14");
	t.Apply(stop, true);
	Check(t.Mode() == ShotMode::Editing, "Stop returns to Editing", "r14: PausePreview -> Mode::Editing");
}

void TestSpace_Playing_StopIgnoresEditorVisibility() {
	// r19: Playing's Space meaning (Stop) is fixed regardless of editor open/closed.
	for (bool editorOpen : {false, true}) {
		ShotTransport t;
		t.Arm(true);
		t.Apply(t.DecideSpace(true, false), true);
		for (int i = 0; i < 30; ++i) t.Tick();
		SpaceAction a = t.DecideSpace(editorOpen, false);
		Check(a == SpaceAction::Stop, "Stop regardless of editor visibility", "r19");
	}
}

void TestEsc_UnconditionalFromArmedAndPlaying_IgnoresGuard() {
	// r15: StopPreview (Esc) is NOT gated by the start-pause guard, unlike PausePreview (Space).
	ShotTransport armed;
	armed.Arm(true);
	armed.OnEsc();
	Check(armed.Mode() == ShotMode::Editing, "Esc from Armed", "r10/r15");

	ShotTransport playing;
	playing.Arm(true);
	playing.Apply(playing.DecideSpace(true, false), true); // -> Playing, guard open (30 frames left)
	Check(playing.StartGuardFramesRemaining() > 0, "guard still open", "setup: Esc must beat this");
	playing.OnEsc();
	Check(playing.Mode() == ShotMode::Editing, "Esc stops even under the guard", "r15: unconditional, unlike Space/r14");
	Check(playing.StartGuardFramesRemaining() == 0, "Esc clears the guard too", "no leftover state after a hard stop");
}

void TestEsc_FromEditing_IsNoOp() {
	ShotTransport t;
	t.OnEsc();
	Check(t.Mode() == ShotMode::Editing, "Esc from Editing is a no-op", "nothing to cancel");
}

// ---------------------------------------------------------------------------------------------
// DecideAddMarker (r20-r24)
// ---------------------------------------------------------------------------------------------

void TestAddMarker_IdleState_AddsIntoEmptyList() {
	AddMarkerState s; // all false/idle
	s.tickValid = true;
	AddMarkerDecision d = DecideAddMarker(s, nullptr, 0, 100);
	Check(d.outcome == AddMarkerOutcome::Add, "add into empty list", "0 markers is a valid starting point (r20: no mode gate)");
	Check(d.insertIndex == 0, "empty-list insert index", "CamMarkers::Add on an empty list appends at 0");
}

void TestAddMarker_IdleState_InsertsInSortedGap() {
	AddMarkerState s;
	s.tickValid = true;
	const Tick ticks[] = {10, 20, 30};
	Check(DecideAddMarker(s, ticks, 3, 5).insertIndex == 0, "before first", "new tick before every existing one");
	Check(DecideAddMarker(s, ticks, 3, 15).insertIndex == 1, "between #1 and #2", "strictly-greater scan (r22)");
	Check(DecideAddMarker(s, ticks, 3, 25).insertIndex == 2, "between #2 and #3", "");
	Check(DecideAddMarker(s, ticks, 3, 35).insertIndex == 3, "after last", "appended at the end");
}

void TestAddMarker_DuplicateTick_AlwaysAddsAfterTheGroup_NeverReplace() {
	// r23: a duplicate tick is ALLOWED and lands stably AFTER every existing marker sharing it --
	// never before, never in place of one.
	AddMarkerState s;
	s.tickValid = true;
	const Tick ticks[] = {10, 20, 20, 20, 30};
	AddMarkerDecision d = DecideAddMarker(s, ticks, 5, 20);
	Check(d.outcome == AddMarkerOutcome::Add, "duplicate tick is added", "r23: duplicates are allowed, not refused");
	Check(d.insertIndex == 4, "lands after the whole duplicate group", "stable: strictly-greater scan skips all three 20s");

	// A single existing duplicate too, and the exact-first/exact-last boundary cases.
	const Tick one[] = {20};
	Check(DecideAddMarker(s, one, 1, 20).insertIndex == 1, "single duplicate", "lands after the one existing 20");
	const Tick atStart[] = {5, 5, 10};
	Check(DecideAddMarker(s, atStart, 3, 5).insertIndex == 2, "duplicate group at the very start", "still lands after both 5s");
}

void TestAddMarker_RefusesWhilePlaying() {
	AddMarkerState s;
	s.tickValid = true;
	s.playing = true;
	const Tick ticks[] = {10, 20};
	AddMarkerDecision d = DecideAddMarker(s, ticks, 2, 15);
	Check(d.outcome == AddMarkerOutcome::Refuse, "refuse while playing", "the dolly owns the camera; see block comment");
	Check(d.insertIndex == -1, "no index on refusal", "insertIndex must not look valid");
}

void TestAddMarker_RefusesWhileScrubbing() {
	AddMarkerState s;
	s.tickValid = true;
	s.scrubbing = true;
	Check(DecideAddMarker(s, nullptr, 0, 0).outcome == AddMarkerOutcome::Refuse,
		"refuse while scrubbing", "the pushed pose is the deterministic scrub pose, already on the curve");
}

void TestAddMarker_RefusesWhileSeekSettling() {
	AddMarkerState s;
	s.tickValid = true;
	s.seekSettling = true;
	Check(DecideAddMarker(s, nullptr, 0, 0).outcome == AddMarkerOutcome::Refuse,
		"refuse mid-settle", "the reported tick is the pre-seek tick for up to dozens of frames");
}

void TestAddMarker_RefusesWhenTickInvalid() {
	AddMarkerState s; // tickValid left false
	Check(DecideAddMarker(s, nullptr, 0, 0).outcome == AddMarkerOutcome::Refuse,
		"refuse when tick unavailable", "documented divergence from CS2's tick-0 fallback (r21)");
}

void TestAddMarker_AllFourGuardsIndependentlyRefuse() {
	const Tick ticks[] = {10};
	{ AddMarkerState s; s.tickValid = false; s.playing = false; s.scrubbing = false; s.seekSettling = false;
	  Check(DecideAddMarker(s, ticks, 1, 5).outcome == AddMarkerOutcome::Refuse, "only tickValid false", ""); }
	{ AddMarkerState s; s.tickValid = true; s.playing = true;
	  Check(DecideAddMarker(s, ticks, 1, 5).outcome == AddMarkerOutcome::Refuse, "only playing", ""); }
	{ AddMarkerState s; s.tickValid = true; s.scrubbing = true;
	  Check(DecideAddMarker(s, ticks, 1, 5).outcome == AddMarkerOutcome::Refuse, "only scrubbing", ""); }
	{ AddMarkerState s; s.tickValid = true; s.seekSettling = true;
	  Check(DecideAddMarker(s, ticks, 1, 5).outcome == AddMarkerOutcome::Refuse, "only seekSettling", ""); }
	{ AddMarkerState s; s.tickValid = true; // nothing else set
	  Check(DecideAddMarker(s, ticks, 1, 5).outcome == AddMarkerOutcome::Add, "all guards clear -> Add", ""); }
}

void TestNudgeAboveEpsilon() {
	Check(NudgeAboveEpsilon(0.0) == kDuplicateTickEpsilon, "zero nudges up to epsilon", "CamPathEval.cpp:70/:88-89");
	Check(NudgeAboveEpsilon(0.0005) == kDuplicateTickEpsilon, "below epsilon nudges up", "");
	Check(NudgeAboveEpsilon(0.5) == 0.5, "above epsilon passes through unchanged", "");
	Check(NudgeAboveEpsilon(-0.5) == 0.5, "magnitude, not signed value", "CamPathEval takes fabs(dt) / a chord length (always >=0)");
	Check(NudgeAboveEpsilon(1e-3) == 1e-3, "exactly at epsilon passes through", "strict less-than in CamPathEval.cpp:70/:88");
}

// ---------------------------------------------------------------------------------------------
// ClampRetimeTick (r33-r35)
// ---------------------------------------------------------------------------------------------

void TestRetime_NoNeighbours_OnlyFloorsAtZero() {
	RetimeClamp r = ClampRetimeTick(false, 0, false, 0, /*current*/ 50, /*requested*/ -10);
	Check(r.tick == 0, "floors at 0 with no neighbours", "CameraPath.cpp:834");
	Check(r.changed == true, "changed from 50 to 0", "");

	RetimeClamp r2 = ClampRetimeTick(false, 0, false, 0, 50, 200);
	Check(r2.tick == 200, "no neighbours: any non-negative tick is allowed", "");
}

void TestRetime_ClampsAgainstPrevWithOneTickGap() {
	// prev tick 100 -> lower bound is 101, not 100 (minimum 1-tick gap, r33).
	RetimeClamp r = ClampRetimeTick(true, 100, false, 0, /*current*/ 150, /*requested*/ 100);
	Check(r.tick == 101, "clamped to prev+1", "CameraPath.cpp:831");
	Check(r.changed == true, "101 != 150", "");
}

void TestRetime_ClampsAgainstNextWithOneTickGap() {
	// next tick 200 -> upper bound is 199, not 200.
	RetimeClamp r = ClampRetimeTick(false, 0, true, 200, /*current*/ 150, /*requested*/ 250);
	Check(r.tick == 199, "clamped to next-1", "CameraPath.cpp:833");
}

void TestRetime_ClampsBetweenBothNeighbours() {
	RetimeClamp lo = ClampRetimeTick(true, 100, true, 200, 150, 50);
	Check(lo.tick == 101, "low clamp wins", "requested below the whole range");
	RetimeClamp hi = ClampRetimeTick(true, 100, true, 200, 150, 500);
	Check(hi.tick == 199, "high clamp wins", "requested above the whole range");
	RetimeClamp mid = ClampRetimeTick(true, 100, true, 200, 150, 160);
	Check(mid.tick == 160, "inside the range passes through", "");
	Check(mid.changed == true, "160 != 150", "");
}

void TestRetime_UnchangedIsNoOp() {
	RetimeClamp r = ClampRetimeTick(true, 100, true, 200, /*current*/ 150, /*requested*/ 150);
	Check(r.tick == 150, "requesting the current tick", "");
	Check(r.changed == false, "no-op: no undo entry, no rebuild", "CameraPath.cpp:835, r33/r34");
}

void TestRetime_DegenerateAdjacentNeighbours_HiClampWinsEvenBelowLo() {
	// Neighbours with a zero-tick gap (e.g. left in that state by PlaceReposition, r25, which does
	// NOT re-validate ordering): lo=101, hi=99. CS2 applies lo then unconditionally applies hi
	// second (CameraPath.cpp:831-834), so the result can end up BELOW lo. This is a corner case
	// this file reproduces exactly rather than "fixes", since fixing it would diverge from CS2.
	RetimeClamp r = ClampRetimeTick(true, 100, true, 100, /*current*/ 100, /*requested*/ 150);
	Check(r.tick == 99, "hi clamp applied after lo, unconditionally", "matches CameraPath.cpp's clamp order exactly");
}

void TestRetime_TimeRecompute() {
	Check(RetimeAlsoUpdatesTime(0.015625f) == true, "positive interval updates time", "r35");
	Check(RetimeAlsoUpdatesTime(0.0f) == false, "zero interval leaves time untouched", "r35: guarded, no garbage write");
	Check(RetimeAlsoUpdatesTime(-1.0f) == false, "negative interval also guarded", "");
	double t = RetimedMarkerTime(64, 0.015625f); // 64 ticks @ 64 Hz = 1.0s
	Check(t > 0.999 && t < 1.001, "retimed time formula", "newTick * intervalPerTick, CameraPath.cpp:841");
}

// ---------------------------------------------------------------------------------------------
// Select prev/next: CLAMP, never wrap (r29-r31)
// ---------------------------------------------------------------------------------------------

void TestSelect_EmptyList_IsAlwaysMinusOne() {
	Check(SelectClampedIndex(0, 0) == -1, "empty list selects -1", "CamMarkers::SetSelected");
	Check(SelectClampedIndex(5, 0) == -1, "empty list, any request", "");
	Check(SelectClampedIndex(-5, 0) == -1, "empty list, negative request", "");
}

void TestSelect_ClampsAtBothEnds_NeverWraps() {
	// r30, explicitly: prev at the first marker stays on the first; next at the last stays there.
	Check(SelectClampedIndex(-1, 5) == 0, "below range clamps to 0", "not 4 (which wrapping would give)");
	Check(SelectClampedIndex(-100, 5) == 0, "far below range still clamps to 0", "");
	Check(SelectClampedIndex(5, 5) == 4, "at-count clamps to last index", "not 0 (which wrapping would give)");
	Check(SelectClampedIndex(100, 5) == 4, "far above range still clamps to last", "");
	Check(SelectClampedIndex(2, 5) == 2, "in range passes through", "");
}

void TestSelect_SingleMarker_PrevAndNextBothStay() {
	Check(SelectDeltaIndex(0, -1, 1) == 0, "prev on a single marker stays put", "no lower neighbour to move to");
	Check(SelectDeltaIndex(0, +1, 1) == 0, "next on a single marker stays put", "no upper neighbour to move to");
}

void TestSelect_DeltaFromNegativeSelectionTreatsItAsZero() {
	// CameraPath::SelectDelta: "int base = (m_data.Selected() < 0) ? 0 : m_data.Selected();"
	Check(SelectDeltaIndex(-1, +1, 5) == 1, "no selection + next -> index 1", "CameraPath.cpp:306");
	Check(SelectDeltaIndex(-1, -1, 5) == 0, "no selection + prev -> clamps to 0", "base 0, delta -1, clamped");
}

void TestSelect_WrapAroundWouldFailButClampPasses() {
	// The explicit "does NOT wrap" contract from r30, stated as a negative assertion.
	Check(SelectDeltaIndex(4, +1, 5) != 0, "next past the last index must NOT wrap to 0", "r30");
	Check(SelectDeltaIndex(0, -1, 5) != 4, "prev before the first index must NOT wrap to the last", "r30");
	Check(SelectDeltaIndex(4, +1, 5) == 4, "next at the last marker clamps and stays", "");
	Check(SelectDeltaIndex(0, -1, 5) == 0, "prev at the first marker clamps and stays", "");
}

void TestSelect_TeleportGating() {
	// r29: the editor family always teleports; the popup family only when Auto-Snap is on.
	Check(ShouldTeleportOnSelect(/*viaEditorFamily*/ true, /*autoSnap*/ false) == true, "editor family, autoSnap off", "always teleports");
	Check(ShouldTeleportOnSelect(true, true) == true, "editor family, autoSnap on", "still teleports");
	Check(ShouldTeleportOnSelect(/*viaEditorFamily*/ false, /*autoSnap*/ false) == false, "popup family, autoSnap off (default)", "does not teleport");
	Check(ShouldTeleportOnSelect(false, true) == true, "popup family, autoSnap on", "teleports only because autoSnap is on");
	Check(kAutoSnapDefault == false, "autoSnap defaults off", "CameraPath.h:212, r31");
}

// ---------------------------------------------------------------------------------------------
// SeekSettleTracker (r43, r9)
// ---------------------------------------------------------------------------------------------

void TestSettle_UnarmedIsTriviallySettled() {
	SeekSettleTracker s;
	Check(!s.Armed(), "starts unarmed", "");
	Check(s.Advance(true, 999) == SettleState::Settled, "ticking while unarmed is a no-op settle", "nothing to wait for");
}

void TestSettle_SettlesOnTargetOnceMinFramesPassed() {
	SeekSettleTracker s(kScrubReleaseSettle); // 2 / 60
	s.Arm(100);
	Check(s.Armed(), "armed", "");
	Check(s.Advance(true, 100) == SettleState::Waiting, "1st on-target frame: not enough yet", "minSettleFrames == 2");
	Check(s.Armed(), "still armed after 1 frame", "");
	Check(s.Advance(true, 100) == SettleState::Settled, "2nd frame, on target, settles", "");
	Check(!s.Armed(), "disarmed once settled", "");
}

void TestSettle_WithinToleranceCountsAsOnTarget() {
	// |tick - target| <= 1 counts as on-target (CameraPath.cpp:1096-1097's abs<=1).
	SeekSettleTracker s(kScrubReleaseSettle);
	s.Arm(100);
	s.Advance(true, 99);  // within tolerance
	SettleState r = s.Advance(true, 101); // within tolerance the other side
	Check(r == SettleState::Settled, "off-by-one both directions still counts as on-target", "CameraPath.cpp:1096-1097");
}

void TestSettle_OffTargetFramesDoNotRestartTheCount() {
	// CS2 keeps ONE counter (CameraPath.cpp:998, 1034, 1100): a tick that churns off target and back
	// settles on the frame it comes back, with no streak to rebuild.
	SeekSettleTracker s(kScrubReleaseSettle);
	s.Arm(100);
	Check(s.Advance(true, 100) == SettleState::Waiting, "1st frame on target is too early", "waitFrames 1 < 2");
	Check(s.Advance(true, 500) == SettleState::Waiting, "2nd frame off target", "e.g. mid-seek churn");
	Check(s.Advance(true, 100) == SettleState::Settled, "3rd frame back on target settles at once", "(atTarget && waitFrames >= 2)");
}

void TestSettle_FirstOnTargetFrameAfterTheMinimumSettles() {
	SeekSettleTracker s(kEditorPlaySettle);
	s.Arm(100);
	for (int i = 0; i < 5; ++i)
		Check(s.Advance(true, 9000) == SettleState::Waiting, "seek still churning", "");
	Check(s.Advance(true, 101) == SettleState::Settled, "one on-target frame after five off-target frames settles", "no consecutive-frame requirement");
	Check(s.WaitFrames() == 6, "every frame waited is counted", "");
}

void TestSettle_InvalidTickNeverCountsAsOnTarget() {
	SeekSettleTracker s(kScrubReleaseSettle);
	s.Arm(100);
	Check(s.Advance(false, 100) == SettleState::Waiting, "tickValid=false can't be on-target even at the exact tick", "");
	Check(s.Advance(false, 100) == SettleState::Waiting, "a second invalid read still waits", "even once minSettleFrames have passed");
}

void TestSettle_HardTimeoutFiresWithoutEverSettling() {
	SeekSettleTracker s(kScrubReleaseSettle); // timeout = 60
	s.Arm(100);
	SettleState last = SettleState::Waiting;
	for (int i = 0; i < 60; ++i) last = s.Advance(true, 99999); // never on target
	Check(last == SettleState::TimedOut, "60th never-on-target frame times out", "CameraPath.cpp:1100's >=60 fallback");
	Check(!s.Armed(), "disarmed after timeout", "the tracker must not spin forever");
}

void TestSettle_TargetChangeRestartsTheWait() {
	SeekSettleTracker s(kScrubReleaseSettle);
	s.Arm(100);
	Check(s.Advance(true, 100) == SettleState::Waiting, "1 on-target frame toward 100", "");
	Check(s.WaitFrames() == 1, "wait count is 1", "");

	s.Arm(250); // a new selection/scrub before the first one ever settled
	Check(s.WaitFrames() == 0, "re-arming resets the wait counter", "ArmScrubRelease unconditionally resets, CameraPath.cpp:729-733");
	Check(s.TargetTick() == 250, "target actually changed", "");
	Check(s.Advance(true, 100) == SettleState::Waiting, "old target no longer counts as on-target", "the tracker now waits for 250");
	Check(s.Advance(true, 250) == SettleState::Settled, "the new target settles on its own frame count", "waitFrames 2 and on target");
}

void TestSettle_NamedPresetsMatchCs2Constants() {
	Check(kScrubReleaseSettle.minSettleFrames == 2 && kScrubReleaseSettle.timeoutFrames == 60,
		"scrub-release preset", "CameraPath.cpp:1100");
	Check(kEditorPlaySettle.minSettleFrames == 2 && kEditorPlaySettle.timeoutFrames == 30,
		"editor-play preset", "CameraPath.cpp:998");
	Check(kTimelineArmSettle.minSettleFrames == 2 && kTimelineArmSettle.timeoutFrames == 240,
		"timeline-arm preset", "CameraPath.cpp:1034");
}

void TestSettle_DisarmStopsWaitingImmediately() {
	SeekSettleTracker s(kScrubReleaseSettle);
	s.Arm(100);
	s.Advance(true, 999);
	s.Disarm();
	Check(!s.Armed(), "disarmed", "");
	Check(s.WaitFrames() == 0, "wait counter cleared", "");
	Check(s.Advance(true, 12345) == SettleState::Settled, "ticking after disarm is a trivial settle", "nothing to wait for anymore");
}

// ---------------------------------------------------------------------------------------------
// IsPathHoldParked (the parked-hold predicate)
// ---------------------------------------------------------------------------------------------

ParkedHoldState MakeParkedState() {
	ParkedHoldState s;
	s.demoPaused = true;
	s.editorOpen = true;
	s.markerCount = 2;
	s.playheadTick = 150;
	s.firstMarkerTick = 100;
	s.lastMarkerTick = 200;
	s.freeCam = false;
	s.followLive = false;
	return s;
}

void TestParkedHold_AllConditionsMet_IsParked() {
	Check(IsPathHoldParked(MakeParkedState()), "fully satisfied state is parked", "");
}

void TestParkedHold_EachFlagIndependentlyBreaksIt() {
	{ ParkedHoldState s = MakeParkedState(); s.demoPaused = false;
	  Check(!IsPathHoldParked(s), "not paused -> not parked", ""); }
	{ ParkedHoldState s = MakeParkedState(); s.editorOpen = false;
	  Check(!IsPathHoldParked(s), "editor closed -> not parked", "nothing showing the hold to justify it"); }
	{ ParkedHoldState s = MakeParkedState(); s.markerCount = 1;
	  Check(!IsPathHoldParked(s), "1 marker -> not parked", "CanPlay gate, r1"); }
	{ ParkedHoldState s = MakeParkedState(); s.markerCount = 0;
	  Check(!IsPathHoldParked(s), "0 markers -> not parked", ""); }
	{ ParkedHoldState s = MakeParkedState(); s.freeCam = true;
	  Check(!IsPathHoldParked(s), "free cam active -> not parked", "free cam outranks a held path pose"); }
	{ ParkedHoldState s = MakeParkedState(); s.followLive = true;
	  Check(!IsPathHoldParked(s), "live follow active -> not parked", "follow outranks a held path pose"); }
}

void TestParkedHold_PlayheadRangeIsInclusiveBothEnds() {
	{ ParkedHoldState s = MakeParkedState(); s.playheadTick = s.firstMarkerTick;
	  Check(IsPathHoldParked(s), "exactly at first marker tick is inside the range", "inclusive lower bound"); }
	{ ParkedHoldState s = MakeParkedState(); s.playheadTick = s.lastMarkerTick;
	  Check(IsPathHoldParked(s), "exactly at last marker tick is inside the range", "inclusive upper bound -- FinishPlaybackAtLastKey lands exactly here, r16"); }
	{ ParkedHoldState s = MakeParkedState(); s.playheadTick = s.firstMarkerTick - 1;
	  Check(!IsPathHoldParked(s), "one tick before the first marker is outside the range", ""); }
	{ ParkedHoldState s = MakeParkedState(); s.playheadTick = s.lastMarkerTick + 1;
	  Check(!IsPathHoldParked(s), "one tick past the last marker is outside the range", ""); }
}

void TestParkedHold_TwoMarkersAtSameTick_RangeIsAPoint() {
	ParkedHoldState s = MakeParkedState();
	s.firstMarkerTick = s.lastMarkerTick = 150;
	s.playheadTick = 150;
	Check(IsPathHoldParked(s), "degenerate single-tick range still parks exactly on it", "");
	s.playheadTick = 151;
	Check(!IsPathHoldParked(s), "one tick off a single-tick range is not parked", "");
}

int main() {
	TestCanPlayThreshold();
	TestCanScrubThreshold();
	TestMinMarkerMessagesAreDistinct();

	TestSpace_EditorClosed_IsPlainToggleRegardlessOfMarkers();
	TestSpace_EditorOpen_Editing_ArmsAndPlaysWhenAllowed();
	TestSpace_EditorOpen_Editing_RefusedArmLeavesModeUntouched();
	TestSpace_EditorOpen_Editing_PendingSwallowsSpace();
	TestSpace_Armed_AlwaysPlays_RegardlessOfEditor();
	TestSpace_Armed_FailedPlayStaysArmed();
	TestSpace_Playing_StopsAfterGuardElapses();
	TestSpace_Playing_StopIgnoresEditorVisibility();
	TestEsc_UnconditionalFromArmedAndPlaying_IgnoresGuard();
	TestEsc_FromEditing_IsNoOp();

	TestAddMarker_IdleState_AddsIntoEmptyList();
	TestAddMarker_IdleState_InsertsInSortedGap();
	TestAddMarker_DuplicateTick_AlwaysAddsAfterTheGroup_NeverReplace();
	TestAddMarker_RefusesWhilePlaying();
	TestAddMarker_RefusesWhileScrubbing();
	TestAddMarker_RefusesWhileSeekSettling();
	TestAddMarker_RefusesWhenTickInvalid();
	TestAddMarker_AllFourGuardsIndependentlyRefuse();
	TestNudgeAboveEpsilon();

	TestRetime_NoNeighbours_OnlyFloorsAtZero();
	TestRetime_ClampsAgainstPrevWithOneTickGap();
	TestRetime_ClampsAgainstNextWithOneTickGap();
	TestRetime_ClampsBetweenBothNeighbours();
	TestRetime_UnchangedIsNoOp();
	TestRetime_DegenerateAdjacentNeighbours_HiClampWinsEvenBelowLo();
	TestRetime_TimeRecompute();

	TestSelect_EmptyList_IsAlwaysMinusOne();
	TestSelect_ClampsAtBothEnds_NeverWraps();
	TestSelect_SingleMarker_PrevAndNextBothStay();
	TestSelect_DeltaFromNegativeSelectionTreatsItAsZero();
	TestSelect_WrapAroundWouldFailButClampPasses();
	TestSelect_TeleportGating();

	TestSettle_UnarmedIsTriviallySettled();
	TestSettle_SettlesOnTargetOnceMinFramesPassed();
	TestSettle_WithinToleranceCountsAsOnTarget();
	TestSettle_OffTargetFramesDoNotRestartTheCount();
	TestSettle_FirstOnTargetFrameAfterTheMinimumSettles();
	TestSettle_InvalidTickNeverCountsAsOnTarget();
	TestSettle_HardTimeoutFiresWithoutEverSettling();
	TestSettle_TargetChangeRestartsTheWait();
	TestSettle_NamedPresetsMatchCs2Constants();
	TestSettle_DisarmStopsWaitingImmediately();

	TestParkedHold_AllConditionsMet_IsParked();
	TestParkedHold_EachFlagIndependentlyBreaksIt();
	TestParkedHold_PlayheadRangeIsInclusiveBothEnds();
	TestParkedHold_TwoMarkersAtSameTick_RangeIsAPoint();

	if (g_failures) {
		std::printf("%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("DeadlockShotRulesMath: all checks passed\n");
	return 0;
}
