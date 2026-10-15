#pragma once

// Runtime switch for map-authored baked shadows (light-binner baked contribution).
//
// Drives `lb_enable_baked_shadows` only (Baked OFF => cvar 0). Verified Mirage A-site corner
// (CS2 2000879): stairs/ground plant-frond crops drop hard; fence/live CSM stays. Live cascades
// stay on `dynamicshadows` / `csm_max_num_cascades_override`. Do not use
// `sc_disable_baked_lighting`, `lb_csm_override_staticgeo_*`, or entity DisableShadows here.

#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

void BakedShadowFx_SetEnabled(bool enabled);
// No-op hook kept so DynamicShadowFx can notify without coupling policy.
void BakedShadowFx_OnLiveShadowsChanged();
bool BakedShadowFx_Enabled();
bool BakedShadowFx_Available(std::string* outReason);
bool BakedShadowFx_Applied();

void BakedShadowFx_RunFrame();
void BakedShadowFx_OnLevelInit();

void BakedShadowFx_AppendStateJson(std::ostringstream& o);
void BakedShadowFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
