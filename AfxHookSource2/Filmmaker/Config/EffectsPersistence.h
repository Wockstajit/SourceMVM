#pragma once

// Does a CONFIG tab's state survive leaving one demo and joining another?
//
// One operator-visible promise, made by an identical "Keep between demos" toggle in each tab's
// footer: keep ON and the tab follows you into the next demo, keep OFF and the next demo starts
// from the defaults. Keep OFF therefore RESETS the tab's modules on the first settled pass after a
// level init -- for all three tabs, because that is what the toggle says.
//
// The two halves of the work differ because the tabs own two different kinds of state:
//
//   EFFECTS is partly LEVEL-OWNED. The sky material precache, the cloud material parameters and
//     the sun billboard parameters are torn down and rebuilt by a level load, so with keep ON the
//     panel would go on reporting "Belfast Sunset, clouds 0.40x" while the new map draws its own
//     sky -- keep ON re-asserts them onto the new level. Keep OFF resets the whole tab instead.
//
//   CAMERA and RAGDOLLS are PURE MODULE STATE. Nothing about them is level-owned, so they survive
//     a demo change on their own and keep ON has nothing to do; only keep OFF acts.
//
// A keep-off reset never reaches the settings FILE: it is bracketed by
// FilmmakerSettings_BeginQuietReset(), so the operator's tuned settings are still on disk and
// turning keep back on brings them back. See the long note in the .cpp for why that matters.
//
// Deliberately its own module rather than another branch inside FilmmakerSettingsStore or
// ParticleFx: neither of those is the home for "what happens to settings across a level swap".
// The only shared surface is the level-init detour in main.cpp, which calls one more OnLevelInit.

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// group is "camera" | "effects" | "ragdolls". Unknown names read as true and set nothing.
bool EffectsPersistence_Keep(const char* group);
void EffectsPersistence_SetKeep(const char* group, bool on);

// Main thread, from the engine's LevelInitPreEntity detour (main.cpp) -- i.e. BEFORE the new
// level's entities and materials exist. Only ARMS the work.
//
// The settle that follows is counted in frames where the WORLD IS ACTUALLY UP, never from this
// call: the re-assert walks the engine's live material array and snapshots each material the
// first time it sees it, so running mid-load records a half-loaded material as "the map's
// authored value" and walks an array the engine is still growing. `mirv_filmmaker effects off`
// makes the whole module inert if it ever needs ruling out of a crash.
void EffectsPersistence_OnLevelInit();

// Main thread, from Filmmaker::RunFrame.
void EffectsPersistence_RunFrame();

// mirv_filmmaker effects keep <group> on|off|toggle
// mirv_filmmaker effects reset camera|effects|ragdolls -- same reset as each Config footer
void EffectsPersistence_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
