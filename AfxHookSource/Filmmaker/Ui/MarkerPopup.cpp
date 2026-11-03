#include "MarkerPopup.h"

#include "DirectorMath.h"
#include "HudCard.h"
#include "UiWidgets.h"
#include "WorkspaceLayoutMath.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace SourceFilmmaker {
namespace Ui {

namespace {

using namespace HudCardMetrics;
using Kind = MarkerCardAction::Kind;

// MarkerHudJs.h's sizes, stage px.
constexpr float kCardW = 384;
constexpr float kFadeH = 40;
constexpr float kArrowW = 30, kArrowH = 26, kArrowPadH = 12, kArrowGap = 10;
constexpr float kControlW = 132, kRowMinH = 28, kRowGapBelow = 6;
constexpr float kStepW = 30;
constexpr float kActionPadV = 9, kActionGap = 5;
constexpr float kLabelSize = 13, kHintSize = 11;
const float kSpeedSteps[] = { 0.2f, 0.5f, 0.8f, 1.0f };

constexpr Color kPanel = Rgba(0, 0, 0, 230);            // CS2's rgba(0,0,0,0.9)
constexpr Color kFreeze = Rgba(128, 189, 255);          // CS2's Freeze blue (S.freeze)
constexpr Color kSelAccent = Rgba(255, 176, 0, 51);
constexpr Color kSelFreeze = Rgba(128, 189, 255, 51);

const HudControl kControls[] = {
	{ "K", "Place a camera marker", nullptr },
	{ "J", "Arm the path, then Space plays it", nullptr },
	{ "L", "Delete the marker you are aiming at", nullptr },
	{ "F", "Open these settings (aim at a marker)", nullptr },
};

const char* const kSpeedModeNames[] = { "Manual", "Constant", "Per-Segment" };

struct Hit {
	bool clicked = false, hovered = false, held = false;
};

Hit Button(const char* id, float x0, float y0, float x1, float y1) {
	Hit h;
	if (!(x1 > x0) || !(y1 > y0)) return h;
	ImGui::SetCursorScreenPos(ImVec2(x0, y0));
	h.clicked = ImGui::InvisibleButton(id, ImVec2(x1 - x0, y1 - y0));
	h.hovered = ImGui::IsItemHovered();
	h.held = ImGui::IsItemActive();
	return h;
}

Color Ground(const Hit& h, Color rest) { return h.held ? Rgba(255, 255, 255, 61) : (h.hovered ? Rgba(255, 255, 255, 38) : rest); }

void Push(std::vector<MarkerCardAction>& out, Kind kind, float value = 0) {
	MarkerCardAction a;
	a.kind = kind;
	a.value = value;
	out.push_back(a);
}

float StepSpeed(float current, int direction) {
	int best = 0;
	for (int i = 1; i < 4; ++i)
		if (std::fabs(kSpeedSteps[i] - current) < std::fabs(kSpeedSteps[best] - current)) best = i;
	best += direction > 0 ? 1 : -1;
	return kSpeedSteps[best < 0 ? 0 : (best > 3 ? 3 : best)];
}

void CentredText(HudCard& c, FontRole role, float size, float x0, float y0, float x1, float y1, Color color, const char* text) {
	const ImVec2 ts = c.fonts.Get(role)->CalcTextSizeA(c.Px(size), FLT_MAX, 0, text);
	c.Text(role, size, std::floor((x0 + x1 - ts.x) * 0.5f), std::floor((y0 + y1 - ts.y) * 0.5f), color, text);
}

void Arrow(ImDrawList* list, float cx, float cy, float h, bool left, Color color) {
	const float w = h * 0.8f;
	if (left) list->AddTriangleFilled(ImVec2(cx + w * 0.5f, cy - h), ImVec2(cx + w * 0.5f, cy + h), ImVec2(cx - w, cy), ToU32(color));
	else list->AddTriangleFilled(ImVec2(cx - w * 0.5f, cy - h), ImVec2(cx - w * 0.5f, cy + h), ImVec2(cx + w, cy), ToU32(color));
}

// "label ........ [ value ]": the value is the button, one click cycles it (mkSelector).
bool Selector(HudCard& c, const char* id, const char* label, const char* value, Color valueColor, bool selected, Color selectedGround) {
	const float rowH = c.Px(kRowMinH);
	c.Text(FontRole::Body, kLabelSize, c.x + c.Px(kPadH), c.y + (rowH - c.Px(kLabelSize)) * 0.5f, Theme::Text, label);
	const float bx1 = c.x + c.w - c.Px(kPadH), bx0 = bx1 - c.Px(kControlW);
	const float by0 = c.y + c.Px(1), by1 = c.y + rowH - c.Px(1);
	const Hit h = Button(id, bx0, by0, bx1, by1);
	c.list->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), ToU32(Ground(h, selected ? selectedGround : Theme::ButtonBg)), c.Px(4));
	c.list->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by1), ToU32(selected ? valueColor : Theme::Border), c.Px(4));
	CentredText(c, FontRole::BodyBold, kLabelSize, bx0, by0, bx1, by1, valueColor, value);
	c.y += rowH + c.Px(kRowGapBelow);
	return h.clicked;
}

// "label ........ [ - value + ]" on the speed ladder (mkStepper). Returns -1, +1 or 0.
int Stepper(HudCard& c, const char* id, const char* label, const char* value) {
	const float rowH = c.Px(kRowMinH);
	c.Text(FontRole::Body, kLabelSize, c.x + c.Px(kPadH), c.y + (rowH - c.Px(kLabelSize)) * 0.5f, Theme::Text, label);
	const float gx1 = c.x + c.w - c.Px(kPadH), gx0 = gx1 - c.Px(kControlW);
	const float by0 = c.y + c.Px(1), by1 = c.y + rowH - c.Px(1);
	int step = 0;
	ImGui::PushID(id);
	for (int d = -1; d <= 1; d += 2) {
		const float x0 = d < 0 ? gx0 : gx1 - c.Px(kStepW), x1 = x0 + c.Px(kStepW);
		const Hit h = Button(d < 0 ? "minus" : "plus", x0, by0, x1, by1);
		c.list->AddRectFilled(ImVec2(x0, by0), ImVec2(x1, by1), ToU32(Ground(h, Theme::ButtonBg)), c.Px(4));
		CentredText(c, FontRole::BodyBold, 15, x0, by0, x1, by1, Theme::Accent, d < 0 ? "-" : "+");
		if (h.clicked) step = d;
	}
	ImGui::PopID();
	CentredText(c, FontRole::BodyBold, kLabelSize, gx0 + c.Px(kStepW), by0, gx1 - c.Px(kStepW), by1, Theme::Text, value);
	c.y += rowH + c.Px(kRowGapBelow);
	return step;
}

enum class ActionLook { Primary, Danger };

bool Action(HudCard& c, const char* id, const char* text, ActionLook look, Color textColor) {
	const float h = c.Px(kLabelSize * kLineHeight + 2 * kActionPadV);
	const float x0 = c.x + c.Px(kPadH), x1 = c.x + c.w - c.Px(kPadH);
	const Hit hit = Button(id, x0, c.y, x1, c.y + h);
	Color ground = Theme::ButtonBg, edge = Theme::Border;
	if (look == ActionLook::Primary) {
		ground = hit.held ? Theme::ButtonArmedBg : (hit.hovered ? Rgba(255, 196, 64) : Theme::Accent);
		edge = Rgba(0, 0, 0, 0);
	} else if (hit.hovered || hit.held) {
		ground = hit.held ? Palette::Red : Rgba(192, 28, 0, 60); // red-tinted only on hover, as CS2's
		edge = Palette::Red;
	}
	c.list->AddRectFilled(ImVec2(x0, c.y), ImVec2(x1, c.y + h), ToU32(ground), c.Px(4));
	if (edge.a) c.list->AddRect(ImVec2(x0, c.y), ImVec2(x1, c.y + h), ToU32(edge), c.Px(4));
	CentredText(c, FontRole::BodyBold, kLabelSize, x0, c.y, x1, c.y + h, textColor, text);
	c.y += h + c.Px(kActionGap);
	return hit.clicked;
}

std::string Explanation(const MarkerCardModel& m) {
	if (m.timing == 0)
		return "LIVE: the camera follows the replay, so markers are hit at their tick/time. Use the speed scrubber for pace. "
		       "The speed modes shape FREEZE playback.";
	if (m.speedMode == 0) return "FREEZE \xC2\xB7 Manual: each segment's duration is the tick/time gap to the next marker.";
	if (m.speedMode == 1) return "FREEZE \xC2\xB7 Constant: the whole path glides at one shared speed.";
	if (m.isLast) return "No outgoing segment: the last marker ends the path.";
	return "FREEZE \xC2\xB7 Per-Segment: this marker controls the move to the next marker.";
}

} // namespace

void DrawMarkerCard(MarkerCardState& state, const MarkerCardModel& m, const FontSet& fonts, float clientW, float clientH,
	std::vector<MarkerCardAction>& actions) {
	// The catcher: a clear window over everything, so a click that misses the card is the card's.
	BeginOverlay("##markercard", MakeRect(0, 0, clientW, clientH), Rgba(0, 0, 0, 0));
	const float k = clientH / kStageHeight * DirectorMath::kCardScale;
	const float w = std::floor(kCardW * k);
	const float x = std::floor((clientW - w) * 0.5f);
	const float h = state.height > 0 ? state.height : 560 * k;
	const float y = std::floor((clientH - h) * 0.5f);
	ImDrawList* list = ImGui::GetWindowDrawList();
	HudCard c(list, fonts, k, x, y, w);

	// ---- header: the arrows around the title ----------------------------------------------------
	char title[96];
	std::snprintf(title, sizeof(title), "CAMERA MARKER #%d  \xC2\xB7  %d TOTAL", m.selected >= 0 ? m.selected + 1 : 0, m.count);
	const float headTop = c.y;
	c.Header(title, c.Px(kArrowPadH + kArrowW + kArrowGap - kPadH));
	const float headMid = (headTop + c.y) * 0.5f;
	for (int d = -1; d <= 1; d += 2) {
		const float ax0 = d < 0 ? x + c.Px(kArrowPadH) : x + w - c.Px(kArrowPadH + kArrowW);
		const Hit hit = Button(d < 0 ? "##prev" : "##next", ax0, headMid - c.Px(kArrowH) * 0.5f, ax0 + c.Px(kArrowW), headMid + c.Px(kArrowH) * 0.5f);
		if (hit.hovered || hit.held)
			list->AddRectFilled(ImVec2(ax0, headMid - c.Px(kArrowH) * 0.5f), ImVec2(ax0 + c.Px(kArrowW), headMid + c.Px(kArrowH) * 0.5f),
				ToU32(Ground(hit, Rgba(0, 0, 0, 0))), c.Px(4));
		Arrow(list, ax0 + c.Px(kArrowW) * 0.5f, headMid, c.Px(5), d < 0, Theme::Accent);
		if (hit.clicked) Push(actions, d < 0 ? Kind::Previous : Kind::Next);
	}
	c.Separator();

	// ---- PLAYBACK ------------------------------------------------------------------------------
	c.SectionStart();
	c.Caption("PLAYBACK");
	const bool freeze = m.timing == 1;
	if (Selector(c, "##speedmode", "Speed mode", kSpeedModeNames[m.speedMode >= 0 && m.speedMode < 3 ? m.speedMode : 0], Theme::Text, false, Theme::ButtonBg))
		Push(actions, Kind::CycleSpeedMode);
	if (Selector(c, "##interp", "Path curve", m.interp == 1 ? "Smooth" : "Linear", Theme::Text, false, Theme::ButtonBg))
		Push(actions, Kind::CycleInterp);
	if (Selector(c, "##timing", "Timing (all cameras)", freeze ? "FREEZE" : "LIVE", freeze ? kFreeze : Theme::Accent, true,
		freeze ? kSelFreeze : kSelAccent))
		Push(actions, Kind::ToggleTiming);
	if (Selector(c, "##autosnap", "Auto-snap to selected", m.autoSnap ? "ON" : "OFF", m.autoSnap ? Theme::Accent : Theme::Text,
		m.autoSnap, kSelAccent))
		Push(actions, Kind::ToggleAutoSnap);
	char value[32];
	if (m.speedMode == 1) { // the constant speed, only in Constant (CS2 shows its row only then)
		std::snprintf(value, sizeof(value), "%.1fx", m.constSpeed);
		const int step = Stepper(c, "##const", "Constant speed", value);
		if (step) Push(actions, Kind::ConstSpeed, StepSpeed(m.constSpeed, step));
	}
	if (m.speedMode == 2 && !m.isLast) { // this marker's outgoing segment, only in Per-Segment
		std::snprintf(value, sizeof(value), "%.1fx", m.speedMul);
		const int step = Stepper(c, "##segment", "Segment speed (to next)", value);
		if (step) Push(actions, Kind::SpeedMul, StepSpeed(m.speedMul, step));
	}
	{
		const std::string text = Explanation(m);
		const float wrap = w - c.Px(2 * kPadH);
		c.y += c.Px(3);
		c.Text(FontRole::Note, kHintSize, x + c.Px(kPadH), c.y, Theme::TextDim, text.c_str(), wrap);
		c.y += fonts.Get(FontRole::Note)->CalcTextSizeA(c.Px(kHintSize), FLT_MAX, wrap, text.c_str()).y;
	}
	c.SectionEnd();
	c.Separator();

	// ---- ACTIONS -------------------------------------------------------------------------------
	c.SectionStart();
	c.Caption("ACTIONS");
	if (Action(c, "##reposition", "Reposition Camera", ActionLook::Primary, Palette::Black)) Push(actions, Kind::Reposition);
	if (Action(c, "##remove", "Remove Marker", ActionLook::Danger, Theme::Text)) Push(actions, Kind::Remove);
	const char* removeAll = state.confirmRemoveAll ? "Remove All: click again to confirm" : "Remove All Markers";
	if (Action(c, "##removeall", removeAll, ActionLook::Danger, state.confirmRemoveAll ? Rgba(255, 146, 142) : Theme::Text)) {
		if (state.confirmRemoveAll) {
			state.confirmRemoveAll = false;
			Push(actions, Kind::RemoveAll);
		} else {
			state.confirmRemoveAll = true;
		}
	}
	c.y -= c.Px(kActionGap);
	c.SectionEnd();
	c.Separator();

	// ---- CONTROLS ------------------------------------------------------------------------------
	c.SectionStart();
	c.Caption("CONTROLS");
	for (const HudControl& control : kControls) c.ControlRow(control);
	c.SectionEnd();
	c.Separator();

	// ---- footer: F / ESC Done ------------------------------------------------------------------
	{
		const char* done = "F / ESC   Done";
		const float tw = c.SpacedW(FontRole::BodyBold, kLabelSize, done, 1);
		const float bh = c.Px(kLabelSize * kLineHeight + 10), bw = tw + c.Px(28);
		const float bx1 = x + w - c.Px(kPadH), bx0 = bx1 - bw, by0 = c.y + c.Px(8);
		const Hit hit = Button("##done", bx0, by0, bx1, by0 + bh);
		if (hit.hovered || hit.held) list->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by0 + bh), ToU32(Ground(hit, Theme::ButtonBg)), c.Px(4));
		c.SpacedText(FontRole::BodyBold, kLabelSize, bx0 + c.Px(14), by0 + (bh - c.Px(kLabelSize)) * 0.5f, Theme::Accent, done, 1);
		if (hit.clicked) Push(actions, Kind::Close);
		c.y = by0 + bh + c.Px(10);
	}
	state.height = c.y - y + c.Px(kFadeH);
	c.Finish(kFadeH, kPanel);
	ImGui::End();
}

void DrawPathBanner(ImDrawList* draw, const PathBanner& b, const FontSet& fonts, const Rect& area) {
	if (!area.Valid()) return;
	// MarkerHudJs.h's banner: 360 px wide at the preview's scale, 11% down the game.
	const float k = area.H() / kStageHeight;
	const float w = std::floor(360 * k);
	const float x = std::floor((area.x0 + area.x1 - w) * 0.5f);
	const float y = std::floor(area.y0 + area.H() * 0.11f);
	HudCard c(draw, fonts, k, x, y, w);
	c.y += c.Px(9);
	const float bx = x + c.Px(16);
	const float titleH = c.Px(13 * kLineHeight);
	draw->AddRectFilled(ImVec2(bx, c.y + (titleH - c.Px(13)) * 0.5f), ImVec2(bx + c.Px(3), c.y + (titleH + c.Px(13)) * 0.5f),
		ToU32(Theme::Accent), c.Px(2));
	c.SpacedText(FontRole::BodyBold, 13, bx + c.Px(11), c.y + (titleH - c.Px(13)) * 0.5f, Theme::Accent, b.title, 1.5f);
	c.y += titleH;
	if (b.line && *b.line) {
		c.y += c.Px(6);
		c.Text(FontRole::Body, 12, bx, c.y, Theme::Text, b.line, w - c.Px(32));
		c.y += fonts.Get(FontRole::Body)->CalcTextSizeA(c.Px(12), FLT_MAX, w - c.Px(32), b.line).y;
	}
	if (b.keys && *b.keys) {
		c.y += c.Px(4);
		c.Text(FontRole::Body, 11, bx, c.y, Theme::TextDim, b.keys);
		c.y += c.Px(11 * kLineHeight);
	}
	c.y += c.Px(9);
	c.Finish(18, kPanel);
}

} // namespace Ui
} // namespace SourceFilmmaker
