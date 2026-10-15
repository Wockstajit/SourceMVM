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

#include "../../../Shared/AfxConsole.h"

#include <cstring>

namespace Filmmaker {

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
