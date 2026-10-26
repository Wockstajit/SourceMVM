#include "UiWidgets.h"

#include <cstring>

namespace SourceFilmmaker {
namespace Ui {

float SpacedTextWidth(ImFont* font, const char* text, float spacing) {
	float w = 0;
	int n = 0;
	for (const char* p = text; *p; ++p, ++n) {
		char c[2] = { *p, 0 };
		w += font->CalcTextSizeA(font->FontSize, 1e9f, 0, c).x;
	}
	return n > 1 ? w + spacing * (n - 1) : w;
}

void DrawSpacedText(ImDrawList* draw, ImFont* font, ImVec2 pos, ImU32 color, const char* text, float spacing) {
	for (const char* p = text; *p; ++p) {
		char c[2] = { *p, 0 };
		draw->AddText(font, font->FontSize, pos, color, c);
		pos.x += font->CalcTextSizeA(font->FontSize, 1e9f, 0, c).x + spacing;
	}
}

bool TextTabs(const char* id, const char* const* labels, int count, int& active, ImFont* font,
	const Metrics::Tab& metrics, float scale, float width) {
	ImGui::PushID(id);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const float spacing = metrics.ls * scale;
	const float textH = font->FontSize;
	const float rowH = textH + metrics.padBottom * scale;
	bool changed = false;
	float x = origin.x;
	for (int i = 0; i < count; ++i) {
		const float w = SpacedTextWidth(font, labels[i], spacing);
		ImGui::SetCursorScreenPos(ImVec2(x, origin.y));
		ImGui::PushID(i);
		if (ImGui::InvisibleButton("tab", ImVec2(w, rowH)) && active != i) {
			active = i;
			changed = true;
		}
		const bool hovered = ImGui::IsItemHovered();
		ImGui::PopID();
		const bool on = active == i;
		const ImU32 color = ToU32(on ? Theme::Accent : (hovered ? Theme::Text : Theme::TextDim));
		DrawSpacedText(draw, font, ImVec2(x, origin.y), color, labels[i], spacing);
		if (on) {
			const float u = metrics.underline * scale;
			draw->AddRectFilled(ImVec2(x, origin.y + rowH - u), ImVec2(x + w, origin.y + rowH), ToU32(Theme::Accent));
		}
		x += w + metrics.gap * scale;
	}
	// The rule the tabs sit on.
	draw->AddLine(ImVec2(origin.x, origin.y + rowH), ImVec2(origin.x + width, origin.y + rowH), ToU32(Theme::Border), 1.0f);
	ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rowH + 1));
	ImGui::Dummy(ImVec2(width, 0));
	ImGui::PopID();
	return changed;
}

float ChipWidth(const char* label, ImFont* font, float scale) {
	return SpacedTextWidth(font, label, 0.5f * scale) + 2 * Metrics::chip.padH * scale;
}

bool Chip(const char* id, const char* label, ImVec2 pos, ImFont* font, float scale, bool selected) {
	const float w = ChipWidth(label, font, scale);
	const float h = font->FontSize + 2 * Metrics::chip.padV * scale;
	ImGui::SetCursorScreenPos(pos);
	const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
	const bool hovered = ImGui::IsItemHovered();
	const bool held = ImGui::IsItemActive();
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImVec2 p1(pos.x + w, pos.y + h);
	const Color bg = held ? Theme::ButtonArmedBg
		: (selected ? Theme::Border : (hovered ? Theme::ButtonHoverBg : Theme::ButtonBg));
	draw->AddRectFilled(pos, p1, ToU32(bg));
	draw->AddRect(pos, p1, ToU32(hovered || selected ? Theme::Accent : Theme::Border));
	DrawSpacedText(draw, font, ImVec2(pos.x + Metrics::chip.padH * scale, pos.y + Metrics::chip.padV * scale),
		ToU32(Theme::Accent), label, 0.5f * scale);
	return clicked;
}

} // namespace Ui
} // namespace SourceFilmmaker
