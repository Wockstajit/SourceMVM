#pragma once

#include <Windows.h>

void HookViewmodel(HMODULE clientDll);

namespace Filmmaker {

// The viewmodel override, as the CONFIG panel and the settings store see it.
//
// `universal` is the whole feature in one flag. The detour in ViewModel.cpp runs AFTER CS2's own
// viewmodel calc, so with it off every player simply keeps the viewmodel the game already built
// for them -- their own FOV, their own offsets, their own handedness. That is "per-player", and
// it is the default because it is what a demo actually recorded. Turning it on replaces all of
// that with ONE viewmodel shared by every player, which is what a director wants when the shot
// has to match across a round rather than follow whoever is being spectated.
//
// Each field is owned separately (`own*`). A field nobody owns still falls through to the engine
// even while `universal` is on, so the panel can hand a single axis back without dropping the
// whole override.
struct ViewmodelState {
	bool universal = false;
	float offsetX = 0, offsetY = 0, offsetZ = 0, fov = 68;
	bool leftHanded = false;
	bool ownX = false, ownY = false, ownZ = false, ownFov = false, ownLeftHanded = false;
};

enum class ViewmodelField { OffsetX, OffsetY, OffsetZ, Fov, LeftHanded };

ViewmodelState Viewmodel_State();
void Viewmodel_SetUniversal(bool universal);
// Take a field over at this value. LeftHanded reads the value as a boolean.
void Viewmodel_Assign(ViewmodelField field, float value);
// Hand a field back to the engine without disturbing the others.
void Viewmodel_Release(ViewmodelField field);
// True when nothing is overridden, so the panel can dim its Reset like every other card.
bool Viewmodel_IsDefault();
void Viewmodel_Reset();

} // namespace Filmmaker
