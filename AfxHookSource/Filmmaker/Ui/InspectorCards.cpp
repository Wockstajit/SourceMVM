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
	const float s = c.s;
	ImDrawList* draw = ImGui::GetWindowDrawList();
	ImFont* titleFont = c.Font(FontRole::CardTitle);
	ImFont* noteFont = c.Font(FontRole::Note);
	ImFont* buttonFont = c.Font(FontRole::Button);
	const float btnH = buttonFont->FontSize + 10 * s;
	float slotsW = 0;
	for (int i = 0; i < count; ++i) slotsW += CardButtonWidth(buttons[i].caption, buttonFont, s) + 8 * s;
	const float edgeW = 3 * s, edgeH = 30 * s;
	const float textX = c.x0 + edgeW + 10 * s;
	const std::string caption = Upper(title);
	const float titleW = SpacedTextWidth(titleFont, caption.c_str(), 1.2f * s);
	// The buttons share the title's line while the title fits beside them; otherwise they get a line
	// of their own under the header, so neither is cut.
	const bool ownLine = count > 0 && textX + titleW > c.x1 - slotsW;
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
		ToU32(Theme::Accent));
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
	ImGui::PopID();
	c.y += blockH + (ownLine ? btnH + 6 * s : 0) + 6 * s;
	return clicked;
}

void SubHead(CardColumn& c, const char* text) {
	ImFont* font = c.Font(FontRole::Caption);
	c.y += 10 * c.s;
	DrawSpacedText(ImGui::GetWindowDrawList(), font, ImVec2(c.x0, c.y), ToU32(Paint::Muted), Upper(text).c_str(), 1.5f * c.s);
	c.y += font->FontSize + 1 * c.s;
}

void Note(CardColumn& c, const char* text) {
	if (!text || !*text) return;
	ImFont* font = c.Font(FontRole::Note);
	const float pad = Metrics::line.padH * c.s;
	c.y += 6 * c.s;
	const float wrap = c.W() - 2 * pad;
	Text(ImGui::GetWindowDrawList(), font, ImVec2(c.x0 + pad, c.y), Paint::Muted, text, wrap);
	c.y += TextSize(font, text, wrap).y;
}

ValueEdit ValueRow(CardColumn& c, const char* id, const char* label, double value, double min, double max, int decimals,
	const char* unit, Color fill, bool enabled) {
	const float s = c.s;
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
			raw += (float)(io.MouseDelta.x * span / barW * (io.KeyShift ? 0.08 : 1.0));
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
	const Color text = enabled ? Paint::Label : Paint::Disabled;
	Text(draw, labelFont, ImVec2(row.x0 + pad, row.y0 + (rowH - (labelH - 8 * s)) * 0.5f), text, label, labelW);
	if (barW > 0) {
		double frac = (out.value - min) / span;
		frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);
		const float cy = row.y0 + rowH * 0.5f;
		draw->AddRectFilled(ImVec2(barX0, cy - 2 * s), ImVec2(barX1, cy + 2 * s), ToU32(Paint::TrackBg));
		const float fx = barX0 + (float)frac * barW;
		draw->AddRectFilled(ImVec2(barX0, cy - 2 * s), ImVec2(fx, cy + 2 * s), ToU32(enabled ? fill : Faded(fill, 0.35f)));
		const float knob = 4 * s;
		draw->AddRectFilled(ImVec2(fx - knob, cy - 7 * s), ImVec2(fx + knob, cy + 7 * s),
			ToU32(enabled ? Paint::Value : Paint::Disabled));
	}
	const std::string shown = FormatValue(out.value, decimals, unit);
	const ImVec2 vs = TextSize(valueFont, shown.c_str());
	Text(draw, valueFont, ImVec2(row.x1 - pad - vs.x, row.y0 + (rowH - vs.y) * 0.5f), enabled ? Paint::Value : Paint::Disabled,
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

void RailGroup(CardColumn& c, const char* name, bool first) {
	ImFont* font = c.Font(FontRole::Label);
	if (!first) c.y += Metrics::rail.groupGap * c.s;
	DrawSpacedText(ImGui::GetWindowDrawList(), font, ImVec2(c.x0 + 2 * c.s, c.y), ToU32(Paint::Muted), Upper(name).c_str(), 1.8f * c.s);
	c.y += font->FontSize + 3 * c.s;
}

bool RailRow(CardColumn& c, const char* id, const char* title, const char* summary, bool selected, Color accent) {
	const float s = c.s;
	ImFont* titleFont = c.Font(FontRole::Title);
	ImFont* sumFont = c.Font(FontRole::RailSum);
	c.y += Metrics::rail.rowGap * s;
	const float edge = Metrics::rail.edge * s;
	const float textX = c.x0 + edge + 8 * s, textW = c.x1 - 6 * s - textX;
	const bool hasSummary = summary && *summary;
	const float sumH = hasSummary ? TextSize(sumFont, summary, textW).y + 3 * s : 0;
	const float contentH = titleFont->FontSize + sumH;
	const float minH = Metrics::rail.rowH * s;
	const float rowH = contentH + 10 * s > minH ? contentH + 10 * s : minH;
	const Rect row = MakeRect(c.x0, c.y, c.x1, c.y + rowH);
	ImGui::PushID(id);
	const Hit h = HitArea("row", row, true);
	ImGui::PopID();
	ImDrawList* draw = ImGui::GetWindowDrawList();
	Box(draw, row, selected ? Paint::RailSel : (h.hovered ? Paint::RailHover : Rgba(0, 0, 0, 0)),
		selected ? Paint::RailSelBorder : Rgba(0, 0, 0, 0));
	draw->AddRectFilled(ImVec2(row.x0, row.y0), ImVec2(row.x0 + edge, row.y1), ToU32(accent));
	const float textY = row.y0 + (rowH - contentH) * 0.5f;
	DrawSpacedText(draw, titleFont, ImVec2(textX, textY), ToU32(selected ? Paint::Value : Paint::Label), Upper(title).c_str(), 0.8f * s);
	if (hasSummary) Text(draw, sumFont, ImVec2(textX, textY + titleFont->FontSize + 3 * s), Paint::Muted, summary, textW);
	c.y += rowH;
	return h.clicked;
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
