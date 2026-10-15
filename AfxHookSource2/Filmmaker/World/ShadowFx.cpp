#include "ShadowFx.h"

#include "BakedShadowFx.h"
#include "DynamicShadowFx.h"
#include "../Debug/MvmDebugLog.h"
#include "../../../Shared/AfxConsole.h"

#include <cstring>

namespace Filmmaker {

void ShadowFx_SetEnabled(bool enabled) {
	BakedShadowFx_SetEnabled(enabled);
	DynamicShadowFx_SetEnabled(enabled);
	MvmDebugLog_LinefAlways("world.shadows",
		"enabled=%d baked=%d dynamic=%d",
		enabled ? 1 : 0,
		BakedShadowFx_Enabled() ? 1 : 0,
		DynamicShadowFx_Enabled() ? 1 : 0);
}

bool ShadowFx_Enabled() {
	return BakedShadowFx_Enabled() && DynamicShadowFx_Enabled();
}

bool ShadowFx_Available(std::string* outReason) {
	std::string bakedReason, liveReason;
	const bool bakedOk = BakedShadowFx_Available(&bakedReason);
	const bool liveOk = DynamicShadowFx_Available(&liveReason);
	if (bakedOk && liveOk) return true;
	if (outReason) {
		if (!bakedOk && !liveOk)
			*outReason = "baked and live shadow cvars unresolved.";
		else if (!bakedOk)
			*outReason = bakedReason;
		else
			*outReason = liveReason;
	}
	return false;
}

bool ShadowFx_Applied() {
	return BakedShadowFx_Applied() && DynamicShadowFx_Applied();
}

void ShadowFx_RunFrame() {
	BakedShadowFx_RunFrame();
	DynamicShadowFx_RunFrame();
}

void ShadowFx_AppendStateJson(std::ostringstream& o) {
	std::string reason;
	const bool on = ShadowFx_Enabled();
	o << ",\"shadows\":" << (on ? "true" : "false");
	o << ",\"shadowsApplied\":" << (ShadowFx_Applied() ? "true" : "false");
	o << ",\"shadowsOk\":" << (ShadowFx_Available(&reason) ? "true" : "false");
	// Keep legacy keys so older Panorama / settings still mirror correctly.
	BakedShadowFx_AppendStateJson(o);
	DynamicShadowFx_AppendStateJson(o);
}

void ShadowFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* op = argc >= 4 ? args->ArgV(3) : "";
	if (0 == _stricmp(op, "on") || 0 == _stricmp(op, "enable"))
		ShadowFx_SetEnabled(true);
	else if (0 == _stricmp(op, "off") || 0 == _stricmp(op, "disable"))
		ShadowFx_SetEnabled(false);
	else if (0 == _stricmp(op, "toggle"))
		ShadowFx_SetEnabled(!ShadowFx_Enabled());
	else if (0 == _stricmp(op, "reset"))
		ShadowFx_SetEnabled(true);
	else {
		std::string reason;
		const bool ok = ShadowFx_Available(&reason);
		advancedfx::Message(
			"mirv_filmmaker world shadows on|off|toggle|reset\n"
			"Master engine shadow gate (baked + live CSM).\n"
			"Current: %s%s%s%s\n",
			ShadowFx_Enabled() ? "ON" : "off",
			ShadowFx_Applied() ? "" : " (pending cvars)",
			ok ? "" : " — ",
			ok ? "" : reason.c_str());
		return;
	}
	advancedfx::Message("Shadows %s%s.\n",
		ShadowFx_Enabled() ? "enabled" : "disabled",
		ShadowFx_Applied() ? "" : " (pending engine cvars)");
}

} // namespace Filmmaker
