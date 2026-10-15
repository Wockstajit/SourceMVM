#include "WorldCommand.h"

#include "ShadowFx.h"
#include "BakedShadowFx.h"
#include "DynamicShadowFx.h"
#include "ShadowStageProbe.h"
#include "ShadowMapProbe.h"
#include "CustomShadowFx.h"
#include "CloudFx.h"
#include "LightFx.h"
#include "NightMode.h"
#include "SkyOrient.h"
#include "SkyboxBuilder.h"
#include "SkyboxLibrary.h"
#include "SunFx.h"
#include "WorldModulation.h"
#include "SmokeFx.h"
#include "WorldHealth.h"

#include "../Config/EffectsPersistence.h" // no effects switched on at the main menu

#include "../../../Shared/AfxConsole.h"

#include <cstring>

namespace Filmmaker {

namespace {

// At the main menu a world command may only read, list or put things back: it is refused if none
// of its arguments is one of these. `world shadows on` is the CS2 default, so it counts as putting
// shadows back rather than switching an effect on.
bool RestoresOrReads(int argc, advancedfx::ICommandArgs* args, const char* sub) {
	static const char* const kAllowed[] = { "off", "disable", "reset", "default", "auto", "status", "list", "refresh" };
	if (argc < 4) return true; // bare subcommand: prints its status and usage
	const bool shadows = 0 == _stricmp(sub, "shadows") || 0 == _stricmp(sub, "bakedshadows")
		|| 0 == _stricmp(sub, "dynamicshadows");
	for (int i = 3; i < argc; ++i) {
		const char* a = args->ArgV(i);
		for (const char* allowed : kAllowed)
			if (0 == _stricmp(a, allowed)) return true;
		if (shadows && (0 == _stricmp(a, "on") || 0 == _stricmp(a, "enable"))) return true;
	}
	return false;
}

} // namespace

void World_RunFrame() {
	NightMode_RunFrame();
	WorldModulation_RunFrame();
	SmokeFx_RunFrame();
	ShadowFx_RunFrame();
	SkyboxBuilder_RunFrame();
	SkyOrient_RunFrame();
	SunFx_RunFrame();
	// Each feature adds one call here as it lands. Every one of them must be a cheap no-op when
	// disabled -- this runs on the main pump every frame.
}

void World_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	// argv: mirv_filmmaker world <sub> ...
	const char* sub = (argc >= 3) ? args->ArgV(2) : "";

	if (0 == _stricmp(sub, "health")) {
		WorldHealth_RunCommand(argc, args, cmd);
		return;
	}
	if (*sub && !RestoresOrReads(argc, args, sub) && !EffectsPersistence_AllowEnable("World effects"))
		return;
	if (0 == _stricmp(sub, "exposure")) {
		NightMode_RunCommand(argc, args, cmd);
		return;
	}
	if (0 == _stricmp(sub, "smoke")) {
		SmokeFx_RunCommand(argc, args, cmd);
		return;
	}
	if (0 == _stricmp(sub, "sky")) {
		SkyboxLibrary_RunCommand(argc, args, cmd);
		return;
	}
	if (0 == _stricmp(sub, "clouds")) {
		CloudFx_RunCommand(argc, args, cmd);
		return;
	}
	if (0 == _stricmp(sub, "modulate")) {
		WorldModulation_RunCommand(argc, args, cmd);
		return;
	}
	if (0 == _stricmp(sub, "light")) {
		LightFx_RunCommand(argc, args, cmd);
		return;
	}
	if (0 == _stricmp(sub, "shadows")) {
		ShadowFx_RunCommand(argc, args, cmd);
		return;
	}
	// Thin aliases — product UI uses `world shadows`; research/automation may still call these.
	if (0 == _stricmp(sub, "bakedshadows")) {
		BakedShadowFx_RunCommand(argc, args, cmd);
		return;
	}
	if (0 == _stricmp(sub, "dynamicshadows")) {
		DynamicShadowFx_RunCommand(argc, args, cmd);
		return;
	}
	if (0 == _stricmp(sub, "shadowstage")) {
		ShadowStageProbe_RunCommand(argc, args, cmd);
		return;
	}
	if (0 == _stricmp(sub, "shadowmap")) {
		ShadowMapProbe_RunCommand(argc, args, cmd);
		return;
	}
	if (0 == _stricmp(sub, "customshadow") || 0 == _stricmp(sub, "shadershadows")) {
		CustomShadowFx_RunCommand(argc, args, cmd);
		return;
	}
	if (0 == _stricmp(sub, "sun")) {
		SunFx_RunCommand(argc, args, cmd);
		return;
	}
	if (0 == _stricmp(sub, "skyorient")) {
		SkyOrient_RunCommand(argc, args, cmd);
		return;
	}
	if (0 == _stricmp(sub, "skybuild")) {
		SkyboxBuilder_RunCommand(argc, args, cmd);
		return;
	}

	advancedfx::Message(
		"mirv_filmmaker world - world, lighting and atmosphere controls.\n"
		"Usage:\n"
		"  mirv_filmmaker world health              - schema/feature status (run this after a CS2 update)\n"
		"  mirv_filmmaker world exposure <v>|auto   - pin the tonemapper (night mode)\n"
		"  mirv_filmmaker world smoke ...           - smoke grenade colour\n"
		"  mirv_filmmaker world sky ...             - skybox library (list/apply/default/refresh)\n"
		"  mirv_filmmaker world clouds ...          - cloud density/brightness/size/drift\n"
		"  mirv_filmmaker world sun ...             - sun position/size/brightness + glow ring\n"
		"  mirv_filmmaker world shadows ...         - engine shadows (baked + live CSM)\n"
		"  mirv_filmmaker world customshadow ...    - Phase A HLSL contact shadows + tuning\n"
		"  mirv_filmmaker world shadowstage ...     - SST->CSM copy probe (research)\n"
		"  mirv_filmmaker world shadowmap probe ... - native shadow-resource probe (research)\n"
		"Docs: Docs/effects/README.md\n");
}

} // namespace Filmmaker
