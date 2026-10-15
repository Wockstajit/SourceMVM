#include "RageShake.h"

#include <cstring>
#include <string>
#include <vector>

// The GTA V camera-shake STYLE TABLE: every shake name the SHAKE_CAM / SHAKE_GAMEPLAY_CAM native
// accepts, expressed in the oscillator + envelope model that RageShake.cpp evaluates.
//
// -------------------------------------------------------------------------------------------
// PROVENANCE -- read this before "fixing" a number.
//
// Two halves, with very different confidence:
//
//   * The NAME LIST is authoritative: the 86 entries documented on SHAKE_GAMEPLAY_CAM
//     (citizenfx/natives, CAM/).
//
//   * The PER-STYLE NUMBERS are ours. Rockstar's authored values live in
//     common.rpf/data/cameras.ymt, which is not redistributable and has no public dump. So each name is mapped onto a FAMILY whose oscillator/envelope
//     shape matches what that shake is for, with per-name amplitude and frequency scales.
//
// shake-styles.json mirrors this table for regeneration.
//
// Amplitudes are RAGE units: RADIANS for Rot*, METRES for Pos*, DEGREES for Fov.
// RageShake.cpp converts to Source units / degrees on output.
// -------------------------------------------------------------------------------------------

namespace Filmmaker {

namespace {

using C = RageShakeComp;
using W = RageWaveform;

// A family's component before the per-name scales are applied.
struct FamComp {
	C comp;
	W wave;
	double amp;
	double freq;
	double phase;   // < 0 = deterministic per-index offset (see RageShake.cpp ResolvePhase)
	double decay;   // DampedSine only
};

struct Family {
	const FamComp* comps;
	int count;
	// One overall envelope per family. A LOOPING style is holdMs < 0; a one-shot has a release.
	RageEnvelope env;
};

// ---- LOOPING families (infinite hold: they run for as long as the keyframe segment does) ----

// Operator holding the camera: slow wander on all three rotation axes plus a faster micro-tremble.
const FamComp kHandheld[] = {
	{ C::RotX, W::Sine, 0.0170, 0.90, -1.0, 0.0 },
	{ C::RotZ, W::Sine, 0.0210, 0.63, -1.0, 0.0 },
	{ C::RotY, W::Sine, 0.0110, 0.41, -1.0, 0.0 },
	{ C::RelPosZ, W::Sine, 0.0300, 1.10, -1.0, 0.0 },
	{ C::RotX, W::Sine, 0.0050, 3.70, -1.0, 0.0 },
	{ C::RotZ, W::Sine, 0.0060, 2.90, -1.0, 0.0 },
};
// HAND_SHAKE_ROLL: the same hand, weighted into bank instead of pan/tilt.
const FamComp kHandheldRoll[] = {
	{ C::RotY, W::Sine, 0.0400, 0.55, -1.0, 0.0 },
	{ C::RotY, W::Sine, 0.0100, 2.30, -1.0, 0.0 },
	{ C::RotX, W::Sine, 0.0090, 0.87, -1.0, 0.0 },
	{ C::RotZ, W::Sine, 0.0100, 0.61, -1.0, 0.0 },
};
// Engine / rotor buzz: tiny, fast, mostly translation.
const FamComp kVibration[] = {
	{ C::RelPosZ, W::Sine, 0.0260, 24.0, -1.0, 0.0 },
	{ C::RelPosX, W::Sine, 0.0190, 19.0, -1.0, 0.0 },
	{ C::RotX, W::Sine, 0.0090, 17.0, -1.0, 0.0 },
	{ C::RotZ, W::Sine, 0.0080, 21.0, -1.0, 0.0 },
	{ C::RotY, W::Sine, 0.0050, 26.0, -1.0, 0.0 },
};
// Tyres on tarmac: lower and heavier than VIBRATE, dominated by vertical travel.
const FamComp kRoadVibration[] = {
	{ C::RelPosZ, W::Sine, 0.0200, 11.0, -1.0, 0.0 },
	{ C::RotX, W::Sine, 0.0065, 9.50, -1.0, 0.0 },
	{ C::RotY, W::Sine, 0.0050, 7.00, -1.0, 0.0 },
	{ C::RelPosX, W::Sine, 0.0100, 13.0, -1.0, 0.0 },
};
// The Rockstar Editor's GROUND VIBRATION. Road rumble through a mount: fast vertical chatter
// with just enough tilt and FOV to stop it reading as a pure translation.
const FamComp kHighSpeed[] = {
	{ C::RelPosZ, W::Sine, 0.0350, 11.0, -1.0, 0.0 },
	{ C::RotX, W::Sine, 0.0120, 9.00, -1.0, 0.0 },
	{ C::RotZ, W::Sine, 0.0100, 6.50, -1.0, 0.0 },
	{ C::RotY, W::Sine, 0.0060, 8.00, -1.0, 0.0 },
	{ C::RelPosX, W::Sine, 0.0200, 13.0, -1.0, 0.0 },
	{ C::Fov, W::Sine, 0.9000, 5.00, -1.0, 0.0 },
};
// Air turbulence: mid-frequency, wide, no fixed rhythm.
const FamComp kTurbulence[] = {
	{ C::RotZ, W::Sine, 0.0320, 1.40, -1.0, 0.0 },
	{ C::RotX, W::Sine, 0.0260, 1.90, -1.0, 0.0 },
	{ C::RelPosZ, W::Sine, 0.0240, 2.30, -1.0, 0.0 },
	{ C::RotY, W::Sine, 0.0180, 0.97, -1.0, 0.0 },
};
// The Rockstar Editor's AIR TURBULENCE. Free fall: everything moving at once, big and loose.
const FamComp kSkyDiving[] = {
	{ C::RotZ, W::Sine, 0.0550, 1.30, -1.0, 0.0 },
	{ C::RotX, W::Sine, 0.0460, 1.70, -1.0, 0.0 },
	{ C::RotY, W::Sine, 0.0380, 2.10, -1.0, 0.0 },
	{ C::RelPosZ, W::Sine, 0.0450, 2.60, -1.0, 0.0 },
	{ C::RelPosX, W::Sine, 0.0380, 1.90, -1.0, 0.0 },
	{ C::RotX, W::Sine, 0.0170, 5.10, -1.0, 0.0 },
};
// Drunk: very slow, roll-led, with the FOV breathing.
const FamComp kDrunk[] = {
	{ C::RotY, W::Sine, 0.0950, 0.21, -1.0, 0.0 },
	{ C::RotZ, W::Sine, 0.0580, 0.17, -1.0, 0.0 },
	{ C::RotX, W::Sine, 0.0380, 0.13, -1.0, 0.0 },
	{ C::Fov, W::Sine, 2.5000, 0.11, -1.0, 0.0 },
	{ C::RelPosX, W::Sine, 0.0900, 0.19, -1.0, 0.0 },
};
// FAMILY5_DRUG_TRIP: slower and far wider than drunk, FOV-dominant.
const FamComp kDrugTrip[] = {
	{ C::RotY, W::Sine, 0.1500, 0.090, -1.0, 0.0 },
	{ C::Fov, W::Sine, 6.0000, 0.070, -1.0, 0.0 },
	{ C::RotZ, W::Sine, 0.0750, 0.050, -1.0, 0.0 },
	{ C::RotX, W::Sine, 0.0500, 0.110, -1.0, 0.0 },
};
// Boat/water float: long vertical swell with a lazy tilt.
const FamComp kWaterBob[] = {
	{ C::RelPosZ, W::Sine, 0.0800, 0.55, -1.0, 0.0 },
	{ C::RotX, W::Sine, 0.0200, 0.50, -1.0, 0.0 },
	{ C::RotY, W::Sine, 0.0160, 0.35, -1.0, 0.0 },
};
// WOBBLY: a drunk-adjacent bank that stays readable.
const FamComp kWobbly[] = {
	{ C::RotY, W::Sine, 0.0450, 0.55, -1.0, 0.0 },
	{ C::RotZ, W::Sine, 0.0320, 0.42, -1.0, 0.0 },
	{ C::RelPosX, W::Sine, 0.0600, 0.60, -1.0, 0.0 },
};
// Aim sway: the smallest family. Barely-there drift for a held sight picture.
const FamComp kAim[] = {
	{ C::RotZ, W::Sine, 0.0035, 1.10, -1.0, 0.0 },
	{ C::RotX, W::Sine, 0.0028, 1.40, -1.0, 0.0 },
};

// ---- ONE-SHOT families: plain sines under a hard release envelope ----
//
// These used DAMPED_SINEWAVE, which was a mistake worth recording. The damped waveform's decay
// sweep starts at d=0.5, so a burst OPENS at exp(-decay/2) of its nominal amplitude -- with the
// decay values that were guessed here, explosions began at ~17% strength and were effectively
// invisible on the exact frames they are supposed to hit hardest. The envelope's exponential
// release already does the decaying, and it does it predictably, so the oscillators are plain
// sines and the amplitude you author is the amplitude you get on frame one.

const FamComp kExplosion[] = {
	{ C::RotX, W::Sine, 0.0900, 8.00, -1.0, 0.0 },
	{ C::RotZ, W::Sine, 0.0750, 6.70, -1.0, 0.0 },
	{ C::RotY, W::Sine, 0.0600, 5.30, -1.0, 0.0 },
	{ C::RelPosZ, W::Sine, 0.0600, 7.70, -1.0, 0.0 },
	{ C::Fov, W::Sine, 2.0000, 5.50, -1.0, 0.0 },
};
const FamComp kRecoil[] = {
	{ C::RotX, W::Sine, 0.0200, 14.0, -1.0, 0.0 },
	{ C::RotZ, W::Sine, 0.0130, 11.0, -1.0, 0.0 },
	{ C::RelPosY, W::Sine, 0.0120, 13.0, -1.0, 0.0 },
};
const FamComp kImpact[] = {
	{ C::RotX, W::Sine, 0.0450, 16.0, -1.0, 0.0 },
	{ C::RotY, W::Sine, 0.0340, 13.0, -1.0, 0.0 },
	{ C::RotZ, W::Sine, 0.0300, 18.0, -1.0, 0.0 },
	{ C::RelPosZ, W::Sine, 0.0280, 15.0, -1.0, 0.0 },
};
const FamComp kJolt[] = {
	{ C::RelPosZ, W::Sine, 0.0900, 9.00, -1.0, 0.0 },
	{ C::RotX, W::Sine, 0.0500, 10.0, -1.0, 0.0 },
	{ C::RotY, W::Sine, 0.0260, 7.50, -1.0, 0.0 },
};
// DEATH_FAIL / FPS_DEATH: not a hit, a slow swoon. Long attack, long release, FOV-led.
const FamComp kDeathFail[] = {
	{ C::RotY, W::Sine, 0.0800, 0.35, -1.0, 0.0 },
	{ C::Fov, W::Sine, 4.0000, 0.30, -1.0, 0.0 },
	{ C::RotX, W::Sine, 0.0400, 0.22, -1.0, 0.0 },
};

// Envelope shapes. Looping ones hold forever (holdMs < 0) so a style keeps running for as long
// as the keyframe segment it is attached to; one-shots release and stop.
RageEnvelope EnvLoop(int attackMs) {
	RageEnvelope e;
	e.attackMs = attackMs;
	e.sustainLevel = 1.0;
	e.holdMs = -1;
	e.attackCurve = RageCurve::SlowInOut;
	return e;
}
RageEnvelope EnvOneShot(int attackMs, int holdMs, int releaseMs, RageCurve release = RageCurve::Exponential) {
	RageEnvelope e;
	e.attackMs = attackMs;
	e.sustainLevel = 1.0;
	e.holdMs = holdMs;
	e.releaseMs = releaseMs;
	e.attackCurve = RageCurve::Linear;
	e.releaseCurve = release;
	return e;
}

enum class Fam {
	Handheld, HandheldRoll, Vibration, RoadVibration, HighSpeed, Turbulence, SkyDiving,
	Drunk, DrugTrip, WaterBob, Wobbly, Aim,
	Explosion, Recoil, Impact, Jolt, DeathFail,
	Count
};

const Family& FamilyOf(Fam f) {
	static const Family s_families[(int)Fam::Count] = {
		{ kHandheld,      (int)(sizeof(kHandheld) / sizeof(kHandheld[0])),           EnvLoop(400) },
		{ kHandheldRoll,  (int)(sizeof(kHandheldRoll) / sizeof(kHandheldRoll[0])),   EnvLoop(400) },
		{ kVibration,     (int)(sizeof(kVibration) / sizeof(kVibration[0])),         EnvLoop(200) },
		{ kRoadVibration, (int)(sizeof(kRoadVibration) / sizeof(kRoadVibration[0])), EnvLoop(250) },
		{ kHighSpeed,     (int)(sizeof(kHighSpeed) / sizeof(kHighSpeed[0])),         EnvLoop(300) },
		{ kTurbulence,    (int)(sizeof(kTurbulence) / sizeof(kTurbulence[0])),       EnvLoop(500) },
		{ kSkyDiving,     (int)(sizeof(kSkyDiving) / sizeof(kSkyDiving[0])),         EnvLoop(600) },
		{ kDrunk,         (int)(sizeof(kDrunk) / sizeof(kDrunk[0])),                 EnvLoop(1500) },
		{ kDrugTrip,      (int)(sizeof(kDrugTrip) / sizeof(kDrugTrip[0])),           EnvLoop(2500) },
		{ kWaterBob,      (int)(sizeof(kWaterBob) / sizeof(kWaterBob[0])),           EnvLoop(800) },
		{ kWobbly,        (int)(sizeof(kWobbly) / sizeof(kWobbly[0])),               EnvLoop(700) },
		{ kAim,           (int)(sizeof(kAim) / sizeof(kAim[0])),                     EnvLoop(400) },
		{ kExplosion,     (int)(sizeof(kExplosion) / sizeof(kExplosion[0])),         EnvOneShot(0, 0, 1400) },
		{ kRecoil,        (int)(sizeof(kRecoil) / sizeof(kRecoil[0])),               EnvOneShot(0, 0, 300) },
		{ kImpact,        (int)(sizeof(kImpact) / sizeof(kImpact[0])),               EnvOneShot(0, 0, 350) },
		{ kJolt,          (int)(sizeof(kJolt) / sizeof(kJolt[0])),                   EnvOneShot(0, 0, 500) },
		{ kDeathFail,     (int)(sizeof(kDeathFail) / sizeof(kDeathFail[0])),
		                  EnvOneShot(1200, 1500, 2000, RageCurve::SlowInOut) },
	};
	int i = (int)f;
	if (i < 0 || i >= (int)Fam::Count) i = 0;
	return s_families[i];
}

// The 86 names SHAKE_GAMEPLAY_CAM documents, in the doc's own (alphabetical) order so the
// timeline's left/right arrows step through them predictably.
struct StyleRow {
	const char* name;
	Fam fam;
	double ampScale;
	double freqScale;
};

const StyleRow kStyles[] = {
	{ "ASSAULT_RIFLE_RECOIL_SHAKE",                 Fam::Recoil,        1.00, 1.00 },
	{ "BOAT_WATER_ENTRY_SHAKE",                     Fam::Impact,        1.20, 0.80 },
	{ "CAMERA_OPERATOR_SHAKE_X",                    Fam::Handheld,      0.90, 1.00 },
	{ "CAMERA_OPERATOR_SHAKE_Z",                    Fam::Handheld,      0.90, 0.85 },
	{ "CAMERA_OPERATOR_TURBULENCE_SHAKE",           Fam::Turbulence,    1.00, 1.00 },
	{ "CARBINE_RIFLE_ACCURACY_OFFSET_SHAKE",        Fam::Aim,           1.40, 1.00 },
	{ "CARBINE_RIFLE_RECOIL_SHAKE",                 Fam::Recoil,        0.90, 1.05 },
	{ "CINEMATIC_SHOOTING_RUN_SHAKE",               Fam::Handheld,      1.60, 1.50 },
	{ "DAMPED_HAND_SHAKE",                          Fam::Handheld,      0.60, 0.85 },
	{ "DEATH_FAIL_IN_EFFECT_SHAKE",                 Fam::DeathFail,     1.00, 1.00 },
	{ "DEATH_FAIL_OUT_EFFECT_SHAKE",                Fam::DeathFail,     0.80, 0.85 },
	{ "DEFAULT_DEPLOY_PARACHUTE_SHAKE",             Fam::Jolt,          1.30, 1.00 },
	{ "DEFAULT_FIRST_PERSON_RECOIL_SHAKE",          Fam::Recoil,        1.10, 1.00 },
	{ "DEFAULT_KILL_EFFECT_SHAKE",                  Fam::Impact,        0.90, 1.00 },
	{ "DEFAULT_THIRD_PERSON_ACCURACY_OFFSET_SHAKE", Fam::Aim,           1.00, 1.00 },
	{ "DEFAULT_THIRD_PERSON_RECOIL_SHAKE",          Fam::Recoil,        0.85, 0.95 },
	{ "DRUNK_SHAKE",                                Fam::Drunk,         1.00, 1.00 },
	{ "FAMILY5_DRUG_TRIP_SHAKE",                    Fam::DrugTrip,      1.00, 1.00 },
	{ "FIRST_PERSON_AIM_SHAKE",                     Fam::Aim,           1.20, 1.00 },
	{ "FIRST_PERSON_DEPLOY_PARACHUTE_SHAKE",        Fam::Jolt,          1.50, 1.10 },
	{ "FOLLOW_RUN_SHAKE",                           Fam::Handheld,      1.80, 1.60 },
	{ "FOLLOW_SWIM_SHAKE",                          Fam::WaterBob,      1.20, 1.30 },
	{ "FPS_ASSAULT_RIFLE_RECOIL_SHAKE",             Fam::Recoil,        1.20, 1.00 },
	{ "FPS_BOLT_RELOAD_SHAKE",                      Fam::Impact,        0.50, 1.30 },
	{ "FPS_BULLET_HIT_SHAKE",                       Fam::Impact,        1.10, 1.15 },
	{ "FPS_DEATH_SHAKE",                            Fam::DeathFail,     1.20, 1.00 },
	{ "FPS_GRENADE_LAUNCHER_RECOIL_SHAKE",          Fam::Recoil,        1.60, 0.85 },
	{ "FPS_MAG_DROP_SHAKE",                         Fam::Impact,        0.35, 1.20 },
	{ "FPS_MAG_RELOAD_SHAKE",                       Fam::Impact,        0.40, 1.20 },
	{ "FPS_MELEE_HIT_SHAKE",                        Fam::Impact,        1.30, 0.95 },
	{ "FPS_MG_RECOIL_SHAKE",                        Fam::Recoil,        1.35, 1.05 },
	{ "FPS_MINIGUN_RECOIL_SHAKE",                   Fam::Vibration,     1.60, 1.20 },
	{ "FPS_PISTOL_RECOIL_SHAKE",                    Fam::Recoil,        0.80, 1.10 },
	{ "FPS_RPG_RECOIL_SHAKE",                       Fam::Explosion,     0.60, 1.10 },
	{ "FPS_SHOTGUN_PUMP_SHAKE",                     Fam::Impact,        0.60, 1.00 },
	{ "FPS_SHOTGUN_RECOIL_SHAKE",                   Fam::Recoil,        1.70, 0.80 },
	{ "FPS_SMG_RECOIL_SHAKE",                       Fam::Recoil,        0.70, 1.20 },
	{ "FPS_STEERING_WHEEL_HIT_SHAKE",               Fam::Impact,        0.90, 1.00 },
	{ "FPS_TANK_RECOIL_SHAKE",                      Fam::Explosion,     0.80, 0.90 },
	{ "FPS_THROW_SHAKE",                            Fam::Impact,        0.50, 0.90 },
	{ "FPS_VEHICLE_HIT_SHAKE",                      Fam::Impact,        1.20, 0.95 },
	{ "FPS_ZOOM_IN_SHAKE",                          Fam::Aim,           0.80, 1.30 },
	{ "GAMEPLAY_EXPLOSION_SHAKE",                   Fam::Explosion,     1.00, 1.00 },
	{ "GRENADE_EXPLOSION_SHAKE",                    Fam::Explosion,     1.10, 1.05 },
	{ "GRENADE_LAUNCHER_RECOIL_SHAKE",              Fam::Recoil,        1.50, 0.85 },
	{ "HAND_SHAKE",                                 Fam::Handheld,      1.00, 1.00 },
	{ "HAND_SHAKE_ROLL",                            Fam::HandheldRoll,  1.00, 1.00 },
	{ "HIGH_DIVE_SHAKE",                            Fam::SkyDiving,     0.80, 1.10 },
	{ "HIGH_FALL_SHAKE",                            Fam::SkyDiving,     1.10, 1.20 },
	{ "HIGH_SPEED_BOAT_SHAKE",                      Fam::HighSpeed,     1.40, 0.80 },
	{ "HIGH_SPEED_POV_SHAKE",                       Fam::HighSpeed,     1.00, 1.00 },
	{ "HIGH_SPEED_VEHICLE_SHAKE",                   Fam::HighSpeed,     1.20, 1.00 },
	{ "HIGH_SPEED_VIBRATION_POV_SHAKE",             Fam::Vibration,     1.30, 1.10 },
	{ "IDLE_HAND_SHAKE",                            Fam::Handheld,      0.45, 0.70 },
	{ "JOLT_SHAKE",                                 Fam::Jolt,          1.00, 1.00 },
	{ "KILL_SHOT_SHAKE",                            Fam::Impact,        1.40, 0.90 },
	{ "LARGE_EXPLOSION_SHAKE",                      Fam::Explosion,     1.60, 0.85 },
	{ "LOW_ORBIT_HIGH_SPEED_CAMERA_SHAKE",          Fam::Turbulence,    0.70, 0.60 },
	{ "LOW_ORBIT_INACCURACY_CAMERA_SHAKE",          Fam::Handheld,      0.50, 0.50 },
	{ "MEDIUM_EXPLOSION_SHAKE",                     Fam::Explosion,     1.00, 0.95 },
	{ "MG_RECOIL_SHAKE",                            Fam::Recoil,        1.20, 1.00 },
	{ "MINIGUN_RECOIL_SHAKE",                       Fam::Vibration,     1.40, 1.10 },
	{ "PARACHUTING_SHAKE",                          Fam::Turbulence,    1.10, 0.90 },
	{ "PISTOL_RECOIL_SHAKE",                        Fam::Recoil,        0.70, 1.10 },
	{ "PLANE_PART_SPEED_SHAKE",                     Fam::Vibration,     1.10, 0.80 },
	{ "POV_IDLE_SHAKE",                             Fam::Handheld,      0.50, 0.80 },
	{ "REPLAY_DRUNK_SHAKE",                         Fam::Drunk,         1.00, 1.00 },
	{ "REPLAY_EXPLOSION_SHAKE",                     Fam::Explosion,     1.00, 1.00 },
	{ "REPLAY_HAND_SHAKE",                          Fam::Handheld,      1.00, 1.00 },
	{ "REPLAY_HIGH_SPEED_VEHICLE_SHAKE",            Fam::HighSpeed,     1.00, 1.00 },
	{ "REPLAY_SKY_DIVING_SHAKE",                    Fam::SkyDiving,     1.00, 1.00 },
	{ "ROAD_VIBRATION_SHAKE",                       Fam::RoadVibration, 1.00, 1.00 },
	{ "RPG_RECOIL_SHAKE",                           Fam::Explosion,     0.55, 1.10 },
	{ "SHOTGUN_RECOIL_SHAKE",                       Fam::Recoil,        1.60, 0.80 },
	{ "SKY_DIVING_SHAKE",                           Fam::SkyDiving,     1.00, 1.00 },
	{ "SMALL_EXPLOSION_SHAKE",                      Fam::Explosion,     0.55, 1.15 },
	{ "SMG_RECOIL_SHAKE",                           Fam::Recoil,        0.65, 1.20 },
	{ "STUNT_HAND_SHAKE",                           Fam::Handheld,      1.50, 1.20 },
	{ "SWITCH_HAND_SHAKE",                          Fam::Handheld,      1.20, 1.30 },
	{ "TANK_RECOIL_SHAKE",                          Fam::Explosion,     0.75, 0.90 },
	{ "VEH_IMPACT_HEADING_SHAKE",                   Fam::Impact,        1.00, 0.90 },
	{ "VEH_IMPACT_PITCH_HEADING_SHAKE_FPS",         Fam::Impact,        1.10, 0.95 },
	{ "VEH_IMPACT_PITCH_SHAKE",                     Fam::Impact,        1.00, 1.00 },
	{ "VIBRATE_SHAKE",                              Fam::Vibration,     1.00, 1.00 },
	{ "WATER_BOB_SHAKE",                            Fam::WaterBob,      1.00, 1.00 },
	{ "WOBBLY_SHAKE",                               Fam::Wobbly,        1.00, 1.00 },
};

constexpr int kStyleCount = (int)(sizeof(kStyles) / sizeof(kStyles[0]));

// Built once: the family template with the row's scales baked in, so evaluation is a plain read.
struct StyleTable {
	std::vector<RageShakeComponent> storage; // all components, contiguous; defs point into it
	std::vector<RageShakeDef> defs;

	StyleTable() {
		int total = 0;
		for (int i = 0; i < kStyleCount; ++i)
			total += FamilyOf(kStyles[i].fam).count;
		storage.reserve((size_t)total); // reserve up front: defs hold raw pointers into this
		defs.reserve((size_t)kStyleCount);

		for (int i = 0; i < kStyleCount; ++i) {
			const StyleRow& row = kStyles[i];
			const Family& fam = FamilyOf(row.fam);
			const size_t first = storage.size();
			for (int c = 0; c < fam.count; ++c) {
				const FamComp& fc = fam.comps[c];
				RageShakeComponent rc;
				rc.component = fc.comp;
				rc.osc.waveform = fc.wave;
				rc.osc.amplitude = fc.amp * row.ampScale;
				rc.osc.frequency = fc.freq * row.freqScale;
				rc.osc.phase = fc.phase;
				rc.osc.decay = fc.decay;
				rc.hasEnvelope = false; // these families envelope at the shake level, not per component
				storage.push_back(rc);
			}
			RageShakeDef def;
			def.name = row.name;
			def.components = storage.data() + first;
			def.componentCount = fam.count;
			def.hasOverallEnvelope = true;
			def.overallEnvelope = fam.env;
			defs.push_back(def);
		}
	}
};

const StyleTable& Table() {
	static const StyleTable s_table;
	return s_table;
}

// ---------------------------------------------------------------------------------------------
// THE MENU LIST -- the camera shakes, in the order the timeline's arrows step through them.
//
// The full 86 stay settable by name from the console, but ~60 of them are not camera work at all:
// ASSAULT_RIFLE_RECOIL, FPS_MAG_DROP, FPS_BOLT_RELOAD, CARBINE_RIFLE_ACCURACY_OFFSET and the rest
// are weapon and hit reactions that RAGE routes through the same shake system because it is the
// only shake system it has. In a director's style picker they are noise -- you would step past
// forty magazine drops to reach "Drunk".
//
// What is left is grouped by feel (hand-held -> operator -> vibration -> flight -> altered ->
// impact) so holding an arrow sweeps through related looks instead of jumping at random, and each
// carries a short label because "DEFAULT_THIRD_PERSON_ACCURACY_OFFSET_SHAKE" does not fit in a
// value box and does not read as anything at a glance.
struct MenuRow { const char* style; const char* label; };

const MenuRow kMenu[] = {
	// ---- THE ROCKSTAR EDITOR SET ----
	// The Rockstar Editor's camera marker offers None plus five shakes: Hand, Drunk, Explosion,
	// Air Turbulence and Ground Vibration. They correspond to the five REPLAY_-prefixed names in
	// the native list.
	//
	// Everything the previous version had here was a GAMEPLAY shake wearing a camera-sounding
	// name. It is kept below, clearly separated, because this is a filmmaking tool and the extra
	// looks are useful -- but these five are the Rockstar Editor, and they come first.
	{ "REPLAY_HAND_SHAKE",                   "Hand" },
	{ "REPLAY_DRUNK_SHAKE",                  "Drunk" },
	{ "REPLAY_EXPLOSION_SHAKE",              "Explosion" },
	{ "REPLAY_SKY_DIVING_SHAKE",             "Air Turbulence" },
	{ "REPLAY_HIGH_SPEED_VEHICLE_SHAKE",     "Ground Vibration" },

	// ---- EXTRAS (ours, not the Rockstar Editor) ----
	// Camera-behaviour shakes from the wider native list, grouped by feel. Reachable from the
	// dropdown under their own header and from the arrows after the five above.
	{ "DAMPED_HAND_SHAKE",                   "Hand Soft" },
	{ "STUNT_HAND_SHAKE",                    "Hand Stunt" },
	{ "HAND_SHAKE_ROLL",                     "Hand Roll" },
	{ "FOLLOW_RUN_SHAKE",                    "Running" },
	{ "CAMERA_OPERATOR_TURBULENCE_SHAKE",    "Turbulence" },
	{ "ROAD_VIBRATION_SHAKE",                "Road Vibration" },
	{ "VIBRATE_SHAKE",                       "Vibrate" },
	{ "WATER_BOB_SHAKE",                     "Water Bob" },
	{ "PARACHUTING_SHAKE",                   "Parachuting" },
	{ "WOBBLY_SHAKE",                        "Wobbly" },
	{ "FAMILY5_DRUG_TRIP_SHAKE",             "Drug Trip" },
	{ "SMALL_EXPLOSION_SHAKE",               "Explosion Small" },
	{ "LARGE_EXPLOSION_SHAKE",               "Explosion Large" },
	{ "JOLT_SHAKE",                          "Jolt" },
};

// How many leading kMenu entries are the genuine Rockstar Editor set. The dropdown draws a
// section break here.
constexpr int kRockstarEditorCount = 5;

constexpr int kMenuCount = (int)(sizeof(kMenu) / sizeof(kMenu[0]));

// kMenu is written by NAME, so it is resolved to style indices once and any typo shows up as a
// missing menu entry rather than as a silently wrong shake.
struct MenuTable {
	std::vector<int> styleIndex;               // menu position -> style index
	std::vector<int> menuOfStyle;              // style index   -> menu position (-1 = off-menu)
	std::vector<const char*> labelOfStyle;     // style index   -> label (name when off-menu)

	MenuTable() {
		menuOfStyle.assign((size_t)kStyleCount, -1);
		labelOfStyle.assign((size_t)kStyleCount, nullptr);
		for (int i = 0; i < kStyleCount; ++i)
			labelOfStyle[(size_t)i] = kStyles[i].name;
		styleIndex.reserve((size_t)kMenuCount);
		for (int i = 0; i < kMenuCount; ++i) {
			const int si = RageShake_FindStyle(kMenu[i].style);
			if (si < 0) continue; // name typo: leave it off the menu rather than guess
			menuOfStyle[(size_t)si] = (int)styleIndex.size();
			labelOfStyle[(size_t)si] = kMenu[i].label;
			styleIndex.push_back(si);
		}
	}
};

const MenuTable& Menu() {
	static const MenuTable s_menu;
	return s_menu;
}

} // namespace

int RageShake_StyleCount() { return kStyleCount; }

const RageShakeDef& RageShake_Style(int index) {
	const StyleTable& t = Table();
	if (index < 0) index = 0;
	if (index >= (int)t.defs.size()) index = (int)t.defs.size() - 1;
	return t.defs[(size_t)index];
}

const char* RageShake_StyleName(int index) {
	if (index < 0 || index >= kStyleCount) return "";
	return kStyles[index].name;
}

int RageShake_FindStyle(const char* name) {
	if (!name || !*name) return -1;
	for (int i = 0; i < kStyleCount; ++i) {
		if (0 == _stricmp(kStyles[i].name, name))
			return i;
	}
	return -1;
}

int RageShake_MenuCount() { return (int)Menu().styleIndex.size(); }

int RageShake_RockstarEditorCount() {
	const int n = (int)Menu().styleIndex.size();
	return (kRockstarEditorCount < n) ? kRockstarEditorCount : n;
}

int RageShake_MenuStyle(int menuIndex) {
	const MenuTable& m = Menu();
	if (menuIndex < 0 || menuIndex >= (int)m.styleIndex.size()) return -1;
	return m.styleIndex[(size_t)menuIndex];
}

int RageShake_MenuIndexOfStyle(int styleIdx) {
	const MenuTable& m = Menu();
	if (styleIdx < 0 || styleIdx >= (int)m.menuOfStyle.size()) return -1;
	return m.menuOfStyle[(size_t)styleIdx];
}

const char* RageShake_StyleLabel(int styleIdx) {
	const MenuTable& m = Menu();
	if (styleIdx < 0 || styleIdx >= (int)m.labelOfStyle.size()) return "";
	return m.labelOfStyle[(size_t)styleIdx];
}

} // namespace Filmmaker
