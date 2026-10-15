#pragma once

// LensFx: depth of field with a configurable iris (bokeh), and cinematic lens flare.
//
// A post pass over the finished world frame -- after the environment renderer, before the
// Panorama UI and before clean movie capture, so it is in every recorded frame and never touches
// the HUD. It reads the scene colour and the scene depth the engine already produced, so the blur
// follows real distances (not a screen-space guess), and it is a module of its own: nothing here
// depends on the environment editor being enabled.
//
//   main thread : settings (console, Config panel)       -> LensFx_Set / _RunCommand / _AppendStateJson
//   render thread: the passes                             -> LensFx_BeginDevice / _Render / _EndDevice
//
// Settings cross the thread boundary as a copied snapshot behind a mutex; the render thread never
// holds the lock while it records commands.
//
// See Docs/features/filmmaker_lens.md for the parameter contract and the pass list.

#include "LensFxMath.h"

#include <d3d11.h>
#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// ---- main thread ----------------------------------------------------------------------------

Lens::Values LensFx_Get();

// Sets one parameter by key ("dof.focus"), clamped to its range. False for an unknown key.
bool LensFx_Set(const char* key, double value);
void LensFx_ResetGroup(Lens::Group group);
bool LensFx_Preset(Lens::Group group, const char* name);

// True when either effect is switched on -- the only test the render hook needs before doing work.
bool LensFx_Active();

// Which built-in custom iris is loaded ("" when the custom slot holds a file or nothing).
bool LensFx_SetBuiltInIris(const char* name);
bool LensFx_LoadIrisFile(const char* path, std::string* error);

// Appends `,"lens":{...}` for the Config panel's state blob.
void LensFx_AppendStateJson(std::ostringstream& out);

void LensFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

// ---- render thread ----------------------------------------------------------------------------

void LensFx_BeginDevice(ID3D11Device* device);
void LensFx_EndDevice();

// Runs the enabled effects over `target` in place. `worldToClip` is the engine's row-major
// world-to-clip matrix for this frame (16 floats); `depthMin`/`depthMax` are the world viewport's
// depth band (CS2 keeps the rest for the 3D skybox). Returns true when something was drawn.
bool LensFx_Render(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target,
	ID3D11DepthStencilView* depth, const float* worldToClip, float depthMin, float depthMax);

} // namespace Filmmaker
