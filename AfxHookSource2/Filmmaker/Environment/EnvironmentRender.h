#pragma once
#include "EnvironmentScene.h"
#include <d3d11.h>
namespace Filmmaker::Environment {
// Runtime uses background preparation; standalone GPU tests may initialize synchronously.
void BeginDevice(ID3D11Device *device, bool background = false);
bool RendererReady();
// Render-thread poll; returns immediately while bytecode is being prepared.
bool PumpDevicePreparation();
void EndDevice();
// Render-thread only, before LDR capture and before Panorama/scaled preview.
// CS2's smoke absorbance (the full-resolution zeroth OIT moment, R32) for the next Render call, or
// null when none was captured this frame. Every addition is hidden by 1-exp(-b0) (SmokeCover).
void SetSmokeAbsorbance(ID3D11ShaderResourceView *view, unsigned width, unsigned height);
// Debug: draw the smoke coverage as grey instead of the scene (environment smokeview).
void SetSmokeDebugView(bool on);
bool SmokeDebugView();
// False-colour which sun cascade each pixel's shadow lookup resolved to:
// red close, green detail, blue middle, yellow outer, magenta none. Magenta is the one that
// matters -- it marks surfaces no cascade projects, which therefore cannot be shadowed at all.
void SetShadowCascadeView(bool on);
bool ShadowCascadeView();
void SetRainExposureView(bool on);
bool RainExposureView();
bool Render(ID3D11DeviceContext *context, ID3D11RenderTargetView *target, ID3D11DepthStencilView *depth,
            const Frame &frame, float depthMin = 0, float depthMax = 1,
            ID3D11Texture2D *nativeHdrSource = nullptr);
} // namespace Filmmaker::Environment
