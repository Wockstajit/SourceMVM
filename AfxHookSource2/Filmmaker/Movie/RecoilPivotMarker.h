#pragma once

// RecoilPivotMarker: shows the point the viewmodel HINGES about (WeaponPivot.h) as a real object
// in the world, so the Pivot sliders have something visible attached to them in first person.
//
// TWO MARKERS, ONE PER EFFECT. Weapon Recoil and Weapon Bob own separate hinges, so they get
// separate orbs in separate colours -- amber for recoil, cyan for bob (WeaponPivotOwnerColour).
// A single orb could only ever show one of the two, which is worse than showing none: it would
// sit confidently at recoil's hinge while you dragged bob's Forward slider and nothing moved.
// Everything below is therefore indexed by WeaponPivotOwner, including the pin and auto-show
// state, so each card's "Show Pivot Marker" governs only its own orb.
//
// WHY THIS EXISTS AT ALL. Pivot is the one setting whose effect you cannot read off the number.
// It is where the weapon HINGES (ARC9's VisualRecoilCenter, "where your hand is"), and every
// value produces a plausible-looking motion -- what changes is whether the whole gun sweeps
// across the screen or the muzzle moves while the grip stays planted.
//
// WHY IT IS NOT A PANORAMA OVERLAY. It was one first, and that was wrong: the pivot sits a few
// units from the eye, INSIDE the weapon, so a flat disc painted over the finished frame reads as
// a UI dot stuck to the screen rather than a thing in the gun. It has to share the scene's depth.
// So the marker is a world-space wireframe sphere drawn by CCampathDrawer alongside the camera
// gizmos -- which is that class's existing job, and brings correct depth, the additive glow pass,
// and capture suppression (it never lands in a recorded frame) for free.
//
// This module owns only the decision of WHETHER and WHERE. CameraShakeStack hands it the world
// point it already computes per owner, and it pushes a marker to that owner's drawer slot. It
// READS WeaponRecoil / WeaponBob (enabled); nothing writes back -- neither effect knows this
// exists.

#include "WeaponPivot.h"           // WeaponPivotOwner + the per-owner marker colour
#include "RecoilPivotMarkerMath.h" // near-plane escape (why the orb used to vanish up close)

#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// The hinge's WORLD position for this frame, from CameraShakeStack (the only place holding the
// pre-shake camera basis the pivot is defined against). `pivotUnits` is the forward distance that
// produced it, kept for diagnostics; `radius` is the world radius to draw at, which is NOT the
// constant it used to be -- a hinge closer to the eye than the near plane is drawn further out
// and proportionally larger so it survives clipping at the same screen position and size
// (RecoilPivotMarkerMath.h). `editSerial` is the pivot store's edit counter and is what arms the
// auto-show. Pushes straight through to the drawer slot for that owner. Call once per owner
// per frame.
void RecoilPivotMarker_SetWorldPoint(WeaponPivotOwner owner,
	double x, double y, double z, double pivotUnits, double radius,
	unsigned long long editSerial);

// Module off / gated camera mode / no spectated weapon: clear that owner's marker so it cannot
// hang in the world wherever the last good frame left it.
void RecoilPivotMarker_Clear(WeaponPivotOwner owner);
void RecoilPivotMarker_ClearAll();

class RecoilPivotMarker {
public:
	void SetEnabled(WeaponPivotOwner o, bool v) { Slot(o).enabled = v; }
	void Toggle(WeaponPivotOwner o) { SetEnabled(o, !Enabled(o)); }
	bool Enabled(WeaponPivotOwner o) const { return Slot(o).enabled; }

	// Hide NOW: drops the pin AND cancels a running auto-show flash. Switching the pin off alone
	// leaves the 2.5 s edit flash alive, so after a slider drag the orb kept drawing for a moment
	// and the "off" pill looked ignored. Auto-show itself stays armed for the next edit; turn that
	// off with `pivotorb <owner> auto off`.
	void Hide(WeaponPivotOwner o) { Slot(o).enabled = false; Slot(o).autoShowUntil = 0.0; }

	// Auto-show: an orb appears on its own for a few seconds whenever its own Pivot is EDITED, so
	// dragging a slider shows you what you are dragging without a second click. Polled inside
	// SetWorldPoint rather than pushed, so the pivot store keeps no reference to this module.
	//
	// It is armed off the store's EDIT SERIAL, not off the pivot value. Watching the value was
	// the "the pivot marker is always on" bug: the resolved pivot also changes when the weapon in
	// shot changes, and in a match demo a rifle -> knife -> pistol swap re-armed the 2.5 s flash
	// far more often than it could expire. The orb was permanently lit, which made the pill next
	// to it look broken -- switching it off changed nothing visible.
	void SetAutoShow(WeaponPivotOwner o, bool v) { Slot(o).autoShow = v; }
	bool AutoShow(WeaponPivotOwner o) const { return Slot(o).autoShow; }

	// True while THIS owner's marker should be drawn (pinned on, or an auto-show still running,
	// and that owner's effect enabled).
	bool Showing(WeaponPivotOwner o) const;

	// State bookkeeping for the free functions above. Public methods rather than a friend
	// declaration: there are several callers, and a friend list that has to grow with them is the
	// kind of thing that silently rots.
	void NoteEdit(WeaponPivotOwner o, unsigned long long editSerial);
	void NoteWorldPoint(WeaponPivotOwner o, double x, double y, double z, double pivotUnits,
		double radius);
	void NoteCleared(WeaponPivotOwner o) { Slot(o).haveWorld = false; }

	std::string DebugStateJson() const;

private:
	struct MarkerSlot {
		bool enabled = false;  // explicit "keep it on"
		bool autoShow = true;  // flash it when this owner's Pivot is edited

		// The store's edit counter as of the previous frame. haveSerial is an explicit flag so
		// the FIRST sample adopts silently rather than flashing the orb the moment a demo loads.
		unsigned long long lastEditSerial = 0;
		bool haveSerial = false;
		double autoShowUntil = 0.0; // wall-clock seconds; 0 = not showing

		// Last published hinge, for `pivotorb state`.
		double x = 0.0, y = 0.0, z = 0.0, pivot = 0.0;
		double radius = 0.0; // as DRAWN -- see the near-plane escape
		bool haveWorld = false;
	};

	MarkerSlot m_slots[kWeaponPivotOwnerCount];

	MarkerSlot& Slot(WeaponPivotOwner o) {
		const int i = (int)o;
		return m_slots[(i >= 0 && i < kWeaponPivotOwnerCount) ? i : 0];
	}
	const MarkerSlot& Slot(WeaponPivotOwner o) const {
		const int i = (int)o;
		return m_slots[(i >= 0 && i < kWeaponPivotOwnerCount) ? i : 0];
	}

	std::string SlotJson(WeaponPivotOwner o) const;
};

RecoilPivotMarker& RecoilPivotMarkerRef();
void RecoilPivotMarker_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
