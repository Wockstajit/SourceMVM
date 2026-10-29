#include "DirectorHud.h"

#include "DirectorMath.h"
#include "HudCard.h"
#include "UiWidgets.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

namespace SourceFilmmaker {
namespace Ui {

namespace {

using namespace DirectorMath;
using namespace HudCardMetrics;

// CS2's list (MovieHudJs.h), with the keys CS:S has so far. F8 is the blackout; CS2's card still
// calls it "Show / hide this panel" from before the blackout.
const HudControl kControls[] = {
	{ "F8", "Blackout on / off", nullptr },
	{ "Scroll", "First person / Chase / Free cam", nullptr },
	{ "Hold click", "Look around (Chase, Free cam)", nullptr },
	{ "\xE2\x86\x90 / \xE2\x86\x92", "Skip 5s", "Left / Right" },
	{ "Space", "Pause / play", nullptr },
};

void F1(double v, char* out, size_t size) { std::snprintf(out, size, "%.1f", std::round(v * 10.0) / 10.0); }

// A tile keeps only a player initial and health. The complete name stays in its tooltip.
void PlayerSquare(const DirectorPlayer& p, const FontSet& fonts, float s, const Rect& box,
    bool interactive, DirectorFrame& frame) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const Color team = p.team == 3 ? Palette::TeamCT : Theme::Accent;
    ImGui::PushID(p.userId);
    ImGui::SetCursorScreenPos(ImVec2(box.x0, box.y0));
    ImGui::BeginDisabled(!interactive);
    const bool clicked = ImGui::InvisibleButton("##player", ImVec2(box.W(), box.H()));
    const bool hover = ImGui::IsItemHovered();
    ImGui::EndDisabled();
    if (clicked) frame.observeUserId = p.userId;
    draw->AddRectFilled(ImVec2(box.x0, box.y0), ImVec2(box.x1, box.y1),
        ToU32(hover ? Rgba(43, 48, 56, 245) : Rgba(13, 17, 22, 230)));
    draw->AddRect(ImVec2(box.x0, box.y0), ImVec2(box.x1, box.y1),
        ToU32(p.observed ? team : Faded(team, p.alive ? 0.35f : 0.15f)));
    ImFont* font = fonts.Get(FontRole::Value);
    const int bytes = p.name.empty() ? 0 : Utf8CharBytes(p.name.c_str());
    const std::string initial = bytes ? p.name.substr(0, bytes) : "?";
    const float tw = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, initial.c_str()).x;
    draw->AddText(font, font->FontSize, ImVec2(std::round(box.x0 + (box.W() - tw) / 2), box.y0 + 6 * s),
        ToU32(p.alive ? team : Theme::TextDisabled), initial.c_str());
    const float hp = (std::max)(0, (std::min)(100, p.health)) / 100.0f;
    if (p.alive) draw->AddRectFilled(ImVec2(box.x0 + 2 * s, box.y1 - 4 * s),
        ImVec2(box.x0 + 2 * s + (box.W() - 4 * s) * hp, box.y1 - 2 * s), ToU32(team));
    else {
        const float inset = 5 * s;
        draw->AddLine(ImVec2(box.x0 + inset, box.y1 - inset), ImVec2(box.x1 - inset, box.y0 + inset),
            ToU32(Rgba(255, 255, 255, 75)), 1.0f);
    }
    if (hover) {
        if (p.alive) ImGui::SetTooltip("%s  ·  %d HP\nWatch player", p.name.c_str(), p.health);
        else ImGui::SetTooltip("%s  ·  Dead\nWatch player", p.name.c_str());
    }
    ImGui::PopID();
}

} // namespace

DirectorFrame DrawMatchHud(const DirectorCardsModel& model, const FontSet& fonts, float clientH,
    const Rect& picture, bool interactive) {
    DirectorFrame frame;
    if (model.players.empty() || !picture.Valid()) return frame;
    const float s = clientH / kStageHeight;
    std::vector<const DirectorPlayer*> ct, t;
    for (const DirectorPlayer& p : model.players) (p.team == 3 ? ct : t).push_back(&p);
    // Five-player teams occupy 343 px at 720p. Larger teams grow symmetrically and remain squares.
    const int slots = (int)(std::max)(ct.size(), t.size());
    const float centerW = 84 * s, gap = 3 * s;
    const float square = (std::min)(40 * s, (picture.W() - centerW - 32 * s) / (2 * (std::max)(1, slots)) - gap);
    if (square <= 0) return frame;
    const float side = slots * (square + gap), width = 2 * side + centerW;
    const float x = (picture.x0 + picture.x1 - width) / 2, y = picture.y0 + 8 * s;
    frame.roster = MakeRect(x, y, x + width, y + square);
    BeginOverlay("##matchroster", frame.roster, Rgba(0, 0, 0, 0));
    for (size_t i = 0; i < ct.size(); ++i) {
        const float px = x + side - gap - square - i * (square + gap);
        PlayerSquare(*ct[i], fonts, s, MakeRect(px, y, px + square, y + square), interactive, frame);
    }
    for (size_t i = 0; i < t.size(); ++i) {
        const float px = x + side + centerW + i * (square + gap);
        PlayerSquare(*t[i], fonts, s, MakeRect(px, y, px + square, y + square), interactive, frame);
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float mid = x + side + centerW / 2;
    draw->AddRectFilled(ImVec2(x + side, y), ImVec2(x + side + centerW - gap, y + square), ToU32(Rgba(8, 12, 17, 225)));
    ImFont* timer = fonts.Get(FontRole::Value);
    ImFont* score = fonts.Get(FontRole::Button);
    char time[32], left[24], right[24];
    if (model.haveRoundTime) std::snprintf(time, sizeof(time), "%d:%02d", model.roundSeconds / 60, model.roundSeconds % 60);
    else std::snprintf(time, sizeof(time), "--:--");
    const float tw = timer->CalcTextSizeA(timer->FontSize, FLT_MAX, 0, time).x;
    draw->AddText(timer, timer->FontSize, ImVec2(std::round(mid - tw / 2), y + 2 * s),
        ToU32(model.freeze ? Theme::Accent : Theme::Text), time);
    if (model.haveScores) {
        std::snprintf(left, sizeof(left), "CT %d", model.ctScore);
        std::snprintf(right, sizeof(right), "%d T", model.tScore);
    } else { std::snprintf(left, sizeof(left), "CT -"); std::snprintf(right, sizeof(right), "- T"); }
    const float lw = score->CalcTextSizeA(score->FontSize, FLT_MAX, 0, left).x;
    draw->AddText(score, score->FontSize, ImVec2(mid - 6 * s - lw, y + 23 * s), ToU32(Palette::TeamCT), left);
    draw->AddText(score, score->FontSize, ImVec2(mid + 6 * s, y + 23 * s), ToU32(Theme::Accent), right);
    ImGui::End();
    return frame;
}

void DrawCameraAdjust(const CameraAdjustModel& model, const FontSet& fonts, float clientH, const Rect& picture) {
    if (!model.kind || model.age < 0 || model.age >= 1.65 || !picture.Valid()) return;
    const float s = clientH / kStageHeight;
    const float alpha = model.age < 0.12 ? (float)(model.age / 0.12)
        : (model.age <= 1.1 ? 1.0f : (float)((1.65 - model.age) / 0.55));
    ImFont* caption = fonts.Get(FontRole::Caption);
    ImFont* value = fonts.Get(FontRole::CardTitle);
    const char* title = model.kind == 1 ? "CAMERA SPEED" : "FIELD OF VIEW";
    char text[40];
    if (model.kind == 1) std::snprintf(text, sizeof(text), "%.3gx", model.value);
    else std::snprintf(text, sizeof(text), "%.0f°", model.value);
    const float cw = SpacedTextWidth(caption, title, 1.4f * s);
    const float vw = value->CalcTextSizeA(value->FontSize, FLT_MAX, 0, text).x;
    const float w = cw + vw + 44 * s, h = 38 * s;
    const float x = (picture.x0 + picture.x1 - w) / 2;
    const float y = picture.y0 + picture.H() * 0.17f;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), ToU32(Faded(Rgba(10, 14, 20, 225), alpha)), 6 * s);
    draw->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), ToU32(Faded(Rgba(255, 255, 255, 28), alpha)), 6 * s);
    DrawSpacedText(draw, caption, ImVec2(x + 16 * s, y + (h - caption->FontSize) / 2),
        ToU32(Faded(Theme::TextDim, alpha)), title, 1.4f * s);
    draw->AddText(value, value->FontSize, ImVec2(x + 28 * s + cw, y + (h - value->FontSize) / 2),
        ToU32(Faded(Theme::Text, alpha)), text);
}

DirectorFrame DrawDirector(DirectorState& state, const DemoBarModel& bar, const DirectorCardsModel& cards,
	const FontSet& fonts, float clientW, float clientH, float topReserve, float bottomReserve) {
	DirectorFrame frame;
	if (state.blackout) return frame;
	const float s = clientH / kStageHeight;
    frame = DrawMatchHud(cards, fonts, clientH, MakeRect(0, 0, clientW, clientH));

	frame.bar = BarPanelRect(clientW, clientH, bottomReserve);
	BeginPanel("##directorbar", frame.bar, Theme::DockBg);
	frame.demoActions = DrawDemoBar(state.bar, bar, fonts, BarRect(clientW, clientH), s);
	ImGui::End();

	if (!state.cards) return frame;
	ImDrawList* list = ImGui::GetBackgroundDrawList();
	const float k = s * kCardScale;
	char a[32], b[32], c[32], text[128];

	const CardPlacement rightAt = DirectorCard(clientW, clientH, 0);
	HudCard right(list, fonts, k, rightAt.x, rightAt.y, rightAt.w);
	right.Header("MOVIE DIRECTOR");
	right.Separator();
	right.SectionStart();
	right.Row("Mode", cards.mode, cards.freeCam ? Theme::Accent : Theme::Text);
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
	right.Finish(kCardFadeH);

	const CardPlacement leftAt = ControlsCard(clientW, clientH);
	HudCard left(list, fonts, k, leftAt.x, leftAt.y, leftAt.w);
	left.Header("CONTROLS");
	left.Separator();
	left.SectionStart();
	for (const HudControl& control : kControls) left.ControlRow(control);
	left.SectionEnd();
	left.Finish(kCardFadeH);
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
