#pragma once

#include "CameraShakeTypes.h"

#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Handheld camera: makes a locked engine camera read as a camera someone is HOLDING.
//
// One module merged from two Garry's Mod addons that were solving the same problem from
// opposite ends (see reference/gmod-fx/README.md). Shipping both separately would mean two
// handheld systems fighting each other, so they are two LAYERS here instead:
//
//   * DRIFT  ("Camcorder Effect: Shaky Handheld Camera", workshop 3486569466) -- procedural.
//     Slow sine drift on angle and position, a random jitter re-rolled every 0.04-0.22 s, a
//     high-frequency tremble, a speed-scaled walk bob, and an FOV wobble, all pushed through a
//     two-stage smoothing chain that gives it its characteristic lag.
//
//   * CURVE  ("Camera Shakify", workshop 3760884357) -- recorded. Baked motion-capture curves
//     (originally Nathan Vegdahl / Ian Hubert's CC0 Blender addon) auto-switched between an
//     idle / walk / run preset by the spectated player's own speed, with a slow ambient tilt
//     and periodic "re-grip" fumbles on top.
//
// Use either alone or both together -- they sum. Drift is cheap and endless; curves carry real
// human motion but loop. All phase clocks run on DEMO time (ShakeInput::demoTime), never wall
// clock, so a rendered movie is reproducible and a paused demo holds still.
//
// Self-contained: no engine sig-scanning, no dependency on ViewFx. The
// pose tracker and the camera-mode gate belong to CameraShakeStack; this file is only maths.

// One baked motion curve. Defined in the GENERATED HandheldCamCurves.cpp -- regenerate with
// `python FX/tools/convert-shakify-curves.py`, never hand-edit.
struct HandheldCurve {
	const char* key;   // console/UI key, e.g. "INVESTIGATION"
	const char* name;  // display name, e.g. "Investigation"
	float fps;         // bake rate (24 for every shipped curve)
	int frames;        // frame count; `data` holds frames*6 floats
	const float* data; // 6 per frame: right, up, fwd, pitch, yaw, roll
};
extern const HandheldCurve g_handheldCurves[];
extern const int g_handheldCurveCount;

// Index of the curve with this key (case-insensitive), or -1.
int HandheldCurveIndex(const char* key);

// Which movement state selects which curve. The auto-switcher picks one of these per frame.
enum class HandheldState { Idle, Walk, Run };

class HandheldCam {
public:
	// Assigns the default per-state curves (the source addon's own state presets). Runs on
	// first HandheldCamRef() use, i.e. after the generated curve table's static init.
	HandheldCam();

	// ---- settings (main thread writes, render thread reads; benign races, same as ViewFx) ----
	// Restore every USER-FACING setting to its shipped default, leaving integrated motion
	// state alone (Reset() owns that). Backs the per-effect Reset control in the Config panel:
	// the report asks each effect to be resettable on its own, and "off" is not the same thing
	// as "back to defaults".
	void ResetSettings();
	bool SettingsAreDefault() const;
	void SetEnabled(bool on);
	bool Enabled() const { return m_enabled; }
	void SetViewmodelDecoupled(bool on);
	bool ViewmodelDecoupled() const { return m_viewmodelDecoupled; }
	void SetCenterLockEnabled(bool on) { m_centerLockEnabled = on; }
	bool CenterLockEnabled() const { return m_centerLockEnabled; }
	void SetCenterLockStrength(double percent);
	double CenterLockStrength() const { return m_centerLockPct; }

	// All clamped to [0, kShakeMaxIntensity]. 0 turns that layer off.
	void SetDriftIntensity(double percent);
	void SetCurveIntensity(double percent);
	void SetTiltIntensity(double percent);
	void SetRegripIntensity(double percent);
	double DriftIntensity() const { return m_driftPct; }
	double CurveIntensity() const { return m_curvePct; }
	double TiltIntensity() const { return m_tiltPct; }
	double RegripIntensity() const { return m_regripPct; }

	// Which way the slow ambient drift leans, in degrees, 0-360 (wrapped).
	//
	// WHY THIS CONTROL EXISTS. The drift is the source addon's sine stack verbatim, and its
	// slowest terms have HALF-cycles of 10-16 s. Playing GMod for minutes, that averages out to a
	// wander. Inside a 3-8 second clip you only ever see part of one half-cycle, so the camera
	// leans one way and never comes back -- which is the "it only drifts to one side" report.
	// Rather than falsify the reference's motion by speeding it up, this rotates the drift vector
	// so the operator chooses WHICH way that lean goes for the shot they are framing.
	// 0 = unchanged from the reference.
	void SetDriftDirection(double degrees);
	double DriftDirection() const { return m_driftDirDeg; }

	// Curve playback speed, percent of the bake rate. Clamped to [25, 300].
	void SetSpeed(double percent);
	double Speed() const { return m_speedPct; }

	// Loop-to-loop drift that stops a looping curve reading as a loop. Clamped to [0, 100].
	void SetVariance(double percent);
	double Variance() const { return m_variancePct; }

	// Mean seconds between re-grip fumbles. Clamped to [2, 60].
	void SetRegripPeriod(double seconds);
	double RegripPeriod() const { return m_regripPeriod; }

	// Curve assigned to a movement state. Returns false on an unknown key (settings unchanged).
	bool SetStateCurve(HandheldState state, const char* key);
	const char* StateCurveKey(HandheldState state) const;

	// ---- per-frame ----
	// Advances every clock and returns this frame's contribution. Adds nothing on a fresh
	// sample (post-seek) so a pose jump can never read as a shake spike.
	ShakePose Advance(const ShakeInput& in);

	// Drops all integrated state. Called by the stack on seek / spectator switch / disable.
	void Reset();

	// Emits the JSON fields the Config panel mirrors (leading comma included, so callers can
	// append unconditionally).
	void AppendStateJson(std::ostringstream& o) const;

	std::string DebugStateJson() const;

private:
	// A live curve instance. Fixed pool -- the render path never allocates.
	struct Layer {
		bool active = false;
		int curve = -1;
		double startTime = 0.0;   // demoTime the curve's phase 0 maps to
		double influence = 1.0;
		double scale = 1.0;       // positional scale
		double rotScale = 1.0;    // angular scale
		double speed = 1.0;
		double fadeIn = 0.0;      // seconds
		double fadeOutAt = -1.0;  // demoTime the fade-out began, <0 = not fading
		double fadeOutDur = 0.0;
		double fadeOutFrom = 1.0;
		double removeAt = -1.0;   // demoTime to drop the layer, <0 = never
		double varSeed1 = 0.0, varSeed2 = 0.0, varFreq1 = 0.0, varFreq2 = 0.0;
	};
	// Live at once: the state layer, the state layer it is crossfading out of, the re-grip
	// layer, and the re-grip layer that is fading out -- four is the steady-state worst case, so
	// carry headroom rather than silently dropping a layer when they overlap.
	static constexpr int kMaxLayers = 6;

	// Deterministic RNG. Advances only on accepted samples and re-seeds on Reset(), so replaying
	// the same demo segment from the same seek point reproduces the same shake.
	double Rand01();
	double RandRange(double lo, double hi);

	int AddLayer(int curve, double now, double influence, double scale, double rotScale,
	             double speed, double offset, double fadeIn);
	void RemoveLayer(int slot, double now, double fadeSeconds);

	ShakePose SampleCurves(const ShakeInput& in);
	ShakePose SampleDrift(const ShakeInput& in);
	// Maps the cached (already smoothed) drift state to a pose. Shared by SampleDrift's tail and
	// the same-tick hold path, so the two can never disagree about the output mapping.
	ShakePose DriftPoseFromState() const;
	// Same, for the low-passed curve output.
	ShakePose CurvePoseFromState() const;
	void UpdateStateMachine(const ShakeInput& in);
	void UpdateRegrip(const ShakeInput& in);

	bool m_enabled = false;
	bool m_viewmodelDecoupled = false;
	bool m_centerLockEnabled = false;
	double m_centerLockPct = 100.0;
	double m_driftPct = 0.0;
	double m_driftDirDeg = 0.0;   // see SetDriftDirection
	double m_curvePct = 0.0;
	double m_tiltPct = 100.0;
	double m_regripPct = 100.0;
	double m_speedPct = 100.0;
	double m_variancePct = 40.0;   // the source addon's default
	double m_regripPeriod = 9.5;   // source addon picks uniformly in [5, 14]

	// Curve assigned per movement state. Defaults match the source addon's state presets.
	int m_stateCurve[3] = { -1, -1, -1 };

	// ---- integrated state ----
	Layer m_layers[kMaxLayers];
	int m_stateSlot = -1;    // layer driven by the auto-switcher
	int m_regripSlot = -1;   // layer driven by the re-grip timer

	HandheldState m_state = HandheldState::Idle;
	bool m_haveState = false;
	HandheldState m_pendingState = HandheldState::Idle;
	double m_pendingSince = 0.0;

	double m_tiltPhase = 0.0;
	double m_tiltRoll = 0.0;
	double m_nextRegrip = 0.0;
	double m_regripEndsAt = -1.0;

	// Drift layer state (all smoothed, hence held across frames).
	double m_jitterCooldown = 0.0;
	double m_jitterIntensity = 0.0;
	// Fixed-rate jitter re-roll: the clock that decides WHEN to take a new direction sample,
	// and the sample that is held between re-rolls. Held state is what makes the cadence
	// frame-rate independent -- without it the direction changes once per rendered frame.
	double m_jitterResampleClock = 0.0;
	double m_jitterDirP = 0.0;
	double m_jitterDirY = 0.0;
	double m_jitterDirR = 0.0;
	double m_walkPhaseOffset = 0.0;
	double m_tgtAngP = 0.0, m_tgtAngY = 0.0, m_tgtAngR = 0.0;
	double m_tgtPosX = 0.0, m_tgtPosY = 0.0, m_tgtPosZ = 0.0;
	double m_outAngP = 0.0, m_outAngY = 0.0, m_outAngR = 0.0;
	double m_outPosX = 0.0, m_outPosY = 0.0, m_outPosZ = 0.0;
	double m_fovWobble = 0.0;

	// Final low-pass over the summed curve output. The source file warns explicitly not to
	// remove this even though it looks redundant with the per-layer fades: it is what kills the
	// pop when a re-grip or a state switch starts mid-curve at a large value.
	double m_smRight = 0.0, m_smUp = 0.0, m_smFwd = 0.0;
	double m_smPitch = 0.0, m_smYaw = 0.0, m_smRoll = 0.0;

	unsigned long long m_rngState = 0;

	// Diagnostics for "handheld state".
	unsigned long long m_dbgSteps = 0;
	// Held frames: Advance calls that returned the PREVIOUS pose because the demo clock had not
	// moved. The ratio held:stepped IS the staircase the report describes -- the offset updates
	// at the demo's 64 Hz and is reused for every render frame in between -- so it has to be
	// counted before anything is changed about it.
	// Window counters for the rate probe. SEPARATE from m_dbgSteps, which the state JSON reports
	// as a running total -- resetting that one every second would silently turn a lifetime count
	// into a per-second count wherever it is read.
	unsigned long long m_dbgHeldFrames = 0;
	unsigned long long m_dbgWinSteps = 0;
	// WALL seconds in the window. A per-second rate divided by DEMO time reads as nonsense
	// across a seek (measured: 50 rolls over a 63-second demo jump reported 0.8/s), and the
	// whole point of this probe is to compare a real rate against a fixed target.
	double m_dbgWinWall = 0.0;
	unsigned long long m_dbgJitterRolls = 0;
	double m_dbgLastLogTime = 0.0;
	unsigned long long m_dbgResets = 0;
	unsigned long long m_dbgRegrips = 0;
	unsigned long long m_dbgSwitches = 0;
};

HandheldCam& HandheldCamRef();

// Console entry: "mirv_filmmaker handheld ...". Forwarded from FilmmakerCommand.cpp.
void HandheldCam_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
