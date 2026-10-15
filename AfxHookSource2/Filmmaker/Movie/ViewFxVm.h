#pragma once

#include <cstddef>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Viewmodel steering and camera-motion compensation. The production path adjusts the temporary
// CalcViewModelView inputs before CS2 constructs the animated hands, weapon, and attachments.
// Legacy post-animation scene-node write sites remain available through vmtest for diagnostics
// and as a fallback when the CalcViewModelView caller cannot be resolved.
//
// Sites:
//   0 = off
//   1 = inside ViewModel.cpp's setViewmodel hook, right after the original returns (we are
//       INSIDE the engine's vm-calc; cheap, but the caller may overwrite the node afterwards)
//   2 = main-thread FrameStageNotify pump (Filmmaker::RunMainThreadFrame)
//   3 = render-thread present pump (Filmmaker::RunFrame)
//   4 = detour of the CALLER of the setViewmodel helper (CS2's CalcViewModelView itself),
//       post-original -- the strongest timing bet; installed lazily on first selection.
// Fields:
//   1 = CGameSceneNode::m_angRotation (local QAngle, schema-resolved)
//   2 = CGameSceneNode::m_nodeToWorld orientation quaternion (offset auto-detected by norm)
//   3 = both

// Site-1 tap: called by ViewModel.cpp's setViewmodel hook after the original returns.
void ViewFxVm_OnViewmodelCalc();

// Per-frame pumps for sites 2/3.
void ViewFxVm_MainPump();
void ViewFxVm_RenderPump();

// ViewModel.cpp shares the resolved helper address here so site 4 can locate its caller.
void ViewFxVm_NoteViewmodelHelper(size_t helperAddr);

// Deadzone integration: ViewFx pushes the current free-aim (degrees, where the TRUE aim sits
// relative to the lagged camera) every tracker step. Applied as a real rotation when a
// working site/field is configured AND mode is "deadzone".
void ViewFxVm_SetFreeAim(double pitchDeg, double yawDeg);

// Tracks the current POV for viewmodel compensation.
void ViewFxVm_SetPovContext(bool active, int pawnIndex);

// Lazily installs the CalcViewModelView-input hook used by camera compensation.
bool ViewFxVm_EnsureCameraCompensationHook();

// Camera motion to remove from the viewmodel for the current frame. Position is a world-space
// delta; angles are degrees. The caller replaces this complete value every view-setup frame.
void ViewFxVm_SetCameraCompensation(double x, double y, double z,
	double pitchDeg, double yawDeg, double rollDeg);

// Weapon Recoil kick, ADDED to the CalcViewModelView inputs (compensation above is
// subtracted). Position is a world-space delta, angles engine-convention degrees. The engine
// then builds the complete animated arms/weapon from the kicked eye pose, so the firing
// animation composes on top of the offset. CameraShakeStack replaces this complete value
// every view-setup frame (zeroed while gated or disabled).
void ViewFxVm_SetWeaponKick(double x, double y, double z,
	double pitchDeg, double yawDeg, double rollDeg);

// True while vm rotation is engaged for the deadzone -- lets ViewFx suppress the fake
// translation hint (rotation replaces it).
bool ViewFxVm_RotationActive();

// Console: handles "mirv_filmmaker viewfx vmtest ..." (argc/args offset like ViewFx_RunCommand;
// args->ArgV(2) == "vmtest").
void ViewFxVm_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
