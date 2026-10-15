#pragma once

// LIVE smoke-colour re-write, on top of the existing creation-time hook in MirvColors.cpp.
//
// WHY THIS EXISTS: MirvColors' `new_setSmokeProps` detour writes m_vSmokeColor when the game
// applies smoke properties -- i.e. at CREATION. That is the correct place for it and it works,
// but it means changing the colour only affects the NEXT smoke. For a filmmaking tool, where
// you scrub to a moment that already has smoke on screen and want to try colours against that
// exact frame, "wait for the next one" is not usable.
//
// This module re-writes C_SmokeGrenadeProjectile::m_vSmokeColor on every live smoke projectile
// each main-thread frame, which is the approach the reference research takes
// (reference/coding help/smoke/607639-change-smoke-color.md).
//
// WHETHER THAT ACTUALLY RECOLOURS AN ALREADY-DEPLOYED SMOKE IS THE OPEN QUESTION -- CS2 smoke is
// a volumetric voxel effect whose colour may be baked into the volume at spawn. If the per-frame
// write turns out not to change existing smoke, this module reports that plainly rather than
// pretending; see Docs/effects/smoke.md.
//
// It deliberately does NOT replace the MirvColors hook: creation-time is still the right moment
// for newly thrown smoke, and this only adds the live path.

#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Main/UI thread, once per frame from Filmmaker::RunMainThreadFrame. Cheap no-op when live
// re-write is disabled.
void SmokeFx_RunFrame();

// Live re-write on/off. Default ON: it is the behaviour a filmmaker expects, and it is inert
// unless a smoke colour is actually set in MirvColors.
void SmokeFx_SetLive(bool on);
bool SmokeFx_Live();

// Diagnostics for `mirv_filmmaker world health` / the smoke command.
int  SmokeFx_ProjectileCount();   // live smoke projectiles seen on the last frame
bool SmokeFx_Available(std::string* outReason);

// RE-TRIGGER already-deployed smoke so it picks up the current colour.
//
// MEASURED 2026-08-04: CS2 bakes the smoke colour into the voxel volume when the volume is
// CREATED. Writing m_vSmokeColor every frame on a deployed smoke does nothing at all (confirmed
// in-game: a live change only affected the next smoke thrown). So making a colour change visible
// on smoke that is already on screen requires restarting the volume rather than recolouring it.
//
// This rewinds m_nSmokeEffectTickBegin to the current demo tick, which makes the client treat
// the volume as newly spawned and rebuild it -- with the colour now in place. It deliberately
// does NOT touch m_bDidSmokeEffect: the reference research reports a crash from clearing that
// field (reference/coding help/smoke/607639, post 12), while the same thread reports the tick
// field being safe to write.
//
// Returns how many smoke volumes were re-triggered. The smoke restarts its lifetime, so it will
// linger longer than it would have -- acceptable for a filmmaking tool, and stated in the UI.
int SmokeFx_Reapply();

void SmokeFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
