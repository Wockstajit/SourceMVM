#include "RecoilPivotMarker.h"

#include "WeaponRecoil.h"          // read-only: is recoil running (the amber orb's owner)
#include "WeaponBob.h"             // read-only: is bob running (the cyan orb's owner)
#include "HudBlackout.h"           // F8 clears every overlay, ours included
#include "../../CampathDrawer.h"   // the world-space gizmo drawer this marker rides on
#include "../Debug/MvmDebugLog.h"
#include "../../../Shared/AfxConsole.h"

#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace Filmmaker {

namespace {

// How long a marker stays up after its Pivot stops being edited.
constexpr double kAutoShowSeconds = 2.5;

double WallSeconds() { return (double)GetTickCount64() * 0.001; }

// Is the EFFECT that owns this orb running? An amber recoil orb hanging in the air with Weapon
// Recoil switched off is exactly the "which one is this?" confusion the two colours exist to
// remove, so an orb is never drawn without its own effect.
bool OwnerEffectEnabled(WeaponPivotOwner owner) {
	return owner == WeaponPivotOwner::Recoil
		? WeaponRecoilRef().Enabled()
		: WeaponBobRef().Enabled();
}

void PushToDrawer(WeaponPivotOwner owner, bool enabled, double x, double y, double z,
	double radius) {
	CCampathDrawer::PivotMarker marker;
	marker.enabled = enabled && radius > 0.0;
	marker.x = (float)x;
	marker.y = (float)y;
	marker.z = (float)z;
	marker.radius = marker.enabled ? (float)radius : 0.0f;
	// The colour is the whole point of the split: the drawer paints whatever it is handed, so
	// the owner-to-colour mapping lives in one place (WeaponPivot.h) and both orbs get it here.
	const WeaponPivotColour c = WeaponPivotOwnerColour(owner);
	marker.r = c.r; marker.g = c.g; marker.b = c.b;
	g_CampathDrawer.PivotMarker_set((int)owner, marker);
}

} // namespace

RecoilPivotMarker& RecoilPivotMarkerRef() {
	static RecoilPivotMarker s_instance;
	return s_instance;
}

bool RecoilPivotMarker::Showing(WeaponPivotOwner o) const {
	if (HudBlackout_Active()) return false;
	if (!OwnerEffectEnabled(o)) return false;
	const MarkerSlot& s = Slot(o);
	if (s.enabled) return true;
	return s.autoShowUntil > 0.0 && WallSeconds() < s.autoShowUntil;
}

void RecoilPivotMarker::NoteEdit(WeaponPivotOwner o, unsigned long long editSerial) {
	MarkerSlot& s = Slot(o);
	if (!s.haveSerial) {
		s.haveSerial = true;
		s.lastEditSerial = editSerial; // first sample: adopt without flashing the marker
		return;
	}
	if (editSerial == s.lastEditSerial) return;
	s.lastEditSerial = editSerial;
	if (s.autoShow) s.autoShowUntil = WallSeconds() + kAutoShowSeconds;
}

void RecoilPivotMarker::NoteWorldPoint(WeaponPivotOwner o,
	double x, double y, double z, double pivotUnits, double radius)
{
	MarkerSlot& s = Slot(o);
	s.x = x; s.y = y; s.z = z;
	s.pivot = pivotUnits;
	s.radius = radius;
	s.haveWorld = true;
}

std::string RecoilPivotMarker::SlotJson(WeaponPivotOwner o) const {
	const MarkerSlot& s = Slot(o);
	const WeaponPivotColour c = WeaponPivotOwnerColour(o);
	std::ostringstream j;
	j << "{\"pinned\":" << (s.enabled ? "true" : "false")
		<< ",\"autoShow\":" << (s.autoShow ? "true" : "false")
		<< ",\"showing\":" << (Showing(o) ? "true" : "false")
		<< ",\"effectOn\":" << (OwnerEffectEnabled(o) ? "true" : "false")
		<< ",\"haveWorld\":" << (s.haveWorld ? "true" : "false")
		<< ",\"world\":[" << (float)s.x << "," << (float)s.y << "," << (float)s.z << "]"
		<< ",\"pivot\":" << (float)s.pivot
		<< ",\"radius\":" << (float)s.radius
		<< ",\"rgb\":[" << (int)c.r << "," << (int)c.g << "," << (int)c.b << "]}";
	return j.str();
}

std::string RecoilPivotMarker::DebugStateJson() const {
	std::ostringstream o;
	o << "{\"recoil\":" << SlotJson(WeaponPivotOwner::Recoil)
		<< ",\"bob\":" << SlotJson(WeaponPivotOwner::Bob)
		<< ",\"minFront\":" << (float)kRecoilPivotMinFrontDistance << "}";
	return o.str();
}

void RecoilPivotMarker_SetWorldPoint(WeaponPivotOwner owner,
	double x, double y, double z, double pivotUnits, double radius,
	unsigned long long editSerial)
{
	RecoilPivotMarker& m = RecoilPivotMarkerRef();
	m.NoteEdit(owner, editSerial);
	m.NoteWorldPoint(owner, x, y, z, pivotUnits, radius);

	// radius <= 0 means the caller could not place the marker at all -- the hinge is at or
	// behind the eye, which has no screen position (see RecoilPivotMarkerMath.h).
	const bool show = m.Showing(owner) && radius > 0.0;
	PushToDrawer(owner, show, x, y, z, radius);

	if (show && MvmDebugLog_Active()) {
		static unsigned s_throttle[kWeaponPivotOwnerCount] = {};
		if (s_throttle[(int)owner]++ % 30 == 0) {
			MvmDebugLog_Linef("recoil.pivot.marker",
				"owner=%s pivot=%.2f world=(%.2f %.2f %.2f) radius=%.2f pinned=%d editSerial=%llu",
				WeaponPivotOwnerName(owner), pivotUnits, x, y, z, radius,
				m.Enabled(owner) ? 1 : 0, editSerial);
		}
	}
}

void RecoilPivotMarker_Clear(WeaponPivotOwner owner) {
	RecoilPivotMarkerRef().NoteCleared(owner);
	PushToDrawer(owner, false, 0.0, 0.0, 0.0, 0.0);
}

void RecoilPivotMarker_ClearAll() {
	for (int i = 0; i < kWeaponPivotOwnerCount; ++i)
		RecoilPivotMarker_Clear((WeaponPivotOwner)i);
}

void RecoilPivotMarker_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	RecoilPivotMarker& m = RecoilPivotMarkerRef();

	// `pivotorb <recoil|bob> <action>`. The owner is REQUIRED and comes first, matching
	// `mirv_filmmaker pivot`: there are two orbs at two different points, and a command that
	// guessed which one you meant would toggle the wrong one about half the time.
	WeaponPivotOwner owner;
	if (!WeaponPivotOwnerFromName(argc >= 3 ? args->ArgV(2) : "", owner)) {
		advancedfx::Message(
			"%s pivotorb recoil|bob [on|off|toggle] - draw a wireframe sphere in the WORLD at the\n"
			"    point that effect rotates the gun around, so its Pivot sliders have something\n"
			"    visible attached to them. Shares the scene's depth; never appears in a recorded\n"
			"    frame. Recoil's orb is amber, bob's is cyan.\n"
			"%s pivotorb recoil|bob hide - hide it immediately, including a running edit flash.\n"
			"%s pivotorb recoil|bob auto [on|off|toggle] - flash it for a few seconds whenever that\n"
			"    Pivot is EDITED (default on). Not when the weapon in shot changes -- that used to\n"
			"    re-arm it constantly and the orb never went away. Needs its own effect enabled.\n"
			"%s pivotorb recoil|bob state - visibility + last published world point.\n",
			cmd, cmd, cmd, cmd);
		advancedfx::Message("[pivotorb][state] %s\n", m.DebugStateJson().c_str());
		return;
	}

	const char* ownerName = WeaponPivotOwnerName(owner);
	const char* a = (argc >= 4) ? args->ArgV(3) : "";

	if (0 == _stricmp(a, "on") || 0 == _stricmp(a, "off") || 0 == _stricmp(a, "toggle")) {
		const bool on = (0 == _stricmp(a, "toggle")) ? !m.Enabled(owner) : (0 == _stricmp(a, "on"));
		m.SetEnabled(owner, on);
		if (!on) PushToDrawer(owner, false, 0.0, 0.0, 0.0, 0.0);
		if (!(argc >= 5 && 0 == _stricmp(args->ArgV(4), "quiet")))
			advancedfx::Message("mirv_filmmaker: %s pivot marker %s%s.\n", ownerName,
				on ? "ON" : "OFF",
				(on && !OwnerEffectEnabled(owner))
					? " (that effect is off, so nothing is drawn yet)" : "");
		return;
	}
	if (0 == _stricmp(a, "hide")) {
		m.Hide(owner);
		PushToDrawer(owner, false, 0.0, 0.0, 0.0, 0.0);
		if (!(argc >= 5 && 0 == _stricmp(args->ArgV(4), "quiet")))
			advancedfx::Message("mirv_filmmaker: %s pivot marker hidden.\n", ownerName);
		return;
	}
	if (0 == _stricmp(a, "auto")) {
		const char* v = (argc >= 5) ? args->ArgV(4) : "toggle";
		const bool on = (0 == _stricmp(v, "toggle")) ? !m.AutoShow(owner)
			: (0 == _stricmp(v, "on") || 0 == _stricmp(v, "1"));
		m.SetAutoShow(owner, on);
		advancedfx::Message("mirv_filmmaker: %s pivot marker auto-show = %s.\n",
			ownerName, on ? "ON" : "off");
		return;
	}
	if (0 == _stricmp(a, "state") || !*a) {
		advancedfx::Message("[pivotorb][state] %s\n", m.DebugStateJson().c_str());
		return;
	}

	advancedfx::Message("%s pivotorb %s on|off|toggle|auto|state (run `%s pivotorb` for details).\n",
		cmd, ownerName, cmd);
}

} // namespace Filmmaker
