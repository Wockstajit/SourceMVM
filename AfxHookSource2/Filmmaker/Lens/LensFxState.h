#pragma once

// Shared lens controls, presets, masks and renderer snapshots. No graphics API or game engine.
// D3D11 and D3D9 use the same parameter table and command surface.
#include "LensFxMath.h"
#include <sstream>
#include <string>
#include <vector>

namespace advancedfx { class ICommandArgs; }
namespace Filmmaker {

Lens::Values LensFx_Get();

// Sets one parameter by key ("dof.focus"), clamped to its range. False for an unknown key.
bool LensFx_Set(const char* key, double value);
void LensFx_ResetGroup(Lens::Group group);
bool LensFx_Preset(Lens::Group group, const char* name);

// True when either effect is switched on -- the only test the render hook needs before doing work.
bool LensFx_Active();
// True when depth of field is on: the render hook then keeps CS2's smoke absorbance and the smoke
// spheres for the frame.
bool LensFx_DofOn();
// True when the lens flare is on: the main thread then keeps the sky's sun direction fresh
// so the sun can flare.
bool LensFx_FlareOn();

// Which built-in custom iris is loaded ("" when the custom slot holds a file or nothing).
bool LensFx_SetBuiltInIris(const char* name);
bool LensFx_LoadIrisFile(const char* path, std::string* error);

// Appends `,"lens":{...}` for the Config panel's state blob.
void LensFx_AppendStateJson(std::ostringstream& out);

void LensFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

struct LensMaskImage {
	unsigned width = 0, height = 0;
	std::vector<unsigned char> pixels; // one byte per texel, 255 = light passes
	unsigned serial = 0;
	std::string label;
};

struct LensRenderState {
    Lens::Values values;
    LensMaskImage iris, dirt;
    bool irisChanged = false, dirtChanged = false;
};
// Copies masks only when their serial differs from the renderer's uploaded serial.
LensRenderState LensFx_ReadRenderState(unsigned irisSerial,unsigned dirtSerial);
void LensFx_SetRenderStatus(const char* status);
void LensFx_NoteRenderedFrame();
// Picture changes only: tuning and mask edits invalidate cached frames; status/counters do not.
uint64_t LensFx_PictureRevision();

} // namespace Filmmaker
