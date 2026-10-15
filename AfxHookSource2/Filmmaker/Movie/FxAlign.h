#pragma once

// Modern muzzle-FX alignment probe ("mirv_filmmaker fx align ..."): measures, in Source
// units, how far the Modern pack's muzzle flash / barrel smoke actually spawn from the
// spectated player's weapon muzzle attachment. Replaces the screenshot
// pixel audit (Automation/verify/audit-modern-muzzle-alignment.py) as the pass/fail
// metric -- pixels vary with FOV/resolution/flash timing; this measures world distance.
//
// HOW: ParticleFx's Hook_CreateBody notes every Modern muzzle-related swap together with
// the collection pointer the engine's create returned (any thread, mutex-queued). The
// main-thread pump then, one frame later (after the engine's caller has set the system's
// control points), resolves two points:
//   reference = the muzzle attachment world transform -- the first-person viewmodel
//     weapon entity when resolvable (these effects are m_bViewModelEffect), else the
//     spectated pawn's held world weapon, attachments "muzzle" then "muzzle_flash",
//     else the weapon origin (low-confidence, flagged in the sample).
//   spawn = an SEH-guarded scan of the created collection's first bytes for a float3
//     within scan range of the muzzle (control points / emitter origin live there; the
//     nearest hit is the spawn proxy, method "cp-scan"). When the scan finds nothing
//     (freed instance, viewmodel-local storage, null return) the sample falls back to
//     the pack's CONFIGURED C_INIT_PositionOffset mean rotated into world space
//     (method "config-offset" -- measures only the authored offset, not engine binding;
//     the method is recorded per sample so the report can weigh them differently).
//
// Samples append as NDJSON to %APPDATA%\HLAE\fx_align.jsonl (one object per event, the
// schema in the task spec: weapon_class/effect/raw/target/demo_tick/muzzle_world/
// spawn_world/distance_units/local_offset/pass) and aggregate in memory for
// "fx align report". Threading follows the repo model: hook side only queues, all
// engine/entity access happens on the main-thread pump.

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Cheap atomic gate for the hook (skip all work while the probe is off).
bool FxAlign_Enabled();

// Called from Hook_CreateBody (any thread) AFTER the original create returned, only for
// swapped ('>') creations. rawLow/targetLow are the lowercased vanilla + swap-target
// resource paths; instance is whatever the engine's create returned (may be null).
void FxAlign_OnSwapCreate(const char* rawLow, const char* targetLow, void* instance, int demoTick);

// Main-thread pump (driven from ParticleFx::PumpMainThread): drains the pending queue,
// resolves muzzle + spawn, appends NDJSON, updates aggregates.
void FxAlign_PumpMainThread();

// Handles "mirv_filmmaker fx align ..." (argv[2] == "align"); dispatched from
// ParticleFx_RunCommand.
void FxAlign_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
