#include "PathInspector.h"

#include "UiWidgets.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace SourceFilmmaker {
namespace Ui {

namespace {

// UTF-8 for the two non-ASCII characters CS2's copy uses (the atlas carries Latin-1).
#define MIDDOT "\xC2\xB7"
#define DEGREE "\xC2\xB0"
#define PLUSMINUS "\xC2\xB1"

// CS2's channel colours (CameraEditorJs.h S.axis): roll 5, FOV 6, shared with the graph editor.
constexpr Color kAxisRoll = Rgba(0x25, 0xc8, 0xff, 0xcc);
constexpr Color kAxisFov = Rgba(0xff, 0x9a, 0x5a, 0xcc);
constexpr int kChannelRoll = 5, kChannelFov = 6;

const char* const kInterp[] = { "Linear", "Smooth" };
const char* const kEase[] = { "None", "In", "Out", "In/Out" };
const char* const kEaseSummary[] = { "no ease", "ease in", "ease out", "ease in/out" };
// The strip's order is CS2's: Manual, Per-Seg, Constant; the path numbers them 0, 2, 1.
const char* const kSpeedStrip[] = { "Manual", "Per-Seg", "Constant" };
const int kSpeedStripMode[] = { 0, 2, 1 };
const char* const kTiming[] = { "Live", "Freeze" };
const float kSpeedSteps[] = { 0.2f, 0.5f, 0.8f, 1.0f };

const KeyHint kPathKeys[] = {
	{ "K", "place" }, { "J", "arm path" }, { "L", "delete" }, { "F", "edit" }, { "Space", "play" }, { "Left / Right", PLUSMINUS "5 s" },
	{ "Shift+Scroll", "move speed" }, { "Ctrl+Scroll", "FOV" }, { "X / Z", "roll right / left" },
};

std::string Format(const char* fmt, ...) {
	char buf[256];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	return buf;
}

void Push(std::vector<PathInspectorAction>& out, PathInspectorAction::Kind kind, int index = -1, double value = 0) {
	PathInspectorAction a;
	a.kind = kind;
	a.index = index;
	a.value = value;
	out.push_back(a);
}

// The ladder step next to the current amount (CS2's stepMul): 0.20, 0.50, 0.80, 1.00.
float StepSpeed(float current, int direction) {
	int best = 0;
	for (int i = 1; i < 4; ++i)
		if (std::fabs(kSpeedSteps[i] - current) < std::fabs(kSpeedSteps[best] - current)) best = i;
	best += direction > 0 ? 1 : -1;
	return kSpeedSteps[best < 0 ? 0 : (best > 3 ? 3 : best)];
}

void Triangle(ImDrawList* draw, const Rect& r, bool left, Color color, float s) {
	const float cx = (r.x0 + r.x1) * 0.5f, cy = (r.y0 + r.y1) * 0.5f, h = 6 * s, w = 5 * s;
	if (left) draw->AddTriangleFilled(ImVec2(cx + w * 0.5f, cy - h), ImVec2(cx + w * 0.5f, cy + h), ImVec2(cx - w, cy), ToU32(color));
	else draw->AddTriangleFilled(ImVec2(cx - w * 0.5f, cy - h), ImVec2(cx - w * 0.5f, cy + h), ImVec2(cx + w, cy), ToU32(color));
}

void Rail(const PathInspectorModel& m, const FontSet& fonts, float s, const Rect& rail, std::vector<PathInspectorAction>& actions) {
	// The rail's own right rule, as CS2's edRailHost border.
	ImGui::GetWindowDrawList()->AddLine(ImVec2(rail.x1, rail.y0), ImVec2(rail.x1, rail.y1), ToU32(Rgba(255, 255, 255, 18)), 1.0f);
	const Rect inner = MakeRect(rail.x0, rail.y0, rail.x1 - Metrics::rail.padR * s, rail.y1);
	float width = inner.W();
	CardColumn c;
	c.fonts = &fonts;
	c.s = s;
	c.x0 = inner.x0;
	c.y = BeginCardScroll("##pathrail", inner, width);
	c.x1 = inner.x0 + width;
	RailGroup(c, "Keys", true);
	const int count = (int)m.keys.size();
	if (count == 0) {
		RailRow(c, "none", "No cameras", "Press K to place one", true, Theme::Accent);
	}
	for (int i = 0; i < count; ++i) {
		const PathInspectorKey& k = m.keys[(size_t)i];
		std::string sum = Format("Tick %d " MIDDOT " %s", k.tick, kInterp[m.interp == 1 ? 1 : 0]);
		if (i == m.selected) sum += Format(" " MIDDOT " FOV %.1f", k.fov);
		ImGui::PushID(i);
		if (RailRow(c, "camera", Format("Camera %d", i + 1).c_str(), sum.c_str(), i == m.selected, Theme::Accent))
			Push(actions, PathInspectorAction::Kind::Select, i);
		ImGui::PopID();
	}
	if (RailAddButton(c, "addcamera", "+ Add camera")) Push(actions, PathInspectorAction::Kind::Add);
	EndCardScroll(c.x0, c.y + 8 * s);
}

void Card(PathInspectorState& state, const PathInspectorModel& m, const FontSet& fonts, float s, const Rect& pane,
	std::vector<PathInspectorAction>& actions) {
	using Kind = PathInspectorAction::Kind;
	const int count = (int)m.keys.size();
	const bool has = m.selected >= 0 && m.selected < count;
	const PathInspectorKey key = has ? m.keys[(size_t)m.selected] : PathInspectorKey();
	const bool isLast = has && m.selected == count - 1;
	const bool perSeg = m.speedMode == 2, constMode = m.speedMode == 1;

	float width = pane.W();
	CardColumn c;
	c.fonts = &fonts;
	c.s = s;
	c.x0 = pane.x0;
	c.y = BeginCardScroll("##pathpane", pane, width);
	c.x1 = pane.x0 + width;
	ImDrawList* draw = ImGui::GetWindowDrawList();

	// ---- the header: what the path is doing, Clear all and + Add ------------------------------
	const float mul = perSeg ? (has ? key.speedMul : 1.0f) : m.constSpeed;
	std::string summary = has ? Format("Camera %d of %d", m.selected + 1, count) : (count > 0 ? Format("%d cameras", count) : "No cameras");
	summary += Format(" " MIDDOT " %s " MIDDOT " %s " MIDDOT " ", m.interp == 1 ? "smooth" : "linear",
		has ? kEaseSummary[key.ease >= 0 && key.ease < 4 ? key.ease : 0] : "no key");
	summary += perSeg ? Format("per-segment %.2fx", mul) : (constMode ? Format("constant %.2fx", mul) : std::string("manual"));
	const CardAction header[] = { { "Clear all", ButtonKind::Danger, count > 0 }, { "+ Add", ButtonKind::Solid, true } };
	switch (CardHeader(c, "selcam", "Selected camera", summary.c_str(), header, 2)) {
	case 0: state.confirmClear = true; break;
	case 1: Push(actions, Kind::Add); break;
	default: break;
	}

	// ---- which camera: previous, Key #n / count and its tick, next ----------------------------
	{
		c.y += 4 * s;
		const Rect tile = MakeRect(c.x0, c.y, c.x1, c.y + 52 * s);
		draw->AddRectFilled(ImVec2(tile.x0, tile.y0), ImVec2(tile.x1, tile.y1), ToU32(kCardTileBg));
		ImFont* bold = fonts.Get(FontRole::BodyBold);
		ImFont* small = fonts.Get(FontRole::Note);
		const Rect prev = MakeRect(tile.x0 + 6 * s, tile.y0 + 7 * s, tile.x0 + 50 * s, tile.y1 - 7 * s);
		const Rect next = MakeRect(tile.x1 - 50 * s, tile.y0 + 7 * s, tile.x1 - 6 * s, tile.y1 - 7 * s);
		if (CardButton("prev", "", prev, ButtonKind::Normal, count > 0, bold)) Push(actions, Kind::SelectDelta, -1);
		if (CardButton("next", "", next, ButtonKind::Normal, count > 0, bold)) Push(actions, Kind::SelectDelta, 1);
		const Color arrow = count > 0 ? Theme::Text : Theme::TextDisabled;
		Triangle(draw, prev, true, arrow, s);
		Triangle(draw, next, false, arrow, s);
		const std::string which = has ? Format("Key #%d / %d", m.selected + 1, count) : (count > 0 ? Format("- / %d", count) : "No cameras");
		const std::string tick = has ? Format("tick %d", key.tick) : (count > 0 ? "No key selected" : "Press K to place one");
		const ImVec2 ws = bold->CalcTextSizeA(bold->FontSize, 1e9f, 0, which.c_str());
		const ImVec2 ts = small->CalcTextSizeA(small->FontSize, 1e9f, 0, tick.c_str());
		const float midX = (prev.x1 + next.x0) * 0.5f, top = tile.y0 + (tile.H() - ws.y - 2 * s - ts.y) * 0.5f;
		draw->AddText(bold, bold->FontSize, ImVec2(std::floor(midX - ws.x * 0.5f), top), ToU32(Theme::Text), which.c_str());
		draw->AddText(small, small->FontSize, ImVec2(std::floor(midX - ts.x * 0.5f), top + ws.y + 2 * s), ToU32(Theme::TextDim), tick.c_str());
		c.y = tile.y1;
	}

	// ---- THIS CAMERA ---------------------------------------------------------------------------
	const CardAction thisCamera[] = {
		{ "Reposition", ButtonKind::Primary, has && !m.repositioning },
		{ "Move to playhead", ButtonKind::Normal, has },
		{ "Delete", ButtonKind::Danger, has },
	};
	switch (ActionRow(c, "thiscamera", "This camera", thisCamera, 3)) {
	case 0: Push(actions, Kind::Reposition, m.selected); break;
	case 1: {
		PathInspectorAction a;
		a.kind = Kind::MoveToPlayhead;
		a.index = m.selected;
		a.tick = m.tick;
		actions.push_back(a);
		break;
	}
	case 2: Push(actions, Kind::Delete, m.selected); break;
	default: break;
	}
	Note(c, m.repositioning ? "Repositioning: fly the free cam there, then click in the preview (or press F) to drop the camera. Esc cancels."
	                        : "Reposition: fly the free cam to where you want it, then click (or press F) to drop the camera there.");

	// ---- LENS ----------------------------------------------------------------------------------
	SubHead(c, "Lens");
	const auto edit = [&](const ValueEdit& e, int channel) {
		if (e.began) Push(actions, Kind::EditBegin);
		if (e.changed) {
			PathInspectorAction a;
			a.kind = Kind::EditValue;
			a.index = m.selected;
			a.channel = channel;
			a.value = e.value;
			actions.push_back(a);
		}
		if (e.ended) Push(actions, Kind::EditEnd);
	};
	edit(ValueRow(c, "fov", "FOV", has ? key.fov : 1.0, 1, 170, 1, "", kAxisFov, has), kChannelFov);
	edit(ValueRow(c, "roll", "Roll", has ? key.roll : 0.0, -180, 180, 1, DEGREE, kAxisRoll, has), kChannelRoll);
	Note(c, has ? "FOV and roll belong to the selected camera and are keyed with it." : "Select a camera to edit its lens.");

	// ---- CURVE · WHOLE PATH --------------------------------------------------------------------
	SubHead(c, "Curve " MIDDOT " whole path");
	const int interp = ModeRow(c, "curve", "Curve", kInterp, 2, m.interp == 1 ? 1 : 0, true);
	if (interp >= 0) Push(actions, Kind::Interp, interp);
	const int ease = ModeRow(c, "ease", "Ease", kEase, 4, has ? key.ease : -1, has);
	if (ease >= 0) Push(actions, Kind::Ease, m.selected, ease);
	Note(c, "Ease belongs to the selected key; Curve is the whole path.");

	// ---- SPEED ---------------------------------------------------------------------------------
	SubHead(c, "Speed");
	int stripIndex = 0;
	for (int i = 0; i < 3; ++i)
		if (kSpeedStripMode[i] == m.speedMode) stripIndex = i;
	const int speed = ModeRow(c, "speedmode", "Mode", kSpeedStrip, 3, stripIndex, true);
	if (speed >= 0) Push(actions, Kind::SpeedMode, kSpeedStripMode[speed]);
	{
		// One stepper serves Per-segment and Constant; in Manual it is disabled in place.
		const bool usable = (perSeg && has && !isLast) || constMode;
		const float pad = Metrics::line.padH * s, labelW = Metrics::line.label * s;
		c.y += (Metrics::line.gap + 1) * s;
		const Rect row = MakeRect(c.x0, c.y, c.x1, c.y + 30 * s);
		ImFont* labelFont = fonts.Get(FontRole::RowLabel);
		ImFont* valueFont = fonts.Get(FontRole::Value);
		const ImVec2 ls = labelFont->CalcTextSizeA(labelFont->FontSize, 1e9f, 0, "Amount");
		draw->AddText(labelFont, labelFont->FontSize, ImVec2(row.x0 + pad, row.y0 + (row.H() - ls.y) * 0.5f), ToU32(Theme::Text), "Amount");
		const Rect box = MakeRect(row.x0 + pad + labelW + Metrics::line.colGap * s, row.y0, row.x1, row.y1);
		draw->AddRect(ImVec2(box.x0, box.y0), ImVec2(box.x1, box.y1), ToU32(usable ? Theme::Border : Faded(Theme::Border, 0.45f)));
		const Rect minus = MakeRect(box.x0, box.y0, box.x0 + 34 * s, box.y1), plus = MakeRect(box.x1 - 34 * s, box.y0, box.x1, box.y1);
		ImFont* bold = fonts.Get(FontRole::BodyBold);
		const float current = perSeg ? (has ? key.speedMul : 1.0f) : m.constSpeed;
		for (int d = -1; d <= 1; d += 2) {
			if (!CardButton(d < 0 ? "minus" : "plus", d < 0 ? "-" : "+", d < 0 ? minus : plus, ButtonKind::Normal, usable, bold)) continue;
			const float next = StepSpeed(current, d);
			if (perSeg) Push(actions, Kind::SpeedMul, m.selected, next);
			else Push(actions, Kind::ConstSpeed, -1, next);
		}
		const std::string amount = perSeg ? (has ? Format("Segment  x%.2f", key.speedMul) : std::string("select a key"))
		                                  : (constMode ? Format("Constant  x%.2f", m.constSpeed) : std::string("not used in Manual"));
		const ImVec2 as = valueFont->CalcTextSizeA(valueFont->FontSize, 1e9f, 0, amount.c_str());
		draw->AddText(valueFont, valueFont->FontSize, ImVec2(std::floor((minus.x1 + plus.x0 - as.x) * 0.5f), box.y0 + (box.H() - as.y) * 0.5f),
			ToU32(usable ? Theme::Text : Theme::TextDisabled), amount.c_str());
		c.y = row.y1;
	}
	const int timing = ModeRow(c, "timing", "Timing", kTiming, 2, m.timing == 1 ? 1 : 0, true);
	if (timing >= 0 && timing != m.timing) Push(actions, Kind::Timing, timing);
	Note(c, perSeg ? "Per-Segment: each key carries its own speed into the next one."
	               : (constMode ? "Constant: one speed for the whole path." : "Manual: the path follows the demo timeline."));

	EndCardScroll(c.x0, c.y + 10 * s);
}

} // namespace

void DrawPathInspector(PathInspectorState& state, const PathInspectorModel& model, const FontSet& fonts, float s,
	const Rect& rail, const Rect& pane, std::vector<PathInspectorAction>& actions) {
	Rail(model, fonts, s, rail, actions);
	Card(state, model, fonts, s, pane, actions);
}

float PathKeyStripHeight(const FontSet& fonts, float s, float width) {
	return KeyStripHeight(kPathKeys, (int)(sizeof(kPathKeys) / sizeof(kPathKeys[0])), fonts, s, width);
}

void DrawPathKeyStrip(ImDrawList* draw, const FontSet& fonts, float s, const Rect& r) {
	DrawKeyStrip(draw, kPathKeys, (int)(sizeof(kPathKeys) / sizeof(kPathKeys[0])), fonts, s, r);
}

void DrawPathClearConfirm(PathInspectorState& state, const FontSet& fonts, float s, float clientW, float clientH,
	std::vector<PathInspectorAction>& actions) {
	if (!state.confirmClear) return;
	// CS2's confirm overlay: the window dims, the dialog takes every click until it is answered.
	BeginOverlay("##pathclearconfirm", MakeRect(0, 0, clientW, clientH), Rgba(0, 0, 0, 150));
	ImFont* title = fonts.Get(FontRole::CardTitle);
	ImFont* body = fonts.Get(FontRole::RowLabel);
	ImFont* button = fonts.Get(FontRole::Button);
	const char* bodyText = "This deletes every path camera/keyframe for this demo.";
	const float w = 520 * s, pad = 22 * s, btnH = 34 * s;
	const float bodyH = body->CalcTextSizeA(body->FontSize, 1e9f, w - 2 * pad, bodyText).y;
	const float h = pad + title->FontSize + 12 * s + bodyH + 20 * s + btnH + pad;
	const Rect card = MakeRect(std::floor((clientW - w) * 0.5f), std::floor((clientH - h) * 0.5f),
		std::floor((clientW + w) * 0.5f), std::floor((clientH + h) * 0.5f));
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(ImVec2(card.x0, card.y0), ImVec2(card.x1, card.y1), ToU32(Theme::DockBg));
	draw->AddRect(ImVec2(card.x0, card.y0), ImVec2(card.x1, card.y1), ToU32(Theme::Accent));
	DrawSpacedText(draw, title, ImVec2(card.x0 + pad, card.y0 + pad), ToU32(Theme::Accent), "CLEAR ALL CAMERA PATHS?", 1.2f * s);
	draw->AddText(body, body->FontSize, ImVec2(card.x0 + pad, card.y0 + pad + title->FontSize + 12 * s), ToU32(Theme::Text),
		bodyText, nullptr, w - 2 * pad);
	const float by = card.y1 - pad - btnH;
	const Rect yes = MakeRect(card.x1 - pad - 172 * s, by, card.x1 - pad, by + btnH);
	const Rect no = MakeRect(yes.x0 - 8 * s - 132 * s, by, yes.x0 - 8 * s, by + btnH);
	if (CardButton("no", "No", no, ButtonKind::Normal, true, button)) state.confirmClear = false;
	if (CardButton("yes", "Yes, clear all", yes, ButtonKind::Danger, true, button)) {
		state.confirmClear = false;
		Push(actions, PathInspectorAction::Kind::ClearAll);
	}
	ImGui::End();
}

} // namespace Ui
} // namespace SourceFilmmaker
