#pragma once

#include "Globals.h"
#include <windows.h>
#include "../Deps/release/Detours/src/detours.h"

void HookMirvCommands(HMODULE clientDll);

bool shouldGlowProjectile(const char* className, int team);

struct MirvGlow {
	std::map<uint64_t,bool> players;
	std::map<int32_t,bool> entities;

	std::map<std::string, std::map<std::string, bool>> nades {
		{ "smokes",     { {"ct", true}, {"t", true} } },
		{ "grenades",   { {"ct", true}, {"t", true} } },
		{ "flashbangs", { {"ct", true}, {"t", true} } },
		{ "molotovs",   { {"ct", true}, {"t", true} } },
		{ "decoys",     { {"ct", true}, {"t", true} } }
	};
};

extern MirvGlow g_MirvGlow;

namespace Filmmaker {
// `mirv_fov` -- the CAMERA's field of view, i.e. the shot itself. Distinct from the viewmodel
// FOV the VIEWMODEL card carries, which only changes how the weapon is projected and leaves the
// framing alone. Off means the game's own FOV is used untouched. The stored value is whole
// degrees; the CONFIG slider and the settings file both round to that.
bool MirvFov_Enabled();
float MirvFov_Value();
void MirvFov_Set(bool enabled, float value);
// The game's own FOV is a weapon zoom (a scope): FOV overrides leave it alone, same rule as `mirv_fov`.
bool MirvFov_IsZoomedFov(float gameFov);
} // namespace Filmmaker
