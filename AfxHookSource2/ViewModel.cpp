#include "ViewModel.h"
#include "Globals.h"
#include "Filmmaker/Movie/ViewFx.h"
#include "Filmmaker/Movie/ViewFxVm.h"

#include <cstddef>
#include <windows.h>
#include "../Deps/release/Detours/src/detours.h"

namespace {
// Self-contained wall-clock seconds for the sway phase clock -- deliberately NOT tied to
// g_MirvTime (demo time), so idle sway keeps animating the same way whether the demo is
// playing or paused, matching the free-cam wall-clock trick main.cpp's view-setup trampoline
// already uses for the same reason. Same QueryPerformanceCounter pattern, kept local since
// this is the only place in this file that needs a clock.
float WallClockSeconds() {
	static LARGE_INTEGER s_freq = {};
	static LARGE_INTEGER s_start = {};
	if (s_freq.QuadPart == 0) {
		QueryPerformanceFrequency(&s_freq);
		QueryPerformanceCounter(&s_start);
	}
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return (float)((double)(now.QuadPart - s_start.QuadPart) / (double)s_freq.QuadPart);
}
} // namespace

struct MirvViewmodel 
{
	bool enabled = false;
	bool hooked = false;
	bool leftHandedValue = false;
	float offsetX = 0.0f, offsetY = 0.0f, offsetZ = 0.0f;
	float fovValue = 68.0f;
	bool enabledX = false, enabledY = false, enabledZ = false, enabledFOV = false, enabledLeftHanded = false;

	// What one `set` call says about a single field.
	//
	// The enable flags used to be assigned UNCONDITIONALLY from the supplied-ness of each
	// argument, so `*` meant "leave the VALUE alone" but also "turn this axis OFF" -- which made
	// the retained value dead, and made `set 2 * * * *` followed by `set * 3 * * *` silently
	// revert the X the user had just placed. That is exactly the traffic a per-axis slider
	// generates (each drag sends its own axis and `*` for the rest), so the obvious UI on top of
	// this command could only ever move one axis at a time.
	//
	// Now: `*` leaves a field completely alone (value AND enable), a number takes the axis over,
	// and `-` hands it back to the engine -- so nothing that used to be expressible was lost.
	enum class Field { Leave, Assign, Release };

	static void applyFloat(Field f, float in, float& value, bool& enabled)
	{
		if (Field::Assign == f) { value = in; enabled = true; }
		else if (Field::Release == f) enabled = false;
	}

	// Readback tag for the console: whether THIS field is currently overridden by us. A per-axis
	// slider has to restore its own on/off state, and the value alone cannot say whether it is
	// live or merely the last thing that was typed.
	static const char* Own(bool enabled) { return enabled ? "[ours]" : "[engine]"; }

	static void applyBool(Field f, bool in, bool& value, bool& enabled)
	{
		if (Field::Assign == f) { value = in; enabled = true; }
		else if (Field::Release == f) enabled = false;
	}

	void setViewmodel
	(
		float x, float y, float z, float pFov, bool leftHanded,
		Field fX, Field fY, Field fZ, Field fFOV, Field fLeftHanded
	)
	{
		applyFloat(fX, x, offsetX, enabledX);
		applyFloat(fY, y, offsetY, enabledY);
		applyFloat(fZ, z, offsetZ, enabledZ);
		applyFloat(fFOV, pFov, fovValue, enabledFOV);
		applyBool(fLeftHanded, leftHanded, leftHandedValue, enabledLeftHanded);
	};
} g_MirvViewmodel;

typedef void(__fastcall *g_OriginalViewmodelFunc_t)(void* param_1, float* pViewmodelOffsets, float* pFov);
g_OriginalViewmodelFunc_t g_OriginalViewmodelFunc = nullptr;

void __fastcall setViewmodel(void* param_1, float* pViewmodelOffsets, float* pFov)
{
    g_OriginalViewmodelFunc(param_1, pViewmodelOffsets, pFov);

    if (pViewmodelOffsets != nullptr && g_MirvViewmodel.enabled) {
        if (g_MirvViewmodel.enabledX) pViewmodelOffsets[0] = g_MirvViewmodel.offsetX; // x
        if (g_MirvViewmodel.enabledY) pViewmodelOffsets[1] = g_MirvViewmodel.offsetY; // y
        if (g_MirvViewmodel.enabledZ) pViewmodelOffsets[2] = g_MirvViewmodel.offsetZ; // z
		if (g_MirvViewmodel.enabledFOV) pFov[0] = g_MirvViewmodel.fovValue;
    }

    // ViewFx viewmodel modifiers: ADD on top of whichever offset is already in the buffer --
    // the engine's own default, or the static override just above -- so they compose with
    // mirv_viewmodel instead of fighting it. All are no-ops (all-zero) while Off.
    //   * sway: movement-scaled walk bob/drift.
    //   * deadzone: shifts the weapon toward the TRUE aim while the camera lags it (the
    //     "weapon moves first inside an aim deadzone" decoupled-viewmodel effect; the camera
    //     half lives in main.cpp's view-setup trampoline).
    if (pViewmodelOffsets != nullptr) {
        float swayX = 0.0f, swayY = 0.0f, swayZ = 0.0f;
        Filmmaker::ViewFxRef().SwayOffset(WallClockSeconds(), swayX, swayY, swayZ);
        float dzX = 0.0f, dzY = 0.0f, dzZ = 0.0f;
        Filmmaker::ViewFxRef().DeadzoneViewmodelShift(dzX, dzY, dzZ);
        pViewmodelOffsets[0] += swayX + dzX;
        pViewmodelOffsets[1] += swayY + dzY;
        pViewmodelOffsets[2] += swayZ + dzZ;
    }

    // ViewFxVm write-site 1 (helper-post): we are INSIDE CS2's viewmodel calc right now.
    Filmmaker::ViewFxVm_OnViewmodelCalc();
};

typedef bool(__fastcall *g_OriginalHandFunc_t)(int64_t param_1);
g_OriginalHandFunc_t g_OriginalHandFunc = nullptr;

bool __fastcall setHand(int64_t param_1)
{
	bool res = g_OriginalHandFunc(param_1);

	if (g_MirvViewmodel.enabled && g_MirvViewmodel.enabledLeftHanded) {
		return g_MirvViewmodel.leftHandedValue;
	}

	return res;
};

void HookViewmodel(HMODULE clientDll)
{
	if (g_MirvViewmodel.hooked) return;

	// This function references the cvars viewmodel_offset_x, viewmodel_offset_y, viewmodel_offset_z, viewmodel_offset_fov (usually 3rd reference).
	// can be also found with 00 00 88 42 (68.0 in float), which is max limit for fov
	// there are 2 similiar functions, make sure to test if it breakes again to not confuse them
	size_t viewmodelAddr = getAddress(clientDll, "40 55 53 56 41 56 41 57 48 8B EC 48 83 EC 20 4D 8B F8 4C 8B F2 48 8B F1"); 
	if (0 == viewmodelAddr) {
		ErrorBox(MkErrStr(__FILE__, __LINE__));	//TODO
		return;
	}
	// vtable byte offset 0xac8 for "C_CSGO_TeamPreviewModel" class (4th from end.).
	// This function is called right after the first call to viewmodelAddr function.
	size_t handAddr = getAddress(clientDll, "40 53 48 83 EC ?? 80 B9 ?? ?? ?? ?? ?? 48 8B D9 0F 84 ?? ?? ?? ?? 48 8B 89 ?? ?? ?? ?? 48 85 C9 75");
	if (0 == handAddr) {
		ErrorBox(MkErrStr(__FILE__, __LINE__));	
		return;
	}

    g_OriginalViewmodelFunc = (g_OriginalViewmodelFunc_t)(viewmodelAddr);
	g_OriginalHandFunc = (g_OriginalHandFunc_t)(handAddr);

	// Share the (pre-detour) helper address so ViewFxVm's site 4 can locate + detour its CALLER
	// (CS2's CalcViewModelView) -- Detours patches the function body, not call sites, so scanning
	// for E8 calls to this address keeps working after the detour below.
	Filmmaker::ViewFxVm_NoteViewmodelHelper(viewmodelAddr);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    DetourAttach(&(PVOID&)g_OriginalViewmodelFunc, setViewmodel);
	DetourAttach(&(PVOID&)g_OriginalHandFunc, setHand);

	if(NO_ERROR != DetourTransactionCommit()) {
		ErrorBox("Failed to detour Viewmodel functions.");
		return;
	};

	g_MirvViewmodel.hooked = true;
};

void ViewModel_Console(advancedfx::ICommandArgs* args)
{
	int argc = args->ArgC();
	const auto cmd = args->ArgV(0);

	if (argc > 1) {
		char const* subcmd = args->ArgV(1);
		if (0 == _stricmp("enabled", subcmd) ) {
			if (3 == argc) {
				g_MirvViewmodel.enabled = 0 != atoi(args->ArgV(2));
				return;
			};
			advancedfx::Message(
				"%s enabled 0|1 - enable (1) / disable (0) custom viewmodel.\n"
				"Current value: %s\n"
				, cmd
				, g_MirvViewmodel.enabled ? "1 (enabled)" : "0 (disabled)"
			);
		} else
		if (0 == _stricmp("reset", subcmd)) {
			Filmmaker::Viewmodel_Reset();
			advancedfx::Message("Viewmodel reset: every player keeps the viewmodel the game built for them.\n");
			return;
		} else
		if (0 == _stricmp("set", subcmd)) {
			if (argc == 7) {
				auto field = [](const char* a) {
					if (0 == strcmp("*", a)) return MirvViewmodel::Field::Leave;
					if (0 == strcmp("-", a)) return MirvViewmodel::Field::Release;
					return MirvViewmodel::Field::Assign;
				};
				g_MirvViewmodel.setViewmodel(
					(float)atof(args->ArgV(2)),
					(float)atof(args->ArgV(3)),
					(float)atof(args->ArgV(4)),
					(float)atof(args->ArgV(5)),
					atoi(args->ArgV(6)) == 0 ? false : true,
					field(args->ArgV(2)),
					field(args->ArgV(3)),
					field(args->ArgV(4)),
					field(args->ArgV(5)),
					field(args->ArgV(6))
				);
				return;
			};
			advancedfx::Message(
				"%s set <OffsetX|*|-> <OffsetY|*|-> <OffsetZ|*|-> <FOV|*|-> <Hand|*|->\n"
				"Set viewmodel. * leaves a field exactly as it is; - hands it back to the engine.\n"
				"Hand: 0 = Right Handed, 1 = Left Handed\n"
				"Current value: OffsetX: %.1f %s, OffsetY: %.1f %s, OffsetZ: %.1f %s, FOV: %.1f %s, Hand: %i %s\n"
				"Example:\n"
				"%s set 2 2.5 -2 68 0\n"
				, cmd
				, g_MirvViewmodel.offsetX, MirvViewmodel::Own(g_MirvViewmodel.enabledX)
				, g_MirvViewmodel.offsetY, MirvViewmodel::Own(g_MirvViewmodel.enabledY)
				, g_MirvViewmodel.offsetZ, MirvViewmodel::Own(g_MirvViewmodel.enabledZ)
				, g_MirvViewmodel.fovValue, MirvViewmodel::Own(g_MirvViewmodel.enabledFOV)
				, g_MirvViewmodel.leftHandedValue, MirvViewmodel::Own(g_MirvViewmodel.enabledLeftHanded)
				, cmd
			);
		};
	} else {
		advancedfx::Message(
			"%s enabled <0|1> - enable (1) / disable (0) custom viewmodel\n"
			"%s reset - hand every field back to the engine (per-player viewmodels)\n"
			"%s set <OffsetX|*|-> <OffsetY|*|-> <OffsetZ|*|-> <FOV|*|-> <Hand|*|->\n"
			"Set viewmodel. * leaves a field exactly as it is -- value AND whether we own it;\n"
			"- hands that field back to the engine. A per-axis slider can therefore send its\n"
			"own axis and * for the rest without disturbing the others.\n"
			"Hand: 0 = Right Handed, 1 = Left Handed\n"
			"\n"
			"Example 1 - set custom viewmodel\n"
			"%s set 2 2.5 -2 68 0\n"
			"%s enabled 1\n"
			"\n"
			"Example 2 - set custom viewmodel partially\n"
			"%s set 2 2.5 -2 68 *\n"
			"%s enabled 1\n"
			"Note the * in the end: that field is left exactly as it is -- engine-controlled,\n"
			"unless an earlier set took it over. Use - to hand a field back to the engine.\n"
			, cmd, cmd, cmd, cmd, cmd, cmd, cmd
		);
	};
};

CON_COMMAND(mirv_viewmodel, "Set custom viewmodel")
{
	ViewModel_Console(args);
};

namespace Filmmaker {

ViewmodelState Viewmodel_State()
{
	ViewmodelState s;
	s.universal = g_MirvViewmodel.enabled;
	s.offsetX = g_MirvViewmodel.offsetX;
	s.offsetY = g_MirvViewmodel.offsetY;
	s.offsetZ = g_MirvViewmodel.offsetZ;
	s.fov = g_MirvViewmodel.fovValue;
	s.leftHanded = g_MirvViewmodel.leftHandedValue;
	s.ownX = g_MirvViewmodel.enabledX;
	s.ownY = g_MirvViewmodel.enabledY;
	s.ownZ = g_MirvViewmodel.enabledZ;
	s.ownFov = g_MirvViewmodel.enabledFOV;
	s.ownLeftHanded = g_MirvViewmodel.enabledLeftHanded;
	return s;
}

void Viewmodel_SetUniversal(bool universal)
{
	g_MirvViewmodel.enabled = universal;
}

void Viewmodel_Assign(ViewmodelField field, float value)
{
	switch (field) {
	case ViewmodelField::OffsetX: g_MirvViewmodel.offsetX = value; g_MirvViewmodel.enabledX = true; break;
	case ViewmodelField::OffsetY: g_MirvViewmodel.offsetY = value; g_MirvViewmodel.enabledY = true; break;
	case ViewmodelField::OffsetZ: g_MirvViewmodel.offsetZ = value; g_MirvViewmodel.enabledZ = true; break;
	case ViewmodelField::Fov: g_MirvViewmodel.fovValue = value; g_MirvViewmodel.enabledFOV = true; break;
	case ViewmodelField::LeftHanded: g_MirvViewmodel.leftHandedValue = value != 0; g_MirvViewmodel.enabledLeftHanded = true; break;
	}
}

void Viewmodel_Release(ViewmodelField field)
{
	switch (field) {
	case ViewmodelField::OffsetX: g_MirvViewmodel.enabledX = false; break;
	case ViewmodelField::OffsetY: g_MirvViewmodel.enabledY = false; break;
	case ViewmodelField::OffsetZ: g_MirvViewmodel.enabledZ = false; break;
	case ViewmodelField::Fov: g_MirvViewmodel.enabledFOV = false; break;
	case ViewmodelField::LeftHanded: g_MirvViewmodel.enabledLeftHanded = false; break;
	}
}

bool Viewmodel_IsDefault()
{
	// Default is "the game's own viewmodel, per player". Retained values behind a released field
	// do not count as a change: nothing about them reaches the frame.
	return !g_MirvViewmodel.enabled && !g_MirvViewmodel.enabledX && !g_MirvViewmodel.enabledY &&
		!g_MirvViewmodel.enabledZ && !g_MirvViewmodel.enabledFOV && !g_MirvViewmodel.enabledLeftHanded;
}

void Viewmodel_Reset()
{
	g_MirvViewmodel.enabled = false;
	g_MirvViewmodel.enabledX = g_MirvViewmodel.enabledY = g_MirvViewmodel.enabledZ = false;
	g_MirvViewmodel.enabledFOV = g_MirvViewmodel.enabledLeftHanded = false;
	g_MirvViewmodel.offsetX = g_MirvViewmodel.offsetY = g_MirvViewmodel.offsetZ = 0.0f;
	g_MirvViewmodel.fovValue = 68.0f;
	g_MirvViewmodel.leftHandedValue = false;
}

} // namespace Filmmaker
