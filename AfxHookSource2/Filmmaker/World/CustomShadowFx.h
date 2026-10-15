#pragma once

// Phase B: directional shadow map from the native CSM atlas (CustomShadowFx).
// Own module. Engine Shadows master toggle stays on ShadowFx. This pass samples the
// captured sun-space atlas and darkens the colour buffer (Fisheye-style BeforeUi2).

#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

bool CustomShadowFx_Enabled();
void CustomShadowFx_SetEnabled(bool enabled);
float CustomShadowFx_Strength();
void CustomShadowFx_SetStrength(float strength);
float CustomShadowFx_StepPixels();
void CustomShadowFx_SetStepPixels(float stepPixels);
float CustomShadowFx_Thickness();
void CustomShadowFx_SetThickness(float thickness);
int CustomShadowFx_Steps();
void CustomShadowFx_SetSteps(int steps);
void CustomShadowFx_ResetSettings();
void CustomShadowFx_OnDiagFrame();
void CustomShadowFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);
void CustomShadowFx_AppendStateJson(std::ostringstream& o);

} // namespace Filmmaker
