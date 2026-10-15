#pragma once

// WHERE THE VIEWMODEL HINGES -- one store per EFFECT, not one shared between them.
//
// Weapon Recoil and Weapon Bob each get their own independent pivot: their own ALL base, their own
// per-class overrides, their own marker in its own colour. They are tuned for different motions --
// a kick pivots near the shoulder so the muzzle climbs, while a walk cycle often reads better
// hinged further out -- and forcing one number to serve both means every recoil adjustment
// silently retunes the bob and vice versa.
//
// (An earlier version DID share one store, on the reasoning that the hinge is a property of the
// weapon. That was overruled: the operator wants to tune them apart, and two markers in two
// colours is what makes that legible on screen.)
//
// SCOPES, mirroring Weapon Recoil's tuning scopes: an ALL base plus optional per-CLASS
// overrides, where a class override wins. That is what makes a knife possible at all -- see the
// measured anchors below. Each owner carries a full independent copy of that table.
//
// Engine-free and header-only apart from the store's state, so the resolution rule can be unit
// tested directly (Automation/tests/weapon-pivot-tests.cpp).

#include <cstring>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Which EFFECT a pivot belongs to. Two stores, never merged.
enum class WeaponPivotOwner { Recoil, Bob, Count };
constexpr int kWeaponPivotOwnerCount = (int)WeaponPivotOwner::Count;

inline const char* WeaponPivotOwnerName(WeaponPivotOwner o) {
	return o == WeaponPivotOwner::Recoil ? "recoil" : "bob";
}
inline bool WeaponPivotOwnerFromName(const char* name, WeaponPivotOwner& out) {
	if (!name || !*name) return false;
	if (0 == _stricmp(name, "recoil")) { out = WeaponPivotOwner::Recoil; return true; }
	if (0 == _stricmp(name, "bob")) { out = WeaponPivotOwner::Bob; return true; }
	return false;
}

// Marker colours, one per owner, so the two orbs can never be confused on screen. Recoil keeps
// the amber it has always had; bob is cyan, which is far enough away in hue to read instantly
// even when the two hinges sit a couple of units apart.
struct WeaponPivotColour { unsigned char r, g, b; };
inline WeaponPivotColour WeaponPivotOwnerColour(WeaponPivotOwner o) {
	return o == WeaponPivotOwner::Recoil ? WeaponPivotColour{ 255, 190, 60 }
	                                     : WeaponPivotColour{ 60, 200, 255 };
}

// How a weapon is CARRIED. Not the same question as Weapon Recoil's RecoilWeaponType, which
// classifies how a weapon FIRES and therefore has no melee member at all -- a knife never
// recoils, but it very much has a pivot. Utility is the grenades, the bomb, the Zeus and the
// healthshot: carried close, not aimed, so they get their own hinge rather than a gun's.
enum class WeaponPivotClass { Rifle, Smg, Pistol, Shotgun, Sniper, Knife, Utility, Count };

constexpr int kWeaponPivotClassCount = (int)WeaponPivotClass::Count;

// Camera-local units: +fwd is out along the view axis, +right is to the right, +up is up.
// Negative fwd puts the hinge BEHIND the eye, which is correct for a weapon held at the shoulder.
struct WeaponPivotPoint {
	double fwd = 10.0;
	double right = 4.0;
	double up = -3.2;
};

// Names and defaults are INLINE rather than in the .cpp: they are pure data with no engine
// dependency, and the maths unit test needs them without linking the console/store translation
// unit (which pulls in AfxConsole).
inline const char* WeaponPivotClassName(WeaponPivotClass c) {
	switch (c) {
	case WeaponPivotClass::Rifle:   return "rifle";
	case WeaponPivotClass::Smg:     return "smg";
	case WeaponPivotClass::Pistol:  return "pistol";
	case WeaponPivotClass::Shotgun: return "shotgun";
	case WeaponPivotClass::Sniper:  return "sniper";
	case WeaponPivotClass::Knife:   return "knife";
	default:                        return "utility";
	}
}

// Parses a class name; returns false (and leaves `out` alone) for anything unrecognised, so a
// typo in a console command can never silently retune the wrong weapon.
bool WeaponPivotClassFromName(const char* name, WeaponPivotClass& out);

// The measured / derived defaults.
//
// Two entries are MEASURED from the ARC9 Modern Warfare 2019 pack's own `CustomizeRotateAnchor`
// values (GMod's (fwd, left, up) frame, X scaled by 0.75 the way ARC9 scales it):
//
//     M4     Vector( 14, -3,    -4)  ->  10.5 fwd,  3.00 right, -4.0 up
//     knife  Vector(-10, -2.25, -4)  ->  -7.5 fwd,  2.25 right, -4.0 up
//
// The knife's sign flip is the important one: rotating a weapon held at the shoulder about a
// point ten units in FRONT of the eye throws it across the screen. The rest are interpolated by
// how far from the eye the weapon is actually held.
inline WeaponPivotPoint WeaponPivotDefaultFor(WeaponPivotClass c) {
	switch (c) {
	case WeaponPivotClass::Rifle:   return { 10.5, 3.00, -4.0 }; // measured (MW2019 M4)
	case WeaponPivotClass::Smg:     return {  7.0, 2.60, -4.0 };
	case WeaponPivotClass::Pistol:  return {  4.0, 2.25, -3.5 };
	case WeaponPivotClass::Shotgun: return { 11.0, 3.00, -4.0 };
	case WeaponPivotClass::Sniper:  return { 13.0, 3.00, -4.0 };
	case WeaponPivotClass::Knife:   return { -7.5, 2.25, -4.0 }; // measured (MW2019 knife)
	default:                        return {  6.0, 2.50, -4.0 };
	}
}

// Which class a CS2 weapon classname belongs to ("weapon_ak47" -> Rifle).
//
// Melee and utility are matched by NAME here before deferring to Weapon Recoil's classifier,
// because that classifier's enum has no knife member -- it exists to describe how a weapon
// fires, and a knife never does. Everything else defers, so the two features can never disagree
// about what an MP9 is.
WeaponPivotClass WeaponPivotClassOf(const std::string& weaponClassName);

// The store: an ALL base plus per-class overrides.
class WeaponPivotStore {
public:
	WeaponPivotStore();

	// The resolved hinge for a class: its override if one exists, otherwise the ALL base.
	WeaponPivotPoint Resolve(WeaponPivotClass c) const;
	bool HasOverride(WeaponPivotClass c) const;

	const WeaponPivotPoint& All() const { return m_all; }
	void SetAll(const WeaponPivotPoint& p);
	// Weapon Recoil's existing Pivot slider edits only the forward distance, so keep a setter
	// that matches it rather than making every caller build a whole point.
	void SetAllForward(double fwd);

	void SetOverride(WeaponPivotClass c, const WeaponPivotPoint& p);
	void ClearOverride(WeaponPivotClass c);
	void ClearAllOverrides();

	// Back to the measured defaults: ALL to the rifle-ish base, every class to its own entry.
	void ResetToDefaults();
	bool IsDefault() const;

	// Bumped every time the TUNING is actually edited -- a slider drag, a console `set`, a
	// clear, a reset -- and only when a stored number really moves. NOT bumped by NoteActive:
	// the weapon in shot changing is not an edit.
	//
	// It exists because "has the pivot changed?" has two different answers and the marker's
	// auto-show wants the other one. RecoilPivotMarker used to watch the RESOLVED value, so
	// spectating a demo re-armed the flash every time the player swapped rifle -> knife ->
	// pistol (measured: 10.50 -> -7.50 -> 4.00 within a couple of seconds), which in a match
	// demo is more often than the 2.5 s flash lasts. The orb was then permanently lit and the
	// "Show Pivot Marker" pill appeared to do nothing.
	unsigned long long EditSerial() const { return m_editSerial; }

	// Which weapon the spectated player is holding, resolved once per frame by CameraShakeStack
	// and cached so the console, the orb and both effects all agree within a frame.
	void NoteActive(WeaponPivotClass c, const std::string& weaponClassName);
	WeaponPivotClass ActiveClass() const { return m_activeClass; }
	const std::string& ActiveWeapon() const { return m_activeWeapon; }
	WeaponPivotPoint ResolveActive() const { return Resolve(m_activeClass); }

	std::string DebugStateJson() const;
	// `prefix` keys the emitted fields ("pvr" / "pvb"), so both stores can write into the one
	// CONFIG state object without colliding.
	void AppendStateJson(class std::ostringstream& o, const char* prefix) const;

private:
	WeaponPivotPoint m_all;
	WeaponPivotPoint m_class[kWeaponPivotClassCount];
	bool m_hasOverride[kWeaponPivotClassCount] = {};
	WeaponPivotClass m_activeClass = WeaponPivotClass::Rifle;
	std::string m_activeWeapon;
	unsigned long long m_editSerial = 0;
};

// The two stores. There is deliberately no "the" pivot accessor -- every caller has to say which
// effect it means, so a future edit cannot quietly re-merge them by picking the wrong one.
WeaponPivotStore& WeaponPivotRef(WeaponPivotOwner owner);
inline WeaponPivotStore& WeaponPivotRecoilRef() { return WeaponPivotRef(WeaponPivotOwner::Recoil); }
inline WeaponPivotStore& WeaponPivotBobRef() { return WeaponPivotRef(WeaponPivotOwner::Bob); }

// Console: "mirv_filmmaker pivot ...".
void WeaponPivot_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
