#pragma once
namespace Filmmaker::Environment {
// Suppress only the verified directional sun submission while replacement lighting owns it.
// Preserve local lights, shadow switches, tint and baked indirect lighting.
//
// Baked lighting itself is not an engine switch. `sc_disable_baked_lighting` was measured to
// move a Mirage frame by 0.044/255 and blacked some props; the Effects > Lighting switch now
// substitutes the lightmap inputs inside the native-material replay instead
// (EnvironmentNativeMaterial.cpp, CL_bakedSuppressed).
void SetEngineLightingTakeover(bool on);
void ResetEngineLightingTakeover();
void SetEngineSunDiscSuppressed(bool on);
void ResetEngineSunDiscSuppression();
// Persistence captures the user's choices, not the temporary forced-off takeover state.
void CaptureNativeLighting(bool &lights, bool &baked, bool &dynamic);
} // namespace Filmmaker::Environment
