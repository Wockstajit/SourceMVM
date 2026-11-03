#pragma once

// The card language CS2's F8 Movie Director card speaks (MovieHudJs.h: mkCard, mkHeader, mkSection,
// mkKV, mkCaption, mkControl), which CS2's CAMERA MARKER card copies on purpose so the two popups
// over a demo look like one program's (MarkerHudJs.h). Here both are drawn with it in CS:S paint:
// the director's cards (DirectorHud.cpp) and the marker card (MarkerPopup.cpp).
//
// A card is drawn top-down: rows advance y, and the panel goes under them once the height is known
// (draw-list channel 0 under channel 1), with CS2's soft fade strip below. Sizes are CS2's stage px,
// times the caller's scale. Engine-free.

#include "UiStyle.h"
#include "UiWidgets.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace SourceFilmmaker {
namespace Ui {

namespace HudCardMetrics {
constexpr float kHeaderPadV = 11, kPadH = 14;
constexpr float kAccentW = 3, kAccentH = 15, kAccentGap = 9;
constexpr float kTitleSize = 14, kTitleSpacing = 1.5f;
constexpr float kSectionPadV = 8, kRowGap = 4;
constexpr float kBodySize = 14, kReadoutSize = 17;
constexpr float kCaptionSize = 11, kCaptionSpacing = 1, kCaptionGap = 7;
constexpr float kBadgeSize = 12, kBadgeMinW = 46, kBadgePadV = 2, kBadgePadH = 7, kBadgeGap = 9;
constexpr float kDescSize = 13, kControlMinH = 18;
constexpr float kRadius = 5;
constexpr float kLineHeight = 1.25f; // a Panorama label's line box over its font size
constexpr Color kSeparator = Rgba(128, 128, 128, 48);
} // namespace HudCardMetrics

// A [KEY] badge row. fallback spells the key out for a font without its glyphs.
struct HudControl {
	const char* key;
	const char* what;
	const char* fallback;
};

struct HudCard {
	ImDrawList* list;
	const FontSet& fonts;
	float k;   // stage px -> client px
	float x, y0, w;
	float y;

	HudCard(ImDrawList* l, const FontSet& f, float scale, float left, float top, float width)
		: list(l), fonts(f), k(scale), x(left), y0(top), w(width), y(top) {
		list->ChannelsSplit(2);
		list->ChannelsSetCurrent(1);
	}

	float Px(float stage) const { return stage * k; }
	float TextW(FontRole role, float size, const char* text) const {
		return fonts.Get(role)->CalcTextSizeA(Px(size), FLT_MAX, 0, text).x;
	}
	void Text(FontRole role, float size, float tx, float ty, Color c, const char* text, float wrap = 0.0f) const {
		list->AddText(fonts.Get(role), Px(size), ImVec2(tx, ty), ToU32(c), text, nullptr, wrap);
	}
	float SpacedW(FontRole role, float size, const char* text, float spacing) const {
		float total = 0;
		int n = 0;
		for (const char* p = text; *p; ++n) {
			const int bytes = Utf8CharBytes(p);
			total += fonts.Get(role)->CalcTextSizeA(Px(size), FLT_MAX, 0, p, p + bytes).x;
			p += bytes;
		}
		return n > 1 ? total + Px(spacing) * (n - 1) : total;
	}
	void SpacedText(FontRole role, float size, float tx, float ty, Color c, const char* text, float spacing) const {
		for (const char* p = text; *p;) {
			const int bytes = Utf8CharBytes(p);
			list->AddText(fonts.Get(role), Px(size), ImVec2(tx, ty), ToU32(c), p, p + bytes);
			tx += fonts.Get(role)->CalcTextSizeA(Px(size), FLT_MAX, 0, p, p + bytes).x + Px(spacing);
			p += bytes;
		}
	}

	// The accent bar and the title. inset leaves room either side (the marker card's arrows).
	void Header(const char* title, float inset = 0) {
		using namespace HudCardMetrics;
		const float lineH = std::fmax(kAccentH, kTitleSize * kLineHeight);
		const float top = y + Px(kHeaderPadV);
		const float mid = top + Px(lineH) * 0.5f;
		const float ax = x + Px(kPadH) + inset;
		list->AddRectFilled(ImVec2(ax, mid - Px(kAccentH) * 0.5f), ImVec2(ax + Px(kAccentW), mid + Px(kAccentH) * 0.5f),
			ToU32(Theme::Accent), Px(2));
		SpacedText(FontRole::BodyBold, kTitleSize, ax + Px(kAccentW + kAccentGap), mid - Px(kTitleSize) * 0.5f,
			Theme::Accent, title, kTitleSpacing);
		y = top + Px(lineH) + Px(kHeaderPadV);
	}
	void Separator() {
		const float h = std::fmax(1.0f, std::round(Px(1)));
		list->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), ToU32(HudCardMetrics::kSeparator));
		y += h;
	}
	void SectionStart() { y += Px(HudCardMetrics::kSectionPadV); }
	void SectionEnd() { y += Px(HudCardMetrics::kSectionPadV); }
	void Caption(const char* text) {
		using namespace HudCardMetrics;
		SpacedText(FontRole::Label, kCaptionSize, x + Px(kPadH), y, Theme::TextDim, text, kCaptionSpacing);
		y += Px(kCaptionSize * kLineHeight + kCaptionGap);
	}
	// "label .......... value": the label on the left, the value right-aligned and bold.
	void Row(const char* label, const char* value, Color valueColor, float valueSize = HudCardMetrics::kBodySize) {
		using namespace HudCardMetrics;
		const float lineH = Px(std::fmax(kBodySize, valueSize) * kLineHeight);
		Text(FontRole::Body, kBodySize, x + Px(kPadH), y + (lineH - Px(kBodySize)) * 0.5f, Theme::TextDim, label);
		const float vw = TextW(FontRole::BodyBold, valueSize, value);
		Text(FontRole::BodyBold, valueSize, x + w - Px(kPadH) - vw, y + (lineH - Px(valueSize)) * 0.5f, valueColor, value);
		y += lineH + Px(kRowGap);
	}
	// [ KEY ]  what it does
	void ControlRow(const HudControl& c) {
		using namespace HudCardMetrics;
		ImFont* badgeFont = fonts.Get(FontRole::Label);
		const char* key = c.key;
		if (c.fallback && !(badgeFont->FindGlyphNoFallback(0x2190) && badgeFont->FindGlyphNoFallback(0x2192))) key = c.fallback;
		const float badgeH = Px(kBadgeSize * kLineHeight + 2 * kBadgePadV);
		const float rowH = std::fmax(Px(kControlMinH), std::fmax(badgeH, Px(kDescSize * kLineHeight)));
		const float kw = TextW(FontRole::Label, kBadgeSize, key);
		const float bw = std::fmax(Px(kBadgeMinW), kw + Px(2 * kBadgePadH));
		const float bx = x + Px(kPadH), by = y + (rowH - badgeH) * 0.5f;
		list->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + badgeH), ToU32(Theme::ButtonBg), Px(4));
		list->AddRect(ImVec2(bx, by), ImVec2(bx + bw, by + badgeH), ToU32(Theme::Border), Px(4));
		Text(FontRole::Label, kBadgeSize, bx + (bw - kw) * 0.5f, by + (badgeH - Px(kBadgeSize)) * 0.5f, Theme::Text, key);
		Text(FontRole::Body, kDescSize, bx + bw + Px(kBadgeGap), y + (rowH - Px(kDescSize)) * 0.5f, Theme::TextDim, c.what);
		y += rowH + Px(kRowGap);
	}

	// The panel under the rows (rounded top corners), then the soft fade strip under it.
	void Finish(float fadeStage, Color panel = Theme::PanelBg) {
		list->ChannelsSetCurrent(0);
		list->AddRectFilled(ImVec2(x, y0), ImVec2(x + w, y), ToU32(panel), Px(HudCardMetrics::kRadius), ImDrawFlags_RoundCornersTop);
		const ImU32 solid = ToU32(panel), clear = ToU32(Faded(panel, 0));
		list->AddRectFilledMultiColor(ImVec2(x, y), ImVec2(x + w, y + Px(fadeStage)), solid, solid, clear, clear);
		list->ChannelsMerge();
	}
};

} // namespace Ui
} // namespace SourceFilmmaker
