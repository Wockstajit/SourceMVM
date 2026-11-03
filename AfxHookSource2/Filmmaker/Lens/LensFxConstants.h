#pragma once
#include "LensFxMath.h"
namespace Filmmaker::Lens {
// Must match LensConstants in LensFxShaders.h, member for member.
struct F4 { float x = 0, y = 0, z = 0, w = 0; };
struct GpuConstants {
	float inverseViewProjection[16];
	F4 dims, depthInfo, frame, dofA, dofB, dofC, irisA, irisB;
	F4 flareA, flareB, flareC, flareTint, streakTint;
	F4 lowRes, tileInfo, passInfo, modes;
	F4 dofBand, smokeInfo, cameraPos;
	F4 smokeSpheres[8]; // kLensMaxSmokes; the shader's cbuffer spells the 8 out
	F4 sunInfo;
};
static_assert(kLensMaxSmokes == 8, "LensConstants.smokeSpheres holds 8 spheres");
static_assert(sizeof(GpuConstants) % 16 == 0, "D3D constant buffers are 16-byte aligned");

// Common parameter packing for D3D11 and D3D9; host-specific depth, smoke and sun follow.
inline GpuConstants MakeGpuConstants(const Values& L, unsigned width, unsigned height,
    unsigned depthWidth, unsigned depthHeight, const float* inverse, float dt, bool focusValid,
    bool irisLoaded, bool dirtLoaded, bool dofOn, bool flareOn, bool autoFocus) {
    const float W=(float)width,H=(float)height,rad=(float)(kPi/180.0);
	GpuConstants k{};
	for (int i = 0; i < 16; ++i) k.inverseViewProjection[i] = inverse[i];
	k.dims = { W, H, (float)depthWidth, (float)depthHeight };
	k.depthInfo = { 0, 1, 0, 0 };
	k.frame = { dt, L[DofFocusSpeed], 0, focusValid ? 0.f : 1.f };
	k.dofA = { L[DofMode], L[DofFocus], L[DofFstop], autoFocus ? 1.f : 0.f };
	k.dofB = { L[DofNearStart], L[DofNearEnd], L[DofFarStart], L[DofFarEnd] };
	const float maxPx = L[DofMaxBlur] * H / kReferenceHeightPx;
	k.dofC = { maxPx, L[DofGamma], L[DofWeapon], L[DofDebug] };
	k.irisA = { L[DofShape], L[DofBlades], L[DofRotation] * rad, L[DofRoundness] };
	k.irisB = { L[DofSqueeze], L[DofEdge], (float)TapCount(L), (irisLoaded && (int)L[DofShape] == IrisCustom) ? 1.f : 0.f };
	const FlareConstants fc = MakeFlareConstants(L, dirtLoaded);
	k.flareA = { fc.a[0], fc.a[1], fc.a[2], fc.a[3] };
	k.flareB = { fc.b[0], fc.b[1], fc.b[2], fc.b[3] };
	k.flareC = { fc.c[0], fc.c[1], fc.c[2], fc.c[3] };
	k.flareTint = { fc.tint[0], fc.tint[1], fc.tint[2], fc.tint[3] };
	k.streakTint = { fc.streakTint[0], fc.streakTint[1], fc.streakTint[2], fc.streakTint[3] };
	k.lowRes = { (float)((width+3)/4), (float)((height+3)/4), (float)((width+7)/8), (float)((height+7)/8) };
	const unsigned gw = (width + 15) / 16, gh = (height + 15) / 16;
	k.tileInfo = { (float)gw, (float)gh, std::ceil(maxPx / 16.0f) + 1.0f, 0 };
	k.modes = { dofOn ? 1.f : 0.f, flareOn ? 1.f : 0.f, W / H, 0 };
	k.dofBand = { L[DofRange], 0, 0, 0 };
    return k;
}
} // namespace Filmmaker::Lens
