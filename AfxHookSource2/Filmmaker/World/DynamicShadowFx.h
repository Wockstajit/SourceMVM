#pragma once

// Independent live/dynamic shadow switch.  This intentionally has its own cvar resolver and
// state, separate from BakedShadowFx: the two engine paths are different and must be testable
// without one toggle disabling the other.

#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

void DynamicShadowFx_SetEnabled(bool enabled);
bool DynamicShadowFx_Enabled();
// When true, keep cascades filling even if product live shadows are off (CustomShadowFx atlas borrow).
void DynamicShadowFx_SetFillBorrow(bool borrow);
bool DynamicShadowFx_FillBorrow();
bool DynamicShadowFx_Available(std::string* outReason);
bool DynamicShadowFx_Applied();
void DynamicShadowFx_RunFrame();
void DynamicShadowFx_OnLevelInit();
void DynamicShadowFx_AppendStateJson(std::ostringstream& o);
void DynamicShadowFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
