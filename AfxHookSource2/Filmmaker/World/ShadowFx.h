#pragma once

// Product master switch for Valve engine shadows (baked + live CSM together).
//
// Off => lb_enable_baked_shadows 0 AND csm_max_num_cascades_override 0.
// On  => restore both module defaults. Owns the Lighting "Shadows" pill and
// `mirv_filmmaker world shadows`. BakedShadowFx / DynamicShadowFx remain the
// per-cvar implementors; bakedshadows / dynamicshadows stay as thin aliases.

#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

void ShadowFx_SetEnabled(bool enabled);
bool ShadowFx_Enabled();
bool ShadowFx_Available(std::string* outReason);
bool ShadowFx_Applied();

void ShadowFx_RunFrame();
void ShadowFx_AppendStateJson(std::ostringstream& o);
void ShadowFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
