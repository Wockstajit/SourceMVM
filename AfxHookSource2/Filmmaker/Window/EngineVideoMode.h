#pragma once

// Changing the game resolution the way CS2's own video settings menu does it.
//
// WHY THIS EXISTS. Resizing the OS window is not a resolution change. CS2 sizes its scene render
// targets from the VIDEO MODE at startup (Docs/agent/render-system-contract.md, "Size ownership"),
// so a window that grows underneath the engine leaves the swapchain and the scene targets
// disagreeing -- which is exactly the "some things render black" symptom. The window drag has to
// be translated into a real video-mode change, and the engine has to be the one that does it.
//
// HOW IT WAS FOUND. CS2's decompiled Panorama settings code (reference/cs2 refs/scripts/
// settingsmenu_shared.js) applies video settings with:
//
//     $.DispatchEvent("CSGOApplyVideoSettings");
//
// and engine2.dll owns the setting keys `setting.defaultres` / `setting.defaultresheight`
// (verified by string search). `GameInterfaceAPI.SetSettingString` is the documented writer for
// those keys, used all over the native scripts. So: write the two keys, dispatch the apply event,
// and the engine performs the same mode change the settings menu does -- including rebuilding the
// render targets, which is the part nothing else can do.
//
// CS2 has no `mat_setvideomode`; that was a Source 1 command and the string does not exist in any
// CS2 binary. This is the replacement.
//
// THREADING: Panorama runs V8 on the game main thread. Every call here must come from
// Filmmaker::RunMainThreadFrame. Never from the render thread, never from a WndProc.

#include <string>

namespace Filmmaker {

// True when the Panorama bridge can run script right now (engine + RunScript + a context panel).
bool EngineVideoMode_Available();

// Writes setting.defaultres / setting.defaultresheight and dispatches CSGOApplyVideoSettings.
// Returns false with outError set when the bridge is not ready. A true return only means the
// script was submitted -- whether the engine actually changed mode is confirmed by watching the
// backbuffer size (WindowResize does that).
bool EngineVideoMode_Apply(int width, int height, std::string& outError);

// Switches the engine's Display Mode to Windowed the way the Settings > Video page does: select
// "Windowed" in the native DisplayModeEnum dropdown, run its selection-changed handler, then
// dispatch CSGOApplyVideoSettings. Needs the Video settings page to exist in the Panorama context
// (it does whenever the Custom Resolution row is on screen). Returns false with outError set when
// the bridge is not ready; true only means the script was submitted -- the caller watches the SDL
// window flags to see whether the engine really left fullscreen.
bool EngineVideoMode_LeaveFullscreen(std::string& outError);

} // namespace Filmmaker
