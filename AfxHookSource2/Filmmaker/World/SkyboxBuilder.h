#pragma once

// Runs FX/tools/build-skybox.ps1 from inside the game, so "Create custom skybox" and the
// rotate/flip settings are buttons rather than a command you have to go and type in a terminal.
//
// WHY A SUBPROCESS RATHER THAN DOING IT IN THE DLL: turning an image into a CS2 skybox means
// running Valve's resourcecompiler.exe over a staged content tree. That is a tool invocation
// however you slice it, and the script already does it correctly. Reimplementing the staging,
// the DMX .vtex authoring and the cubemap reprojection in C++ would be a second implementation
// of a thing that works, which is exactly what the repo's coupling rule is there to prevent.
//
// WHY ROTATION IS A REBUILD AND NOT A SLIDER: sky.vfx has no rotation parameter. Every shipped
// CS2 sky material was decompiled while building this, and between them they set only
// g_tSkyTexture, g_flBrightnessExposureBias, g_flRenderOnlyExposureBias and the Solar* set --
// there is nothing to write per frame. So yaw and mirror are baked by rolling the source
// panorama and recompiling, which takes a few seconds and then hot-swaps.
//
// Everything runs on a WORKER THREAD. The compile takes seconds and the file dialog is modal;
// either one on the main thread would freeze the game. Nothing here touches Panorama -- the UI
// polls the status string through the normal state mirror.
//
// See Docs/effects/skybox.md.

#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

enum class SkyBuildState { Idle, Picking, Building, Done, Failed };

// Opens a native file-open dialog, then imports whatever was chosen. No-op if a build is already
// running. `name` may be empty, in which case the file's own base name is used.
void SkyboxBuilder_PickAndImport();

// Imports a specific image without a dialog.
void SkyboxBuilder_Import(const std::string& imagePath, const std::string& name,
                          double rotation, bool flip);

// Rebuilds an EXISTING custom skybox from its recorded source image with new rotation/flip.
// False (with a reason) when the entry is unknown, is a stock CS2 sky, or no longer has its
// source image on disk -- all three are things the user can act on, unlike a silent no-op.
bool SkyboxBuilder_Rebuild(const std::string& name, double rotation, bool flip,
                           std::string* outReason);

SkyBuildState SkyboxBuilder_State();
std::string   SkyboxBuilder_Status();   // one line, safe to show in the panel
bool          SkyboxBuilder_Busy();

// True when the tooling this needs is actually present (the script + a PowerShell to run it).
// The UI disables its buttons rather than offering an action that cannot work.
bool SkyboxBuilder_Available(std::string* outReason);

// Main/UI thread, once per frame from World_RunFrame. Applies a build that finished on the
// worker, because applying runs a console command and that is main-thread work.
void SkyboxBuilder_RunFrame();

void SkyboxBuilder_AppendStateJson(std::ostringstream& o);

void SkyboxBuilder_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);


} // namespace Filmmaker
