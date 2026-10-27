#include "DemoBar.h"

#include "UiWidgets.h"

#include <cfloat>
#include <cmath>
#include <cstdio>

namespace SourceFilmmaker {
namespace Ui {

namespace {

using namespace DemoBarMath;

// Stage px. The bar is CS2's 96: the marker band and groove on top, the 36 px control row below.
const float kPadX = 12;
const float kBandTop = 8, kBandH = 24, kGrooveGap = 3, kGrooveH = 5;
const float kRowH = 36, kRowBottom = 8;
const float kGap = 8, kTightGap = 4;
const float kPlayerW = 140, kTimeW = 150;
const float kMinRowScale = 0.62f; // CS2 scales a docked control row down to this before it clips
const float kMarkerHitPx = 6;

struct Draw {
	ImDrawList* list;
	const FontSet& fonts;
	float s; // stage px -> client px for this row
	std::vector<DemoBarAction>& out;

	void Emit(DemoBarAction::Kind kind, int tick = 0, double value = 0) {
		DemoBarAction a;
		a.kind = kind;
		a.tick = tick;
		a.value = value;
		out.push_back(a);
	}
	ImFont* Font(FontRole role) const { return fonts.Get(role); }
	float Size(FontRole role) const { return FontSpecFor(role).size * s; }
	float TextW(FontRole role, const char* text) const {
		return Font(role)->CalcTextSizeA(Size(role), FLT_MAX, 0, text).x;
	}
	void Text(FontRole role, ImVec2 p, Color c, const char* text) const {
		list->AddText(Font(role), Size(role), p, ToU32(c), text);
	}
};

// A control-row button in CS:S paint: square, one-pixel border, red while held (a VGUI armed
// button), lit in litColor while on.
bool BarButton(Draw& d, const char* id, const Rect& r, bool lit = false, Color litColor = Theme::Accent) {
	ImGui::SetCursorScreenPos(ImVec2(r.x0, r.y0));
	const bool clicked = ImGui::InvisibleButton(id, ImVec2(r.W(), r.H()));
	const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
	const Color bg = held ? Theme::ButtonArmedBg : (lit ? Faded(litColor, 0.2f) : (hovered ? Theme::ButtonHoverBg : Theme::ButtonBg));
	const Color edge = lit ? litColor : (hovered ? Theme::Accent : Theme::Border);
	d.list->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), ToU32(bg));
	d.list->AddRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), ToU32(edge));
	return clicked;
}

float LabelButtonW(const Draw& d, const char* label) { return d.TextW(FontRole::Label, label) + 2 * 12 * d.s; }

bool LabelButton(Draw& d, const char* id, const Rect& r, const char* label, Color color = Theme::Accent) {
	const bool clicked = BarButton(d, id, r);
	const float w = d.TextW(FontRole::Label, label);
	d.Text(FontRole::Label, ImVec2(std::round(r.x0 + (r.W() - w) * 0.5f), std::round(r.y0 + (r.H() - d.Size(FontRole::Label)) * 0.5f)), color, label);
	return clicked;
}

void PlayIcon(Draw& d, const Rect& r, bool paused) {
	const ImU32 c = ToU32(Theme::Accent);
	const float cx = (r.x0 + r.x1) * 0.5f, cy = (r.y0 + r.y1) * 0.5f, k = 8 * d.s;
	if (paused) {
		d.list->AddTriangleFilled(ImVec2(cx - k * 0.7f, cy - k), ImVec2(cx - k * 0.7f, cy + k), ImVec2(cx + k, cy), c);
	} else {
		d.list->AddRectFilled(ImVec2(cx - k * 0.8f, cy - k), ImVec2(cx - k * 0.25f, cy + k), c);
		d.list->AddRectFilled(ImVec2(cx + k * 0.25f, cy - k), ImVec2(cx + k * 0.8f, cy + k), c);
	}
}

void LoopIcon(Draw& d, const Rect& r, Color color) {
	const float cx = (r.x0 + r.x1) * 0.5f, cy = (r.y0 + r.y1) * 0.5f, rad = 8 * d.s;
	const ImU32 c = ToU32(color);
	d.list->PathArcTo(ImVec2(cx, cy), rad, 0.35f, 5.6f, 24);
	d.list->PathStroke(c, 0, 2 * d.s);
	// The arrow head at the arc's open end.
	const float a = 0.35f;
	const ImVec2 tip(cx + rad * std::cos(a), cy + rad * std::sin(a));
	d.list->AddTriangleFilled(ImVec2(tip.x - 4 * d.s, tip.y - 1 * d.s), ImVec2(tip.x + 4 * d.s, tip.y - 1 * d.s),
		ImVec2(tip.x, tip.y + 5 * d.s), c);
}

// The marker band and the groove: round cells, kills, bomb events, the LOOP region, the playhead,
// and scrubbing. Scrubbing seeks on release; the ghost playhead follows the drag until then.
void Scrubber(Draw& d, DemoBarUi& ui, const DemoBarModel& m, const Rect& band, const Rect& groove) {
	ImDrawList* l = d.list;
	const float s = d.s;
	const int first = 0, last = m.lastTick > 0 ? m.lastTick : 1;
	auto xAt = [&](int tick) { return XAtTick(tick, band.x0, band.x1, first, last); };

	// Round cells, alternating, each with its number and a rule in the winner's colour at its end.
	l->AddRectFilled(ImVec2(band.x0, band.y0), ImVec2(band.x1, band.y1), ToU32(Rgba(0, 0, 0, 90)));
	for (const DemoBarRound& round : m.rounds) {
		const float x0 = xAt(round.start), x1 = xAt(round.end);
		l->AddRectFilled(ImVec2(x0, band.y0), ImVec2(x1, band.y1), ToU32(Faded(Theme::Accent, round.number % 2 ? 0.07f : 0.15f)));
		l->AddLine(ImVec2(x0, band.y0), ImVec2(x0, band.y1), ToU32(Theme::Border));
		char label[16];
		std::snprintf(label, sizeof(label), "R%d", round.number);
		if (x1 - x0 > d.TextW(FontRole::Small, label) + 8 * s)
			d.Text(FontRole::Small, ImVec2(x0 + 4 * s, band.y0 + 1 * s), Theme::TextDim, label);
		if (round.endTick >= 0 && round.winner.a) {
			const float xe = xAt(round.endTick);
			l->AddRectFilled(ImVec2(xe - 1 * s, band.y0), ImVec2(xe + 1 * s, band.y1), ToU32(round.winner));
		}
	}
	if (m.kills.empty() && m.rounds.empty() && !m.markerNote.empty())
		d.Text(FontRole::Small, ImVec2(band.x0 + 6 * s, band.y0 + (band.H() - d.Size(FontRole::Small)) * 0.5f), Theme::TextDim, m.markerNote.c_str());

	// LOOP region, between its In and Out flags. A held flag is where the drag has it.
	const ImVec2 mouse = ImGui::GetIO().MousePos;
	const bool loopShown = m.loopOn && m.loop.Valid();
	int loopHover = -1;
	if (loopShown) {
		const double span = (double)(last - first);
		if (ui.loopFlag < 0) {
			ui.loopFrac[0] = (m.loop.in - first) / span;
			ui.loopFrac[1] = (m.loop.out - first) / span;
		}
		const float fx[2] = { band.x0 + (float)ui.loopFrac[0] * band.W(), band.x0 + (float)ui.loopFrac[1] * band.W() };
		const float x0 = fx[0] < fx[1] ? fx[0] : fx[1], x1 = fx[0] < fx[1] ? fx[1] : fx[0];
		l->AddRectFilled(ImVec2(x0, band.y0), ImVec2(x1, groove.y1), ToU32(Faded(Theme::Loop, 0.16f)));
		// Two rails in the band, In over Out, each a line at its tick and a tab outside the region.
		const float railH = (band.H() - 2 * s) * 0.5f;
		const bool fine = ImGui::GetIO().KeyShift;
		for (int f = 0; f < 2; ++f) {
			const float ry = band.y0 + f * (railH + 2 * s);
			const float lw = kLoopFlagLineW * s, tw = kLoopFlagTabW * s, gw = kLoopFlagGrabW * s;
			const float tx = LoopFlagTabX(fx[f], f == 0, band.x0, band.x1, tw, lw);
			const bool held = ui.loopFlag == f;
			// Hit targets: the tab and a strip wider than the line. Submitted before the scrubber, so a
			// press on a flag is the flag's.
			ImGui::PushID(f);
			ImGui::SetCursorScreenPos(ImVec2(tx, ry));
			ImGui::InvisibleButton("##looptab", ImVec2(tw, railH));
			bool hovered = ImGui::IsItemHovered(), pressed = ImGui::IsItemActivated(), active = ImGui::IsItemActive();
			ImGui::SetCursorScreenPos(ImVec2(fx[f] - gw * 0.5f, ry));
			ImGui::InvisibleButton("##loopline", ImVec2(gw, railH));
			hovered = hovered || ImGui::IsItemHovered();
			pressed = pressed || ImGui::IsItemActivated();
			active = active || ImGui::IsItemActive();
			ImGui::PopID();
			if (pressed) {
				ui.loopFlag = f;
				ui.loopAnchorFrac = ui.loopFrac[f];
				ui.loopAnchorX = mouse.x;
				ui.loopAnchorFine = fine;
			}
			if (ui.loopFlag == f && active) {
				// Pressing or letting go of Shift mid-drag re-anchors, so the flag never jumps.
				if (fine != ui.loopAnchorFine) {
					ui.loopAnchorFrac = ui.loopFrac[f];
					ui.loopAnchorX = mouse.x;
					ui.loopAnchorFine = fine;
				}
				ui.loopFrac[f] = LoopFlagDrag(ui.loopAnchorFrac, ui.loopAnchorX, mouse.x, band.W(), fine);
			}
			if (ui.loopFlag == f && !active) { // let go: both flags commit, in order
				ui.loopFlag = -1;
				const LoopRegion r = LoopFromFlags((int)std::lround(first + ui.loopFrac[0] * span),
					(int)std::lround(first + ui.loopFrac[1] * span));
				d.Emit(DemoBarAction::Kind::LoopRange, r.in, r.out);
			}
			if (hovered || held) loopHover = f;
			const float x = band.x0 + (float)ui.loopFrac[f] * band.W();
			const Color c = hovered || held ? Rgba(170, 255, 190) : Theme::Loop;
			l->AddRectFilled(ImVec2(x - lw * 0.5f, ry), ImVec2(x + lw * 0.5f, ry + railH), ToU32(c));
			const float tab = LoopFlagTabX(x, f == 0, band.x0, band.x1, tw, lw);
			l->AddRectFilled(ImVec2(tab, ry + 1 * s), ImVec2(tab + tw, ry + railH - 1 * s), ToU32(c));
			const char* letter = f == 0 ? "I" : "O";
			const float lx = tab + (tw - d.TextW(FontRole::Label, letter)) * 0.5f;
			d.Text(FontRole::Label, ImVec2(lx, ry + (railH - d.Size(FontRole::Label)) * 0.5f), Rgba(11, 16, 22), letter);
		}
		// The tick under a held or hovered flag, so it can be set to the tick by eye.
		if (loopHover >= 0) {
			const int tick = (int)std::lround(first + ui.loopFrac[loopHover] * span);
			char time[24], text[96];
			FormatTime(tick * m.intervalPerTick, time, sizeof(time));
			std::snprintf(text, sizeof(text), "%s  %d  \xC2\xB7  %s%s", loopHover == 0 ? "IN" : "OUT", tick, time,
				ui.loopFlag >= 0 && fine ? "  \xC2\xB7  FINE" : "");
			const float w = d.TextW(FontRole::Small, text) + 12 * s, h = d.Size(FontRole::Small) + 6 * s;
			float x = band.x0 + (float)ui.loopFrac[loopHover] * band.W() - w * 0.5f;
			x = x < band.x0 ? band.x0 : (x + w > band.x1 ? band.x1 - w : x);
			const float y = band.y0 - h - 4 * s;
			ImDrawList* fg = ImGui::GetForegroundDrawList();
			fg->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), ToU32(Theme::PanelBg));
			fg->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), ToU32(Theme::Loop));
			fg->AddText(d.Font(FontRole::Small), d.Size(FontRole::Small), ImVec2(x + 6 * s, y + 3 * s), ToU32(Theme::Text), text);
		}
	} else {
		ui.loopFlag = -1;
	}

	// Bomb events: a small orange square with its label.
	for (const DemoBarBomb& bomb : m.bombs) {
		const float x = xAt(bomb.tick), y = band.y0 + band.H() * 0.5f;
		l->AddRectFilled(ImVec2(x - 3 * s, y - 3 * s), ImVec2(x + 3 * s, y + 3 * s), ToU32(Theme::Accent));
	}

	// Kills: a stem from the groove up to a head in the victim's team colour; a headshot gets a ring.
	const bool overBand = mouse.x >= band.x0 && mouse.x <= band.x1 && mouse.y >= band.y0 && mouse.y <= groove.y1;
	int hoveredKill = -1;
	float hoveredDist = kMarkerHitPx * s + 1;
	for (size_t i = 0; i < m.kills.size(); ++i) {
		const DemoBarKill& kill = m.kills[i];
		const float x = std::round(xAt(kill.tick));
		const float headY = band.y0 + band.H() * 0.38f;
		l->AddLine(ImVec2(x, headY), ImVec2(x, band.y1), ToU32(Faded(kill.color, 0.8f)), 1.0f);
		l->AddCircleFilled(ImVec2(x, headY), 3.5f * s, ToU32(kill.color), 10);
		if (kill.headshot) l->AddCircle(ImVec2(x, headY), 5.5f * s, ToU32(kill.color), 12, 1.0f);
		const float dist = std::fabs(mouse.x - x);
		if (overBand && dist < hoveredDist) { hoveredDist = dist; hoveredKill = (int)i; }
	}

	// The groove and the played part.
	l->AddRectFilled(ImVec2(groove.x0, groove.y0), ImVec2(groove.x1, groove.y1), ToU32(Rgba(255, 255, 255, 30)));
	const float playX = xAt(ui.scrubbing ? ui.scrubTick : m.tick);
	l->AddRectFilled(ImVec2(groove.x0, groove.y0), ImVec2(playX, groove.y1), ToU32(Theme::AccentDim));

	// Input: the band and the groove are one scrub target.
	ImGui::SetCursorScreenPos(ImVec2(band.x0, band.y0));
	ImGui::InvisibleButton("##scrub", ImVec2(band.W(), groove.y1 - band.y0));
	if (ImGui::IsItemActivated()) {
		ui.scrubbing = true;
		ui.scrubPressX = mouse.x;
	}
	if (ui.scrubbing && ImGui::IsItemActive()) ui.scrubTick = TickAtX(mouse.x, band.x0, band.x1, first, last);
	if (ui.scrubbing && ImGui::IsItemDeactivated()) {
		ui.scrubbing = false;
		const bool click = std::fabs(mouse.x - ui.scrubPressX) <= kClickSlopPx * s;
		// A click on a kill plays into it; any other click or drag goes where it was let go.
		if (click && hoveredKill >= 0) d.Emit(DemoBarAction::Kind::Seek, m.kills[hoveredKill].tick - EventLeadTicks(m.intervalPerTick));
		else d.Emit(DemoBarAction::Kind::Seek, TickAtX(mouse.x, band.x0, band.x1, first, last));
	}

	// The playhead over everything.
	l->AddRectFilled(ImVec2(playX - 1 * s, band.y0 - 2 * s), ImVec2(playX + 1 * s, groove.y1 + 2 * s), ToU32(Theme::Accent));
	l->AddTriangleFilled(ImVec2(playX - 5 * s, band.y0 - 3 * s), ImVec2(playX + 5 * s, band.y0 - 3 * s),
		ImVec2(playX, band.y0 + 3 * s), ToU32(Theme::Accent));

	// Hover readout: the kill under the pointer, or the time and tick there (a flag shows its own).
	if ((overBand || ui.scrubbing) && loopHover < 0) {
		char text[160];
		if (hoveredKill >= 0 && !ui.scrubbing) {
			char time[24];
			FormatTime(m.kills[hoveredKill].tick * m.intervalPerTick, time, sizeof(time));
			std::snprintf(text, sizeof(text), "%s  %s", m.kills[hoveredKill].text.c_str(), time);
		} else {
			const int tick = ui.scrubbing ? ui.scrubTick : TickAtX(mouse.x, band.x0, band.x1, first, last);
			char time[24];
			FormatTime(tick * m.intervalPerTick, time, sizeof(time));
			std::snprintf(text, sizeof(text), "%s  tick %d", time, tick);
		}
		const float w = d.TextW(FontRole::Small, text) + 12 * s, h = d.Size(FontRole::Small) + 8 * s;
		float x = mouse.x - w * 0.5f;
		if (x < band.x0) x = band.x0;
		if (x + w > band.x1) x = band.x1 - w;
		const float y = band.y0 - h - 4 * s;
		ImDrawList* fg = ImGui::GetForegroundDrawList();
		fg->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), ToU32(Theme::PanelBg));
		fg->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), ToU32(Theme::Border));
		fg->AddText(d.Font(FontRole::Small), d.Size(FontRole::Small), ImVec2(x + 6 * s, y + 4 * s), ToU32(Theme::Text), text);
		if (!ui.scrubbing) fg->AddLine(ImVec2(mouse.x, band.y0), ImVec2(mouse.x, groove.y1), ToU32(Faded(Theme::Text, 0.5f)));
	}
}

} // namespace

bool DrawSpeedDial(ImDrawList* list, const FontSet& fonts, float scale, SpeedDialState& ui, double timescale,
	const Rect& r, const char* id, double& speed) {
	std::vector<DemoBarAction> sent;
	Draw d{ list, fonts, scale, sent };
	ImDrawList* l = d.list;
	const float s = d.s;
	const ImVec2 mouse = ImGui::GetIO().MousePos;
	if (!ui.active) ui.value = SliderFor(timescale);

	ImGui::SetCursorScreenPos(ImVec2(r.x0, r.y0));
	ImGui::InvisibleButton(id, ImVec2(r.W(), r.H()));
	const bool hovered = ImGui::IsItemHovered();
	auto valueAt = [&](float x) { return Clamp01((x - r.x0) / (double)r.W()); };
	if (ImGui::IsItemActivated()) {
		ui.active = true;
		ui.dragged = false;
		ui.pressX = mouse.x;
	}
	if (ui.active && ImGui::IsItemActive()) {
		if (std::fabs(mouse.x - ui.pressX) > kClickSlopPx * s) ui.dragged = true;
		if (ui.dragged) {
			ui.value = valueAt(mouse.x);
			const double dragged = SpeedFor(ui.value);
			if (SpeedChanged(dragged, ui.lastSent)) {
				d.Emit(DemoBarAction::Kind::Speed, 0, dragged);
				ui.lastSent = dragged;
			}
		}
	}
	if (ui.active && ImGui::IsItemDeactivated()) {
		ui.active = false;
		const double released = ui.dragged ? SpeedFor(valueAt(mouse.x)) : kZoneSpeeds[ZoneAt(valueAt(ui.pressX))];
		ui.value = SliderFor(released);
		d.Emit(DemoBarAction::Kind::Speed, 0, released);
		ui.lastSent = released;
	}

	// The strip, its zones (labelled while hovered), the 1x centre mark and the handle.
	l->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), ToU32(Rgba(0, 0, 0, 150)));
	l->AddRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), ToU32(Theme::Border));
	const float cx = (r.x0 + r.x1) * 0.5f;
	const bool atOne = std::fabs(timescale - 1.0) < 0.005;
	l->AddLine(ImVec2(cx, r.y0 + 3 * s), ImVec2(cx, r.y1 - 3 * s), ToU32(Faded(Theme::Text, atOne ? 0.6f : 0.18f)));
	const int hoveredZone = hovered ? ZoneAt(valueAt(mouse.x)) : -1;
	for (int i = 0; i < kZoneCount; ++i) {
		const float zx0 = r.x0 + r.W() * i / kZoneCount;
		if (i > 0 && hovered) l->AddLine(ImVec2(zx0, r.y0 + 6 * s), ImVec2(zx0, r.y1 - 6 * s), ToU32(Faded(Theme::Text, 0.24f)));
		const float mark = r.x0 + (float)SliderFor(kZoneSpeeds[i]) * r.W();
		l->AddLine(ImVec2(mark, r.y1 - 7 * s), ImVec2(mark, r.y1 - 3 * s), ToU32(Faded(Theme::Text, 0.35f)));
		if (hovered && !ui.dragged) {
			const float w = d.TextW(FontRole::Small, kZoneLabels[i]);
			const float zoneCx = zx0 + r.W() / kZoneCount * 0.5f;
			d.Text(FontRole::Small, ImVec2(std::round(zoneCx - w * 0.5f), r.y0 + 2 * s),
				i == hoveredZone ? Theme::Accent : Faded(Theme::Text, 0.45f), kZoneLabels[i]);
		}
	}
	// Slow is the menu's dim green-grey, 1x off-white, fast the HUD orange.
	const double v = ui.value;
	const Color slow = Palette::DimText, mid = Palette::OffWhite, fast = Palette::Orange;
	const Color a = v < 0.5 ? slow : mid, b = v < 0.5 ? mid : fast;
	const float t = (float)(v < 0.5 ? v / 0.5 : (v - 0.5) / 0.5);
	const Color tint = Rgba((int)(a.r + (b.r - a.r) * t), (int)(a.g + (b.g - a.g) * t), (int)(a.b + (b.b - a.b) * t));
	const float hw = (ui.active ? 9 : (hovered ? 7 : 4)) * s;
	const float hx = r.x0 + (float)v * (r.W() - hw);
	l->AddRectFilled(ImVec2(hx, r.y0 + 2 * s), ImVec2(hx + hw, r.y1 - 2 * s), ToU32(tint));
	// The readout pill, faded while the zone labels are showing.
	char text[16];
	FormatSpeed(ui.active ? SpeedFor(v) : timescale, text, sizeof(text));
	const float pw = 70 * s, ph = 20 * s;
	const Rect pill = MakeRect(cx - pw * 0.5f, r.y0 + (r.H() - ph) * 0.5f, cx + pw * 0.5f, r.y0 + (r.H() + ph) * 0.5f);
	const float fade = hovered && !ui.dragged ? 0.25f : 1.0f;
	l->AddRectFilled(ImVec2(pill.x0, pill.y0), ImVec2(pill.x1, pill.y1), ToU32(Faded(Rgba(0, 0, 0, 230), fade)));
	l->AddRect(ImVec2(pill.x0, pill.y0), ImVec2(pill.x1, pill.y1), ToU32(Faded(Theme::Border, fade)));
	const float tw = d.TextW(FontRole::Label, text);
	d.Text(FontRole::Label, ImVec2(std::round(cx - tw * 0.5f), std::round(pill.y0 + (ph - d.Size(FontRole::Label)) * 0.5f)),
		Faded(tint, fade), text);
	if (sent.empty()) return false;
	speed = sent.back().value;
	return true;
}

std::string DemoBarLayoutText(const DemoBarLayout& l) {
	std::string text;
	auto add = [&](const char* name, const Rect& r) {
		if (!r.Valid()) return;
		char part[80];
		std::snprintf(part, sizeof(part), "%s%s=(%.0f %.0f %.0f %.0f)", text.empty() ? "" : " ", name, r.x0, r.y0, r.x1, r.y1);
		text += part;
	};
	add("bar", l.bar); add("band", l.band); add("play", l.play); add("back", l.back); add("forward", l.forward);
	add("roundprev", l.roundPrev); add("roundnext", l.roundNext); add("nextkill", l.nextKill); add("camera", l.camera);
	add("loop", l.loop); add("cameditor", l.camEditor); add("dial", l.dial);
	return text;
}

std::vector<DemoBarAction> DrawDemoBar(DemoBarUi& ui, const DemoBarModel& m, const FontSet& fonts, const Rect& rect, float scale) {
	std::vector<DemoBarAction> out;
	ui.layout = DemoBarLayout();
	if (!rect.Valid() || !(scale > 0)) return out;
	ui.layout.bar = rect;
	ImGui::PushID("demobar");
	Draw d{ ImGui::GetWindowDrawList(), fonts, scale, out };
	const float s = scale;

	if (!m.demo) {
		d.Text(FontRole::Body, ImVec2(rect.x0 + kPadX * s, rect.y0 + (rect.H() - d.Size(FontRole::Body)) * 0.5f), Theme::TextDim,
			"No demo playing. The demo bar appears with playdemo.");
		ImGui::PopID();
		return out;
	}

	// Scrubber across the whole bar.
	const Rect band = MakeRect(rect.x0 + kPadX * s, rect.y0 + kBandTop * s, rect.x1 - kPadX * s, rect.y0 + (kBandTop + kBandH) * s);
	const Rect groove = MakeRect(band.x0, band.y1 + kGrooveGap * s, band.x1, band.y1 + (kGrooveGap + kGrooveH) * s);
	Scrubber(d, ui, m, band, groove);
	ui.layout.band = MakeRect(band.x0, band.y0, band.x1, groove.y1);

	// Control row. Measured at full size first; a narrow bar scales the row down (CS2 keeps it at
	// 62% or more), so nothing overlaps.
	char roundPrev[] = "< ROUND", roundNext[] = "ROUND >", nextKill[] = "NEXT KILL >";
	const char* camera = m.cameraMode.empty() ? "CAMERA" : m.cameraMode.c_str();
	auto rowWidth = [&](Draw& dd) {
		float w = kRowH * dd.s + kGap * dd.s + kTimeW * dd.s + kGap * dd.s;
		w += LabelButtonW(dd, "-5S") + kTightGap * dd.s + LabelButtonW(dd, "+5S") + kGap * dd.s;
		w += LabelButtonW(dd, roundPrev) + kTightGap * dd.s + LabelButtonW(dd, roundNext) + kGap * dd.s + kPlayerW * dd.s;
		w += LabelButtonW(dd, nextKill) + kTightGap * dd.s + LabelButtonW(dd, camera) + kGap * dd.s + kRowH * dd.s;
		if (m.showCamEditorButton) w += kTightGap * dd.s + LabelButtonW(dd, "CAM EDITOR");
		return w + 2 * 16 * dd.s + kDialMinW * dd.s; // the dial at its smallest, with its margins
	};
	const float avail = rect.W() - 2 * kPadX * s;
	float k = 1;
	{
		const float need = rowWidth(d);
		if (need > avail) k = avail / need < kMinRowScale ? kMinRowScale : avail / need;
	}
	Draw r{ d.list, fonts, s * k, out };
	const float rs = r.s;
	const float rowY1 = rect.y1 - kRowBottom * s, rowY0 = rowY1 - kRowH * rs;
	float x = rect.x0 + kPadX * s;
	auto box = [&](float w) { const Rect b = MakeRect(x, rowY0, x + w, rowY1); x += w; return b; };

	// Play / pause.
	const Rect play = box(kRowH * rs);
	ui.layout.play = play;
	if (BarButton(r, "##play", play)) r.Emit(DemoBarAction::Kind::TogglePause);
	PlayIcon(r, play, m.paused);
	x += kGap * rs;

	// Time and tick.
	{
		char now[24], total[24], line[64], tickLine[48];
		FormatTime(m.tick * m.intervalPerTick, now, sizeof(now));
		FormatTime(m.lastTick * m.intervalPerTick, total, sizeof(total));
		std::snprintf(line, sizeof(line), "%s / %s", now, total);
		std::snprintf(tickLine, sizeof(tickLine), "TICK %d / %d", m.tick, m.lastTick);
		const Rect t = box(kTimeW * rs);
		const float gapY = (kRowH * rs - r.Size(FontRole::BodyBold) - r.Size(FontRole::Small)) / 3;
		r.Text(FontRole::BodyBold, ImVec2(t.x0, t.y0 + gapY), Theme::Text, line);
		r.Text(FontRole::Small, ImVec2(t.x0, t.y0 + 2 * gapY + r.Size(FontRole::BodyBold)), Theme::TextDim, tickLine);
	}
	x += kGap * rs;

	// -5s / +5s.
	const int step = TicksForSeconds(5, m.intervalPerTick);
	ui.layout.back = box(LabelButtonW(r, "-5S"));
	if (LabelButton(r, "##back", ui.layout.back, "-5S")) r.Emit(DemoBarAction::Kind::Seek, m.tick - step);
	x += kTightGap * rs;
	ui.layout.forward = box(LabelButtonW(r, "+5S"));
	if (LabelButton(r, "##fwd", ui.layout.forward, "+5S")) r.Emit(DemoBarAction::Kind::Seek, m.tick + step);
	x += kGap * rs;

	// Round stepping: back goes to this round's start, or the one before when already there.
	{
		const int margin = (int)std::lround(1.0 / (m.intervalPerTick > 0 ? m.intervalPerTick : 0.015));
		ui.layout.roundPrev = box(LabelButtonW(r, roundPrev));
		if (LabelButton(r, "##roundprev", ui.layout.roundPrev, roundPrev)) {
			const int t = PreviousTickBefore(m.roundStarts, m.tick, margin);
			r.Emit(DemoBarAction::Kind::Seek, t >= 0 ? t : 0);
		}
		x += kTightGap * rs;
		const int nextRound = NextTickAfter(m.roundStarts, m.tick, 0);
		ui.layout.roundNext = box(LabelButtonW(r, roundNext));
		if (LabelButton(r, "##roundnext", ui.layout.roundNext, roundNext, nextRound >= 0 ? Theme::Accent : Theme::TextDisabled)
			&& nextRound >= 0)
			r.Emit(DemoBarAction::Kind::Seek, nextRound);
		x += kGap * rs;
	}

	// The spectated player.
	{
		const Rect p = box(kPlayerW * rs);
		const float gapY = (kRowH * rs - r.Size(FontRole::Small) - r.Size(FontRole::BodyBold)) / 3;
		r.Text(FontRole::Small, ImVec2(p.x0, p.y0 + gapY), Theme::TextDim, "SPECTATING");
		ImGui::PushClipRect(ImVec2(p.x0, p.y0), ImVec2(p.x1, p.y1), true);
		r.Text(FontRole::BodyBold, ImVec2(p.x0, p.y0 + 2 * gapY + r.Size(FontRole::Small)), Theme::Text,
			m.observed.empty() ? "-" : m.observed.c_str());
		ImGui::PopClipRect();
	}
	const float leftEnd = x;

	// Right group, laid out from the right edge.
	float rx = rect.x1 - kPadX * s;
	auto rbox = [&](float w) { rx -= w; const Rect b = MakeRect(rx, rowY0, rx + w, rowY1); return b; };
	if (m.showCamEditorButton) {
		ui.layout.camEditor = rbox(LabelButtonW(r, "CAM EDITOR"));
		if (LabelButton(r, "##cameditor", ui.layout.camEditor, "CAM EDITOR")) r.Emit(DemoBarAction::Kind::CameraEditor);
		rx -= kTightGap * rs;
	}
	const Rect loop = rbox(kRowH * rs);
	ui.layout.loop = loop;
	const bool loopLit = m.loopOn && m.loop.Valid();
	if (BarButton(r, "##loop", loop, loopLit, Theme::Loop)) r.Emit(DemoBarAction::Kind::ToggleLoop);
	LoopIcon(r, loop, loopLit ? Theme::Loop : Theme::Accent);
	rx -= kGap * rs;
	ui.layout.camera = rbox(LabelButtonW(r, camera));
	if (LabelButton(r, "##camera", ui.layout.camera, camera)) r.Emit(DemoBarAction::Kind::NextCamera);
	rx -= kTightGap * rs;
	{
		// Each kill is reached a moment early, so it plays in; pressing again from there moves on.
		std::vector<int> killStarts;
		for (const DemoBarKill& kill : m.kills) killStarts.push_back(kill.tick - EventLeadTicks(m.intervalPerTick));
		const int next = NextTickAfter(killStarts, m.tick, 0);
		ui.layout.nextKill = rbox(LabelButtonW(r, nextKill));
		if (LabelButton(r, "##nextkill", ui.layout.nextKill, nextKill, next >= 0 ? Theme::Accent : Theme::TextDisabled) && next >= 0)
			r.Emit(DemoBarAction::Kind::Seek, next);
	}
	const float rightStart = rx;

	// The speed dial, centred in the room between the groups.
	const float free = rightStart - leftEnd;
	const float dialW = DialWidth(rect.W() / s, free / rs) * rs;
	if (free > dialW + 2 * 16 * rs) {
		const float cx = (leftEnd + rightStart) * 0.5f;
		ui.layout.dial = MakeRect(cx - dialW * 0.5f, rowY0, cx + dialW * 0.5f, rowY1);
		double speed = 0;
		if (DrawSpeedDial(r.list, fonts, r.s, ui.dial, m.timescale, ui.layout.dial, "##speed", speed))
			r.Emit(DemoBarAction::Kind::Speed, 0, speed);
	}

	ImGui::PopID();
	return out;
}

} // namespace Ui
} // namespace SourceFilmmaker
