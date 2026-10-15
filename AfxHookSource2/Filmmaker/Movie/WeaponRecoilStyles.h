#pragma once

// Weapon Recoil STYLE profiles: the per-channel spring constants and the randomness budget that
// together decide what the kick FEELS like, separated from WeaponRecoil.cpp's integrator.
//
// WHY THIS IS ITS OWN HEADER. The springs, the scope resolution and the feel are three different
// questions. WeaponRecoil.cpp owns "how does a spring advance", WeaponRecoilScope.h owns "whose
// numbers does this shot use", and this owns "what do those numbers taste like". Keeping the
// profiles here means a retune is a data edit that a test can drive directly, rather than
// constants buried in an anonymous namespace next to the integrator.
//
// ---- the two styles -----------------------------------------------------------------------
//
// MW2022 is now derived from data too, from the MW22 pack (Seulyy/ARC9-MW22,
// lua/weapons/arc9_mw22_*.lua). Against MW2019 the pack really does differ, so the choice is
// worth keeping rather than collapsing to one style:
//
//   * it OVERRIDES the springs, which MW2019 leaves at ARC9's defaults -- the M4 sets
//     VisualRecoilDampingConst 80 (vs 120) and VisualRecoilSpringPunchDamping 11 (vs 6). A
//     softer spring with heavier damping: the gun moves further and comes back slower and
//     lazier, where MW2019 snaps back;
//   * more barrel roll -- VisualRecoilRoll 50 on the M4 and the FSS SMG, against 35 on the
//     MW2019 Kilo;
//   * more lateral -- VisualRecoilSide 0.5 against 0.3;
//   * slightly less punch -- 1.5 against 1.7.
//
// So: MW2022 is looser, wider and rollier; MW2019 is tighter and more vertical.
//
// MW2019 is the default, and is derived from real data rather than taste. Sources:
//   * ARC9 base visual recoil -- HaodongMo/ARC-9, lua/weapons/arc9_base/{sh_recoil,shared}.lua
//   * MW2019 weapon values    -- CurlySparkle/ARC9-COD2019, lua/weapons/arc9_cod2019_*.lua
//
// What that data actually says, and what each point changed here:
//
//  1. ONE SPRING CONSTANT SET FOR EVERY AXIS. ARC9 integrates the position vector and the angle
//     vector each with a single (kQuad = VisualRecoilDampingConst 120, kMag =
//     VisualRecoilSpringMagnitude 1, damp = VisualRecoilSpringPunchDamping 6) plus a shared
//     quadratic drag of 0.5 -- and the return force uses the VECTOR magnitude, not per-axis
//     values. Our per-channel stagger let the six channels peak and settle at different rates,
//     which desynchronises the composite: the gun is still rolling after it has finished
//     climbing. That incoherence is the single most likely source of "wonky", so MW2019 runs
//     ARC9's uniform constants.
//
//  2. POSITION IS PUNCH-DOMINATED; POSITIONAL RISE IS ~NOTHING. ARC9 applies
//     `pos -= (0, punch, up * VisualRecoilPositionBumpUp) * fake` with PositionBumpUp = 0.08,
//     so the gun's vertical TRANSLATION is ~8% of its vertical TILT. Our old profiles gave the
//     up channel a third of the back channel. MW2019 drops it to near zero: the muzzle rises by
//     ROTATING about the pivot, not by sliding up the screen.
//
//  3. ROLL IS RANDOM-SIGNED EVERY SHOT, AND IT IS BIG. ARC9 computes
//     `roll = VisualRecoilRoll * SharedRandom(-1, 1) * 0.1`, i.e. an independent coin flip per
//     shot -- it never keeps a lean. The MW2019 pack sets VisualRecoilRoll = 35 on the Kilo 141
//     against an ARC9 base default of 0.23, and after the viewmodel's own x2.5 and the extra
//     x0.3 applied to roll it lands around 2.6 deg -- roughly 2.5x the pitch term. So MW2019 is
//     NOT a low-roll style; it is a random-roll style. Our persistent lean with a 45% flip
//     chance is what reads as the gun shuffling, and it is replaced for roll by ARC9's coin
//     flip. Side/yaw keep a lean (they follow the recoil pattern in ARC9) but flip far less.
//
//     The table did not actually carry that ratio until 2026-09-21: roll sat at 0.93x pitch,
//     i.e. BELOW the climb it is supposed to be two and a half times. Degree for degree those
//     two channels do not read the same, which is why the shortfall was invisible in the
//     numbers and obvious on screen. Pitch swings a muzzle that is ten to twenty units out in
//     front of the eye, so a few degrees moves it a long way across the frame; roll twists the
//     weapon about its own length, where nothing has any lever at all. Matching them in degrees
//     produces a gun that climbs and merely tilts -- "rotation that does not rotate". Roll is
//     now 2.5x pitch on every term, as this note always said it should be, and yaw is roughly
//     doubled so the lateral swing is a motion rather than a rounding error.
//
//  4. THE MW2019 PACK PUTS NO RECOIL ON THE CAMERA. Its base sets `ViewRecoil = false` with
//     `PhysicalVisualRecoil = true`: the weapon moves, the view does not get thrown. Our Camera
//     Kick default of 20% is therefore already generous for this style; the card documents it.
//
// Amplitudes below are the ARC9/MW2019 RATIOS carried into our units and scaled so a first shot
// lands in the same visual range the module already produced -- ARC9's absolute numbers cannot
// transfer directly, because its rotation also runs through VisualRecoilCenter and a chain of
// multipliers with no equivalent here. Every user slider still multiplies on top.

namespace Filmmaker {

enum class RecoilStyle {
	Mw2019 = 0, // ARC9/MW19-derived: coherent springs, punch-led position, random roll -- default
	Mw2022 = 1, // MW22 pack: softer springs, heavier damping, more roll and side
};

// One spring channel's reference constants. Indices: 0=back 1=up 2=side 3=muzzlePitch 4=yaw
// 5=barrelRoll. Positions are world units, rotations degrees.
struct RecoilChannelRef {
	double instant; // per-shot offset add -- the punch, visible the SAME frame
	double kick;    // per-shot velocity add -- carry-through and overshoot
	double kQuad;   // quadratic spring constant (hard yank when far)
	double kMag;    // constant-magnitude return pull (kills the asymptotic tail)
	double damp;    // linear velocity damping, 1/s (lower = slower settle)
	double cap;     // accumulation cap: impulses scale toward 0 as |offset| approaches this
};

struct RecoilStyleProfile {
	RecoilChannelRef channel[6];
	// Chance per shot that the side lean flips. Low = a burst keeps one lean and reads as a
	// single motion; high = the gun shuffles left/right shot to shot.
	double flipChance;
	// Magnitude jitter applied to the SIGNED channels only (side/yaw/roll), as a half-width:
	// 0.12 means 0.88x..1.12x. The dominant channels (back, up, muzzle pitch) are never jittered,
	// so the core punch repeats exactly shot to shot.
	double jitter;
	// Barrel roll takes an independent coin flip every shot instead of following the side lean.
	// This is ARC9's own behaviour (`VisualRecoilRoll * SharedRandom(-1, 1)`) and is what makes
	// a burst read as the gun torquing in the hands rather than leaning one way and staying.
	bool rollRandomSign;
};

// MW2019: the default. Coherent (uniform) springs, punch-led position, prominent random roll.
inline const RecoilStyleProfile& RecoilStyleMw2019() {
	// kQuad/kMag/damp are ARC9's own defaults, identical on every channel (see note 1 above).
	// Only the per-shot amplitudes and the accumulation caps differ between channels.
	static const RecoilStyleProfile k = {
		{
			//  instant  kick    kQuad  kMag  damp   cap
			{ 1.55,  34.0, 120.0, 1.0, 6.0,  3.0 }, // 0 back         punch dominates position
			{ 0.09,   4.0, 120.0, 1.0, 6.0,  0.5 }, // 1 up           ~8% of the tilt (BumpUp 0.08)
			{ 0.26,  11.0, 120.0, 1.0, 6.0,  1.0 }, // 2 side
			{ 2.05,  58.0, 120.0, 1.0, 6.0,  6.6 }, // 3 muzzle pitch the climb
			{ 1.40,  44.0, 120.0, 1.0, 6.0,  4.8 }, // 4 yaw          the lateral swing
			{ 5.15, 145.0, 120.0, 1.0, 6.0, 16.5 }, // 5 barrel roll  2.5x pitch, RANDOM sign
		},
		0.14,  // flipChance -- side/yaw keep a lean far longer than MW2022
		0.14,  // jitter     -- +/-14%
		true,  // rollRandomSign -- ARC9's per-shot coin flip
	};
	return k;
}

// MW2022: looser and rollier. Springs are the pack's own overrides (kQuad 80, damp 11) rather
// than ARC9's defaults, which is the single biggest reason it feels different from MW2019.
inline const RecoilStyleProfile& RecoilStyleMw2022() {
	static const RecoilStyleProfile k = {
		{
			//  instant  kick    kQuad  kMag  damp   cap
			{ 1.35,  30.0,  80.0, 1.0, 11.0,  3.0 }, // 0 back         slightly less punch than MW19
			{ 0.09,   4.0,  80.0, 1.0, 11.0,  0.5 }, // 1 up           still ~8% of the tilt
			{ 0.43,  15.0,  80.0, 1.0, 11.0,  1.4 }, // 2 side         Side 0.5 vs MW19's 0.3
			{ 2.05,  55.0,  80.0, 1.0, 11.0,  6.6 }, // 3 muzzle pitch Up 0.4, same as MW19
			{ 2.10,  52.0,  80.0, 1.0, 11.0,  5.6 }, // 4 yaw          follows the wider side
			{ 7.35, 185.0,  80.0, 1.0, 11.0, 23.6 }, // 5 barrel roll  Roll 50 vs MW19's 35
		},
		0.14, // flipChance
		0.14, // jitter -- +/-14%
		true, // rollRandomSign -- ARC9's per-shot coin flip, same as MW2019
	};
	return k;
}

// WHERE THE HINGE SITS, as a ratio of the Pivot distance.
//
// ARC9's VisualRecoilCenter is a VECTOR -- Vector(2, 4, 2), commented "where your hand is" -- not
// a forward distance. Ours was forward-only, which put the hinge on the view axis: dead centre of
// the screen, nowhere near the weapon. That is why the gun swept oddly and why the pivot orb drew
// over the crosshair instead of on the gun.
//
// These carry ARC9's intent without inheriting its viewmodel-space units, which do not transfer:
// the hinge is placed relative to the Pivot distance, out to the right and down, which is where a
// CS2 viewmodel's grip actually is. One slider still controls the whole thing.
// FIXED offsets, deliberately not a fraction of Pivot. Scaling them WITH Pivot was the bug that
// made the marker look inert: the hinge moved diagonally away from the eye but its screen
// position never changed, because right/forward stayed in constant proportion. Holding them
// fixed while Pivot drives FORWARD slides the hinge ALONG THE BARREL -- small Pivot puts it at
// the receiver, large Pivot out at the muzzle -- which is what the slider should feel like.
// The right/up pair that used to live here as tunable globals now belongs to the per-weapon
// pivot store (WeaponPivot.h), which carries a full 3D hinge per class and per owner. They were
// left behind as a console knob nothing read any more -- `pivotorb offset` moved numbers that
// had no consumer, which is worse than having no knob at all.

inline const RecoilStyleProfile& RecoilStyleProfileFor(RecoilStyle style) {
	return style == RecoilStyle::Mw2022 ? RecoilStyleMw2022() : RecoilStyleMw2019();
}

// The names the operator sees. Deliberately NOT the MW build years the numbers were derived
// from: those are provenance for whoever retunes the tables above, not a promise to the user
// that this is Modern Warfare, and "MW 2019 / MW 2022" in the card read as a version picker for
// something the tool does not ship. The enum keeps the derivation in its name; the UI says
// which of the two feels you are on and what it feels like.
inline const char* RecoilStyleName(RecoilStyle style) {
	return style == RecoilStyle::Mw2022 ? "style2" : "style1";
}

// Parses a style token; returns false (leaving `out` untouched) for anything unrecognised, so a
// typo at the console cannot silently reset the feel. The mw2019/mw2022 spellings stay accepted
// so anything already scripted against them keeps working.
inline bool RecoilStyleParse(const char* text, RecoilStyle& out) {
	if (!text || !*text) return false;
	auto eq = [text](const char* other) {
		const char* a = text; const char* b = other;
		for (;; ++a, ++b) {
			char ca = *a, cb = *b;
			if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
			if (ca != cb) return false;
			if (!ca) return true;
		}
	};
	if (eq("style1") || eq("1") || eq("mw2019") || eq("2019")) { out = RecoilStyle::Mw2019; return true; }
	if (eq("style2") || eq("2") || eq("mw2022") || eq("2022")) { out = RecoilStyle::Mw2022; return true; }
	return false;
}

} // namespace Filmmaker
