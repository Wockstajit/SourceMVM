#pragma once

// Turns UiTheme.h into ImGui state: the style (colours, square VGUI-like frames, CS2 spacing) and the
// font set, both at the current stage scale. Shared by the game and the browser mockup; each host
// tells it where the font files are.

#include "UiTheme.h"

#include <imgui.h>

#include <string>

namespace SourceFilmmaker {
namespace Ui {

inline ImU32 ToU32(Color c) { return IM_COL32(c.r, c.g, c.b, c.a); }

struct FontSet {
	ImFont* fonts[(int)FontRole::Count] = {};
	float scale = 0;   // the stage scale the fonts were built for
	bool ok = false;   // every face loaded from file; false means ImGui's built-in fallback
	ImFont* Get(FontRole role) const { return fonts[(int)role]; }
};

// Rebuilds atlas with every FontRole at its stage size times scale. fontDir is the folder that holds
// verdana.ttf, verdanab.ttf, tahoma.ttf and tahomabd.ttf (Windows' Fonts folder in game). The atlas
// is cleared first; the caller re-uploads its texture afterwards.
FontSet BuildFonts(ImFontAtlas* atlas, const std::string& fontDir, float scale);

// The CS:S look at the given scale.
void ApplyStyle(ImGuiStyle& style, float scale);

} // namespace Ui
} // namespace SourceFilmmaker
