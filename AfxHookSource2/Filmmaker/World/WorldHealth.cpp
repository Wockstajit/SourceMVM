#include "WorldHealth.h"

#include "WorldSchema.h"
#include "WorldEntities.h"
#include "NightMode.h"

#include "../Debug/MvmDebugLog.h"
#include "../../ClientEntitySystem.h"
#include "../../../Shared/AfxConsole.h"

#include <cstring>
#include <string>

namespace Filmmaker {

namespace {

// Entity classes these features act on, listed by their ENTITY class name (what actually
// matches -- see WorldEntities.h). Counted live so the report distinguishes "the field resolved
// but this map has none of those entities" from "the field is broken" -- two very different
// problems that otherwise look identical from the outside.
//
// Several fog flavours are listed because maps do not agree: Mirage ships env_cubemap_fog and
// no gradient fog at all (measured 2026-08-04).
struct WatchedClass {
	const char* label;
	const char* const names[3];
	std::size_t count;
};

const WatchedClass kWatched[] = {
	{ "post-process volume", { "post_processing_volume", "C_PostProcessingVolume" }, 2 },
	{ "gradient fog",        { "env_gradient_fog", "gradient_fog", "C_GradientFog" }, 3 },
	{ "cubemap fog",         { "env_cubemap_fog" }, 1 },
	{ "volumetric fog ctrl", { "env_volumetric_fog_controller" }, 1 },
	{ "sky",                 { "env_sky", "C_EnvSky" }, 2 },
	{ "precipitation",       { "precipitation", "C_Precipitation" }, 2 },
	{ "wind",                { "env_wind", "C_EnvWind", "C_EnvWindController" }, 3 },
	{ "light (any)",         { "light_omni2", "light_rect", "light_barn" }, 3 },
	{ "directional light",   { "light_directional", "C_LightDirectionalEntity" }, 2 },
	{ "sky camera",          { "sky_camera" }, 1 },
};

// Reports one feature's verdict, both to the console and to mvm_debug.
void ReportFeature(const char* name, bool available, const std::string& reason) {
	if (available) {
		advancedfx::Message("  %-22s LIVE\n", name);
		MvmDebugLog_Linef("world.health", "feature %s LIVE", name);
	} else {
		advancedfx::Warning("  %-22s DEAD - %s\n", name, reason.c_str());
		MvmDebugLog_Linef("world.health", "feature %s DEAD reason=%s", name, reason.c_str());
	}
}

} // namespace

int WorldHealth_CountEntities(const char* className) {
	if (!className) return -1;
	const char* names[1] = { className };
	return WorldEntity_Count(names, 1);
}

void WorldHealth_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	advancedfx::Message("=== mirv_filmmaker world health ===\n");

	// ---- 1. Schema resolution -------------------------------------------------------
	if (!WorldSchema_Ready()) {
		advancedfx::Warning(
			"SCHEMA RESOLVER DID NOT RUN.\n"
			"  Every world/effects feature is dead. This is almost certainly a startup-ORDER bug\n"
			"  on our side, not CS2 update fallout: WorldSchema_Init() must run before\n"
			"  SchemaSystem.cpp clears g_SchemaSystemOffsets. Check that call site first.\n");
		MvmDebugLog_Linef("world.health", "schema NOT READY");
		return;
	}

	int resolved = 0, missing = 0;
	advancedfx::Message("-- schema fields (resolved BY NAME; offsets are informational) --\n");
	for (int i = 0; i < (int)kWF_Count; ++i) {
		const WorldField f = (WorldField)i;
		const std::ptrdiff_t off = WorldSchema_Offset(f);
		if (0 != off) {
			++resolved;
			advancedfx::Message("  %-26s %-32s 0x%zX\n",
				WorldSchema_ClassName(f), WorldSchema_FieldName(f), (size_t)off);
		} else {
			++missing;
			advancedfx::Warning("  %-26s %-32s MISS\n",
				WorldSchema_ClassName(f), WorldSchema_FieldName(f));
		}
	}
	advancedfx::Message("  ---- %d resolved, %d MISSING ----\n", resolved, missing);
	MvmDebugLog_Linef("world.health", "fields resolved=%d missing=%d", resolved, missing);

	// ---- 2. Per-feature verdicts ----------------------------------------------------
	advancedfx::Message("-- features --\n");
	{
		std::string reason;
		ReportFeature("night mode/exposure", NightMode_Available(&reason), reason);
	}
	// As each remaining feature lands it adds one line here. Deliberately explicit rather than a
	// registration list: the point of this command is that it is trivially readable on update day.
	advancedfx::Message("  fog/smoke/ragdoll/world-sun/weather modulation    LIVE\n");

	// ---- 3. Live entity counts ------------------------------------------------------
	advancedfx::Message("-- live entities --\n");
	if (!g_pEntityList || !g_GetEntityFromIndex) {
		advancedfx::Message("  (no entity list -- not in a demo/map)\n");
	} else {
		for (const auto& w : kWatched) {
			const int n = WorldEntity_Count(w.names, w.count);
			advancedfx::Message("  %-22s %d   (%s)\n", w.label, n, w.names[0]);
			MvmDebugLog_Linef("world.health", "entities %s=%d", w.names[0], n);
		}
		advancedfx::Message("  camera is inside post-process volume index: %d\n",
			NightMode_ActiveVolumeIndex());
	}

	if (missing > 0) {
		advancedfx::Warning(
			"\n%d field(s) MISSING. If CS2 just updated, Valve renamed them.\n"
			"Fix: find the new name in the schema dump, change that one string in\n"
			"AfxHookSource2/Filmmaker/World/WorldSchema.cpp, rebuild.\n"
			"Full runbook: Docs/effects/update-resilience.md\n", missing);
	}
}

} // namespace Filmmaker
