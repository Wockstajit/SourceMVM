#pragma once

#include <windows.h>

void HookDeathMsg(HMODULE clientDll);
void HookPanorama(HMODULE panoramaDll);

// Exposes the resolved Panorama UI engine instance pointer (a u_char**), or
// nullptr if not yet resolved. Used by the filmmaker Panorama bridge so it does
// not have to re-derive the engine address.
unsigned char** AfxHookSource2_GetPanoramaUiEngine();

// Exposes the resolved CUIEngine::RunScript function pointer (or nullptr).
void* AfxHookSource2_GetPanoramaRunScript();

// Exposes the in-game HUD root panel (CUIPanel*), valid while a map/demo is
// loaded. CS2 may retain a zero-layout HUD in the main menu; callers must check
// their feature's game/level state before choosing it over the menu context.
unsigned char* AfxHookSource2_GetPanoramaHudPanel();

// Exposes the main-menu root panel (CUIPanel*), valid on the home screen (where
// the top navbar lives); may be nullptr very early. Usable as a RunScript context.
unsigned char* AfxHookSource2_GetPanoramaMainMenuPanel();

struct currentGameCamera {
	double origin[3];
	double angles[3];
	float time;
};

extern currentGameCamera g_CurrentGameCamera;

namespace CS2 {
// https://github.com/danielkrupinski/Osiris
	namespace PanoramaUIPanel {
		extern ptrdiff_t getAttributeString;
		extern ptrdiff_t setAttributeString;

		// TODO: get these from pattern matching
		constexpr ptrdiff_t panelId = 0x10;
		constexpr ptrdiff_t children = 0x28;
		constexpr ptrdiff_t panelStyle = 0x68;
		constexpr ptrdiff_t panelFlags = 0x11c;

		constexpr ptrdiff_t k_EPanelFlag_HasOwnLayoutFile = 0x40;
	}

	namespace PanoramaPanelStyle {
		extern ptrdiff_t setPanelStyleProperty;
	}

	namespace PanoramaUIEngine {
		extern ptrdiff_t makeSymbol;
	}
};
