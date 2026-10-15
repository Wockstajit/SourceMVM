#pragma once

// Camera-path ("shot") rules ported from CS2's marker/camera-path system -- CameraPath.cpp,
// CamMarkers.cpp and MovieMode.cpp under AfxHookSource2/Filmmaker/Movie/ -- so Deadlock's own
// camera path behaves EXACTLY like CS2's, rule for rule, everywhere CS2's behaviour is decidable
// from plain state: no engine, no spline library, no g_CamPath, no viewport.
//
// Header-only and engine-free on purpose, same reason as the neighbouring DeadlockDemoClockMath.h:
// the hook itself cannot be unit tested, this can, and marker/playback state-machine logic is
// exactly the kind of "looks right, one off-by-one and it silently drifts" code that stays broken
// for months if it is only ever exercised by hand in a running game.
//
// ONE DELIBERATE INPUT-MAPPING DIFFERENCE FROM CS2, called out here so it is never mistaken for a
// missed rule later: CS2 splits camera-path transport across TWO keys. J arms/advances a 3-state
// cycle (Editing -> Armed -> Playing -> Editing, MovieMode.cpp:370-380) and Space plays/pauses once
// armed (CameraPath.cpp:504-531, :649-669) or, with the path editor closed, is a plain demo
// pause/resume toggle (MovieMode.cpp:451-463). Deadlock's J is already its native mouse-capture
// toggle (see Docs/deadlock/console-commands.md) and is not free to take over, so the whole
// pipeline is asked to funnel through Space alone: with the editor open and nothing armed, ONE
// Space press has to do the job CS2 needs J-then-Space for (arm AND play in one motion).
// ShotTransport::DecideSpace below is where that collapsing happens (see SpaceAction::ArmAndPlay);
// every other branch reproduces CS2's Space priority chain unchanged.
//
// ALSO NOT REPRODUCED ON PURPOSE: select prev/next is sometimes paraphrased as "wraparound" --
// it is not, in CS2. CamMarkers::SetSelected (CamMarkers.cpp:13-18) and CameraPath::
// SelectEditorDelta (CameraPath.cpp:348-355) both CLAMP at the ends (index<0 -> 0, index>=count ->
// count-1) and never wrap. SelectClampedIndex/SelectDeltaIndex below reproduce the clamp; nothing
// here wraps, because CS2 doesn't either -- wrapping would be a Deadlock/CS2 behavioural mismatch,
// which is the one thing this file exists to prevent.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Deadlock {
namespace ShotRules {

using Tick = std::int32_t; // demo ticks; kept distinct from plain ints/frame-counts for clarity

// -------------------------------------------------------------------------------------------
// Refusal / notice messages (CameraPath::Notify callers, CameraPath.cpp:178-181).
//
// Two DIFFERENT minimum-marker messages exist in CS2 -- play's mentions "camera", scrub's does
// not -- and they are genuinely separate strings, not one shared constant (r1 vs r3). A verifier
// that diffs Deadlock's banner text against CS2's would catch "close enough" wording, so these
// are copied verbatim rather than paraphrased.
// -------------------------------------------------------------------------------------------

constexpr int kMinMarkersToPlay = 2;   // CameraPath.cpp:469-474 (ArmPreview) / :549-554 (PlayPath) / :590-595 (PlayFromEditor)
constexpr int kMinMarkersToScrub = 2;  // CameraPath.cpp:692-695 (ScrubToTick)

constexpr const char* kNeedMarkersToPlayMessage   = "Need at least 2 camera markers to play path.";
constexpr const char* kNeedMarkersToScrubMessage  = "Need at least 2 markers to scrub the path.";
constexpr const char* kPathNotEvaluableMessage    = "Camera path could not be built.";
constexpr const char* kLastMarkerNoSegmentMessage = "Last marker has no outgoing segment.";
constexpr const char* kNothingToUndoMessage       = "Nothing to undo.";
constexpr const char* kNothingToRedoMessage       = "Nothing to redo.";

// r1/r3: the marker count is NECESSARY but, per r2, not SUFFICIENT -- ArmPreview/PlayPath/
// PlayFromEditor/ScrubToTick also require the rebuilt path to be evaluable (g_CamPath.CanEval()),
// which delegates to the shared spline engine and is therefore NOT reproduced here (r2 is marked
// pureLogic:false in the plan this file implements, for exactly that reason). A caller must still
// check the engine's CanEval() -- or the Deadlock equivalent -- before actually starting; CanPlay/
// CanScrub only rule out the case that IS decidable from plain data.
inline bool CanPlay(int markerCount) { return markerCount >= kMinMarkersToPlay; }
inline bool CanScrub(int markerCount) { return markerCount >= kMinMarkersToScrub; }

// -------------------------------------------------------------------------------------------
// Arm -> play -> stop transport, driven by ONE Space input (see the file banner for why Deadlock
// collapses CS2's J+Space onto Space alone).
//
// Mirrors the three CameraPath::Mode states this cycle actually visits (CameraPath.h:44;
// Reposition is a separate gesture, not part of this cycle) plus the two playback-side guards
// that make holding Space down behave sanely instead of fighting itself:
//   * m_liveStartPauseGuard (CameraPath.cpp:514/572/1004, 30 frames): a Space auto-repeat or a
//     second physical press landing the instant playback starts must not immediately re-pause it
//     (PausePreview's early-out, CameraPath.cpp:649-657, r14).
//   * PlaybackPending() (CameraPath.cpp:658, read at MovieMode.cpp:446): while an arm-and-play
//     seek is already in flight, a repeated Space must be swallowed rather than re-issued.
// -------------------------------------------------------------------------------------------

enum class ShotMode {
	Editing,  // CameraPath::Mode::Editing -- nothing armed
	Armed,    // CameraPath::Mode::PreviewArmed -- holding the first marker's pose
	Playing,  // CameraPath::Mode::PreviewPlaying -- the dolly is driving the camera
};

enum class SpaceAction {
	PlainDemoToggle, // editor closed, Editing: plain demo_pause/demo_resume (MovieMode.cpp:451-463, r18 case 5/r19)
	ArmAndPlay,      // editor open, Editing, nothing pending: CS2's J-then-Space, collapsed onto Space (see file banner)
	Play,            // Armed: start the dolly (CameraPath.cpp:504-531, r13)
	Stop,            // Playing, start guard elapsed: stop/pause (CameraPath.cpp:649-669, r14)
	Swallow,         // consumed, no side effect (start guard still open, or a start is already pending)
};

class ShotTransport {
public:
	ShotMode Mode() const { return m_mode; }
	int StartGuardFramesRemaining() const { return m_startGuardFrames; }

	// Call once per frame, before reading input for that frame -- mirrors CameraPath::RunFrame's
	// "if (m_liveStartPauseGuard > 0) --m_liveStartPauseGuard;" (CameraPath.cpp:958).
	void Tick() {
		if (m_startGuardFrames > 0) --m_startGuardFrames;
	}

	// Decide what one Space press means right now. Pure: does not mutate `mode` by itself. Call
	// Apply() with the result (and, for ArmAndPlay/Play, whether the CanPlay()+CanEval() gate
	// actually passed) to fold the decision back into the transport.
	SpaceAction DecideSpace(bool editorOpen, bool playbackPending) const {
		if (m_mode == ShotMode::Playing) {
			// r14: PausePreview no-ops while the start guard is open; Esc (OnEsc, r15) is the
			// only unconditional way out of Playing during that window.
			if (m_startGuardFrames > 0) return SpaceAction::Swallow;
			return SpaceAction::Stop;
		}
		if (m_mode == ShotMode::Armed) return SpaceAction::Play; // r13: always allowed, no guard
		// Editing:
		if (!editorOpen) return SpaceAction::PlainDemoToggle; // MovieMode.cpp:451-463
		if (playbackPending) return SpaceAction::Swallow;      // MovieMode.cpp:446 (!PlaybackPending() guard)
		return SpaceAction::ArmAndPlay;
	}

	// Fold a decided action back into `mode`. `canPlay` stands for CanPlay(markerCount) (and, in a
	// full implementation, the engine's CanEval() per r2) and is ignored by actions that never
	// attempt to change mode.
	void Apply(SpaceAction action, bool canPlay) {
		switch (action) {
		case SpaceAction::ArmAndPlay:
			// A refused arm leaves mode exactly where it was (Editing) -- ArmPreview's refusal
			// (r1) is Notify + return with NO other side effect, not a bounce through Armed.
			if (canPlay) {
				m_mode = ShotMode::Playing;
				m_startGuardFrames = kStartGuardFrames;
			}
			break;
		case SpaceAction::Play:
			// StartPreviewPlay's failure path (CameraPath.cpp:510, !CanEval()) leaves mode at
			// Armed, still holding the first frame -- it does not fall back to Editing either.
			if (canPlay) {
				m_mode = ShotMode::Playing;
				m_startGuardFrames = kStartGuardFrames;
			}
			break;
		case SpaceAction::Stop:
			m_mode = ShotMode::Editing;
			m_startGuardFrames = 0;
			break;
		case SpaceAction::PlainDemoToggle:
		case SpaceAction::Swallow:
			break; // no mode change, ever
		}
	}

	// Esc: unconditional stop from Armed or Playing, NOT gated by the start guard (r15 -- unlike
	// Stop above, which models r14's Space-only guard). A no-op from Editing.
	void OnEsc() {
		m_mode = ShotMode::Editing;
		m_startGuardFrames = 0;
	}

	// Enter Armed directly, e.g. a standalone "arm" action outside the Space cycle (ArmPreview
	// called on its own, r6-r8). A refused arm (canPlay == false) leaves mode untouched, same as
	// the ArmAndPlay branch of Apply().
	void Arm(bool canPlay) {
		if (canPlay) {
			m_mode = ShotMode::Armed;
			m_startGuardFrames = 0;
		}
	}

private:
	static constexpr int kStartGuardFrames = 30; // CameraPath.cpp:514/572/1004

	ShotMode m_mode = ShotMode::Editing;
	int m_startGuardFrames = 0;
};

// -------------------------------------------------------------------------------------------
// Placing a marker. CS2's PlaceMarker (CameraPath.cpp:242-258) has NO mode gate at all (r20) --
// only the K *hotkey* is gated to Editing at the input layer (MovieMode.cpp:523-528), and that
// gate is measured against CameraPath::Mode, which STAYS Editing through both a drag-scrub and a
// scrub-release settle (RunFrame only leaves Editing for Armed/Playing/Reposition, CameraPath.cpp:
// 930-1112). So by the letter of CS2's shipped code, K still fires mid-scrub and mid-settle, and
// PlaceMarker still runs.
//
// What it would capture then is not trustworthy, and CS2 has simply never been asked the
// question:
//   * mid-scrub, the pose CameraBridge_GetCurrentCamera would report is ScrubToTick's own
//     deterministic path pose (TickScrub, r44) -- placing a marker there records a point that is
//     already ON the curve, not a new position the user meant to add.
//   * mid-settle (an ArmScrubRelease or editor-play seek in flight, r43/r9), the demo tick CS2
//     reads is the PRE-seek tick for up to dozens of frames -- the marker would be stamped with a
//     tick the camera is already leaving.
//   * while the dolly is Playing, "current camera" IS the dolly's own output (TickPlay); feeding
//     it back in as a new marker records the path's own output as new input to itself.
//   * g_MirvTime.GetCurrentDemoTick() can simply fail, and r21 says CS2's answer is "use tick 0"
//     -- tolerable on CS2, which has shipped that fallback for years without anyone reporting it;
//     not a risk worth re-taking on a port that (per project memory) has never been run in a live
//     game yet.
//
// So DecideAddMarker below is a DELIBERATE, DOCUMENTED DIVERGENCE from CS2 for exactly these four
// states: it refuses instead of silently reproducing CS2's untested behaviour. In every other
// state -- the only one CS2's shipped UI (K / "+ Add") actually reaches -- it reproduces CS2's
// insert exactly: CamMarkers::Add's ordering (r22) and its duplicate-tick handling (r23): a
// duplicate tick is ALLOWED and is never coalesced into the existing marker, only inserted stably
// after it.
// -------------------------------------------------------------------------------------------

struct AddMarkerState {
	bool playing = false;      // ShotMode::Playing-equivalent: the dolly owns the camera
	bool scrubbing = false;    // a drag-scrub (ScrubToTick(seek=false)) is in progress
	bool seekSettling = false; // an ArmScrubRelease / editor-play seek has not yet landed
	bool tickValid = false;    // the engine reported a current demo tick THIS frame
};

enum class AddMarkerOutcome {
	Add,    // insert at AddMarkerDecision::insertIndex, exactly like CamMarkers::Add (r22/r23)
	Refuse, // one of the four untrustworthy-tick states documented above
	// "Replace" deliberately does not exist: CS2 NEVER coalesces a new marker into an existing one
	// at the same tick (r23 -- the ordering scan is `tick > m.tick`, so an EQUAL tick does not stop
	// it, and the new marker lands stably AFTER every existing one sharing it). A caller that
	// replaced on a duplicate tick would be diverging from CS2, not matching it.
};

struct AddMarkerDecision {
	AddMarkerOutcome outcome = AddMarkerOutcome::Refuse;
	int insertIndex = -1; // valid only when outcome == Add
};

// CamMarkers::Add's insertion index (CamMarkers.cpp:20-34): the first position whose tick is
// STRICTLY greater than newTick; a duplicate group is scanned past (not stopped on), so a new
// member of an existing group always lands last within that group. `ticksSortedAscending` must
// already be sorted -- CamMarkers keeps that invariant at all times and this function does not
// restore it, matching CamMarkers::Add itself.
inline int MarkerInsertIndex(const Tick* ticksSortedAscending, int count, Tick newTick) {
	int idx = count;
	for (int i = 0; i < count; ++i) {
		if (ticksSortedAscending[i] > newTick) {
			idx = i;
			break;
		}
	}
	return idx;
}

inline AddMarkerDecision DecideAddMarker(const AddMarkerState& state,
                                          const Tick* existingTicksSortedAscending,
                                          int existingCount,
                                          Tick newTick) {
	AddMarkerDecision d;
	if (state.playing || state.scrubbing || state.seekSettling || !state.tickValid) {
		d.outcome = AddMarkerOutcome::Refuse;
		return d;
	}
	d.outcome = AddMarkerOutcome::Add;
	d.insertIndex = MarkerInsertIndex(existingTicksSortedAscending, existingCount, newTick);
	return d;
}

// r24: downstream (CamPathEval::Rebuild), duplicate/near-duplicate ticks are handled not by
// refusing them here but by nudging the SHAPE chord length and the Manual-mode TIMING delta up to
// a shared epsilon so both axes stay strictly increasing -- the exact mechanism that lets Freeze
// cinematics put several markers on one tick. Exposed here (rather than left to run invisibly
// only inside the spline rebuild) so it has a test of its own.
constexpr double kDuplicateTickEpsilon = 1e-3; // CamPathEval.cpp:15

inline double NudgeAboveEpsilon(double magnitude, double epsilon = kDuplicateTickEpsilon) {
	const double v = magnitude < 0.0 ? -magnitude : magnitude; // CamPathEval nudges |chord| / |Δtime|
	return v < epsilon ? epsilon : v;
}

// -------------------------------------------------------------------------------------------
// Retiming an existing marker (MoveKey, CameraPath.cpp:827-845 -- the editor's "Tick -> Here"
// button is exactly this call with the current editor tick). Clamped STRICTLY between neighbours
// with a minimum 1-tick gap: unlike placing a NEW marker (which allows two markers to share a
// tick, r23), retiming an EXISTING one onto another tick is never allowed to actually collide --
// it stops one tick short instead.
// -------------------------------------------------------------------------------------------

struct RetimeClamp {
	Tick tick = 0;
	bool changed = false; // false => CS2 treats this as a no-op: no undo entry, no rebuild (r33)
};

// hasPrev/prevTick and hasNext/nextTick describe the immediate neighbours in the sorted marker
// list (absent at either end of the list); currentTick is the marker's tick BEFORE the move. The
// clamp order matters and is copied exactly from CameraPath.cpp:831-834: lower bound first, upper
// bound second, unconditionally -- so two neighbours with zero tick gap between them (already an
// out-of-spec state MoveKey itself cannot produce, but PlaceReposition can, per r25) clamp to
// whatever the upper bound leaves, even if that lands below the lower bound.
inline RetimeClamp ClampRetimeTick(bool hasPrev, Tick prevTick, bool hasNext, Tick nextTick,
                                    Tick currentTick, Tick requestedTick) {
	const Tick lo = hasPrev ? static_cast<Tick>(prevTick + 1) : Tick{0};
	Tick newTick = requestedTick < lo ? lo : requestedTick;
	if (hasNext) {
		const Tick hi = static_cast<Tick>(nextTick - 1);
		if (newTick > hi) newTick = hi;
	}
	if (newTick < 0) newTick = 0;

	RetimeClamp result;
	result.tick = newTick;
	result.changed = (newTick != currentTick); // CameraPath.cpp:835
	return result;
}

// r35: MoveKey also refreshes the marker's recorded demo TIME from its new tick, but ONLY if the
// demo has reported a positive interval-per-tick -- a zero/negative value (no demo loaded) leaves
// time untouched rather than writing garbage.
inline bool RetimeAlsoUpdatesTime(float intervalPerTick) { return intervalPerTick > 0.0f; }

inline double RetimedMarkerTime(Tick newTick, float intervalPerTick) {
	return static_cast<double>(newTick) * static_cast<double>(intervalPerTick);
}

// -------------------------------------------------------------------------------------------
// Select prev/next: CS2 CLAMPS at both ends and NEVER wraps (r30) -- see the file banner for why
// that is called out explicitly. CamMarkers::SetSelected (CamMarkers.cpp:13-18) and CameraPath::
// SelectEditorDelta (CameraPath.cpp:348-355) apply the identical clamp.
// -------------------------------------------------------------------------------------------

inline int SelectClampedIndex(int requestedIndex, int markerCount) {
	if (markerCount <= 0) return -1; // CamMarkers::SetSelected: an empty list selects -1
	return std::clamp(requestedIndex, 0, markerCount - 1);
}

inline int SelectDeltaIndex(int currentSelected, int delta, int markerCount) {
	const int base = currentSelected < 0 ? 0 : currentSelected;
	return SelectClampedIndex(base + delta, markerCount);
}

// r29: the two select "families" gate teleport differently. The standalone marker-popup family
// (SelectIndex/SelectDelta, CameraPath.cpp:291-308) only moves the viewer when Auto-Snap is on;
// the camera-editor/timeline family (SelectForEditor/SelectEditorDelta, CameraPath.cpp:310-355)
// ALWAYS teleports, independent of Auto-Snap.
inline bool ShouldTeleportOnSelect(bool viaEditorFamily, bool autoSnapEnabled) {
	return viaEditorFamily || autoSnapEnabled;
}

constexpr bool kAutoSnapDefault = false; // CameraPath.h:212 ("bool m_autoSnap = false;"), r31

// -------------------------------------------------------------------------------------------
// Seek-settle: hold a deterministic pose while an async engine seek (demo_gototick) lands, then
// release it. The SAME shape, with different constants, times three separate CS2 handshakes:
//   * ArmScrubRelease, after a released scrub's own seek (CameraPath.cpp:1093-1108)  -- 2 / 60 frames
//   * the editor-play seek-then-play handshake (CameraPath.cpp:986-1009)             -- 2 / 30 frames
//   * the timeline arm-then-play handshake (CameraPath.cpp:1022-1039)                -- 2 / 240 frames
// so it is written once, generically, here -- the three CS2 constants are named presets rather
// than encoded three times over with three chances to typo one of them.
//
// ONE COUNTER, as all three CS2 sites write it: every frame waited increments it, and the wait ends
// on the first frame whose tick is within 1 of the target once at least `minSettleFrames` frames
// have passed -- `(atTarget && waitFrames >= 2) || waitFrames >= 30` (CameraPath.cpp:998). There is
// no consecutive-frame streak, so a tick that churns off target and back settles on the frame it
// comes back. The hard timeout fires regardless, because a match's tech-pause can churn the demo
// tick so an exact settle is never reachable (CameraPath.cpp:991-997's comment).
//
// Arming with a NEW target always restarts the wait from zero -- ArmScrubRelease unconditionally
// resets m_scrubReleaseWaitFrames (CameraPath.cpp:729-733) -- there is no "already settling,
// ignore the new target" special case.
// -------------------------------------------------------------------------------------------

struct SettleConfig {
	int minSettleFrames; // frames waited before an on-target frame settles (all three CS2 sites use 2)
	int timeoutFrames;   // hard fallback regardless of the tick ever landing
};

constexpr SettleConfig kScrubReleaseSettle{2, 60};  // CameraPath.cpp:1100 (>=2 && atTarget, or >=60)
constexpr SettleConfig kEditorPlaySettle{2, 30};    // CameraPath.cpp:998  (>=2 && atTarget, or >=30)
constexpr SettleConfig kTimelineArmSettle{2, 240};  // CameraPath.cpp:1034 (>=2 && atStart,  or >=240)

enum class SettleState {
	Waiting,  // still holding the pose; keep calling Advance()
	Settled,  // the seek landed within tolerance for long enough -- release the held pose
	TimedOut, // the hard fallback fired before the tick ever settled -- release anyway (r43/r9)
};

class SeekSettleTracker {
public:
	explicit SeekSettleTracker(SettleConfig config = kScrubReleaseSettle) : m_config(config) {}

	bool Armed() const { return m_armed; }
	Tick TargetTick() const { return m_targetTick; }
	int WaitFrames() const { return m_waitFrames; }

	// (Re)arm for a new target tick. ALWAYS restarts the wait, even if already armed for a
	// different (or the same) target -- CS2 has no "ignore, already settling" branch (see the
	// block comment above).
	void Arm(Tick targetTick) {
		m_armed = true;
		m_targetTick = targetTick;
		m_waitFrames = 0;
	}

	void Disarm() {
		m_armed = false;
		m_waitFrames = 0;
	}

	// One frame of the wait. Named Advance() rather than Tick() because this class already uses
	// `Tick` as the demo-tick TYPE (see the top of this header) -- a member function of the same
	// name would shadow that type for the rest of the class body. Pass currentTickValid=false
	// when the engine can't report a tick this frame (e.g. between demos) -- that frame simply
	// can't be on-target, but it still counts toward the hard timeout, exactly like a frame that
	// reports a wildly-off tick would.
	SettleState Advance(bool currentTickValid, Tick currentTick) {
		if (!m_armed) return SettleState::Settled; // nothing armed: trivially "done"

		++m_waitFrames;
		const Tick diff = currentTick - m_targetTick;
		const bool atTarget = currentTickValid && (diff < 0 ? -diff : diff) <= 1;

		if (atTarget && m_waitFrames >= m_config.minSettleFrames) {
			m_armed = false;
			return SettleState::Settled;
		}
		if (m_waitFrames >= m_config.timeoutFrames) {
			m_armed = false;
			return SettleState::TimedOut;
		}
		return SettleState::Waiting;
	}

private:
	SettleConfig m_config;
	bool m_armed = false;
	Tick m_targetTick = 0;
	int m_waitFrames = 0;
};

// -------------------------------------------------------------------------------------------
// "Parked hold": should a held path pose (HoldEditorAtMarker, CameraPath.cpp:196-206/228-238)
// still be considered the active view? This is deliberately consistent with the priority rules in
// DeadlockViewOwnerMath.h: a held path pose is that file's ViewOwner::ShotHold, and FreeCam / a
// live follow camera both OUTRANK it there (project memory: "the free camera outranks the paused,
// held path pose, so go-to-player and flying off to place a marker are never frozen by a hold").
// This predicate is the Deadlock-specific gate on TOP of that priority: even where nothing else is
// claiming the view, a hold only counts while genuinely parked -- paused, with the editor actually
// open to be looking at, at least 2 markers (CanPlay, r1), and the playhead still somewhere inside
// the path's own [first, last] range (inclusive both ends -- landing exactly on the last marker's
// tick, e.g. after FinishPlaybackAtLastKey, r16, must still read as parked).
// -------------------------------------------------------------------------------------------

struct ParkedHoldState {
	bool demoPaused = false;
	bool editorOpen = false;
	int markerCount = 0;
	Tick playheadTick = 0;
	Tick firstMarkerTick = 0;
	Tick lastMarkerTick = 0;
	bool freeCam = false;
	bool followLive = false;
};

inline bool IsPathHoldParked(const ParkedHoldState& s) {
	if (!s.demoPaused || !s.editorOpen) return false;
	if (!CanPlay(s.markerCount)) return false;
	if (s.playheadTick < s.firstMarkerTick || s.playheadTick > s.lastMarkerTick) return false;
	if (s.freeCam || s.followLive) return false;
	return true;
}

} // namespace ShotRules
} // namespace Deadlock
