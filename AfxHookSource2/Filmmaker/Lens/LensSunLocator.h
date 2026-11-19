#pragma once

// LensSunLocator: where CS2 drew the sun disc this frame, found on the GPU, for the lens flare.
//
// Most maps paint the sun with a small csgo_effects sprite (sun_disc_glow), not with the sky
// shader's SolarPosition, so on Dust 2 or Mirage the sky material holds no sun direction at all and
// the flare had no sun outside the Environment. The disc is a dedicated quad (a few vertices, a
// tiny index buffer) whose g_flColorBoost is far above 1; its broad halo uses 1.0 (measured in
// SkyGpuRotation.cpp). Right after CS2 draws such a sprite, the same draw is issued again into a
// 256 x 128 float target with an additive pixel shader that reads the colour boost from the
// draw's own constants on the GPU and writes (u, v, 1) for every covered texel of the disc. The
// mips of that target average it, so the last mip holds the disc's centre as xy / z on screen.
// Nothing is read back to the CPU, so finding the sun never stalls the frame.
//
//   main thread  : LensSunLocator_SetWanted (the flare is on and the sun flares)
//   render thread: LensSunLocator_AfterDraw from the four draw detours,
//                  LensSunLocator_Resolve / _Clear from the lens pass (its deferred context)

#include <d3d11.h>

namespace Filmmaker {

void LensSunLocator_SetWanted(bool wanted);
bool LensSunLocator_Wanted();

// One engine draw, as it was called, so the locator can issue it again.
struct LensSunDraw {
	enum Kind { Draw, DrawIndexed, DrawInstanced, DrawIndexedInstanced } kind = Draw;
	UINT count = 0;      // vertices or indices per instance
	UINT instances = 1;
	UINT start = 0;      // start vertex or start index
	INT baseVertex = 0;  // indexed draws
	UINT startInstance = 0;
};

// After an engine draw on the immediate context. Re-issues it (through `redraw`, which calls the
// original draw function) into the locator target when it can be the sun disc, and restores every
// piece of state it touched. Only called while wanted and outside our own draws.
void LensSunLocator_AfterDraw(ID3D11DeviceContext* immediate, const LensSunDraw& draw,
	void (*redraw)(ID3D11DeviceContext* ctx, const LensSunDraw& draw));

// The averaged result for the flare shaders (sample its last mip: xy / z is the disc's centre in
// screen uv, z 0 when no disc was drawn). Null before any disc candidate was seen on this device.
ID3D11ShaderResourceView* LensSunLocator_Result();
// On the lens pass's context: build the mips (before the flare reads the result) and clear the
// target for the next render (after it).
void LensSunLocator_Resolve(ID3D11DeviceContext* ctx);
void LensSunLocator_Clear(ID3D11DeviceContext* ctx);

// How many disc draws were located since the last call (for the debug log).
unsigned LensSunLocator_TakeDrawCount();

void LensSunLocator_EndDevice();

} // namespace Filmmaker
