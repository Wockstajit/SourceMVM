#pragma once

// Dear ImGui configuration for the Source 1 UI (IMGUI_USER_CONFIG), shared by the game build and the
// browser mockup build.

#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#define IMGUI_DISABLE_DEMO_WINDOWS
#define IMGUI_DISABLE_DEBUG_TOOLS

#ifndef __EMSCRIPTEN__
// The game renders through D3D9, whose vertex colours are 0xAARRGGBB. The browser mockup renders
// through WebGL and keeps ImGui's default RGBA order.
#define IMGUI_USE_BGRA_PACKED_COLOR
#endif
