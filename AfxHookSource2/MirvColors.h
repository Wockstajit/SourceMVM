#pragma once

#include "Globals.h"
#include <windows.h>
#include "../Deps/release/Detours/src/detours.h"

void HookMirvColors(HMODULE clientDll);

// Main-thread grenade-flight pump. Smoke grenades do not create CS2's native thrown
// incendiary particle, so when Filmmaker Nade Trails is enabled this dispatches that
// same entity-owned effect path for them. This is deliberately independent of the
// spectator/X-ray trajectory-trail hook below.
void MirvColors_PumpSmokeGrenadeTrails();

struct AfxBasicColor {
	const char* name;
	afxUtils::RGBA value;
};

extern std::vector<AfxBasicColor> afxBasicColors;

struct MyColor {
	afxUtils::RGBA value;
	afxUtils::RGBA defaultValue;
	std::string userValue = "";
	bool use = false;

	bool setColor(const char* arg);
	bool setColor(advancedfx::ICommandArgs* args);
};

// Read-only smoke-colour state, so the Config panel's ATMOSPHERE tab can mirror what
// `mirv_colors smokes t|ct` currently holds instead of keeping a second copy that can drift.
// `ct` selects the CT colour, otherwise T.
//
// Returns the colour NAME as the user set it ("red", "blue", ...) because that is what the UI
// needs to highlight a swatch, and returns "default" when that team is not overridden. A
// numerically-set colour ("255 0 0") returns that same string; the UI simply highlights nothing,
// which is the honest result -- no swatch corresponds to it.
const char* MirvColors_GetSmokeColorName(bool ct);
void MirvColors_SetSmokeColor(bool ct,bool enabled,int r,int g,int b);

// The same state as RGB, for the live per-frame re-write in Filmmaker/World/SmokeFx.cpp. Returns
// false (and leaves the out params untouched) when that team is not overridden.
bool MirvColors_GetSmokeColorRgb(bool ct, unsigned char* outR, unsigned char* outG, unsigned char* outB);

// The team's DEFAULT smoke colour (the game's own). Needed by the re-trigger path: when a team
// is switched back to `default`, an already-deployed volume has to be rebuilt with this colour,
// otherwise it would rebuild carrying the override we previously wrote into the field.
void MirvColors_GetSmokeDefaultRgb(bool ct, unsigned char* outR, unsigned char* outG, unsigned char* outB);
