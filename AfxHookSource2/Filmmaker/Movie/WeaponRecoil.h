#pragma once

#include "CameraShakeTypes.h"
#include "WeaponRecoilScope.h"
#include "WeaponRecoilPresets.h"
#include "WeaponRecoilStyles.h"

#include <atomic>
#include <sstream>
#include <string>
#include <vector>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

class JsonBuilder;
struct JsonValue;

// Weapon Recoil: heavily exaggerated PROCEDURAL viewmodel recoil (Modern Warfare II 2022
// style). The weapon punches back toward the camera, the muzzle pitches up, the gun shifts
// rolls around the barrel axis, then returns. Punch is the positional kick (back/up/side);
// rotation is muzzle pitch, yaw, and barrel roll. Every channel springs with overshoot.
// During automatic fire the impulses stack because each shot lands before the previous one
// has recovered.
//
// RecoilShake -- the old screen-shake module this used to sit beside -- was REMOVED
// 2026-09-01: it kept shaking the camera and the gun with Weapon Recoil switched off.
// viewmodel through the CalcViewModelView input hook (ViewFxVm), so CS2's own firing
// animation composes on top instead of being replaced or overwritten.
//
// Model: six storage channels -- position (back/up/side) + rotation (muzzle pitch, yaw, barrel
// roll) -- following the spring design of ARC9's visual recoil (the GMod MW-style weapon
// base, github.com/HaodongMo/ARC-9 sh_recoil.lua), which is the closest open implementation
// of the MWII feel:
//
//     on shot:  offset += instantKick            (the punch is VISIBLE THE SAME FRAME)
//               velocity += velocityKick         (carry-through + overshoot)
//               both scaled by (1 - |offset|/cap)   (bounded stacking: auto fire converges
//                                                    to a rhythmic bounce, never mush)
//     per step: accel = -offset*|offset|*kQuad   (quadratic spring: yanks hard when far,
//               - sign(offset)*kMag                gentle near rest -- the crisp settle)
//               - vel*damp - vel*|vel|*dragC     (linear + quadratic drag)
//
// The channels still peak/settle at staggered rates (back snappiest, barrel roll loosest);
// the stagger produces the chaotic composite -- there is NO random screenshake. Rotation is
// applied about a virtual pivot ~`pivot` units in front of the camera (ARC9's
// VisualRecoilCenter): the muzzle climbs while the grip stays planted, instead of the whole
// gun sweeping across the screen.
//
// Integration runs on the demo clock via CameraShakeStack's ShakeInput and is substepped at
// a fixed rate, so the same demo renders identically at any frame rate. A `test` kick on a
// PAUSED demo animates on the wall clock instead (ShakeInput.wallDt) so tuning never needs
// playback; real shots always use demo time. The only randomness (side/roll direction and
// magnitude jitter) comes from a fixed-seed RNG that reseeds on Reset, keeping exports
// reproducible.
class WeaponRecoil {
public:
	// Back to the constructed SETTINGS defaults -- distinct from Reset(), which clears the live
	// spring state and is called on every enable and every fire. The store persists the state
	// (WriteState/LoadState), but the card's Reset still comes here: it also clears the
	// per-weapon overrides and the edit scope, which a group reset has no notion of.
	void ResetSettings();
	bool SettingsAreDefault() const;

	void SetEnabled(bool on);
	bool Enabled() const { return m_enabled; }

	// 0-150%, 100% = the reference tuning below.
	void SetAmount(double percent);    // master impulse scale (everything)
	void SetPunch(double percent);     // positional punch (back/up/side)
	void SetRotation(double percent);  // rotation channels only (pitch/yaw/roll)
	double Amount() const { return m_editView.amount; }
	double Punch() const { return m_editView.punch; }
	double Rotation() const { return m_editView.rotation; }

	// 0-100% each: how much of the weapon's kick the CAMERA also receives, split by channel so
	// the screen shake can be shaped rather than scaled as one lump (MW gives the camera roughly
	// 15-30% of the rotation, and much less of the position).
	void SetCameraRot(double percent);
	void SetCameraPos(double percent);
	void SetCameraRoll(double percent);
	double CameraRot() const { return m_editView.cameraRot; }
	double CameraPos() const { return m_editView.cameraPos; }
	double CameraRoll() const { return m_editView.cameraRoll; }

	// 25-400%. Snap scales the spring return forces (higher = harder yank back to rest);
	// recovery scales damping relative to that (lower = looser, more overshoot and wobble).
	void SetSnap(double percent);
	void SetRecovery(double percent);
	double Snap() const { return m_editView.snap; }
	double Recovery() const { return m_editView.recovery; }

	// Which reference feel the springs run with. Global, not per-scope: it selects the channel
	// table itself, and the per-weapon tunings are multipliers ON TOP of whichever table is
	// active -- so switching style rescales every weapon rather than discarding their settings.
	void SetStyle(RecoilStyle style);
	RecoilStyle Style() const { return m_style; }

	// 0-30 world units: distance of the rotation pivot in front of the camera (ARC9's
	// VisualRecoilCenter). 0 rotates about the camera (whole gun sweeps); ~10 puts the pivot
	// near the grip so the muzzle climbs while the stock stays planted.
	void SetPivot(double units);
	double Pivot() const { return m_editView.pivot; }

	// ---- scope ------------------------------------------------------------------------
	//
	// Which weapons the knobs above are editing. They address the SELECTED scope rather than a
	// second parallel set of controls, so every existing command and the whole Panorama card
	// keep working unchanged -- picking a scope just repoints them.
	//
	// Selecting a scope with no override yet shows what it INHERITS (see RecoilInheritedTuning),
	// and the override is not created until a knob actually moves, seeded with exactly those
	// values. So opening "AK-47" and closing it again cannot change how the AK already felt --
	// which matters, because the alternative is a UI that silently pins every weapon you look at.
	void SetEditScope(RecoilScope scope, const char* key);
	RecoilScope EditScope() const { return m_editScope; }
	const std::string& EditKey() const { return m_editKey; }

	// Drop the override for the selected scope, so it inherits again. Returns false when there
	// was none (or the selection is All, whose tuning is the base -- ResetSettings clears that).
	bool ClearEditOverride();
	// Drop every override, leaving only the All tuning. The base tuning is untouched.
	void ClearOverrides();
	int OverrideCount() const { return (int)m_overrides.size(); }

	// Scope selection + the full override list, for the CONFIG card.
	void AppendScopeJson(std::ostringstream& o) const;

	// Named tuning snapshots. A preset restores the selected scope before writing its values, so
	// "Heavy Rifles" cannot accidentally be applied to whichever scope happened to be open.
	bool SavePreset(const char* name);
	bool ApplyPreset(const char* name);
	bool DeletePreset(const char* name);
	void WritePresets(JsonBuilder& b) const;
	void LoadPresets(const JsonValue* array);

	// The live settings (enabled, style, the All tuning and every per-weapon override), saved by
	// the settings store so they survive a restart like the rest of the CAMERA tab.
	void WriteState(JsonBuilder& b) const;
	void LoadState(const JsonValue* object);
	const std::vector<WeaponRecoilPreset>& Presets() const { return m_presets.Items(); }

	// The gun the spectated player is holding, as a recoil-table needle ("ak47"), or "" when
	// nothing recognisable resolves. Falls back to the last gun that fired, because the
	// spectated-POV reading flickers to -1 while the CONFIG panel is open -- the one screen this
	// is read from.
	std::string LiveWeapon() const;

	// Which gun a Test Shot fires when the operator did not name one. ALWAYS one the selected
	// scope covers: a fixed weapon here means pressing Test Shot while editing "pistols"
	// resolves some other category's tuning, and the override you just moved does nothing.
	std::string TestWeapon() const;

	// A shot from the spectated player. Called by CameraShakeStack after the userid filter,
	// gun-only filter, and duplicate suppression. `weapon` is the event's weapon field and
	// selects the per-weapon impulse preset ("" = default rifle).
	void OnFire(const char* weapon);

	// Console "test": same as OnFire, but also arms the wall-clock so the kick animates on a
	// PAUSED demo. The wall clock disarms the moment real demo time advances or the springs
	// settle, so playback determinism is untouched.
	void TestFire(const char* weapon);

	// Advances the six springs on the demo clock and returns the CAMERA's share of the kick
	// (added into the stack's summed pose). The full-strength weapon transform is cached for
	// WeaponPose(). Same call discipline as the other modules: dt == 0 holds, fresh resets.
	ShakePose Advance(const ShakeInput& in);

	// The weapon transform for this frame, ready for the CalcViewModelView input hook.
	// Position is in the camera basis (world units); angles are ENGINE convention degrees to
	// be ADDED to the viewmodel eye angles (+pitch = down, +yaw = left, +roll = clockwise) --
	// muzzle-up therefore comes out negative in `pitch`. All-zero at rest.
	void WeaponPose(double& fwd, double& right, double& up,
		double& pitch, double& yaw, double& roll) const;

	void Reset();

	void AppendStateJson(std::ostringstream& o) const;
	std::string DebugStateJson() const;

private:
	struct Spring {
		double offset = 0.0;
		double vel = 0.0;
	};

	// Materialises the override for the selected scope on first write (seeded from m_editView,
	// i.e. what it was already inheriting) and returns the tuning the knobs should write to.
	RecoilTuning& EditTuning();
	// Re-reads the edit view and the active tuning after anything changed them.
	void AfterEdit();

	bool m_enabled = false;
	RecoilStyle m_style = RecoilStyle::Mw2019;

	RecoilTuning m_all;                     // the All-weapons tuning: the base every scope inherits
	std::vector<RecoilOverride> m_overrides;
	RecoilScope m_editScope = RecoilScope::All;
	std::string m_editKey;
	RecoilTuning m_editView;                // what the knobs read back for the selected scope
	WeaponRecoilPresetBook m_presets;       // persistent named snapshots; live recoil stays transient

	// What the springs are running with. The impulse knobs are applied PER SHOT from that
	// shot's own resolved tuning, but snap/recovery/pivot/camera drive the shared integration
	// and cannot differ per shot -- there is one set of springs -- so they come from the most
	// recent shot. Only one gun fires at a time, so in practice this is simply "the gun in use".
	RecoilTuning m_active;
	std::string m_lastWeapon; // the gun m_active was resolved from; "" until the first shot

	// Last live weapon that resolved, cached because the reading is not stable: the spectated
	// POV flickers to -1 while the CONFIG panel is open, so an uncached lookup would blink the
	// card's "in use" readout and its scope preselect on and off every other frame. Written only
	// from LiveWeapon(), which is main-thread only (CONFIG state build and console commands).
	mutable std::string m_liveWeapon;

	// Queued shots. OnFire arrives on the game-event thread and Advance consumes on the
	// view-setup path, so this is a single-producer/single-consumer ring: the slot is filled
	// with a plain write and PUBLISHED by the release-store of the write index, which the
	// consumer picks up with an acquire load.
	//
	// It carries the weapon TOKEN, not a resolved tuning, on purpose. Resolving needs the
	// override vector, and reading a std::vector on the event thread while a console command or
	// the CONFIG card edits it on the main thread is a crash rather than the tolerable torn
	// double the old code risked. Advance resolves instead, on the same thread that owns the
	// vector.
	//
	// This also replaces a single shared multiplier atomic, which gave EVERY shot queued between
	// two frames the last one's weapon -- wrong the moment a spray spans a frame boundary during
	// a weapon switch. 32 slots is over a second of the fastest cyclic rate in the game; a burst
	// longer than one frame can hold overwrites the oldest, which is the right thing to drop.
	static const int kPendingRing = 32;
	struct PendingShot { char weapon[32]; };
	PendingShot m_pending[kPendingRing] = {};
	std::atomic<unsigned> m_pendingWrite{ 0 }; // producer: OnFire
	std::atomic<unsigned> m_pendingRead{ 0 };  // consumer: Advance

	int PendingCount() const {
		return (int)(m_pendingWrite.load(std::memory_order_acquire)
			- m_pendingRead.load(std::memory_order_relaxed));
	}

	// Springs, indexed by channel: 0=back 1=up 2=side 3=muzzlePitch 4=yaw 5=roll.
	Spring m_spring[6];

	// Deterministic per-shot direction state: the side kick keeps its lean and occasionally
	// flips; yaw follows the side, roll cants the opposite way (gun leans off its shifted side).
	double m_sideSign = 1.0;

	// Wall-clock preview for `test` kicks on a paused demo. Armed by TestFire, disarmed as
	// soon as demo time advances or everything settles.
	bool m_testClock = false;

	unsigned long long m_rngState = 0;
	double Rand01();

	unsigned long long m_dbgShots = 0;
	unsigned long long m_dbgResets = 0;
};

WeaponRecoil& WeaponRecoilRef();

// Console entry: "mirv_filmmaker weaponrecoil ...". Forwarded from FilmmakerCommand.cpp.
void WeaponRecoil_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
