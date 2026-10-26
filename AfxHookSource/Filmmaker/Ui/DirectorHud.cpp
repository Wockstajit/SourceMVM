#include "DirectorHud.h"

#include "DirectorMath.h"
#include "UiWidgets.h"

#include <cfloat>
#include <cmath>
#include <cstdio>

namespace SourceFilmmaker {
namespace Ui {

namespace {

using namespace DirectorMath;

// CS2's card geometry (MovieHudJs.h: mkCard, mkHeader, mkSection, mkKV, mkCaption, mkControl), stage
// px before the cards' 0.8 scale. The colours are CS:S's.
const float kHeaderPadV = 11, kPadH = 14;
const float kAccentW = 3, kAccentH = 15, kAccentGap = 9;
const float kTitleSize = 14, kTitleSpacing = 1.5f;
const float kSectionPadV = 8, kRowGap = 4;
const float kBodySize = 14, kReadoutSize = 17;
const float kCaptionSize = 11, kCaptionSpacing = 1, kCaptionGap = 7;
const float kBadgeSize = 12, kBadgeMinW = 46, kBadgePadV = 2, kBadgePadH = 7, kBadgeGap = 9;
const float kDescSize = 13, kControlMinH = 18;
const float kRadius = 5;
const float kLineHeight = 1.25f; // a Panorama label's line box over its font size

const Color kSeparator = Rgba(128, 128, 128, 48);

struct Control {
	const char* key;
	const char* what;
	const char* fallback; // the key spelled out, for a font without its glyphs
};
// CS2's list (MovieHudJs.h), with the keys CS:S has so far. F8 is the blackout; CS2's card still
// calls it "Show / hide this panel" from before the blackout.
const Control kControls[] = {
	{ "F8", "Blackout on / off", nullptr },
	{ "Scroll", "First person / Chase / Free cam", nullptr },
	{ "Hold click", "Look around (Chase, Free cam)", nullptr },
	{ "\xE2\x86\x90 / \xE2\x86\x92", "Skip 5s", "Left / Right" },
	{ "Space", "Pause / play", nullptr },
};

void F1(double v, char* out, size_t size) { std::snprintf(out, size, "%.1f", std::round(v * 10.0) / 10.0); }

// One card, drawn top-down: rows advance y; the background goes under them once the height is
// known (draw-list channel 0 under channel 1).
struct Card {
	ImDrawList* list;
	const FontSet& fonts;
	float k;   // stage px -> client px, the card scale included
	float x, y0, w;
	float y;

	Card(ImDrawList* l, const FontSet& f, float scale, const CardPlacement& p)
		: list(l), fonts(f), k(scale), x(p.x), y0(p.y), w(p.w), y(p.y) {
		list->ChannelsSplit(2);
		list->ChannelsSetCurrent(1);
	}

	float Px(float stage) const { return stage * k; }
	float TextW(FontRole role, float size, const char* text) const {
		return fonts.Get(role)->CalcTextSizeA(Px(size), FLT_MAX, 0, text).x;
	}
	void Text(FontRole role, float size, float tx, float ty, Color c, const char* text) const {
		list->AddText(fonts.Get(role), Px(size), ImVec2(tx, ty), ToU32(c), text);
	}
	float SpacedW(FontRole role, float size, const char* text, float spacing) const {
		float total = 0;
		int n = 0;
		for (const char* p = text; *p; ++p, ++n) {
			const char c[2] = { *p, 0 };
			total += TextW(role, size, c);
		}
		return n > 1 ? total + Px(spacing) * (n - 1) : total;
	}
	void SpacedText(FontRole role, float size, float tx, float ty, Color c, const char* text, float spacing) const {
		for (const char* p = text; *p; ++p) {
			const char ch[2] = { *p, 0 };
			Text(role, size, tx, ty, c, ch);
			tx += TextW(role, size, ch) + Px(spacing);
		}
	}

	void Header(const char* title) {
		const float lineH = std::fmax(kAccentH, kTitleSize * kLineHeight);
		const float top = y + Px(kHeaderPadV);
		const float mid = top + Px(lineH) * 0.5f;
		const float ax = x + Px(kPadH);
		list->AddRectFilled(ImVec2(ax, mid - Px(kAccentH) * 0.5f), ImVec2(ax + Px(kAccentW), mid + Px(kAccentH) * 0.5f),
			ToU32(Theme::Accent), Px(2));
		SpacedText(FontRole::BodyBold, kTitleSize, ax + Px(kAccentW + kAccentGap), mid - Px(kTitleSize) * 0.5f,
			Theme::Accent, title, kTitleSpacing);
		y = top + Px(lineH) + Px(kHeaderPadV);
	}
	void Separator() {
		const float h = std::fmax(1.0f, std::round(Px(1)));
		list->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), ToU32(kSeparator));
		y += h;
	}
	void SectionStart() { y += Px(kSectionPadV); }
	void SectionEnd() { y += Px(kSectionPadV); }
	void Caption(const char* text) {
		SpacedText(FontRole::Label, kCaptionSize, x + Px(kPadH), y, Theme::TextDim, text, kCaptionSpacing);
		y += Px(kCaptionSize * kLineHeight + kCaptionGap);
	}
	// "label .......... value": the label on the left, the value right-aligned and bold.
	void Row(const char* label, const char* value, Color valueColor, float valueSize = kBodySize) {
		const float lineH = Px(std::fmax(kBodySize, valueSize) * kLineHeight);
		Text(FontRole::Body, kBodySize, x + Px(kPadH), y + (lineH - Px(kBodySize)) * 0.5f, Theme::TextDim, label);
		const float vw = TextW(FontRole::BodyBold, valueSize, value);
		Text(FontRole::BodyBold, valueSize, x + w - Px(kPadH) - vw, y + (lineH - Px(valueSize)) * 0.5f, valueColor, value);
		y += lineH + Px(kRowGap);
	}
	// [ KEY ]  what it does
	void ControlRow(const Control& c) {
		// CS2 writes the skip keys as arrows; Verdana has none, so they are spelled out there.
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

	// The panel under the rows (rounded top corners), then CS2's soft fade strip under it.
	void Finish() {
		list->ChannelsSetCurrent(0);
		list->AddRectFilled(ImVec2(x, y0), ImVec2(x + w, y), ToU32(Theme::PanelBg), Px(kRadius), ImDrawFlags_RoundCornersTop);
		const ImU32 solid = ToU32(Theme::PanelBg), clear = ToU32(Faded(Theme::PanelBg, 0));
		list->AddRectFilledMultiColor(ImVec2(x, y), ImVec2(x + w, y + Px(kCardFadeH)), solid, solid, clear, clear);
		list->ChannelsMerge();
	}
};

} // namespace

DirectorFrame DrawDirector(DirectorState& state, const DemoBarModel& bar, const DirectorCardsModel& cards,
	const FontSet& fonts, float clientW, float clientH, float topReserve, float bottomReserve) {
	DirectorFrame frame;
	if (state.blackout) return frame;
	const float s = clientH / kStageHeight;

	frame.bar = BarPanelRect(clientW, clientH, bottomReserve);
	BeginPanel("##directorbar", frame.bar, Theme::DockBg);
	frame.demoActions = DrawDemoBar(state.bar, bar, fonts, BarRect(clientW, clientH), s);
	ImGui::End();

	if (!state.cards) return frame;
	ImDrawList* list = ImGui::GetBackgroundDrawList();
	const float k = s * kCardScale;
	char a[32], b[32], c[32], text[128];

	Card right(list, fonts, k, DirectorCard(clientW, clientH, topReserve));
	right.Header("MOVIE DIRECTOR");
	right.Separator();
	right.SectionStart();
	right.Row("Mode", cards.mode, Theme::Text);
	right.Row("Playback", cards.playing ? (cards.paused ? "Paused" : "Playing") : "No demo", cards.paused ? Theme::Accent : Theme::Text);
	if (cards.speed >= 0) {
		F1(cards.speed, a, sizeof(a));
		std::snprintf(text, sizeof(text), "%sx", a);
	} else {
		std::snprintf(text, sizeof(text), "-");
	}
	right.Row("Cam speed", text, Theme::Text);
	right.SectionEnd();
	right.Separator();
	right.SectionStart();
	right.Caption("CAMERA");
	F1(cards.pos[0], a, sizeof(a)); F1(cards.pos[1], b, sizeof(b)); F1(cards.pos[2], c, sizeof(c));
	std::snprintf(text, sizeof(text), "%s, %s, %s", a, b, c);
	right.Row("Pos", text, Theme::Text, kReadoutSize);
	F1(cards.ang[0], a, sizeof(a)); F1(cards.ang[1], b, sizeof(b)); F1(cards.ang[2], c, sizeof(c));
	std::snprintf(text, sizeof(text), "%s, %s, %s", a, b, c);
	right.Row("Ang", text, Theme::Text, kReadoutSize);
	F1(cards.fov, a, sizeof(a));
	right.Row("FOV", a, Theme::Text);
	if (cards.haveTick) {
		F1(cards.time, a, sizeof(a));
		std::snprintf(text, sizeof(text), "%d  \xC2\xB7  %ss", cards.tick, a);
	} else {
		std::snprintf(text, sizeof(text), "-  \xC2\xB7  -");
	}
	right.Row("Tick / Time", text, Theme::Text);
	right.SectionEnd();
	right.Finish();

	Card left(list, fonts, k, ControlsCard(clientW, clientH));
	left.Header("CONTROLS");
	left.Separator();
	left.SectionStart();
	for (const Control& control : kControls) left.ControlRow(control);
	left.SectionEnd();
	left.Finish();
	return frame;
}

std::string DirectorCardsText(const DirectorCardsModel& cards) {
	char text[256];
	std::snprintf(text, sizeof(text), "mode=%s playback=%s speed=%.2f pos=(%.1f %.1f %.1f) ang=(%.1f %.1f %.1f) fov=%.1f tick=%d time=%.1f",
		cards.mode, cards.playing ? (cards.paused ? "paused" : "playing") : "none", cards.speed,
		cards.pos[0], cards.pos[1], cards.pos[2], cards.ang[0], cards.ang[1], cards.ang[2], cards.fov,
		cards.haveTick ? cards.tick : -1, cards.time);
	return text;
}

} // namespace Ui
} // namespace SourceFilmmaker
