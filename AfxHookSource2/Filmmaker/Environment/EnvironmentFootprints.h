#pragma once
// Footprints (EnvironmentFootprintMath.h): follows the players, keeps the image of their marks and
// the last few steps, and hands both to the composite.
#include "EnvironmentScene.h"
#include <d3d11.h>

namespace Filmmaker::Environment {
struct FootprintBinding {
    ID3D11ShaderResourceView *view = nullptr;
    float steps[32][4]{}; // xyz where a foot came down, w the frame clock then
    float impact[32]{};   // how hard each came down (FootImpact): about .55 a walk, .85 a run, 1.5 and up a landing
    int stepCount = 0;
    // For the foot view (`environment footview`): every foot as it was read this frame, xyz the
    // middle of the boot at the height of its sole, w how high the sole is (over 1e8: in the air).
    float feet[24][4]{}; // kFootLive
    int feetCount = 0;
};
// Render thread, every frame snow or wet ground is on. `clock` is the composite's own clock
// (climateClock.x), which is what the shader compares step times with.
FootprintBinding PrepareFootprints(ID3D11Device *device, ID3D11DeviceContext *ctx, const Frame &frame, float clock);
void EndFootprints();
} // namespace Filmmaker::Environment
