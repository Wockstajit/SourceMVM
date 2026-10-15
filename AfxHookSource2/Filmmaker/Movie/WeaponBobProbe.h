#pragma once

// Weapon Bob / Reload Impact -- PHASE 0 RESEARCH PROBE. Read-only. Writes nothing to the engine
// and changes nothing on screen; it exists purely to answer the questions a weapon-bob and
// reload-impact design cannot be written without, per the instrumentation-first rule.
//
// It is deliberately its OWN module rather than a few lines bolted into ViewFx or
// CameraShakeStack: it is a separable, throwaway job with its own lifetime (on for one demo
// pass, then deleted or promoted), and folding it into either of those would leave probe code
// tangled in a shipping effect.
//
// The five questions, and the line that answers each:
//
//   1. Does `weapon_reload` survive into demo playback at all?          -> bob.event
//   2. Does `player_footstep` arrive often enough to phase-lock a bob?  -> bob.event
//   3. Is C_CSWeaponBase::m_bInReload readable + true during a reload?  -> bob.reload
//   4. Does the FIRST-PERSON weapon's muzzle attachment actually move
//      per render frame (route B: derive reload key points from the
//      animation instead of an authored per-weapon table)?             -> bob.frame (dCam)
//   5. Does CS2 apply its own viewmodel bob we should scale rather
//      than add to? (dCam oscillating with speed, all our modules off)  -> bob.frame
//
// Everything is SEH-guarded and gated on both `bobprobe on` AND MvmDebugLog_Active(), so it is
// exactly zero cost in a normal session.

namespace advancedfx { class ICommandArgs; }
namespace SOURCESDK { namespace CS2 { class CGameEvent; } }

namespace Filmmaker {

// Main-thread per-frame sampler. No-op unless armed and a log is open.
void WeaponBobProbe_RunMainThreadFrame();

// Game-event tap: records weapon_reload / player_footstep / weapon_fire arrival, whether the
// event belonged to the spectated player, and the demo tick. Called from GameEvents.cpp on both
// the server and client-side paths (demo events arrive on the client-side one).
void WeaponBobProbe_OnGameEvent(SOURCESDK::CS2::CGameEvent* event);

// Console: "mirv_filmmaker bobprobe on|off|state".
void WeaponBobProbe_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
