#pragma once

// CS:S-painted widgets with CS2's grammar, drawn on ImGui. ImGui gives input, clipping and layout;
// these draw the shapes CS2's workspaces use (text tabs on a rule, header chips) in CS:S colours.
// Every size is in stage px and multiplied by the scale the caller passes.

#include "UiStyle.h"

namespace SourceFilmmaker {
namespace Ui {

// A borderless, fixed ImGui window over client rect, filled with bg: the panels the workspaces are
// built from. Pair with ImGui::End().
void BeginPanel(const char* name, const Rect& client, Color bg);

// The length in bytes of the UTF-8 character at p (1 for a stray continuation byte).
int Utf8CharBytes(const char* p);

// Text drawn with extra space after every character, as CS2's tab captions are (letter-spacing).
float SpacedTextWidth(ImFont* font, const char* text, float spacing);
void DrawSpacedText(ImDrawList* draw, ImFont* font, ImVec2 pos, ImU32 color, const char* text, float spacing);

// A row of text tabs on a rule (CS2's "tabTop" / "tab" grammar): uppercase captions, the active one
// in the accent colour with an underline, a hairline under the whole row. Returns true when the
// active index changed this frame. Advances the cursor below the rule.
bool TextTabs(const char* id, const char* const* labels, int count, int& active, ImFont* font,
	const Metrics::Tab& metrics, float scale, float width);

// A header chip (CS2's "ESC EXIT", "Reset"): bordered, accent caption, red when pressed like a VGUI
// armed button. selected marks the chosen one of a group (the dock tabs). Drawn at pos; returns true
// when clicked.
bool Chip(const char* id, const char* label, ImVec2 pos, ImFont* font, float scale, bool selected = false);
float ChipWidth(const char* label, ImFont* font, float scale);

} // namespace Ui
} // namespace SourceFilmmaker
