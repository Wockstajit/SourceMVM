#pragma once

// Opt-in research probe for the Sparse Shadow Tree -> CSM copy boundary.
//
// Anchors CopySSTDepthToCSM_PS / CopySSTDepthToCSM_CopyTex inside scenesystem.dll
// (builder RVA ~0x157510 on ClientVersion 2000880). Default mode only logs through
// mvm_debug. Suppress mode skips the builder for Mirage A/B; it is NOT a product
// toggle until regional crops prove plant removal with live CSM retained.
//
// Own module on purpose: do not fold into BakedShadowFx / DynamicShadowFx.

#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

void ShadowStageProbe_Install(void* sceneSystemDll);
bool ShadowStageProbe_Available(std::string* outReason = nullptr);

// Log-only arm. Safe default.
void ShadowStageProbe_SetLogging(bool enabled);
bool ShadowStageProbe_Logging();

// When true, the CopySSTDepthToCSM builder returns without scheduling the pass.
void ShadowStageProbe_SetSuppressCopy(bool enabled);
bool ShadowStageProbe_SuppressCopy();

void ShadowStageProbe_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);
void ShadowStageProbe_AppendStateJson(std::ostringstream& o);

} // namespace Filmmaker
