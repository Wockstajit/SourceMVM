#pragma once

// The two per-frame entry points, and the only two.
//
// The CS2 hook has four (render, main, main-late, main-post) and a hand-ordered script of roughly
// forty-five subsystem calls inside the main one, with load-bearing ordering comments. Three of
// those four exist for reasons this port does not have: the late pump re-asserts each weapon's
// mesh-group mask because the CS2 client restores it from the demo snapshot during its own frame
// stage, and the post pump handles ragdoll pose and flex weights. No weapons, no ragdolls, no
// flex here, so two pumps is the whole story.
//
// Which thread does what is not a style choice. Panorama's V8 lives on the main thread, so a
// RunScript call from the render thread is a crash, not a race. Anything touching a panel goes in
// Frame_MainThread; the render pump is for thread-safe backend work only.

namespace Deadlock {

// RENDER thread. Called from the DX11 present hook.
void Frame_Render();

// MAIN thread. Called after stage 12 in the signature-checked CSource2Client dispatcher.
// DeadlockMain.cpp verifies the initialization thread. Panorama work belongs here only.
void Frame_MainThread();

// Suppresses the Panorama panels entirely, for `-mvmNoUi`. One hook off per launch is how a
// process that dies before drawing gets localised.
void Frame_SetUiDisabled(bool disabled);
bool Frame_UiDisabled();

// Pump counters, for health. A render count of zero means the present hook never installed, which
// is the first thing to check when "nothing happens"; a main count of zero means the frame-stage
// hook has not driven the main pump. Check its signature, installation and diagnostic switches.
unsigned long long Frame_RenderCount();
unsigned long long Frame_MainCount();

} // namespace Deadlock
