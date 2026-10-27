#include "DirectorHud.h"

#include "DirectorMath.h"
#include "HudCard.h"
#include "UiWidgets.h"

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

	const CardPlacement rightAt = DirectorCard(clientW, clientH, topReserve);
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
