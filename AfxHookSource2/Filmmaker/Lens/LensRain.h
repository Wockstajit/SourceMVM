#pragma once

// Lens rain: water on the front of the lens while the scene has active rain
// (CONFIG > CAMERA > LENS RAIN, `mirv_filmmaker lens rain ...`).
//
//   main thread : LensRain_RunMainThreadFrame steps the drops (LensRainMath.h) with the demo's time
//                 and the camera's pose, and hands a copy to the render thread.
//   render thread: LensRain_Render draws them over the finished world frame, after the depth of field
//                 and the flare, before the recorder. Water refracts and softens the scene locally.
//
// It is a Beauty effect like the rest of the lens: the render hook leaves it out of Key, Depth and
// Normal passes and of the console green screen. Settings are the `rain.*` rows of the lens table
// (LensFxMath.h), so they save, reset, preset and key in Scene Keys with the rest of the lens.

#include <d3d11.h>

namespace Filmmaker {

void LensRain_RunMainThreadFrame(float rainIntensity);
// Wipes every drop off the lens (`lens rain clear`, and a new demo).
void LensRain_Clear();
// True when there is anything to draw this frame.
bool LensRain_Active();

void LensRain_BeginDevice(ID3D11Device* device);
void LensRain_EndDevice();
bool LensRain_Render(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target, bool preview, bool scoped);

} // namespace Filmmaker
