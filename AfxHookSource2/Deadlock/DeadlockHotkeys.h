#pragma once

// KEYBOARD SHORTCUTS for the Deadlock filmmaker workspace.
//
// The camera editor has advertised four of these in its SHORTCUTS card since the port began, and
// not one of them did anything: there was no hotkey layer at all. DeadlockInput.cpp polls the
// keyboard, but only to feed MirvInput's free camera -- a key it forwards flies the camera, and a
// key it does not forward does nothing.
//
// ITS OWN MODULE, not a branch inside DeadlockInput. That file's job is to keep one camera in step
// with the physical keyboard; this one turns a key press into a console command. They share a
// question ("is this key down") and nothing else, and the failure modes are completely different --
// a bug here issues the wrong command, a bug there flies the camera into a wall.
//
// HOW A SHORTCUT RUNS. Through Commands_DispatchLine, exactly like a Panorama button
// (DeadlockUiCommandBridge.h), so a key and the button beside it cannot drift apart: both end up in
// the same subcommand with the same argument parsing. Called from the MAIN pump, because that is
// the only thread a console command may be executed on.
//
// THE GATES THAT MATTER.
//
// 1. SPACE and the arrow keys need the workspace OPEN. Deadlock's own native HUD binds these same
//    physical keys during ordinary spectating -- SPACE changes team, the arrows drive its own
//    scrubber -- and this port POLLS rather than intercepts (see "what this cannot do" below), so
//    our command always fires ALONGSIDE the game's, never instead of it. Claiming them whenever a
//    replay is loaded would mean firing demo_pause or a seek as a surprise side effect of just
//    switching teams, with none of our UI on screen to explain why.
//
//    K and F8 have NO such collision -- Deadlock binds neither to anything -- and CS2's own
//    equivalent proves they do not need one either: CS2's OnKey (MovieMode.cpp) gates K on nothing
//    but IsDemoActive(), commented "any camera mode", independent of whether CS2's own Camera
//    Editor panel is open. So those two work with the workspace closed, matching CS2, and are what
//    the CONTROLS card (DeadlockMovieHudJs.h) advertises for that state.
//
// 2. The arrow keys additionally stand down while the FREE CAMERA owns the input. MirvInput binds
//    left/right to camera yaw, so the same press cannot also be a seek: you are either flying or
//    scrubbing, and Input_OwnsCamera() already answers which. K and SPACE have no such conflict --
//    MirvInput puts free-cam vertical movement on the numpad and does not bind either -- so they
//    fire in both.
//
// WHAT THIS CANNOT DO. Keys are POLLED, not intercepted; Deadlock still receives every one of them.
// SPACE therefore also does whatever the game has it bound to. Suppressing that would mean owning
// the window procedure, which this port deliberately does not do.

namespace Deadlock {

// MAIN thread, once per frame, after Input_RunFrame so the focus and camera-ownership state it
// reads is this frame's.
void Hotkeys_RunMainThreadFrame(bool demoLoaded, bool workspaceOpen);

// `mirv_filmmaker health`. A shortcut that "does nothing" is either gated off or never seen, and
// these counters say which.
void Hotkeys_Report();

} // namespace Deadlock
