#pragma once

// The Deadlock workspace driver: builds the Panorama panels once, pushes state once per frame.
//
// Same contract as every CS2 *Hud.cpp -- build a panel imperatively through RunScript, then set a
// JSON blob on the root's `state` attribute and call the panel's render() -- with the CS2 content
// left out and Deadlock's own replay controls delegated to rather than replaced.

#include <string>

namespace Filmmaker {

enum class DeadlockWorkspace {
	Off,
	CameraEditor,
	Graph,
	Config,
	Render,
};

// Called every MAIN-thread frame. `demoLoaded` is the debounced flag, not a raw IsPlayingDemo:
// both that and IsDemoPaused blip false mid-seek, and a panel gated on the raw value flickers away
// and back on every scrub.
void Workspaces_RunMainThreadFrame(bool demoLoaded);

// The screen size the PANEL measured, in virtual pixels, reported through
// `mirv_filmmaker ui screen <w> <h>` whenever it changes.
//
// Panorama is the only side that can measure this, and C++ owns every number derived from it, so
// this one command is the whole channel between the two. Out-of-range values are dropped: a panel
// that measures before layout settles reports zero, and remembering that would place the entire
// workspace against a screen that does not exist.
void Workspaces_SetScreenSize(int width, int height);
void Workspaces_GetScreenSize(int& width, int& height);

// The inspector column's requested width, dragged by the panel's grip and clamped by the
// solver. mirv_filmmaker ui inspectorw <px>.
void Workspaces_SetInspectorWidth(int px);
int Workspaces_InspectorWidth();

// The game's own HUD is SCALED into the preview rect by default, which is what the CS2 editor does
// and what a reviewer usually wants. Hiding it outright is the clean-plate alternative.
void Workspaces_SetHudHidden(bool hidden);
bool Workspaces_HudHidden();

DeadlockWorkspace Workspaces_Current();
void Workspaces_Set(DeadlockWorkspace workspace);
bool Workspaces_FromName(const char* name, DeadlockWorkspace* out);
const char* Workspaces_Name(DeadlockWorkspace workspace);

// True while the workspace is open, which is when the camera runtime must let panels have the
// cursor.
bool Workspaces_Open();

// Was the panel ever successfully built? False here with a non-zero main pump count means
// RunScript is not reaching Panorama, which is a different failure from "the panel is empty".
bool Workspaces_Built();

// Forces a rebuild on the next frame. Behind `mirv_filmmaker ui rebuild`, for iterating on the
// panels without restarting the game.
void Workspaces_RequestRebuild();

// Runs arbitrary Panorama JavaScript in the panel's context. Behind `mirv_filmmaker ui_eval`, and
// the reason the netcon channel is hosted in-process: the engine's own console path truncates a
// command at 256 bytes, which is what forces the CS2 UI to be driven through short registered
// helpers instead of inline JavaScript.
bool Workspaces_Eval(const char* js);

// A one-line summary for health.
void Workspaces_Report();

} // namespace Filmmaker
