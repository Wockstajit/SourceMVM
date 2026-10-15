#pragma once
#include "RelightMath.h"
#include <d3d11.h>
#include <string>

namespace Filmmaker::Relight {
// Light editor: per-map on/off choice for every authored light, picked in the world.
//
// While the editor is on, every map light gets a marker (bright when it shines, faded when it is
// off) and the light under the crosshair is ringed; F switches that light off or on, Esc leaves.
// Choices are saved per map (%APPDATA%\HLAE\source_mvm\relight\lights\<map>.txt) and applied to
// the frame's lights.

void SetLightEditor(bool on);
bool LightEditorActive();
// Applies the map's saved choices (and remembers which lights the frame holds, for the markers).
void ApplyLightChoices(Scene& scene);
// Switches every light of the current map back on.
void ResetLightChoices();
unsigned LightsSwitchedOff();
// Window-thread key hook. True when the key was used by the editor.
bool LightEditorKey(int vkey, bool down);
// Draws the markers over the finished frame (render thread). No-op unless the editor is on.
void DrawLightMarkers(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target, const float* worldToClip,
                      Vec3 camera, float width, float height);
void EndLightEditor();
} // namespace Filmmaker::Relight
