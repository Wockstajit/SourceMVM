#include "InspectorCards.h"

#include "UiWidgets.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>

namespace SourceFilmmaker {
namespace Ui {

namespace {

// CS2's card palette (CameraEditorCardsJs.h, CameraEditorJs.h) in CS:S paint: the label and value
// tiers are the off-white body text, summaries and hints the dim text, grounds translucent black,
// selection the spectator orange.
namespace Paint {
constexpr Color Value = Theme::Text;
constexpr Color Label = Theme::Text;
constexpr Color Muted = Theme::TextDim;
constexpr Color Disabled = Theme::TextDisabled;
constexpr Color TileBg = kCardTileBg;
constexpr Color TileHover = Rgba(255, 176, 0, 24);
constexpr Color Outline = Theme::Border;
constexpr Color TrackBg = Rgba(255, 255, 255, 36);
constexpr Color SegSel = Rgba(255, 176, 0, 46);
constexpr Color SegSelHover = Rgba(255, 176, 0, 64);
constexpr Color Hover = Rgba(255, 255, 255, 13);
constexpr Color Down = Rgba(255, 255, 255, 20);
constexpr Color RailSel = Rgba(255, 176, 0, 28);
constexpr Color RailSelBorder = Rgba(255, 176, 0, 89);
constexpr Color RailHover = Rgba(255, 255, 255, 12);
constexpr Color PrimaryBg = Rgba(255, 176, 0, 31);
constexpr Color PrimaryHover = Rgba(255, 176, 0, 51);
constexpr Color PrimaryBorder = Rgba(255, 176, 0, 128);
constexpr Color DangerText = Rgba(255, 96, 72);
constexpr Color DangerBorder = Rgba(192, 28, 0, 200);
constexpr Color DangerHover = Rgba(192, 28, 0, 60);
constexpr Color KeyCapBg = Rgba(255, 255, 255, 16);
constexpr Color KeyCapBorder = Rgba(255, 255, 255, 64);
constexpr Color Rule = Rgba(255, 255, 255, 20);
constexpr Color EdgeOff = Rgba(42, 48, 55); // a switched-off card's edge (CS2's edgeOff)
} // namespace Paint

std::string Upper(const char* text) {
	std::string out = text ? text : "";
	for (char& ch : out)
		if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
	return out;
}

ImVec2 TextSize(ImFont* font, const char* text, float wrap = 0.0f) {
	return font->CalcTextSizeA(font->FontSize, FLT_MAX, wrap, text);
}

void Text(ImDrawList* draw, ImFont* font, ImVec2 pos, Color color, const char* text, float wrap = 0.0f) {
	draw->AddText(font, font->FontSize, pos, ToU32(color), text, nullptr, wrap);
}

void CenteredText(ImDrawList* draw, ImFont* font, const Rect& r, Color color, const char* text) {
	const ImVec2 size = TextSize(font, text);
	Text(draw, font, ImVec2(std::floor(r.x0 + (r.W() - size.x) * 0.5f), std::floor(r.y0 + (r.H() - size.y) * 0.5f)), color, text);
}

// Press state of an invisible button covering r. hovered and held are false while disabled.
struct Hit {
	bool clicked = false;
	bool hovered = false;
	bool held = false;
};

Hit HitArea(const char* id, const Rect& r, bool enabled) {
	Hit h;
	if (!r.Valid()) return h;
	ImGui::SetCursorScreenPos(ImVec2(r.x0, r.y0));
	h.clicked = ImGui::InvisibleButton(id, ImVec2(r.W(), r.H())) && enabled;
	h.hovered = enabled && ImGui::IsItemHovered();
	h.held = enabled && ImGui::IsItemActive();
	return h;
}

void Box(ImDrawList* draw, const Rect& r, Color bg, Color border) {
	if (bg.a) draw->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), ToU32(bg));
	if (border.a) draw->AddRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), ToU32(border));
}

std::string FormatValue(double v, int decimals, const char* unit) {
	char buf[48];
	std::snprintf(buf, sizeof(buf), "%.*f%s", decimals, v, unit ? unit : "");
	return buf;
}

double Quantize(double v, int decimals) {
	const double mul = std::pow(10.0, decimals);
	return std::round(v * mul) / mul;
}

// CS2's square switch in CS:S paint: a track, and a square knob that slides right when on.
void DrawSwitch(ImDrawList* draw, const Rect& track, bool on, bool enabled, float knob) {
	const Color edge = !enabled ? Faded(Paint::Outline, 0.45f) : (on ? Theme::Accent : Paint::Outline);
	Box(draw, track, on && enabled ? Paint::SegSel : Rgba(0, 0, 0, 0), edge);
	const float inset = (track.H() - knob) * 0.5f;
	const float kx = on ? track.x1 - inset - knob : track.x0 + inset;
	draw->AddRectFilled(ImVec2(kx, track.y0 + inset), ImVec2(kx + knob, track.y0 + inset + knob),
		ToU32(!enabled ? Paint::Disabled : (on ? Theme::Accent : Paint::Muted)));
}

} // namespace

float CardButtonWidth(const char* caption, ImFont* font, float s) { return TextSize(font, caption).x + 22.0f * s; }

bool CardButton(const char* id, const char* caption, const Rect& r, ButtonKind kind, bool enabled, ImFont* font) {
	ImGui::PushID(id);
	const Hit h = HitArea("button", r, enabled);
	ImGui::PopID();
	Color bg = Rgba(0, 0, 0, 0), border = Paint::Outline, text = Paint::Label;
	switch (kind) {
	case ButtonKind::Primary:
		bg = h.hovered || h.held ? Paint::PrimaryHover : Paint::PrimaryBg;
		border = Paint::PrimaryBorder;
		text = Theme::Accent;
		break;
	case ButtonKind::Danger:
		bg = h.hovered || h.held ? Paint::DangerHover : Rgba(0, 0, 0, 0);
		border = Paint::DangerBorder;
		text = Paint::DangerText;
		break;
	case ButtonKind::Solid:
		bg = h.held ? Theme::ButtonArmedBg : (h.hovered ? Rgba(255, 196, 64) : Theme::Accent);
		border = Rgba(0, 0, 0, 0);
		text = Palette::Black;
		break;
	default:
		bg = h.held ? Paint::Down : (h.hovered ? Paint::Hover : Theme::ButtonBg);
		break;
	}
	if (!enabled) {
		bg = Rgba(0, 0, 0, 0);
		border = Faded(border, 0.45f);
		text = Paint::Disabled;
	}
	ImDrawList* draw = ImGui::GetWindowDrawList();
	Box(draw, r, bg, border);
	CenteredText(draw, font, r, text, caption);
	return h.clicked;
}

int CardHeader(CardColumn& c, const char* id, const char* title, const char* summary, const CardAction* buttons, int count) {
	return CardHeader(c, id, title, summary, buttons, count, Theme::Accent, CardSwitch());
}

int CardHeader(CardColumn& c, const char* id, const char* title, const char* summary, const CardAction* buttons, int count,
	Color edge, CardSwitch master) {
	const float s = c.s;
	ImDrawList* draw = ImGui::GetWindowDrawList();
	ImFont* titleFont = c.Font(FontRole::CardTitle);
	ImFont* noteFont = c.Font(FontRole::Note);
	ImFont* buttonFont = c.Font(FontRole::Button);
	const float btnH = buttonFont->FontSize + 10 * s;
	const float swW = Metrics::sw.w * s, swH = Metrics::sw.h * s;
	float slotsW = master.show ? swW + 8 * s : 0;
	for (int i = 0; i < count; ++i) slotsW += CardButtonWidth(buttons[i].caption, buttonFont, s) + 8 * s;
	const float edgeW = 3 * s, edgeH = 30 * s;
	const float textX = c.x0 + edgeW + 10 * s;
	const std::string caption = Upper(title);
	const float titleW = SpacedTextWidth(titleFont, caption.c_str(), 1.2f * s);
	// The buttons share the title's line while the title fits beside them; otherwise they get a line
	// of their own under the header, so neither is cut.
	const bool ownLine = slotsW > 0 && textX + titleW > c.x1 - slotsW;
	const float textR = ownLine ? c.x1 : c.x1 - slotsW;
	const bool hasSummary = summary && *summary;
	// The summary keeps two lines whatever it says, so the rows under the header stay put as it
	// changes (CS2 cuts it to one line; here nothing is cut). Longer text still wraps on.
	const float twoLines = 2 * noteFont->FontSize;
	float summaryH = hasSummary ? TextSize(noteFont, summary, textR - textX).y : 0;
	if (hasSummary) summaryH = (summaryH > twoLines ? summaryH : twoLines) + 2 * s;
	const float contentH = titleFont->FontSize + summaryH;
	const float blockH = contentH > edgeH ? contentH : edgeH;
	draw->AddRectFilled(ImVec2(c.x0, c.y + (blockH - edgeH) * 0.5f), ImVec2(c.x0 + edgeW, c.y + (blockH + edgeH) * 0.5f),
		ToU32(master.show && !master.on ? Paint::EdgeOff : edge));
	const float textY = c.y + (blockH - contentH) * 0.5f;
	DrawSpacedText(draw, titleFont, ImVec2(textX, textY), ToU32(Paint::Value), caption.c_str(), 1.2f * s);
	if (hasSummary)
		Text(draw, noteFont, ImVec2(textX, textY + titleFont->FontSize + 2 * s), Paint::Muted, summary, textR - textX);
	int clicked = -1;
	ImGui::PushID(id);
	float bx = c.x1 - slotsW + 8 * s;
	float by = c.y + (blockH - btnH) * 0.5f;
	if (ownLine) {
		bx = c.x1 - slotsW + 8 * s;
		by = c.y + blockH + 6 * s;
	}
	for (int i = 0; i < count; ++i) {
		const float w = CardButtonWidth(buttons[i].caption, buttonFont, s);
		ImGui::PushID(i);
		if (CardButton("slot", buttons[i].caption, MakeRect(bx, by, bx + w, by + btnH), buttons[i].kind, buttons[i].enabled, buttonFont))
			clicked = i;
		ImGui::PopID();
		bx += w + 8 * s;
	}
	if (master.show) {
		const float sy = ownLine ? by + (btnH - swH) * 0.5f : c.y + (blockH - swH) * 0.5f;
		const Rect track = MakeRect(bx, sy, bx + swW, sy + swH);
		const Hit h = HitArea("switch", track, true);
		DrawSwitch(draw, track, master.on, true, Metrics::sw.knob * s);
		if (h.hovered) draw->AddRect(ImVec2(track.x0, track.y0), ImVec2(track.x1, track.y1), ToU32(Paint::Value));
		if (h.clicked) clicked = count;
	}
	ImGui::PopID();
	c.y += blockH + (ownLine ? btnH + 6 * s : 0) + 6 * s;
	return clicked;
}

float BeginCardScroll(const char* id, const Rect& r, float& width) {
	ImGui::SetCursorScreenPos(ImVec2(r.x0, r.y0));
	ImGui::BeginChild(id, ImVec2(r.W(), r.H()), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
	// One pixel in from the clip, so an outline on the right edge is not cut off.
	width = r.W() - 1.0f - (ImGui::GetScrollMaxY() > 0 ? ImGui::GetStyle().ScrollbarSize : 0.0f);
	return ImGui::GetCursorScreenPos().y;
}

void EndCardScroll(float x0, float contentBottom) {
	ImGui::SetCursorScreenPos(ImVec2(x0, contentBottom));
	ImGui::Dummy(ImVec2(1, 1)); // the content's height, for the scroll range
	ImGui::EndChild();
}

void SubHead(CardColumn& c, const char* text) {
	ImFont* font = c.Font(FontRole::Caption);
	c.y += 10 * c.s;
	DrawSpacedText(ImGui::GetWindowDrawList(), font, ImVec2(c.x0, c.y), ToU32(Paint::Muted), Upper(text).c_str(), 1.5f * c.s);
	c.y += font->FontSize + 1 * c.s;
}

void Note(CardColumn& c, const char* text) { Note(c, text, Paint::Muted); }

void Note(CardColumn& c, const char* text, Color color) {
	if (!text || !*text) return;
	ImFont* font = c.Font(FontRole::Note);
	const float pad = Metrics::line.padH * c.s;
	c.y += 6 * c.s;
	const float wrap = c.W() - 2 * pad;
	Text(ImGui::GetWindowDrawList(), font, ImVec2(c.x0 + pad, c.y), color, text, wrap);
	c.y += TextSize(font, text, wrap).y;
}

ValueEdit ValueRow(CardColumn& c, const char* id, const char* label, double value, double min, double max, int decimals,
	const char* unit, Color fill, bool enabled) {
	return ValueRow(c, id, label, value, min, max, decimals, unit, fill, enabled, ValueRowStyle());
}

ValueEdit ValueRow(CardColumn& c, const char* id, const char* label, double value, double min, double max, int decimals,
	const char* unit, Color fill, bool enabled, const ValueRowStyle& style) {
	const float s = c.s;
	const float ink = style.dim ? 0.4f : 1.0f; // a dimmed row keeps working at 40% (CS2's opacity 0.4)
	ValueEdit out;
	out.value = value;
	ImFont* labelFont = c.Font(FontRole::RowLabel);
	ImFont* valueFont = c.Font(FontRole::Value);
	const float pad = Metrics::line.padH * s, labelW = Metrics::line.label * s;
	const float valueW = Metrics::line.value * s, colGap = Metrics::line.colGap * s;
	c.y += Metrics::line.gap * s;
	const float labelH = TextSize(labelFont, label, labelW).y + 8 * s;
	const float rowH = labelH > Metrics::line.h * s ? labelH : Metrics::line.h * s;
	const Rect row = MakeRect(c.x0, c.y, c.x1, c.y + rowH);
	const float barX0 = row.x0 + pad + labelW + colGap, barX1 = row.x1 - pad - valueW - colGap;
	const float barW = barX1 - barX0;
	const double span = max - min > 0 ? max - min : 1.0;

	ImGui::PushID(id);
	const Hit h = HitArea("row", row, enabled);
	// The drag is RELATIVE (CS2's edValueTile): the value moves with the pointer from where it was and
	// never jumps to the click; Shift is fine control. The unrounded value rides the gesture in ImGui's
	// storage, so slow drags still add up.
	ImGuiStorage* storage = ImGui::GetStateStorage();
	const ImGuiID key = ImGui::GetItemID(), movedKey = ImGui::GetID("moved");
	bool moved = false;
	if (enabled && ImGui::IsItemActivated()) {
		storage->SetFloat(key, (float)value);
		storage->SetBool(movedKey, false);
		out.began = true;
	}
	if (h.held) {
		const ImGuiIO& io = ImGui::GetIO();
		float raw = storage->GetFloat(key, (float)value);
		if (io.MouseDelta.x != 0.0f && barW > 0) {
			raw += (float)(io.MouseDelta.x * span / barW * (io.KeyShift ? style.fine : 1.0));
			raw = raw < (float)min ? (float)min : (raw > (float)max ? (float)max : raw);
			storage->SetFloat(key, raw);
			storage->SetBool(movedKey, true);
		}
		moved = storage->GetBool(movedKey, false);
		const double q = Quantize(raw, decimals);
		if (q != value) {
			out.changed = true;
			out.value = q;
		}
	}
	if (ImGui::IsItemDeactivated()) out.ended = true;
	ImGui::PopID();

	ImDrawList* draw = ImGui::GetWindowDrawList();
	Box(draw, row, h.hovered || h.held ? Paint::TileHover : Paint::TileBg, moved ? Faded(Theme::Accent, 0.5f) : Rgba(0, 0, 0, 0));
	const Color text = enabled ? Faded(Paint::Label, ink) : Paint::Disabled;
	Text(draw, labelFont, ImVec2(row.x0 + pad, row.y0 + (rowH - (labelH - 8 * s)) * 0.5f), text, label, labelW);
	if (barW > 0) {
		double frac = (out.value - min) / span;
		frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);
		const float cy = row.y0 + rowH * 0.5f;
		draw->AddRectFilled(ImVec2(barX0, cy - 2 * s), ImVec2(barX1, cy + 2 * s), ToU32(Faded(Paint::TrackBg, ink)));
		if (style.centreTick) {
			const float mx = std::floor((barX0 + barX1) * 0.5f);
			draw->AddRectFilled(ImVec2(mx, cy - 5 * s), ImVec2(mx + 1, cy + 5 * s), ToU32(Faded(Rgba(255, 255, 255, 64), ink)));
		}
		const float fx = barX0 + (float)frac * barW;
		if (!style.noFill)
			draw->AddRectFilled(ImVec2(barX0, cy - 2 * s), ImVec2(fx, cy + 2 * s), ToU32(Faded(enabled ? fill : Faded(fill, 0.35f), ink)));
		const float knob = 4 * s;
		draw->AddRectFilled(ImVec2(fx - knob, cy - 7 * s), ImVec2(fx + knob, cy + 7 * s),
			ToU32(enabled ? Faded(style.knob, ink) : Paint::Disabled));
	}
	const std::string shown = style.atMin && min == 0 && out.value <= min ? std::string(style.atMin) : FormatValue(out.value, decimals, unit);
	const ImVec2 vs = TextSize(valueFont, shown.c_str());
	Text(draw, valueFont, ImVec2(row.x1 - pad - vs.x, row.y0 + (rowH - vs.y) * 0.5f), enabled ? Faded(Paint::Value, ink) : Paint::Disabled,
		shown.c_str());
	c.y += rowH;
	return out;
}

int ModeRow(CardColumn& c, const char* id, const char* label, const char* const* captions, int count, int selected,
	bool enabled) {
	const float s = c.s;
	ImFont* labelFont = c.Font(FontRole::RowLabel);
	ImFont* capFont = c.Font(FontRole::Button);
	const float pad = Metrics::line.padH * s, labelW = Metrics::line.label * s, colGap = Metrics::line.colGap * s;
	const float cellH = capFont->FontSize + 12 * s;
	float widest = 0;
	for (int i = 0; i < count; ++i) {
		const float w = TextSize(capFont, captions[i]).x + 8 * s;
		widest = w > widest ? w : widest;
	}
	// Every cell is as wide as the widest caption needs; strips that do not fit beside the label go
	// under it (edCaptionsFit / edStackRow).
	const bool stacked = widest * count > c.W() - 2 * pad - labelW - colGap;
	c.y += Metrics::line.gap * s;
	float rowH, stripX0, stripY, labelY;
	const float labelTextH = TextSize(labelFont, label, stacked ? c.W() - 2 * pad : labelW).y;
	if (!stacked) {
		const float inner = (labelTextH + 4 * s) > cellH ? labelTextH + 4 * s : cellH;
		rowH = inner + 6 * s > Metrics::line.h * s ? inner + 6 * s : Metrics::line.h * s;
		stripX0 = c.x0 + pad + labelW + colGap;
		stripY = c.y + (rowH - cellH) * 0.5f;
		labelY = c.y + (rowH - labelTextH) * 0.5f;
	} else {
		rowH = 3 * s + labelTextH + 4 * s + cellH + 3 * s;
		stripX0 = c.x0 + pad;
		labelY = c.y + 3 * s;
		stripY = labelY + labelTextH + 4 * s;
	}
	const Rect row = MakeRect(c.x0, c.y, c.x1, c.y + rowH);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const bool rowHot = enabled && ImGui::IsMouseHoveringRect(ImVec2(row.x0, row.y0), ImVec2(row.x1, row.y1));
	Box(draw, row, rowHot ? Paint::TileHover : Paint::TileBg, Rgba(0, 0, 0, 0));
	Text(draw, labelFont, ImVec2(c.x0 + pad, labelY), enabled ? Paint::Label : Paint::Disabled, label,
		stacked ? c.W() - 2 * pad : labelW);
	const Rect strip = MakeRect(stripX0, stripY, c.x1 - pad, stripY + cellH);
	const float cellW = count > 0 ? strip.W() / count : 0;
	int clicked = -1;
	ImGui::PushID(id);
	for (int i = 0; i < count; ++i) {
		const Rect cell = MakeRect(strip.x0 + cellW * i, strip.y0, strip.x0 + cellW * (i + 1), strip.y1);
		ImGui::PushID(i);
		const Hit h = HitArea("cell", cell, enabled);
		ImGui::PopID();
		if (h.clicked) clicked = i;
		const bool on = i == selected;
		Color bg = Rgba(0, 0, 0, 0);
		if (on) bg = h.hovered ? Paint::SegSelHover : Paint::SegSel;
		else if (h.held) bg = Paint::Down;
		else if (h.hovered) bg = Paint::Hover;
		Box(draw, cell, bg, Rgba(0, 0, 0, 0));
		CenteredText(draw, capFont, cell, !enabled ? Paint::Disabled : (on ? Paint::Value : Paint::Muted), captions[i]);
	}
	ImGui::PopID();
	Box(draw, strip, Rgba(0, 0, 0, 0), enabled ? Paint::Outline : Faded(Paint::Outline, 0.45f));
	c.y += rowH;
	return clicked;
}

int Segmented(CardColumn& c, const char* id, const char* const* captions, int count, int selected, const bool* live) {
	const float s = c.s;
	ImFont* capFont = c.Font(FontRole::Button);
	const float cellH = capFont->FontSize + 12 * s;
	const Rect strip = MakeRect(c.x0, c.y, c.x1, c.y + cellH);
	const float cellW = count > 0 ? strip.W() / count : 0;
	ImDrawList* draw = ImGui::GetWindowDrawList();
	int clicked = -1;
	ImGui::PushID(id);
	for (int i = 0; i < count; ++i) {
		const bool on = !live || live[i];
		const Rect cell = MakeRect(strip.x0 + cellW * i, strip.y0, strip.x0 + cellW * (i + 1), strip.y1);
		ImGui::PushID(i);
		const Hit h = HitArea("cell", cell, on);
		ImGui::PopID();
		if (h.clicked) clicked = i;
		const bool sel = i == selected;
		Color bg = Rgba(0, 0, 0, 0);
		if (sel) bg = h.hovered ? Paint::SegSelHover : Paint::SegSel;
		else if (h.held) bg = Paint::Down;
		else if (h.hovered) bg = Paint::Hover;
		Box(draw, cell, bg, Rgba(0, 0, 0, 0));
		const Color ink = sel ? Paint::Value : Paint::Muted;
		CenteredText(draw, capFont, cell, on ? ink : Faded(ink, 0.35f), captions[i]);
	}
	ImGui::PopID();
	Box(draw, strip, Rgba(0, 0, 0, 0), Paint::Outline);
	c.y += cellH;
	return clicked;
}

int CellRow(CardColumn& c, const char* id, const char* const* captions, int count, int selected, int columns) {
	const float s = c.s;
	ImFont* capFont = c.Font(FontRole::Button);
	const float cellH = capFont->FontSize + 10 * s, gap = 4 * s;
	ImDrawList* draw = ImGui::GetWindowDrawList();
	float x = c.x0, y = c.y;
	int clicked = -1;
	ImGui::PushID(id);
	for (int i = 0; i < count; ++i) {
        const float w = columns>0 ? (c.W()-gap*(columns-1))/columns : TextSize(capFont,captions[i]).x+20*s;
        if ((columns>0 && i>0 && i%columns==0) || (columns<=0 && x>c.x0 && x+w>c.x1)) { // wrap
			x = c.x0;
			y += cellH + gap;
		}
		const Rect cell = MakeRect(x, y, x + w, y + cellH);
		ImGui::PushID(i);
		const Hit h = HitArea("cell", cell, true);
		ImGui::PopID();
		if (h.clicked) clicked = i;
		const bool sel = i == selected;
		Box(draw, cell, sel ? (h.hovered ? Paint::SegSelHover : Paint::SegSel) : (h.hovered ? Paint::Hover : Rgba(0, 0, 0, 0)),
			sel ? Theme::Accent : (h.hovered ? Faded(Paint::Value, 0.25f) : Paint::Outline));
		CenteredText(draw, capFont, cell, sel || h.hovered ? Paint::Value : Paint::Label, captions[i]);
		x += w + gap;
	}
	ImGui::PopID();
	c.y = y + cellH;
	return clicked;
}

int ActionRow(CardColumn& c, const char* id, const char* heading, const CardAction* buttons, int count) {
	const float s = c.s;
	ImFont* headFont = c.Font(FontRole::Caption);
	ImFont* buttonFont = c.Font(FontRole::Button);
	c.y += 10 * s;
	if (heading && *heading) {
		DrawSpacedText(ImGui::GetWindowDrawList(), headFont, ImVec2(c.x0, c.y), ToU32(Paint::Muted), Upper(heading).c_str(), 1.5f * s);
		c.y += headFont->FontSize + 6 * s;
	}
	const float btnH = 32 * s, gap = 6 * s;
	const float equalW = count > 0 ? (c.W() - gap * (count - 1)) / count : 0;
	bool fits = true;
	for (int i = 0; i < count; ++i) fits = fits && CardButtonWidth(buttons[i].caption, buttonFont, s) <= equalW;
	int clicked = -1;
	ImGui::PushID(id);
	for (int i = 0; i < count; ++i) {
		const Rect r = fits ? MakeRect(c.x0 + (equalW + gap) * i, c.y, c.x0 + (equalW + gap) * i + equalW, c.y + btnH)
		                    : MakeRect(c.x0, c.y + (btnH + gap) * i, c.x1, c.y + (btnH + gap) * i + btnH);
		ImGui::PushID(i);
		if (CardButton("action", buttons[i].caption, r, buttons[i].kind, buttons[i].enabled, buttonFont)) clicked = i;
		ImGui::PopID();
	}
	ImGui::PopID();
	c.y += fits ? btnH : (btnH + gap) * count - gap;
	return clicked;
}

int LabeledActionRow(CardColumn& c, const char* id, const char* label, const CardAction* buttons, int count) {
	const float s = c.s;
	ImFont* labelFont = c.Font(FontRole::RowLabel);
	ImFont* buttonFont = c.Font(FontRole::Button);
	const float pad = Metrics::line.padH * s, labelW = Metrics::line.label * s, colGap = Metrics::line.colGap * s;
	const float btnH = 32 * s, gap = 6 * s;
	c.y += (Metrics::line.gap + 3) * s;
	const float x0 = c.x0 + pad + labelW + colGap, w = c.x1 - x0;
	const float equalW = count > 0 ? (w - gap * (count - 1)) / count : 0;
	bool fits = true;
	for (int i = 0; i < count; ++i) fits = fits && CardButtonWidth(buttons[i].caption, buttonFont, s) <= equalW;
	// The label is centred on the first row of buttons (edActionGroup's 32 px box).
	const float labelH = TextSize(labelFont, label, labelW).y;
	Text(ImGui::GetWindowDrawList(), labelFont, ImVec2(c.x0 + pad, c.y + (btnH - labelH) * 0.5f), Paint::Label, label, labelW);
	int clicked = -1;
	ImGui::PushID(id);
	for (int i = 0; i < count; ++i) {
		const Rect r = fits ? MakeRect(x0 + (equalW + gap) * i, c.y, x0 + (equalW + gap) * i + equalW, c.y + btnH)
		                    : MakeRect(x0, c.y + (btnH + gap) * i, c.x1, c.y + (btnH + gap) * i + btnH);
		ImGui::PushID(i);
		if (CardButton("action", buttons[i].caption, r, buttons[i].kind, buttons[i].enabled, buttonFont)) clicked = i;
		ImGui::PopID();
	}
	ImGui::PopID();
	c.y += fits || count == 0 ? btnH : (btnH + gap) * count - gap;
	return clicked;
}

bool SwitchRow(CardColumn& c, const char* id, const char* label, bool on, bool enabled) {
	const float s = c.s;
	ImFont* labelFont = c.Font(FontRole::RowLabel);
	const float pad = Metrics::line.padH * s, labelW = Metrics::line.label * s;
	c.y += Metrics::line.gap * s;
	const float labelH = TextSize(labelFont, label, labelW).y + 8 * s;
	const float rowH = labelH > Metrics::line.h * s ? labelH : Metrics::line.h * s;
	const Rect row = MakeRect(c.x0, c.y, c.x1, c.y + rowH);
	ImGui::PushID(id);
	const Hit h = HitArea("row", row, enabled);
	ImGui::PopID();
	ImDrawList* draw = ImGui::GetWindowDrawList();
	Box(draw, row, h.hovered || h.held ? Paint::TileHover : Paint::TileBg, Rgba(0, 0, 0, 0));
	Text(draw, labelFont, ImVec2(row.x0 + pad, row.y0 + (rowH - (labelH - 8 * s)) * 0.5f),
		!enabled ? Paint::Disabled : (on ? Paint::Value : Paint::Label), label, labelW);
	// CS2's small square switch (swSm), right in the value column: a track and a knob that slides.
	const float sw = Metrics::swSm.w * s, sh = Metrics::swSm.h * s;
	const Rect track = MakeRect(row.x1 - pad - sw, row.y0 + (rowH - sh) * 0.5f, row.x1 - pad, row.y0 + (rowH + sh) * 0.5f);
	DrawSwitch(draw, track, on, enabled, Metrics::swSm.knob * s);
	c.y += rowH;
	return h.clicked;
}

DropClick DropRow(CardColumn& c, const char* id, const char* label, const char* shown, Color shownColor, const char* side,
	bool enabled) {
	const float s = c.s;
	DropClick out;
	ImFont* labelFont = c.Font(FontRole::RowLabel);
	ImFont* valueFont = c.Font(FontRole::Value);
	ImFont* buttonFont = c.Font(FontRole::Button);
	const float pad = Metrics::line.padH * s, labelW = Metrics::line.label * s, colGap = Metrics::line.colGap * s;
	const float fieldH = 30 * s;
	c.y += (Metrics::line.gap + 1) * s;
	const float sideW = side ? CardButtonWidth(side, buttonFont, s) : 0;
	const float fieldX0 = c.x0 + pad + labelW + colGap, fieldX1 = c.x1 - (side ? sideW + 6 * s : 0);
	// The field's text wraps rather than being cut (a long target line takes a second line).
	const float caret = 14 * s, textW = fieldX1 - fieldX0 - 10 * s - caret;
	const float textH = TextSize(valueFont, shown, textW).y;
	const float rowH = textH + 10 * s > fieldH ? textH + 10 * s : fieldH;
	const Rect field = MakeRect(fieldX0, c.y, fieldX1, c.y + rowH);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const float labelH = TextSize(labelFont, label, labelW).y;
	Text(draw, labelFont, ImVec2(c.x0 + pad, c.y + (rowH - labelH) * 0.5f), enabled ? Paint::Label : Paint::Disabled, label, labelW);
	ImGui::PushID(id);
	const Hit h = HitArea("field", field, enabled);
	if (side && CardButton("side", side, MakeRect(c.x1 - sideW, c.y + (rowH - fieldH) * 0.5f, c.x1, c.y + (rowH + fieldH) * 0.5f),
		ButtonKind::Normal, enabled, buttonFont))
		out.side = true;
	ImGui::PopID();
	out.field = h.clicked;
	out.fieldRect = field;
	draw->AddRectFilled(ImVec2(field.x0, field.y0), ImVec2(field.x1, field.y1), ToU32(h.hovered || h.held ? Paint::TileHover : Paint::TileBg),
		4 * s);
	draw->AddRect(ImVec2(field.x0, field.y0), ImVec2(field.x1, field.y1), ToU32(enabled ? Paint::Outline : Faded(Paint::Outline, 0.45f)), 4 * s);
	Text(draw, valueFont, ImVec2(field.x0 + 10 * s, field.y0 + (rowH - textH) * 0.5f), enabled ? shownColor : Paint::Disabled, shown, textW);
	const float cx = field.x1 - 6 * s - caret * 0.5f, cy = field.y0 + rowH * 0.5f;
	draw->AddTriangleFilled(ImVec2(cx - 4 * s, cy - 2 * s), ImVec2(cx + 4 * s, cy - 2 * s), ImVec2(cx, cy + 3 * s),
		ToU32(enabled ? Paint::Label : Paint::Disabled));
	c.y += rowH;
	return out;
}

namespace {
int DropListImpl(const char* id, const Rect& field, const std::vector<DropOption>& options, const FontSet& fonts, float s,
	float clientW, float clientH, bool& closed, int* deleted);
} // namespace

int DropList(const char* id, const Rect& field, const std::vector<DropOption>& options, const FontSet& fonts, float s,
	float clientW, float clientH, bool& closed) {
	return DropListImpl(id, field, options, fonts, s, clientW, clientH, closed, nullptr);
}

int DropList(const char* id, const Rect& field, const std::vector<DropOption>& options, const FontSet& fonts, float s,
	float clientW, float clientH, bool& closed, int& deleted) {
	deleted = -1;
	return DropListImpl(id, field, options, fonts, s, clientW, clientH, closed, &deleted);
}

namespace {

int DropListImpl(const char* id, const Rect& field, const std::vector<DropOption>& options, const FontSet& fonts, float s,
	float clientW, float clientH, bool& closed, int* deleted) {
	closed = false;
	ImFont* font = fonts.Get(FontRole::Value);
	const float itemPad = 8 * s, gap = 2 * s, maxH = 340 * s;
	const float w = field.W();
	float contentH = 2 * gap;
	for (const DropOption& o : options) contentH += TextSize(font, o.text.c_str(), w - 2 * itemPad).y + 2 * itemPad * 0.75f;
	const float h = contentH < maxH ? contentH : maxH;
	float y = field.y1 + 2 * s;
	if (y + h > clientH - 2 && field.y0 - 2 * s - h >= 2) y = field.y0 - 2 * s - h; // no room below: above the field
	const Rect menu = MakeRect(field.x0, y, field.x1, y + h);

	// A click anywhere outside the list closes it, and is not passed on.
	const std::string catchName = std::string(id) + "catch", menuName = std::string(id) + "menu";
	BeginOverlay(catchName.c_str(), MakeRect(0, 0, clientW, clientH), Rgba(0, 0, 0, 0));
	ImGui::SetCursorScreenPos(ImVec2(0, 0));
	if (ImGui::InvisibleButton("##catch", ImVec2(clientW, clientH))) closed = true;
	ImGui::End();

	int picked = -1;
	BeginOverlay(menuName.c_str(), menu, Rgba(0, 0, 0, 0));
	ImDrawList* draw = ImGui::GetWindowDrawList();
	// Opaque, so the rows under the list do not show through it.
	draw->AddRectFilled(ImVec2(menu.x0, menu.y0), ImVec2(menu.x1, menu.y1), ToU32(Rgba(10, 10, 10)), 4 * s);
	draw->AddRect(ImVec2(menu.x0, menu.y0), ImVec2(menu.x1, menu.y1), ToU32(Paint::Outline), 4 * s);
	ImGui::SetCursorScreenPos(ImVec2(menu.x0, menu.y0));
	// No window padding: the entries start at the list's top, as its height counts them.
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	const bool scrolls = contentH > maxH;
	ImGui::BeginChild("##list", ImVec2(menu.W(), menu.H()), ImGuiChildFlags_None,
		ImGuiWindowFlags_NoBackground | (scrolls ? 0 : ImGuiWindowFlags_NoScrollbar));
	ImGui::PopStyleVar();
	// Rows wrap at the width they were measured at, less the scrollbar only when there is one: a row
	// that wraps once more than measured would run off the list's end.
	const float innerW = menu.W() - (scrolls ? ImGui::GetStyle().ScrollbarSize : 0.0f);
	const float delW = deleted ? 22 * s : 0; // the x's column
	ImDrawList* list = ImGui::GetWindowDrawList();
	float iy = ImGui::GetCursorScreenPos().y + gap;
	for (int i = 0; i < (int)options.size(); ++i) {
		const DropOption& o = options[(size_t)i];
		const float th = TextSize(font, o.text.c_str(), innerW - 2 * itemPad - delW).y;
		const Rect row = MakeRect(menu.x0 + gap, iy, menu.x0 + innerW - gap, iy + th + 2 * itemPad * 0.75f);
		const Rect item = MakeRect(row.x0, row.y0, row.x1 - delW, row.y1);
		ImGui::PushID(i);
		const Hit hit = HitArea("item", item, true);
		Hit del;
		if (deleted) del = HitArea("delete", MakeRect(item.x1, row.y0, row.x1, row.y1), true);
		ImGui::PopID();
		if (o.selected || hit.hovered)
			list->AddRectFilled(ImVec2(row.x0, row.y0), ImVec2(row.x1, row.y1), ToU32(o.selected ? Paint::SegSel : Paint::Hover), 3 * s);
		Text(list, font, ImVec2(item.x0 + itemPad, item.y0 + itemPad * 0.75f), o.selected ? Theme::Accent : o.color, o.text.c_str(),
			innerW - 2 * itemPad - delW);
		if (deleted) {
			const float cx = (item.x1 + row.x1) * 0.5f, cy = (row.y0 + row.y1) * 0.5f, k = 3.5f * s;
			const ImU32 ink = ToU32(del.hovered ? Paint::DangerText : Paint::Muted);
			list->AddLine(ImVec2(cx - k, cy - k), ImVec2(cx + k, cy + k), ink, 1.5f * s);
			list->AddLine(ImVec2(cx - k, cy + k), ImVec2(cx + k, cy - k), ink, 1.5f * s);
			if (del.clicked) *deleted = i;
		}
		if (hit.clicked) {
			picked = i;
			closed = true;
		}
		iy = row.y1;
	}
	// The content ends where the list does, so a list that fits has nothing to scroll.
	ImGui::SetCursorScreenPos(ImVec2(menu.x0, iy + gap));
	ImGui::Dummy(ImVec2(1, 0));
	ImGui::EndChild();
	ImGui::End();
	return picked;
}

} // namespace

void RailGroup(CardColumn& c, const char* name, bool first) {
	ImFont* font = c.Font(FontRole::Label);
	if (!first) c.y += Metrics::rail.groupGap * c.s;
	DrawSpacedText(ImGui::GetWindowDrawList(), font, ImVec2(c.x0 + 2 * c.s, c.y), ToU32(Paint::Muted), Upper(name).c_str(), 1.8f * c.s);
	c.y += font->FontSize + 3 * c.s;
}

namespace {

// A spaced caption broken at spaces into lines no wider than width (a word wider than the line
// keeps a line of its own: nothing is cut).
std::vector<std::string> WrapSpaced(ImFont* font, const std::string& text, float spacing, float width) {
	std::vector<std::string> lines;
	std::string line;
	size_t i = 0;
	while (i < text.size()) {
		size_t j = text.find(' ', i);
		if (j == std::string::npos) j = text.size();
		const std::string word = text.substr(i, j - i);
		const std::string tried = line.empty() ? word : line + " " + word;
		if (!line.empty() && SpacedTextWidth(font, tried.c_str(), spacing) > width) {
			lines.push_back(line);
			line = word;
		} else {
			line = tried;
		}
		i = j + 1;
	}
	if (!line.empty() || lines.empty()) lines.push_back(line);
	return lines;
}

} // namespace

namespace {

RailHit RailRowDraw(CardColumn& c, const char* id, const char* title, const char* summary, bool selected, Color accent,
	const char* badge, Color badgeColor, CardSwitch master) {
	const float s = c.s;
	ImFont* titleFont = c.Font(FontRole::Title);
	ImFont* sumFont = c.Font(FontRole::RailSum);
	ImFont* badgeFont = c.Font(FontRole::Caption);
	c.y += Metrics::rail.rowGap * s;
	const float edge = Metrics::rail.edge * s;
	// CS2's badge is a column of its own at the row's right (18 px tall, 6 px side padding), so the
	// title and summary wrap in what is left; a Config card's switch takes the same place.
	const bool hasBadge = badge && *badge;
	const float badgeW = hasBadge ? SpacedTextWidth(badgeFont, badge, 0.6f * s) + 12 * s : 0;
	const float swW = Metrics::swSm.w * s, swH = Metrics::swSm.h * s;
	const float sideW = hasBadge ? badgeW + 8 * s : (master.show ? swW + 6 * s : 0);
	const float textX = c.x0 + edge + 8 * s, textR = c.x1 - 6 * s - sideW, textW = textR - textX;
	const std::vector<std::string> titleLines = WrapSpaced(titleFont, Upper(title), 0.8f * s, textW);
	const float titleH = titleFont->FontSize * (float)titleLines.size();
	const bool hasSummary = summary && *summary;
	const float sumH = hasSummary ? TextSize(sumFont, summary, textW).y + 3 * s : 0;
	const float contentH = titleH + sumH;
	const float minH = Metrics::rail.rowH * s;
	const float rowH = contentH + 10 * s > minH ? contentH + 10 * s : minH;
	const Rect row = MakeRect(c.x0, c.y, c.x1, c.y + rowH);
	const Rect track = MakeRect(row.x1 - 6 * s - swW, row.y0 + (rowH - swH) * 0.5f, row.x1 - 6 * s, row.y0 + (rowH + swH) * 0.5f);
	ImGui::PushID(id);
	const Hit h = HitArea("row", row, true);
	ImGui::PopID();
	RailHit hit = RailHit::None;
	if (h.clicked) {
		const ImVec2 at = ImGui::GetIO().MousePos;
		const bool onSwitch = master.show && at.x >= track.x0 - 4 * s && at.x <= track.x1 + 4 * s && at.y >= track.y0 - 4 * s
			&& at.y <= track.y1 + 4 * s;
		hit = onSwitch ? RailHit::Switch : RailHit::Row;
	}
	const bool off = master.show && !master.on;
	ImDrawList* draw = ImGui::GetWindowDrawList();
	Box(draw, row, selected ? Paint::RailSel : (h.hovered ? Paint::RailHover : Rgba(0, 0, 0, 0)),
		selected ? Paint::RailSelBorder : Rgba(0, 0, 0, 0));
	draw->AddRectFilled(ImVec2(row.x0, row.y0), ImVec2(row.x0 + edge, row.y1), ToU32(off ? Paint::EdgeOff : accent));
	const float textY = row.y0 + (rowH - contentH) * 0.5f;
	const Color titleColor = selected ? Paint::Value : (off ? Paint::Muted : Paint::Label);
	for (size_t i = 0; i < titleLines.size(); ++i)
		DrawSpacedText(draw, titleFont, ImVec2(textX, textY + titleFont->FontSize * (float)i), ToU32(titleColor),
			titleLines[i].c_str(), 0.8f * s);
	if (hasSummary) Text(draw, sumFont, ImVec2(textX, textY + titleH + 3 * s), Paint::Muted, summary, textW);
	if (hasBadge) {
		// Solid, with dark text, as CS2's 'live' badge.
		const float bh = 18 * s;
		const Rect box = MakeRect(row.x1 - 8 * s - badgeW, row.y0 + (rowH - bh) * 0.5f, row.x1 - 8 * s, row.y0 + (rowH + bh) * 0.5f);
		draw->AddRectFilled(ImVec2(box.x0, box.y0), ImVec2(box.x1, box.y1), ToU32(badgeColor), 3 * s);
		DrawSpacedText(draw, badgeFont, ImVec2(box.x0 + 6 * s, std::floor(box.y0 + (bh - badgeFont->FontSize) * 0.5f)),
			ToU32(Rgba(0x0c, 0x0e, 0x12)), badge, 0.6f * s);
	} else if (master.show) {
		DrawSwitch(draw, track, master.on, true, Metrics::swSm.knob * s);
	}
	c.y += rowH;
	return hit;
}

} // namespace

bool RailRow(CardColumn& c, const char* id, const char* title, const char* summary, bool selected, Color accent,
	const char* badge, Color badgeColor) {
	return RailRowDraw(c, id, title, summary, selected, accent, badge, badgeColor, CardSwitch()) == RailHit::Row;
}

RailHit RailRow(CardColumn& c, const char* id, const char* title, const char* summary, bool selected, Color accent,
	CardSwitch master) {
	return RailRowDraw(c, id, title, summary, selected, accent, nullptr, Theme::Accent, master);
}

bool RailAddButton(CardColumn& c, const char* id, const char* caption) {
	const float s = c.s;
	ImFont* font = c.Font(FontRole::Button);
	c.y += 6 * s;
	const Rect r = MakeRect(c.x0, c.y, c.x1, c.y + font->FontSize + 14 * s);
	ImGui::PushID(id);
	const Hit h = HitArea("add", r, true);
	ImGui::PopID();
	ImDrawList* draw = ImGui::GetWindowDrawList();
	Box(draw, r, h.held ? Paint::Down : (h.hovered ? Paint::Hover : Rgba(0, 0, 0, 0)), Paint::Outline);
	const float w = SpacedTextWidth(font, caption, 0.8f * s);
	DrawSpacedText(draw, font, ImVec2(std::floor(r.x0 + (r.W() - w) * 0.5f), std::floor(r.y0 + (r.H() - font->FontSize) * 0.5f)),
		ToU32(Paint::Label), caption, 0.8f * s);
	c.y = r.y1;
	return h.clicked;
}

namespace {

struct KeyStripLayout {
	float capPadH, capPadV, capGap, itemGap, rowGap, padTop, padBottom;
};

KeyStripLayout KeyStripMetrics(float s) { return { 8 * s, 3 * s, 7 * s, 16 * s, 7 * s, 10 * s, 2 * s }; }

// Lays the strip out left to right, wrapping; calls draw (if any) per item. Returns the height.
template <typename Fn>
float LayKeyStrip(const KeyHint* hints, int count, const FontSet& fonts, float s, float x0, float y0, float width, Fn&& each) {
	const KeyStripLayout m = KeyStripMetrics(s);
	ImFont* keyFont = fonts.Get(FontRole::Label);
	ImFont* doesFont = fonts.Get(FontRole::RowLabel);
	const float capH = keyFont->FontSize + 2 * m.capPadV;
	float x = x0, y = y0 + m.padTop;
	for (int i = 0; i < count; ++i) {
		const float keyW = TextSize(keyFont, hints[i].keys).x + 2 * m.capPadH;
		const float itemW = keyW + m.capGap + TextSize(doesFont, hints[i].does).x;
		if (x > x0 && x + itemW > x0 + width) {
			x = x0;
			y += capH + m.rowGap;
		}
		each(hints[i], x, y, keyW, capH);
		x += itemW + m.itemGap;
	}
	return (count > 0 ? y + capH + m.rowGap : y) - y0 + m.padBottom;
}

} // namespace

float KeyStripHeight(const KeyHint* hints, int count, const FontSet& fonts, float s, float width) {
	return LayKeyStrip(hints, count, fonts, s, 0, 0, width, [](const KeyHint&, float, float, float, float) {});
}

void DrawKeyStrip(ImDrawList* draw, const KeyHint* hints, int count, const FontSet& fonts, float s, const Rect& r) {
	ImFont* keyFont = fonts.Get(FontRole::Label);
	ImFont* doesFont = fonts.Get(FontRole::RowLabel);
	draw->AddLine(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y0), ToU32(Paint::Rule), 1.0f);
	LayKeyStrip(hints, count, fonts, s, r.x0, r.y0, r.W(), [&](const KeyHint& hint, float x, float y, float keyW, float capH) {
		const Rect cap = MakeRect(x, y, x + keyW, y + capH);
		Box(draw, cap, Paint::KeyCapBg, Paint::KeyCapBorder);
		CenteredText(draw, keyFont, cap, Paint::Value, hint.keys);
		const ImVec2 ds = TextSize(doesFont, hint.does);
		Text(draw, doesFont, ImVec2(cap.x1 + 7 * s, y + (capH - ds.y) * 0.5f), Paint::Label, hint.does);
	});
}

} // namespace Ui
} // namespace SourceFilmmaker
