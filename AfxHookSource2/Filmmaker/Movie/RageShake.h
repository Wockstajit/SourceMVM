#pragma once

// RageShake: an oscillator + envelope camera-shake evaluator, so a SOURCE:MVM camera path can
// carry shake styles in the spirit of a GTA V / Rockstar Editor shot.
//
// This module is PURE MATH and knows nothing about camera paths, keyframes, demos or Panorama.
// It answers exactly one question: "given shake style S, how much has the camera moved by
// `tSec` seconds into it?". CamPathShake owns the per-keyframe settings and does the applying.
//
// It is deliberately INDEPENDENT of CameraShakeStack / HandheldCam / WeaponRecoil. Those model
// a hand-held operator and weapon kick and are gated OFF for camera-path playback (see
// CameraShakeTypes.h ShakeCamMode::CameraPath). This is the authored, per-keyframe layer.
//
// ---------------------------------------------------------------------------------------------
// THE MODEL
//
// A shake is a list of FRAME COMPONENTS. Each component is one oscillator, optionally gated by
// its own envelope, writing into one channel of the camera frame:
//
//     shakeValue = oscillator(t) * componentEnvelope(t) * overallEnvelope(t) * amplitude
//
// and shakeValue is ADDED to that channel. Nothing is multiplied into the authored pose, so the
// layer is non-destructive by construction: remove the shake and the pose is bit-identical.
//
//   Oscillator: waveform, amplitude, frequency (Hz), phase, dcOffset, decay
//     SINE:         level = A * sin(2pi*(f*t + phase))
//     DAMPED_SINE:  d     = 2pi * (t / f)
//                   level = A * exp(-decay * (sin(d)*0.5 + 0.5)) * sin(2pi*(f*t + phase))
//     level += dcOffset
//
//   Envelope: an ADSHR with curve types on attack and release --
//     preDelay -> attack -> decay -> hold -> release, durations in MILLISECONDS.
//     attack targets 1.0 when a decay phase exists, else sustainLevel;
//     decay lerps 1.0 -> sustainLevel; hold sits at sustainLevel (hold < 0 = INFINITE, the shake
//     never ends -- this is what every looping style uses); release curves sustainLevel -> 0.
//     The default release curve is exp(-7x), which is below 0.001 by x = 1.
//
// ---------------------------------------------------------------------------------------------
// TWO DELIBERATE CHOICES, both required by a demo/render tool:
//
// 1. ANALYTIC, NOT INTEGRATED. Advancing the phase by 2pi*f*dt once per rendered frame would
//    make the result depend on the frame cadence, which would mean a scrub and a render of the
//    same tick disagree, and two renders of the same range disagree with each other. We evaluate
//    the closed form at an absolute t instead. With a constant frequency scale (which is all we
//    use -- a runtime frequency-scale blend is a gameplay feature, not an authoring one) the two
//    are the same function, so this costs nothing and buys exact reproducibility and correct
//    scrubbing.
//
// 2. NO RANDOM PHASE. A randomly seeded phase gives a different shake every time you watch the
//    same shot. An exported movie must be frame-identical on re-render, so a
//    negative authored phase resolves to a fixed value derived from the component's index.
//
// Units: styles are authored in metres and radians; the pose this returns is SOURCE UNITS and
// DEGREES.

namespace Filmmaker {

// Oscillator waveforms.
enum class RageWaveform { Sine = 0, DampedSine = 1 };

// Envelope curve types used on attack and release. Anything else falls back to Linear.
enum class RageCurve {
	Exponential = 0, Linear,
	SlowIn, SlowOut, SlowInOut,
	VerySlowIn, VerySlowOut, VerySlowInSlowOut, SlowInVerySlowOut, VerySlowInVerySlowOut,
	QuadraticEaseIn, QuadraticEaseOut, QuadraticEaseInOut,
	CubicEaseIn, CubicEaseOut, CubicEaseInOut,
	QuarticEaseIn, QuarticEaseOut, QuarticEaseInOut,
	CircularEaseIn, CircularEaseOut, CircularEaseInOut
};

// Camera-frame channels a shake component can drive.
// RelPos* are the CAMERA basis (X = right, Y = front, Z = up).
// Rot* are the YXZ euler channels (X = pitch, Y = roll, Z = yaw).
enum class RageShakeComp {
	WorldPosX = 0, WorldPosY, WorldPosZ,
	RelPosX, RelPosY, RelPosZ,
	RotX, RotY, RotZ,
	Fov
};

struct RageOscillator {
	RageWaveform waveform = RageWaveform::Sine;
	double amplitude = 0.0;   // radians (Rot*), metres (Pos*), degrees (Fov)
	double frequency = 1.0;   // Hz
	double phase = -1.0;      // turns, 0..1; < 0 = "random", resolved deterministically here
	double dcOffset = 0.0;
	double decay = 1.0;       // DampedSine only
};

struct RageEnvelope {
	int preDelayMs = 0;
	int attackMs = 0;
	int decayMs = 0;
	double sustainLevel = 1.0;
	int holdMs = -1;          // < 0 = infinite hold (the shake loops forever)
	int releaseMs = 0;
	RageCurve attackCurve = RageCurve::Linear;
	RageCurve releaseCurve = RageCurve::Exponential;
};

struct RageShakeComponent {
	RageShakeComp component = RageShakeComp::RotX;
	RageOscillator osc;
	bool hasEnvelope = false;
	RageEnvelope env;
};

struct RageShakeDef {
	const char* name = "";
	const RageShakeComponent* components = nullptr;
	int componentCount = 0;
	bool hasOverallEnvelope = false;
	RageEnvelope overallEnvelope;
};

// One frame's contribution, ready to be added to a camera pose.
// Position offsets are SOURCE UNITS, angles are DEGREES, fov is degrees.
struct RageShakePose {
	double worldX = 0.0, worldY = 0.0, worldZ = 0.0; // world-space offset
	double right = 0.0, fwd = 0.0, up = 0.0;         // camera-basis offset
	double pitch = 0.0, yaw = 0.0, roll = 0.0;
	double fov = 0.0;

	bool Any() const;
	void AddScaled(const RageShakePose& o, double k);
};

// Evaluate `def` at `tSec` seconds after the shake started.
//   amplitude - the shake amplitude scalar;
//               1.0 = the authored strength.
//   timeScale - the shake clock's speed; 1.0 = the authored speed.
//
// timeScale scales the shake's WHOLE CLOCK, not just oscillator frequency -- that is what the
// Rockstar Editor's per-marker "shake speed" behaves like: the whole timestep is scaled, which
// reaches the envelope too, so a slowed one-shot also takes correspondingly longer to attack and
// release. Scaling only the frequency -- which is
// what an earlier version of this did -- leaves a one-shot's decay running at full speed while its
// oscillation crawls, which sounds subtle and is very much not.
//
// A finished one-shot (past its release) returns an all-zero pose.
RageShakePose RageShake_Eval(const RageShakeDef& def, double tSec, double amplitude, double timeScale);

// True once every envelope in `def` has finished, i.e. the shake can never contribute again.
// Always false for looping styles (infinite hold).
bool RageShake_Finished(const RageShakeDef& def, double tSec, double timeScale = 1.0);

// --- the style table (RageShakeStyles.cpp) ---

int RageShake_StyleCount();
const RageShakeDef& RageShake_Style(int index);          // index is clamped
const char* RageShake_StyleName(int index);              // "" when out of range
int RageShake_FindStyle(const char* name);               // case-insensitive; -1 if unknown

// --- the MENU list: the camera shakes, curated ---
//
// All 86 native styles stay reachable by name from the console, but most of them are not camera
// work -- they are weapon recoil, magazine drops, bolt reloads, accuracy offsets and hit
// reactions that RAGE happens to route through the same shake system. Putting those in a
// director's style picker is noise. The menu therefore offers only the shakes that describe how a
// CAMERA is behaving, under short readable labels rather than raw enum names, the way the
// Rockstar Editor presents them.
int RageShake_MenuCount();
// How many of the leading menu entries are the genuine Rockstar Editor set (its camera marker
// offers exactly these). The rest are ours. The dropdown draws a section break at this index.
int RageShake_RockstarEditorCount();
int RageShake_MenuStyle(int menuIndex);       // -> style index; -1 if out of range
int RageShake_MenuIndexOfStyle(int styleIdx); // -> menu position; -1 if not on the menu
// Short display label ("Hand Held"). Falls back to the raw style name for the off-menu styles,
// which can still be set from the console.
const char* RageShake_StyleLabel(int styleIdx);

} // namespace Filmmaker
