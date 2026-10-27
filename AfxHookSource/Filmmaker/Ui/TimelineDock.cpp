#include "TimelineDock.h"

#include "UiWidgets.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace SourceFilmmaker {
namespace Ui {

namespace {

using namespace TimelineDockMath;
using Filmmaker::GraphExpModel;
using Action = TimelineDockAction;
using Gesture = TimelineDockState::Gesture;

// CS2's dock colours (GraphEditorJs.h `S`) in CS:S paint: CS2's gold is CS:S's HUD orange, the text
// tiers are the menu scheme's, and the white hairlines and button washes stay.
namespace Paint {
constexpr Color Panel = Theme::DockBg;
constexpr Color Border = Rgba(255, 255, 255, 31);
constexpr Color Chip = Rgba(255, 255, 255, 26);
constexpr Color RowLine = Rgba(255, 255, 255, 8);
constexpr Color LayerBg = Rgba(255, 255, 255, 10);
constexpr Color LayerSel = Rgba(255, 176, 0, 38);
constexpr Color RowSel = Rgba(255, 176, 0, 28);
constexpr Color Value = Theme::Text;
constexpr Color Label = Theme::Text;
constexpr Color Hint = Theme::TextDim;
constexpr Color Muted = Theme::TextDim;
constexpr Color Dim = Theme::TextDisabled;
constexpr Color Accent = Theme::Accent;
constexpr Color DangerText = Rgba(255, 96, 72);
constexpr Color DangerBg = Rgba(192, 28, 0, 38);
constexpr Color DangerBorder = Rgba(192, 28, 0, 102);
constexpr Color Playhead = Rgba(255, 90, 90, 238);
constexpr Color Glyph = Theme::Text;
constexpr Color Summary = Rgba(255, 255, 255, 140);
constexpr Color BtnBg = Rgba(255, 255, 255, 20);
constexpr Color BtnOn = Rgba(255, 176, 0, 51);
constexpr Color BtnHover = Rgba(255, 255, 255, 38);
constexpr Color BtnDown = Rgba(255, 255, 255, 61);
constexpr Color BtnOnHover = Rgba(255, 176, 0, 82);
constexpr Color EaseOn = Rgba(255, 176, 0, 46);
constexpr Color EaseHover = Rgba(255, 255, 255, 13);
constexpr Color OvBg = Rgba(0, 0, 0, 64);
constexpr Color OvTrack = Rgba(255, 255, 255, 13);
constexpr Color OvView = Rgba(255, 255, 255, 32);
constexpr Color OvViewHot = Rgba(255, 255, 255, 51);
constexpr Color OvViewBorder = Rgba(255, 255, 255, 77);
constexpr Color OvViewBorderHot = Rgba(255, 176, 0, 204);
constexpr Color OvKey = Rgba(255, 255, 255, 102);
constexpr Color Grip = Rgba(48, 55, 64);
constexpr Color RulerBg = Rgba(0, 0, 0, 64);
constexpr Color TickMajor = Rgba(255, 255, 255, 85);
constexpr Color TickMinor = Rgba(255, 255, 255, 38);
constexpr Color BoxFill = Rgba(255, 176, 0, 34);
constexpr Color ScrollTrack = Rgba(0, 0, 0, 115);
constexpr Color ScrollThumb = Rgba(255, 255, 255, 51);
constexpr Color TipBg = Rgba(8, 10, 14, 248);
constexpr Color MenuBg = Rgba(16, 20, 26, 252);
constexpr Color Underline = Rgba(255, 255, 255, 51);
} // namespace Paint

// CS2's camera channel colours (GraphEditorExperimentHud.cpp): x, y, z, pitch, yaw, roll, FOV.
const Color kChannel[7] = { Rgba(255, 136, 133), Rgba(128, 214, 128), Rgba(105, 167, 255), Rgba(253, 150, 215),
	Rgba(200, 156, 255), Rgba(37, 200, 255), Rgba(255, 154, 90) };

constexpr float kPi = 3.14159265358979f;
constexpr float kTipDelay = 2.0f;
constexpr double kDoubleClickSeconds = 0.4;
constexpr float kCompactStripW = 1180; // under this dock width the strip shows its glyphs only

// ---- the CAMERA PATH tree (buildTree) ----------------------------------------------------------
// The layer, its Transform group, Position, Rotation and Field of view, and a split property's
// one-channel rows. CS2's Path > Camera shake rows come with camera shakes.

enum class NodeKind { Layer, Group, Prop };

struct Node {
	NodeKind kind;
	const char* name;
	int parent;
	int count;           // channels of a property; the layer and group stand for all seven
	int chans[3];
	const char* comps[3];
	int dec;
	const char* unit;
	int split;           // a splittable property's index into TimelineDockState::split, else -1
};

const char kDegree[] = "\xC2\xB0";

const Node kNodes[] = {
	{ NodeKind::Layer, "CAMERA PATH", -1, 0, { 0, 0, 0 }, { "", "", "" }, 0, "", -1 },
	{ NodeKind::Group, "Transform", 0, 0, { 0, 0, 0 }, { "", "", "" }, 0, "", -1 },
	{ NodeKind::Prop, "Position", 1, 3, { 0, 1, 2 }, { "X", "Y", "Z" }, 0, "", 0 },
	{ NodeKind::Prop, "Rotation", 1, 3, { 3, 4, 5 }, { "Pitch", "Yaw", "Roll" }, 1, kDegree, 1 },
	{ NodeKind::Prop, "Field of view", 1, 1, { 6, 0, 0 }, { "FOV", "", "" }, 1, "", -1 },
	{ NodeKind::Prop, "X Position", 2, 1, { 0, 0, 0 }, { "X", "", "" }, 0, "", -1 },
	{ NodeKind::Prop, "Y Position", 2, 1, { 1, 0, 0 }, { "Y", "", "" }, 0, "", -1 },
	{ NodeKind::Prop, "Z Position", 2, 1, { 2, 0, 0 }, { "Z", "", "" }, 0, "", -1 },
	{ NodeKind::Prop, "Pitch", 3, 1, { 3, 0, 0 }, { "Pitch", "", "" }, 1, kDegree, -1 },
	{ NodeKind::Prop, "Yaw", 3, 1, { 4, 0, 0 }, { "Yaw", "", "" }, 1, kDegree, -1 },
	{ NodeKind::Prop, "Roll", 3, 1, { 5, 0, 0 }, { "Roll", "", "" }, 1, kDegree, -1 },
};
constexpr int kNodeCount = (int)(sizeof(kNodes) / sizeof(kNodes[0]));
constexpr int kLayerNode = 0;

bool IsSplit(const TimelineDockState& st, int n) { return kNodes[n].split >= 0 && st.split[kNodes[n].split]; }
bool Expandable(const TimelineDockState& st, int n) { return kNodes[n].kind != NodeKind::Prop || IsSplit(st, n); }

std::vector<int> ChildrenOf(const TimelineDockState& st, int n) {
	std::vector<int> out;
	if (kNodes[n].kind == NodeKind::Prop && !IsSplit(st, n)) return out;
	for (int i = 0; i < kNodeCount; ++i)
		if (kNodes[i].parent == n) out.push_back(i);
	return out;
}

std::vector<int> ChansOf(int n) {
	std::vector<int> out;
	if (kNodes[n].kind != NodeKind::Prop) {
		for (int c = 0; c < GraphExpModel::kChannelCount; ++c) out.push_back(c);
	} else {
		for (int i = 0; i < kNodes[n].count; ++i) out.push_back(kNodes[n].chans[i]);
	}
	return out;
}

// The row a channel's keys are listed on.
int NodeOfChan(const TimelineDockState& st, int ch) {
	if (ch < 3) return st.split[0] ? 5 + ch : 2;
	if (ch < 6) return st.split[1] ? 8 + (ch - 3) : 3;
	return 4;
}

struct Row {
	int node = 0;
	int depth = 0;
};

std::vector<Row> Flatten(const TimelineDockState& st) {
	std::vector<Row> rows;
	struct Walk {
		const TimelineDockState& st;
		std::vector<Row>& rows;
		void Go(int n, int depth) {
			Row r;
			r.node = n;
			r.depth = depth;
			rows.push_back(r);
			if (Expandable(st, n) && st.open[n])
				for (int c : ChildrenOf(st, n)) Go(c, depth + 1);
		}
	};
	Walk{ st, rows }.Go(kLayerNode, 0);
	return rows;
}

float RowPad(const Row& r) {
	if (kNodes[r.node].kind == NodeKind::Layer) return 6;
	if (kNodes[r.node].kind == NodeKind::Group) return 24;
	return 10 + r.depth * 18.0f;
}

// ---- keys ------------------------------------------------------------------------------------

std::vector<KeyRef> KeysOf(const GraphExpModel& m, const std::vector<int>& chans) {
	std::vector<KeyRef> out;
	for (int c : chans)
		for (const GraphExpModel::Keyframe& k : m.GetChannel(c).keys) {
			KeyRef r;
			r.ch = c;
			r.id = k.id;
			r.t = k.tick;
			r.sel = m.IsSelected(c, k.id);
			out.push_back(r);
		}
	return out;
}

std::vector<MergedKey> MergedOf(const GraphExpModel& m, int node) { return MergeKeys(KeysOf(m, ChansOf(node))); }

std::vector<double> TimesOf(const std::vector<MergedKey>& keys) {
	std::vector<double> out;
	for (const MergedKey& k : keys) out.push_back(k.t);
	return out;
}

std::vector<std::pair<int, int>> MembersOf(const MergedKey& k) {
	std::vector<std::pair<int, int>> out;
	for (const KeyRef& r : k.members) out.emplace_back(r.ch, r.id);
	return out;
}

Side LeftSide(const GraphExpModel::Keyframe& k) { return SideOf(k.cpLeft.active, k.cpLeft.autoTan); }
Side RightSide(const GraphExpModel::Keyframe& k) { return SideOf(k.cpRight.active, k.cpRight.autoTan); }

// A channel's value at the playhead: its curve, or the live camera on a channel with no keys.
double ChannelValue(const TimelineDockModel& m, int ch) {
	double v = 0;
	if (m.keys->SampleChannel(ch, m.playhead, v)) return v;
	return m.live[ch];
}

// ---- drawing -----------------------------------------------------------------------------------

struct D {
	TimelineDockState& st;
	const TimelineDockModel& m;
	const GraphExpModel& keys;
	const FontSet& fonts;
	ImDrawList* l;
	float s;            // client px per stage px
	float ox, oy;       // the dock's top left, client px
	float w;            // the dock's width, stage px
	Widths g;
	Span domain;
	std::vector<Action>& out;
	ImVec2 mouse;       // dock-local stage px
	const char* tip = nullptr;
	ImGuiID tipItem = 0;
	float tipX0 = 0, tipY0 = 0, tipX1 = 0;

	ImVec2 P(float x, float y) const { return ImVec2(ox + x * s, oy + y * s); }
	ImFont* F(FontRole r) const { return fonts.Get(r); }
	float Size(FontRole r) const { return FontSpecFor(r).size; }
	float TextW(FontRole r, const char* t, float spacing = 0) const { return SpacedTextWidth(F(r), t, spacing * s) / s; }
	void Text(FontRole r, float x, float y, Color c, const char* t, float spacing = 0) const {
		const ImVec2 p = P(x, y);
		DrawSpacedText(l, F(r), ImVec2(std::round(p.x), std::round(p.y)), ToU32(c), t, spacing * s);
	}
	// Text centred in a box, vertically and (with x1 > x0) horizontally.
	void TextIn(FontRole r, float x0, float y0, float x1, float y1, Color c, const char* t, float spacing = 0) const {
		const float tw = TextW(r, t, spacing);
		Text(r, x1 > x0 ? x0 + (x1 - x0 - tw) * 0.5f : x0, y0 + (y1 - y0 - Size(r)) * 0.5f, c, t, spacing);
	}
	void Fill(float x0, float y0, float x1, float y1, Color c, float radius = 0) const {
		if (c.a == 0 || x1 <= x0 || y1 <= y0) return;
		l->AddRectFilled(P(x0, y0), P(x1, y1), ToU32(c), radius * s);
	}
	void Outline(float x0, float y0, float x1, float y1, Color c, float radius = 0) const {
		if (c.a == 0) return;
		const ImVec2 a = P(x0, y0), b = P(x1, y1);
		l->AddRect(ImVec2(std::floor(a.x) + 0.5f, std::floor(a.y) + 0.5f), ImVec2(std::floor(b.x) - 0.5f, std::floor(b.y) - 0.5f),
			ToU32(c), radius * s, 0, 1.0f);
	}
	// One-pixel rules, whatever the scale.
	void HLine(float x0, float x1, float y, Color c) const {
		const float py = std::floor(P(0, y).y);
		l->AddRectFilled(ImVec2(P(x0, 0).x, py), ImVec2(P(x1, 0).x, py + 1), ToU32(c));
	}
	void Dotted(float x0, float x1, float y, Color c) const {
		const float py = std::floor(P(0, y).y), end = P(x1, 0).x;
		for (float px = std::floor(P(x0, 0).x); px < end; px += 2) l->AddRectFilled(ImVec2(px, py), ImVec2(px + 1, py + 1), ToU32(c));
	}
	void VLine(float x, float y0, float y1, Color c) const {
		const float px = std::floor(P(x, 0).x);
		l->AddRectFilled(ImVec2(px, P(0, y0).y), ImVec2(px + 1, P(0, y1).y), ToU32(c));
	}
	void Emit(Action::Kind kind, int ch = 0, int id = 0, double a = 0, double b = 0, bool flag = false) {
		Action act;
		act.kind = kind;
		act.ch = ch;
		act.id = id;
		act.a = a;
		act.b = b;
		act.flag = flag;
		out.push_back(act);
	}
	void EmitSelectSet(const std::vector<std::pair<int, int>>& keys) {
		Action act;
		act.kind = Action::Kind::SelectSet;
		act.keys = keys;
		out.push_back(act);
	}
	// The shared hover tip, for the item just submitted.
	void Tip(const char* text) {
		if (!text || !ImGui::IsItemHovered()) return;
		tip = text;
		tipItem = ImGui::GetItemID();
		const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
		tipX0 = (a.x - ox) / s;
		tipY0 = (a.y - oy) / s;
		tipX1 = (b.x - ox) / s;
	}
};

struct Hit {
	bool clicked = false, hovered = false, held = false;
};

// An invisible button over a stage rect.
Hit HitBox(const char* id, const D& d, float x0, float y0, float x1, float y1) {
	Hit h;
	if (x1 - x0 <= 0 || y1 - y0 <= 0) return h;
	ImGui::SetCursorScreenPos(d.P(x0, y0));
	h.clicked = ImGui::InvisibleButton(id, ImVec2((x1 - x0) * d.s, (y1 - y0) * d.s));
	h.hovered = ImGui::IsItemHovered();
	h.held = ImGui::IsItemActive();
	return h;
}

// ---- shapes --------------------------------------------------------------------------------------

enum class Icon { None, Prev, Next, Play, Pause, Undo, Redo, Close };

void DrawIcon(const D& d, Icon icon, float cx, float cy, Color c) {
	const ImU32 col = ToU32(c);
	ImDrawList* l = d.l;
	const float s = d.s;
	auto P = [&](float x, float y) { return d.P(cx + x, cy + y); };
	switch (icon) {
	case Icon::Prev:
		l->AddRectFilled(P(-5, -5), P(-3.5f, 5), col);
		l->AddTriangleFilled(P(5, -5), P(5, 5), P(-2, 0), col);
		break;
	case Icon::Next:
		l->AddTriangleFilled(P(-5, -5), P(-5, 5), P(2, 0), col);
		l->AddRectFilled(P(3.5f, -5), P(5, 5), col);
		break;
	case Icon::Play:
		l->AddTriangleFilled(P(-4, -6), P(-4, 6), P(6, 0), col);
		break;
	case Icon::Pause:
		l->AddRectFilled(P(-4.5f, -6), P(-1.5f, 6), col);
		l->AddRectFilled(P(1.5f, -6), P(4.5f, 6), col);
		break;
	case Icon::Undo:
	case Icon::Redo: {
		// An open circle, three quarters round, its head at the top: pointing left for Undo (the gap
		// upper left), right for Redo (the gap upper right).
		const float r = 4.5f, dir = icon == Icon::Undo ? -1.0f : 1.0f;
		const float a0 = icon == Icon::Undo ? -0.5f * kPi : 0.0f;
		l->PathArcTo(P(0, 1), r * s, a0, a0 + 1.5f * kPi, 18);
		l->PathStroke(col, 0, 1.5f * s > 1 ? 1.5f * s : 1.0f);
		const float ty = 1 - r;
		l->AddTriangleFilled(P(dir * 3.0f, ty), P(-dir * 1.5f, ty - 3.0f), P(-dir * 1.5f, ty + 3.0f), col);
		break;
	}
	case Icon::Close: {
		const float t = 1.5f * s > 1 ? 1.5f * s : 1.0f;
		l->AddLine(P(-4, -4), P(4, 4), col, t);
		l->AddLine(P(-4, 4), P(4, -4), col, t);
		break;
	}
	default:
		break;
	}
}

// One half of a key glyph (glyphHalf), centred on (cx, cy) with the glyph's full size.
void GlyphHalf(ImDrawList* l, ImVec2 c, float size, int side, Side kind, ImU32 col) {
	const float h = size * 0.5f, sx = side < 0 ? -1.0f : 1.0f;
	const float outer = c.x + sx * h;
	switch (kind) {
	case Side::Linear:
		l->AddTriangleFilled(ImVec2(c.x, c.y - h), ImVec2(outer, c.y), ImVec2(c.x, c.y + h), col);
		break;
	case Side::Auto:
		l->PathArcTo(c, h, side < 0 ? 0.5f * kPi : -0.5f * kPi, side < 0 ? 1.5f * kPi : 0.5f * kPi, 10);
		l->PathFillConvex(col);
		break;
	case Side::Ease: {
		// The half square less a notch on its outer edge, its apex 62 % of the way in: a bar on the
		// inner side and a triangle above and below the apex, as CS2 builds it.
		const float apex = c.x + sx * 0.38f * h;
		l->AddRectFilled(ImVec2((std::min)(c.x, apex), c.y - h), ImVec2((std::max)(c.x, apex), c.y + h), col);
		l->AddTriangleFilled(ImVec2(outer, c.y - h), ImVec2(apex, c.y - h), ImVec2(apex, c.y), col);
		l->AddTriangleFilled(ImVec2(outer, c.y + h), ImVec2(apex, c.y), ImVec2(apex, c.y + h), col);
		break;
	}
	case Side::Hold:
		l->AddRectFilled(ImVec2((std::min)(c.x, outer), c.y - h), ImVec2((std::max)(c.x, outer), c.y + h), col);
		break;
	}
}

void DrawGlyph(const D& d, float x, float y, float size, Side left, Side right, Color c) {
	const ImVec2 p = d.P(x, y);
	GlyphHalf(d.l, p, size * d.s, -1, left, ToU32(c));
	GlyphHalf(d.l, p, size * d.s, 1, right, ToU32(c));
}

// A 45-degree diamond of side `side` (CS2's dot(..., rotated)), filled or outlined.
void Diamond(const D& d, float x, float y, float side, Color c, bool filled = true) {
	const float r = side * 0.70710678f;
	const ImVec2 p = d.P(x, y);
	const float k = r * d.s;
	const ImVec2 pts[4] = { ImVec2(p.x, p.y - k), ImVec2(p.x + k, p.y), ImVec2(p.x, p.y + k), ImVec2(p.x - k, p.y) };
	if (filled) d.l->AddConvexPolyFilled(pts, 4, ToU32(c));
	else d.l->AddPolyline(pts, 4, ToU32(c), ImDrawFlags_Closed, 1.2f * d.s > 1 ? 1.2f * d.s : 1.0f);
}

// ---- small widgets ------------------------------------------------------------------------------

// CS2's toolbar button (btn): radius 3, compact padding, bold 13 px, lit while on.
bool ToolButton(D& d, const char* id, float x, float y, float w, float h, const char* label, Icon icon, Color fg,
	bool on, const char* tip, bool enabled = true) {
	Hit hit;
	if (enabled) {
		hit = HitBox(id, d, x, y, x + w, y + h);
		d.Tip(tip);
	}
	Color bg = Paint::BtnBg;
	if (hit.held) bg = Paint::BtnDown;
	else if (on) bg = hit.hovered ? Paint::BtnOnHover : Paint::BtnOn;
	else if (hit.hovered) bg = Paint::BtnHover;
	d.Fill(x, y, x + w, y + h, bg, 3);
	const Color c = !enabled ? Paint::Dim : (on ? Paint::Accent : fg);
	const float lw = label && *label ? d.TextW(FontRole::Label, label) : 0;
	const float iconW = icon != Icon::None ? 12.0f : 0.0f, gap = icon != Icon::None && lw > 0 ? 5.0f : 0.0f;
	const float x0 = x + (w - iconW - gap - lw) * 0.5f;
	if (icon != Icon::None) DrawIcon(d, icon, x0 + iconW * 0.5f, y + h * 0.5f, c);
	if (lw > 0) d.Text(FontRole::Label, x0 + iconW + gap, y + (h - d.Size(FontRole::Label)) * 0.5f, c, label);
	return hit.clicked && enabled;
}

float ToolButtonW(const D& d, const char* label, Icon icon) {
	const float lw = label && *label ? d.TextW(FontRole::Label, label) : 0;
	const float iconW = icon != Icon::None ? 12.0f : 0.0f, gap = icon != Icon::None && lw > 0 ? 5.0f : 0.0f;
	return 9 + iconW + gap + lw + 9;
}

// Ellipsised to fit maxW (stage px), as CS2's one-line labels are.
std::string Fit(const D& d, FontRole role, const std::string& text, float maxW, float spacing = 0) {
	if (d.TextW(role, text.c_str(), spacing) <= maxW) return text;
	std::string t = text;
	while (!t.empty()) {
		t.pop_back();
		while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80) t.pop_back();
		const std::string candidate = t + "\xE2\x80\xA6";
		if (d.TextW(role, candidate.c_str(), spacing) <= maxW) return candidate;
	}
	return std::string();
}

void TimeText(const TimelineDockModel& m, double tick, char* out, size_t size, bool shortForm = false) {
	FormatTime(tick, m.tickRate, out, size, shortForm);
}

// ---- the window ---------------------------------------------------------------------------------

void ManualView(D& d) { NoteManualView(d.st.paging, d.m.playing); }

void SeekTo(D& d, double t) {
	t = (std::max)(0.0, std::round(t));
	d.Emit(Action::Kind::Playhead, 0, 0, t);
	d.Emit(Action::Kind::PlayheadRelease, 0, 0, t);
	Reveal(d.st.window, d.domain, t, t);
}

void FitAll(D& d) {
	TimelineDockState& st = d.st;
	const bool again = st.fitT0 == st.window.t0 && st.fitT1 == st.window.t1;
	double a = DBL_MAX, b = -DBL_MAX;
	auto take = [&](double t) { a = (std::min)(a, t); b = (std::max)(b, t); };
	const std::vector<KeyRef> all = KeysOf(d.keys, ChansOf(kLayerNode));
	if (!again) {
		for (const KeyRef& k : all) if (k.sel) take(k.t);
		if (b < a && st.selRow >= 0)
			for (const MergedKey& k : MergedOf(d.keys, st.selRow)) take(k.t);
	}
	if (b < a) for (const KeyRef& k : all) take(k.t);
	if (b < a) { a = d.m.playhead - 500; b = d.m.playhead + 500; }
	st.window = FrameTicks(a, b, d.domain);
	st.fitT0 = st.window.t0;
	st.fitT1 = st.window.t1;
}

// Keys that were not in the model last frame and sit outside the window are revealed: the only way
// to make one off screen is from a playhead you cannot see.
void RevealNewKeys(D& d) {
	std::vector<std::pair<int, int>> seen;
	double a = DBL_MAX, b = -DBL_MAX;
	for (int c = 0; c < GraphExpModel::kChannelCount; ++c)
		for (const GraphExpModel::Keyframe& k : d.keys.GetChannel(c).keys) {
			seen.emplace_back(c, k.id);
			if (d.st.knownValid && !d.st.window.Contains(k.tick)
				&& std::find(d.st.knownKeys.begin(), d.st.knownKeys.end(), std::make_pair(c, k.id)) == d.st.knownKeys.end()) {
				a = (std::min)(a, k.tick);
				b = (std::max)(b, k.tick);
			}
		}
	d.st.knownKeys.swap(seen);
	d.st.knownValid = true;
	if (b >= a) Reveal(d.st.window, d.domain, a, b);
}

// ---- toolbar (y 0..72) --------------------------------------------------------------------------

struct EaseItem {
	EaseKind kind;
	const char* label;
	Side left, right;
	const char* tip;
};

const EaseItem kEaseItems[] = {
	{ EaseKind::Smooth, "Smooth", Side::Auto, Side::Auto, "Smooth tangents (no selection: the whole camera path)" },
	{ EaseKind::Linear, "Linear", Side::Linear, Side::Linear, "Straight lines between keys (no selection: the whole camera path)" },
	{ EaseKind::In, "Ease In", Side::Ease, Side::Linear, "Ease into the selected keys (Shift+F9)" },
	{ EaseKind::Out, "Ease Out", Side::Linear, Side::Ease, "Ease out of the selected keys (Ctrl+Shift+F9)" },
	{ EaseKind::InOut, "In/Out", Side::Ease, Side::Ease, "Easy Ease the selected keys (F9)" },
	{ EaseKind::Hold, "Hold", Side::Linear, Side::Hold, "Hold the value until the next key, then jump (scene keys)" },
};

// applyEase: with keys selected it shapes those keys; with none, Smooth and Linear set the camera
// path's own interpolation and the eases do nothing.
void ApplyEase(D& d, EaseKind kind) {
	const bool sel = d.keys.SelectionCount() > 0;
	switch (kind) {
	case EaseKind::Smooth:
	case EaseKind::Linear:
		d.Emit(Action::Kind::Interp, 0, sel ? 1 : 0, 0, 0, kind == EaseKind::Smooth);
		break;
	case EaseKind::In:
	case EaseKind::Out:
	case EaseKind::InOut:
		if (sel) d.Emit(Action::Kind::Ease, 0, kind == EaseKind::In ? 0 : (kind == EaseKind::Out ? 1 : 2), 0, 0, true);
		break;
	default:
		break;
	}
}

// The lead key's kind, or the path's interpolation with nothing selected.
EaseKind CurrentEase(const D& d) {
	int ch = 0, id = 0;
	if (d.keys.FirstSelected(ch, id)) {
		if (const GraphExpModel::Keyframe* k = d.keys.FindKeyConst(ch, id)) return EaseOfSides(LeftSide(*k), RightSide(*k));
	}
	return d.m.smoothPath ? EaseKind::Smooth : EaseKind::Linear;
}

std::vector<double> AllTimes(const D& d) { return TimesOf(MergedOf(d.keys, kLayerNode)); }

void JumpKey(D& d, int dir) {
	const std::vector<double> times = d.st.selRow >= 0 ? TimesOf(MergedOf(d.keys, d.st.selRow)) : AllTimes(d);
	double t = 0;
	if (NeighbourTime(times, d.m.playhead, dir, t)) SeekTo(d, t);
}

std::string SelectionInfo(const D& d) {
	const int n = d.keys.SelectionCount();
	char buf[256];
	int ch = 0, id = 0;
	if (n == 1 && d.keys.FirstSelected(ch, id)) {
		const GraphExpModel::Keyframe* k = d.keys.FindKeyConst(ch, id);
		const int node = NodeOfChan(d.st, ch);
		const std::vector<MergedKey> list = MergedOf(d.keys, node);
		int idx = 0;
		for (size_t i = 0; i < list.size(); ++i)
			if (k && std::fabs(list[i].t - k->tick) < 0.26) idx = (int)i + 1;
		char time[24];
		TimeText(d.m, k ? k->tick : 0, time, sizeof(time));
		std::snprintf(buf, sizeof(buf), "%s  \xE2\x80\xBA  %s  \xC2\xB7  key %d of %d  \xC2\xB7  %s", kNodes[kLayerNode].name,
			kNodes[node].name, idx, (int)list.size(), time);
		return buf;
	}
	if (n > 1) {
		std::snprintf(buf, sizeof(buf), "%d keys selected  \xC2\xB7  drag to move  \xC2\xB7  F9 eases  \xC2\xB7  Del deletes", n);
		return buf;
	}
	if (d.st.selRow >= 0) {
		const Node& node = kNodes[d.st.selRow];
		std::snprintf(buf, sizeof(buf), "%s%s%s  \xC2\xB7  click a key to select it", node.kind == NodeKind::Layer ? "" : kNodes[kLayerNode].name,
			node.kind == NodeKind::Layer ? "" : "  \xE2\x80\xBA  ", node.name);
		return buf;
	}
	return "Click a key to select it  \xC2\xB7  drag to move";
}

// Draws the toolbar; returns SNAP's right edge, which its dropdown lines up with.
float DrawToolbar(D& d) {
	TimelineDockState& st = d.st;
	const TimelineDockModel& m = d.m;
	const float right = d.w - 14;

	// The timecode column, 150 wide.
	{
		char tc[24], tick[64];
		TimeText(m, m.playhead, tc, sizeof(tc));
		std::snprintf(tick, sizeof(tick), "tick %.0f  \xC2\xB7  %.0f tps", std::floor(m.playhead + 0.5), m.tickRate);
		const float top = (kHeaderH - (d.Size(FontRole::Timecode) + 4 + d.Size(FontRole::Hint))) * 0.5f;
		d.Text(FontRole::Timecode, 14, top, Paint::Accent, tc);
		d.Text(FontRole::Hint, 14, top + d.Size(FontRole::Timecode) + 4, Paint::Hint, tick, 0.5f);
	}

	// The right column: transport and speed over the interpolation strip, SNAP and history.
	const bool compact = d.w < kCompactStripW;
	const float btnH = 24;
	float topW = 0, botW = 0;
	const float prevW = 34, playW = 46, nextW = 34, cacheW = ToolButtonW(d, "Cache", Icon::None), speedW = 250;
	topW = prevW + 5 + playW + 5 + nextW + 11 + cacheW + 11 + speedW;
	float itemW[6];
	float stripW = 0;
	for (int i = 0; i < 6; ++i) {
		itemW[i] = compact ? 28 : 9 + 10 + 6 + d.TextW(FontRole::Button, kEaseItems[i].label) + 9;
		stripW += itemW[i];
	}
	char snapLabel[32];
	if (st.snap == kSnapOff) std::snprintf(snapLabel, sizeof(snapLabel), "SNAP");
	else if (st.snap == kSnapAuto) std::snprintf(snapLabel, sizeof(snapLabel), "SNAP \xC2\xB7 Auto");
	else std::snprintf(snapLabel, sizeof(snapLabel), "SNAP \xC2\xB7 %d", st.snap);
	const float snapW = ToolButtonW(d, snapLabel, Icon::None), undoW = ToolButtonW(d, "Undo", Icon::Undo),
		redoW = ToolButtonW(d, "Redo", Icon::Redo), delW = 28;
	botW = 2 + stripW + 9 + snapW + 5 + undoW + 5 + redoW + 5 + delW;
	const float rightColW = (std::max)(topW, botW);
	const float topY = 6, botY = topY + 30 + 5;
	float snapRight = right;

	{
		float x = right - topW;
		const float y = topY + (30 - btnH) * 0.5f;
		if (ToolButton(d, "##tlprev", x, y, prevW, btnH, "", Icon::Prev, Paint::Value, false, "Previous keyframe on the selected layer"))
			JumpKey(d, -1);
		x += prevW + 5;
		const bool playing = (m.drive && m.pathPlaying) || m.playing;
		if (ToolButton(d, "##tlplay", x, y, playW, btnH, "", playing ? Icon::Pause : Icon::Play, Paint::Value, playing, "Play / pause (Space)"))
			d.Emit(Action::Kind::PlayPause);
		x += playW + 5;
		if (ToolButton(d, "##tlnext", x, y, nextW, btnH, "", Icon::Next, Paint::Value, false, "Next keyframe on the selected layer"))
			JumpKey(d, 1);
		x += nextW + 11;
		// CS2's frame cache keeps every frame played for instant scrubbing; CS:S has no cache yet.
		ToolButton(d, "##tlcache", x, y, cacheW, btnH, "Cache", Icon::None, Paint::Value, false, nullptr, false);
		x += cacheW + 11;
		double speed = 0;
		const ImVec2 a = d.P(x, topY), b = d.P(x + speedW, topY + 30);
		if (DrawSpeedDial(d.l, d.fonts, d.s, st.speed, m.timescale, MakeRect(a.x, a.y, b.x, b.y), "##tlspeed", speed))
			d.Emit(Action::Kind::Speed, 0, 0, speed);
	}
	{
		float x = right - botW;
		const float y = botY;
		const float stripH = 24;
		const bool anySel = d.keys.SelectionCount() > 0;
		const EaseKind cur = CurrentEase(d);
		d.Outline(x, y, x + stripW + 2, y + stripH, Paint::Chip, 3);
		float ix = x + 1;
		for (int i = 0; i < 6; ++i) {
			const EaseItem& it = kEaseItems[i];
			const bool enabled = it.kind == EaseKind::Hold ? false
				: (anySel ? true : (it.kind == EaseKind::Smooth || it.kind == EaseKind::Linear));
			const bool on = enabled && it.kind == cur;
			ImGui::PushID(i);
			const Hit h = HitBox("##tlease", d, ix, y + 1, ix + itemW[i], y + stripH - 1);
			d.Tip(it.tip);
			ImGui::PopID();
			d.Fill(ix, y + 1, ix + itemW[i], y + stripH - 1, on ? Paint::EaseOn : (h.hovered && enabled ? Paint::EaseHover : Rgba(0, 0, 0, 0)));
			const float fade = enabled ? 1.0f : 0.45f;
			DrawGlyph(d, ix + 9 + 5, y + stripH * 0.5f, 10, it.left, it.right, Faded(on ? Paint::Accent : Paint::Glyph, fade));
			if (!compact)
				d.Text(FontRole::Button, ix + 9 + 10 + 6, y + (stripH - d.Size(FontRole::Button)) * 0.5f,
					Faded(on ? Paint::Value : Paint::Label, fade), it.label);
			if (h.clicked && enabled) ApplyEase(d, it.kind);
			ix += itemW[i];
		}
		x += stripW + 2 + 9;
		if (ToolButton(d, "##tlsnap", x, y, snapW, btnH, snapLabel, Icon::None, Paint::Value, st.snap != kSnapOff,
				"Time snap: quantise the playhead and key placement to a tick grid"))
			st.snapMenu = !st.snapMenu;
		snapRight = x + snapW;
		x += snapW + 5;
		const Color undoC = d.keys.CanUndo() ? Paint::Value : Paint::Dim, redoC = d.keys.CanRedo() ? Paint::Value : Paint::Dim;
		if (ToolButton(d, "##tlundo", x, y, undoW, btnH, "Undo", Icon::Undo, undoC, false, "Undo (Ctrl+Z)")) d.Emit(Action::Kind::Undo);
		x += undoW + 5;
		if (ToolButton(d, "##tlredo", x, y, redoW, btnH, "Redo", Icon::Redo, redoC, false, "Redo (Ctrl+Shift+Z)")) d.Emit(Action::Kind::Redo);
		x += redoW + 5;
		{
			const Hit h = HitBox("##tldelete", d, x, y, x + delW, y + btnH);
			d.Tip("Delete the selected keyframes (Del)");
			d.Fill(x, y, x + delW, y + btnH, anySel ? (h.held ? Paint::DangerBorder : Paint::DangerBg) : (h.hovered ? Paint::BtnHover : Paint::BtnBg), 3);
			d.Outline(x, y, x + delW, y + btnH, anySel ? Paint::DangerBorder : Paint::Border, 3);
			DrawIcon(d, Icon::Close, x + delW * 0.5f, y + btnH * 0.5f, anySel ? Paint::DangerText : Paint::Dim);
			if (h.clicked && anySel) d.Emit(Action::Kind::DeleteSelected);
		}
	}

	// The middle column: KEYFRAMES and GRAPH over what you have hold of.
	{
		const float x0 = 14 + 150 + 18, x1 = right - rightColW - 18;
		const float tabH = 27, top = (kHeaderH - (tabH + 6 + d.Size(FontRole::Hint))) * 0.5f;
		float x = x0;
		const char* tabs[2] = { "KEYFRAMES", "GRAPH" };
		const char* tips[2] = { "Every key, one row per property", "Curves of the selected layer or property" };
		for (int i = 0; i < 2; ++i) {
			const float tw = d.TextW(FontRole::BodyBold, tabs[i], 1.4f);
			const bool enabled = i == 0; // GRAPH comes with its own part of this step
			Hit h;
			if (enabled) {
				h = HitBox(i == 0 ? "##tlkeys" : "##tlgraph", d, x, top, x + tw, top + tabH);
				d.Tip(tips[i]);
			}
			const bool sel = (int)st.view == i;
			const Color c = !enabled ? Paint::Dim : (sel ? Paint::Accent : (h.hovered ? Paint::Value : Paint::Muted));
			d.Text(FontRole::BodyBold, x, top + 4, c, tabs[i], 1.4f);
			if (sel || h.hovered) d.Fill(x, top + tabH - 2, x + tw, top + tabH, sel ? Paint::Accent : Faded(Paint::Accent, 0.4f));
			if (h.clicked) st.view = (TimelineView)i;
			x += tw + 16;
		}
		const std::string info = Fit(d, FontRole::Hint, SelectionInfo(d), (std::max)(0.0f, x1 - x0), 0.5f);
		d.Text(FontRole::Hint, x0, top + tabH + 6, Paint::Hint, info.c_str(), 0.5f);
	}
	d.HLine(0, d.w, kHeaderH - 1, Paint::Border);
	return snapRight;
}

// The SNAP dropdown: its own window, over the dock, with a catcher behind it that closes it.
void DrawSnapMenu(D& d, float anchorRight) {
	const float anchorBottom = 6 + 30 + 5 + 24;
	TimelineDockState& st = d.st;
	if (!st.snapMenu) return;
	const float mw = 168, headH = 22, itemH = 26;
	const int values[] = { kSnapAuto, 1, 10, 100, 1000, kSnapOff };
	const char* labels[] = { "Auto", "1", "10", "100", "1000", "Off" };
	const int n = 6;
	const float mh = 5 + headH + n * itemH + 5;
	float x = (std::min)(anchorRight, d.w - 2) - mw, y = anchorBottom + 2;
	if (x < 2) x = 2;
	if (y + mh > kDockH - 2) y = kDockH - 2 - mh;

	const ImVec2 dock0 = d.P(0, 0), dock1 = d.P(d.w, kDockH);
	BeginOverlay("##tlsnapcatch", MakeRect(dock0.x, dock0.y, dock1.x, dock1.y), Rgba(0, 0, 0, 0));
	ImGui::SetCursorScreenPos(dock0);
	if (ImGui::InvisibleButton("##catch", ImVec2(dock1.x - dock0.x, dock1.y - dock0.y))) st.snapMenu = false;
	ImGui::End();

	const ImVec2 a = d.P(x, y), b = d.P(x + mw, y + mh);
	BeginOverlay("##tlsnapmenu", MakeRect(a.x, a.y, b.x, b.y), Rgba(0, 0, 0, 0));
	D md = d;
	md.l = ImGui::GetWindowDrawList();
	md.Fill(x, y, x + mw, y + mh, Paint::MenuBg, 5);
	md.Outline(x, y, x + mw, y + mh, Paint::Border, 5);
	md.Text(FontRole::Caption, x + 5 + 11, y + 5 + (headH - md.Size(FontRole::Caption)) * 0.5f, Paint::Hint, "TIME SNAP (TICKS)", 1.4f);
	for (int i = 0; i < n; ++i) {
		const float iy = y + 5 + headH + i * itemH;
		ImGui::PushID(i);
		const Hit h = HitBox("##snapitem", md, x + 5, iy, x + mw - 5, iy + itemH);
		ImGui::PopID();
		const bool current = st.snap == values[i];
		md.Fill(x + 5, iy, x + mw - 5, iy + itemH, current ? Paint::BtnOn : (h.hovered ? Paint::BtnHover : Rgba(0, 0, 0, 0)), 3);
		md.Text(FontRole::Label, x + 5 + 11, iy + (itemH - md.Size(FontRole::Label)) * 0.5f, current ? Paint::Accent : Paint::Value, labels[i]);
		if (h.clicked) {
			st.snap = values[i];
			st.snapMenu = false;
		}
	}
	ImGui::End();
}

// ---- column header, overview and ruler (y 72..112) ---------------------------------------------

void DrawHeaderBand(D& d) {
	const TimelineDockState& st = d.st;
	// The column header over the layer list.
	{
		const float y0 = kHeaderH, y1 = kRowsY;
		d.VLine(kLeftW - 1, y0, y1, Paint::Border);
		d.HLine(0, kLeftW, y1 - 1, Paint::Border);
		const float y = y0 + (y1 - y0 - d.Size(FontRole::Caption)) * 0.5f;
		const float navX0 = kLeftW - 8 - kNavW, valX1 = navX0 - 6;
		d.Text(FontRole::Caption, 10, y, Paint::Hint, "LAYERS", 2);
		d.Text(FontRole::Caption, valX1 - d.TextW(FontRole::Caption, "VALUE", 2), y, Paint::Hint, "VALUE", 2);
		d.Text(FontRole::Caption, navX0 + (kNavW - d.TextW(FontRole::Caption, "KEY", 2)) * 0.5f, y, Paint::Hint, "KEY", 2);
	}
	const float ox = kLeftW;
	// The overview: the whole demo, its keys, the playhead and the window box.
	{
		const float y0 = kHeaderH;
		d.Fill(ox, y0, ox + d.g.ow, y0 + kOverviewH, Paint::OvBg);
		d.l->PushClipRect(d.P(ox, y0), d.P(ox + d.g.ow, y0 + kOverviewH), true);
		d.Fill(ox, y0 + 3, ox + d.g.ow, y0 + 11, Paint::OvTrack);
		for (const MergedKey& k : MergedOf(d.keys, kLayerNode)) {
			const float x = (float)DomToX(d.domain, d.g.ow, k.t);
			if (x < 0 || x > d.g.ow) continue;
			d.Fill(ox + x, y0 + 5, ox + x + 1, y0 + 9, k.sel ? Rgba(255, 255, 255) : Paint::OvKey);
		}
		const float px = (float)DomToX(d.domain, d.g.ow, d.m.playhead);
		if (px >= 0 && px <= d.g.ow) d.Fill(ox + px, y0, ox + px + 1, y0 + kOverviewH, Paint::Playhead);
		float bx0 = 0, bx1 = 0;
		OverviewBox(st.window, d.domain, d.g.ow, d.g.tw, bx0, bx1);
		const float tx = d.mouse.x - ox;
		const bool over = d.mouse.y >= y0 && d.mouse.y < y0 + kOverviewH && tx >= 0 && tx <= d.g.ow && ImGui::IsWindowHovered();
		const bool dragging = st.gesture == Gesture::Overview;
		const bool hot = (over && tx >= bx0 && tx <= bx1) || (dragging && st.overviewPart == 3);
		const bool hotL = (over && std::fabs(tx - bx0) <= 6) || (dragging && st.overviewPart == 1);
		const bool hotR = (over && std::fabs(tx - bx1) <= 6) || (dragging && st.overviewPart == 2);
		d.Fill(ox + bx0, y0 + 1, ox + (std::max)(bx0 + 3, bx1), y0 + 13, hot ? Paint::OvViewHot : Paint::OvView, 2);
		d.Outline(ox + bx0, y0 + 1, ox + (std::max)(bx0 + 3, bx1), y0 + 13, hot ? Paint::OvViewBorderHot : Paint::OvViewBorder, 2);
		const float gl = (std::max)(0.0f, (std::min)(d.g.ow - 6, bx0 - 1)), gr = (std::max)(0.0f, (std::min)(d.g.ow - 6, bx1 - 5));
		d.Fill(ox + gl, y0 + 1, ox + gl + 6, y0 + 13, hotL ? Paint::Accent : Paint::Grip, 2);
		d.Fill(ox + gr, y0 + 1, ox + gr + 6, y0 + 13, hotR ? Paint::Accent : Paint::Grip, 2);
		d.l->PopClipRect();
	}
	// The ruler: ticks that follow the zoom, labelled about 70 px apart.
	{
		const float y0 = kHeaderH + kOverviewH;
		d.Fill(ox, y0, ox + d.g.ow, y0 + kRulerH, Paint::RulerBg);
		d.l->PushClipRect(d.P(ox, y0), d.P(ox + d.g.ow, y0 + kRulerH), true);
		const RulerStep r = RulerSteps(st.window, d.g.cw, d.m.tickRate);
		for (double t = std::ceil(st.window.t0 / r.minor) * r.minor; t <= st.window.t1; t += r.minor) {
			const float x = (float)TickToX(st.window, d.g.cw, t);
			if (x < 0 || x > d.g.tw) continue;
			const bool major = std::fabs(t / r.major - std::round(t / r.major)) < 1e-6;
			d.Fill(ox + x, y0 + kRulerH - (major ? 10 : 5), ox + x + 1, y0 + kRulerH, major ? Paint::TickMajor : Paint::TickMinor);
			if (major) {
				char text[24];
				TimeText(d.m, t, text, sizeof(text), true);
				d.Text(FontRole::Hint, ox + x + 4, y0 + 9 - 2, Paint::Hint, text);
			}
		}
		d.l->PopClipRect();
		d.HLine(ox, ox + d.g.ow, y0 + kRulerH - 1, Paint::Border);
	}
}

// ---- rows (y 112..414) ------------------------------------------------------------------------

void PickRow(D& d, int node) {
	d.st.selRow = node;
	if (kNodes[node].kind != NodeKind::Layer) return;
	// A layer's name picks the whole layer: every key in it lights up, so Delete removes them.
	if (d.keys.TotalKeys() > 0) d.Emit(Action::Kind::SelectAll);
	else d.Emit(Action::Kind::SelectClear);
}

// ◇: the row's keys on the playhead's whole tick go; with none there, every channel of the row is
// keyed at the snapped playhead with the value you are looking at.
void ToggleKeyAt(D& d, int node) {
	const double tick = std::floor(d.m.playhead + 0.5);
	std::vector<std::pair<int, int>> hits;
	for (int c : ChansOf(node))
		for (const GraphExpModel::Keyframe& k : d.keys.GetChannel(c).keys)
			if (std::floor(k.tick + 0.5) == tick) hits.emplace_back(c, k.id);
	if (!hits.empty()) {
		d.EmitSelectSet(hits);
		d.Emit(Action::Kind::DeleteSelected);
		return;
	}
	const double at = SnapTick(d.m.playhead, SnapStep(d.st.snap, d.st.window), ImGui::GetIO().KeyAlt);
	d.Emit(Action::Kind::EditBegin);
	for (int c : ChansOf(node)) d.Emit(Action::Kind::AddKey, c, 0, at, d.m.live[c]);
}

void NavButton(D& d, const char* id, float x, float y, int kind, Color c, const char* tip, bool& clicked) {
	const Hit h = HitBox(id, d, x, y, x + 17, y + 20);
	d.Tip(tip);
	if (h.hovered) d.Fill(x, y, x + 17, y + 20, Paint::BtnHover, 3);
	const float cx = x + 8.5f, cy = y + 10;
	const ImU32 col = ToU32(c);
	if (kind < 0) d.l->AddTriangleFilled(d.P(cx + 3, cy - 4), d.P(cx + 3, cy + 4), d.P(cx - 3.5f, cy), col);
	else if (kind > 0) d.l->AddTriangleFilled(d.P(cx - 3, cy - 4), d.P(cx - 3, cy + 4), d.P(cx + 3.5f, cy), col);
	clicked = h.clicked;
}

void DrawLeftCell(D& d, const Row& r, int index, float y) {
	TimelineDockState& st = d.st;
	const Node& n = kNodes[r.node];
	const float pad = RowPad(r), x1 = kLeftW - 8;
	const float midY = y + kRowH * 0.5f;
	ImGui::PushID(index);
	float x = pad;
	// The chevron.
	if (Expandable(st, r.node)) {
		const Hit h = HitBox("##chev", d, x, y, x + 14, y + kRowH);
		const ImU32 col = ToU32(h.hovered ? Paint::Value : Paint::Hint);
		const float cx = x + 7;
		if (st.open[r.node]) d.l->AddTriangleFilled(d.P(cx - 4, midY - 2), d.P(cx + 4, midY - 2), d.P(cx, midY + 3), col);
		else d.l->AddTriangleFilled(d.P(cx - 2, midY - 4), d.P(cx - 2, midY + 4), d.P(cx + 3, midY), col);
		if (h.clicked) st.open[r.node] = !st.open[r.node];
	}
	x += 14 + 6;
	if (n.kind == NodeKind::Layer) {
		d.Fill(x, midY - 5, x + 10, midY + 5, Paint::Accent, 2);
		x += 10 + 6;
	}
	float nameX1 = x1;
	if (n.kind == NodeKind::Layer) {
		const bool on = d.m.drive;
		// The switch: on, scrubbing and key edits preview the path; off, the free cam stays yours.
		const float swX = x1 - 4 - 34;
		{
			const Hit h = HitBox("##switch", d, swX, midY - 9, swX + 34, midY + 9);
			d.Tip("Camera path on: scrubbing and key edits preview the path. Off: the free cam stays yours.");
			d.Fill(swX, midY - 9, swX + 34, midY + 9, on ? Paint::BtnOn : Paint::BtnBg, 2);
			d.Outline(swX, midY - 9, swX + 34, midY + 9, on ? Paint::Accent : (h.hovered ? Paint::Value : Paint::Chip), 2);
			const float kx = swX + (on ? 18 : 2);
			d.Fill(kx, midY - 7, kx + 12, midY + 5, on ? Paint::Accent : Paint::Dim, 1);
			if (h.clicked) d.Emit(Action::Kind::Drive, 0, 0, 0, 0, !on);
		}
		// AUTO: while the timeline plays, the view follows the camera path inside its keys.
		const float apW = 5 + d.TextW(FontRole::Caption, "AUTO", 0.5f) + 5, apX = swX - 6 - apW;
		{
			const Hit h = HitBox("##auto", d, apX, midY - 9, apX + apW, midY + 9);
			d.Tip("AUTO on: while the timeline plays, the view follows the camera path whenever the playhead is inside its keys. "
				"Off: Space only plays the demo. J plays the path either way.");
			const bool ap = d.m.autoPath;
			d.Fill(apX, midY - 9, apX + apW, midY + 9, ap ? Paint::BtnOn : (h.hovered ? Paint::BtnHover : Paint::BtnBg), 2);
			d.Outline(apX, midY - 9, apX + apW, midY + 9, ap ? Paint::Accent : Paint::Chip, 2);
			d.TextIn(FontRole::Caption, apX, midY - 9, apX + apW, midY + 9, ap ? Paint::Accent : Paint::Muted, "AUTO", 0.5f);
			if (h.clicked) d.Emit(Action::Kind::AutoPath, 0, 0, 0, 0, !ap);
		}
		// The key count.
		const int nk = (int)MergedOf(d.keys, r.node).size();
		char meta[24];
		std::snprintf(meta, sizeof(meta), "%d %s", nk, nk == 1 ? "key" : "keys");
		const float metaX1 = apX - 6;
		d.Text(FontRole::Hint, metaX1 - d.TextW(FontRole::Hint, meta), midY - d.Size(FontRole::Hint) * 0.5f, Paint::Muted, meta);
		nameX1 = metaX1 - 60 - 6;
	}
	if (n.kind == NodeKind::Prop) {
		// The key navigator: previous, add or remove at the playhead, next.
		const float navX0 = x1 - kNavW;
		if (!IsSplit(st, r.node)) {
			const double tick = std::floor(d.m.playhead + 0.5);
			bool any = false, at = false;
			for (int c : ChansOf(r.node))
				for (const GraphExpModel::Keyframe& k : d.keys.GetChannel(c).keys) {
					any = true;
					if (std::floor(k.tick + 0.5) == tick) at = true;
				}
			const float bx = navX0 + (kNavW - 51) * 0.5f, by = y + 1;
			bool prev = false, dia = false, next = false;
			NavButton(d, "##prev", bx, by, -1, any ? Paint::Hint : Paint::Dim, "Previous keyframe", prev);
			{
				const Hit h = HitBox("##dia", d, bx + 17, by, bx + 34, by + 20);
				d.Tip("Add or remove a keyframe at the playhead");
				if (h.hovered) d.Fill(bx + 17, by, bx + 34, by + 20, Paint::BtnHover, 3);
				Diamond(d, bx + 25.5f, by + 10, 6.5f, at ? Paint::Accent : Paint::Hint, at);
				dia = h.clicked;
			}
			NavButton(d, "##next", bx + 34, by, 1, any ? Paint::Hint : Paint::Dim, "Next keyframe", next);
			if (prev || next) {
				st.selRow = r.node;
				double t = 0;
				if (NeighbourTime(TimesOf(MergedOf(d.keys, r.node)), d.m.playhead, prev ? -1 : 1, t)) SeekTo(d, t);
			}
			if (dia) {
				st.selRow = r.node;
				ToggleKeyAt(d, r.node);
			}
		}
		// The values, right-aligned in front of the navigator, in each channel's colour.
		const FontRole vr = n.count > 2 ? FontRole::Caption : FontRole::Button;
		float vx = navX0 - 6;
		if (!IsSplit(st, r.node)) {
			for (int i = n.count - 1; i >= 0; --i) {
				const int c = n.chans[i];
				char text[32];
				std::snprintf(text, sizeof(text), "%.*f%s", n.dec, CleanValue(ChannelValue(d.m, c), n.dec), n.unit);
				const float tw = d.TextW(vr, text);
				vx -= tw;
				const float ty = midY - d.Size(vr) * 0.5f;
				d.Text(vr, vx, ty, kChannel[c], text);
				d.Dotted(vx, vx + tw, ty + d.Size(vr) + 1, Paint::Underline); // CS2's dotted underline
				vx -= 5;
			}
			vx += 5;
		}
		nameX1 = vx - 6;
	}
	// The name: a click picks the row (a layer's, every key in it).
	{
		const Hit h = HitBox("##name", d, x, y, (std::max)(x + 1, nameX1), y + kRowH);
		if (std::strlen(n.name) > 18) d.Tip(n.name);
		const bool bold = n.kind != NodeKind::Prop;
		const FontRole role = bold ? FontRole::Button : FontRole::Note;
		const float spacing = n.kind == NodeKind::Layer ? 1.0f : (n.kind == NodeKind::Group ? 0.5f : 0.0f);
		Color c = n.kind == NodeKind::Group ? Paint::Label : Paint::Value;
		if (n.kind == NodeKind::Layer && !d.m.drive) c = Paint::Muted;
		const std::string name = Fit(d, role, n.name, (std::max)(0.0f, nameX1 - x), spacing);
		d.Text(role, x, midY - d.Size(role) * 0.5f, c, name.c_str(), spacing);
		if (h.clicked) PickRow(d, r.node);
	}
	ImGui::PopID();
}

void DrawTrackCell(D& d, const Row& r, float y) {
	const TimelineDockState& st = d.st;
	const Node& n = kNodes[r.node];
	const float ox = kLeftW, midY = y + kRowH * 0.5f;
	auto summary = [&](const std::vector<MergedKey>& ks) {
		for (const MergedKey& k : ks) {
			const float x = (float)TickToX(st.window, d.g.cw, k.t);
			if (x < -8 || x > d.g.tw + 8) continue;
			Diamond(d, ox + x, midY, 7, k.sel ? Paint::Accent : Paint::Summary);
		}
	};
	if (n.kind == NodeKind::Layer) {
		const std::vector<MergedKey> ks = MergedOf(d.keys, r.node);
		if (!ks.empty()) {
			const bool on = d.m.drive;
			const float x0 = (float)TickToX(st.window, d.g.cw, ks.front().t), x1 = (std::max)(x0 + 4, (float)TickToX(st.window, d.g.cw, ks.back().t));
			const float bx0 = (std::max)(-6.0f, x0), bx1 = (std::min)(d.g.tw + 6, x1);
			if (bx1 > bx0) {
				d.Fill(ox + bx0, y + 4, ox + bx1, y + 17, Faded(Paint::Accent, on ? 0.25f : 0.094f), 3);
				d.Outline(ox + bx0, y + 4, ox + bx1, y + 17, Faded(Paint::Accent, on ? 0.667f : 0.267f), 3);
			}
			if (!st.open[r.node]) summary(ks);
		}
		return;
	}
	if (n.kind == NodeKind::Group || IsSplit(st, r.node)) {
		if (!st.open[r.node]) summary(MergedOf(d.keys, r.node));
		return;
	}
	for (const MergedKey& k : MergedOf(d.keys, r.node)) {
		const float x = (float)TickToX(st.window, d.g.cw, k.t);
		if (x < -kGlyph || x > d.g.tw + kGlyph) continue;
		const KeyRef& lead = k.members.front();
		const GraphExpModel::Keyframe* kf = d.keys.FindKeyConst(lead.ch, lead.id);
		DrawGlyph(d, ox + x, midY, kGlyph, kf ? LeftSide(*kf) : Side::Linear, kf ? RightSide(*kf) : Side::Linear,
			k.sel ? Paint::Accent : Paint::Glyph);
	}
}

// The key under the pointer on a property row, within reach: the nearest drawn one.
const MergedKey* KeyNear(const std::vector<MergedKey>& keys, const D& d, float tx) {
	const MergedKey* best = nullptr;
	float bestD = kKeyHit;
	for (const MergedKey& k : keys) {
		const float dx = std::fabs((float)TickToX(d.st.window, d.g.cw, k.t) - tx);
		if (dx <= bestD) { bestD = dx; best = &k; }
	}
	return best;
}

bool AllSelected(const MergedKey& k) {
	for (const KeyRef& r : k.members) if (!r.sel) return false;
	return true;
}

void SelectMembers(D& d, const std::vector<std::pair<int, int>>& members, bool add) {
	if (add) {
		for (const auto& m : members) d.Emit(Action::Kind::Select, m.first, m.second, 0, 0, true);
	} else {
		d.EmitSelectSet(members);
	}
}

bool DoubleClick(TimelineDockState& st, double t) {
	const double now = ImGui::GetTime();
	const bool dbl = st.lastKeyPressTime >= 0 && now - st.lastKeyPressTime <= kDoubleClickSeconds && std::fabs(st.lastKeyPressTick - t) <= 0.5;
	st.lastKeyPressTime = dbl ? -1 : now;
	st.lastKeyPressTick = t;
	return dbl;
}

void StartKeyDrag(D& d, double lead, float tx, float gy) {
	TimelineDockState& st = d.st;
	st.gesture = Gesture::Keys;
	st.pressX = tx;
	st.pressY = gy;
	st.leadTick = lead;
	st.lastDt = 0;
	st.dragStarted = false;
}

// A press on the KEYFRAMES track (pressTrackKeys): a key, a layer's bar, the playhead, else a box.
void PressTrack(D& d, const std::vector<Row>& rows, float tx, float gy) {
	TimelineDockState& st = d.st;
	const bool shift = ImGui::GetIO().KeyShift;
	const int ri = (int)std::floor((gy + st.rowScroll) / kRowH);
	const Row* row = ri >= 0 && ri < (int)rows.size() ? &rows[ri] : nullptr;
	st.narrow = false;
	if (row && kNodes[row->node].kind == NodeKind::Prop && !IsSplit(st, row->node)) {
		const std::vector<MergedKey> keys = MergedOf(d.keys, row->node);
		if (const MergedKey* k = KeyNear(keys, d, tx)) {
			st.selRow = row->node;
			if (DoubleClick(st, k->t)) {
				SeekTo(d, k->t);
				st.gesture = Gesture::None;
				return;
			}
			const bool wasSelected = AllSelected(*k);
			if (!wasSelected) SelectMembers(d, MembersOf(*k), shift);
			if (wasSelected && !shift) {
				st.narrow = true;
				st.narrowTo = MembersOf(*k);
			}
			StartKeyDrag(d, k->t, tx, gy);
			return;
		}
	}
	if (row && kNodes[row->node].kind == NodeKind::Layer) {
		const std::vector<MergedKey> keys = MergedOf(d.keys, row->node);
		if (!keys.empty()) {
			const float x0 = (float)TickToX(st.window, d.g.cw, keys.front().t);
			const float x1 = (std::max)(x0 + 4, (float)TickToX(st.window, d.g.cw, keys.back().t));
			if (tx >= x0 - 3 && tx <= x1 + 3) {
				if (const MergedKey* k = KeyNear(keys, d, tx)) {
					if (DoubleClick(st, k->t)) {
						SeekTo(d, k->t);
						st.gesture = Gesture::None;
						return;
					}
				}
				PickRow(d, row->node);
				StartKeyDrag(d, keys.front().t, tx, gy);
				return;
			}
		}
	}
	const float px = (float)TickToX(st.window, d.g.cw, d.m.playhead);
	if (std::fabs(tx - px) <= 4) {
		st.gesture = Gesture::Scrub;
		return;
	}
	if (row) st.selRow = row->node;
	st.gesture = Gesture::Box;
	st.pressX = tx;
	st.pressY = gy;
}

void DragKeys(D& d) {
	TimelineDockState& st = d.st;
	const float tx = d.mouse.x - kLeftW, gy = d.mouse.y - kRowsY;
	const float dx = tx - st.pressX, dy = gy - st.pressY;
	if (!st.dragStarted) {
		if (std::sqrt(dx * dx + dy * dy) < kDragPx) return;
		st.dragStarted = true;
		d.Emit(Action::Kind::EditBegin);
	}
	double dt = dx / d.g.cw * st.window.Len();
	const double step = SnapStep(st.snap, st.window);
	if (step > 0) dt = SnapTick(st.leadTick + dt, step, ImGui::GetIO().KeyAlt) - st.leadTick;
	if (dt != st.lastDt) {
		d.Emit(Action::Kind::MoveSelected, 0, 0, dt - st.lastDt, 0);
		st.lastDt = dt;
	}
}

void ReleaseBox(D& d, const std::vector<Row>& rows) {
	TimelineDockState& st = d.st;
	const bool shift = ImGui::GetIO().KeyShift;
	const float tx = d.mouse.x - kLeftW, gy = d.mouse.y - kRowsY;
	if (std::fabs(tx - st.pressX) < kDragPx && std::fabs(gy - st.pressY) < kDragPx) {
		if (!shift) d.Emit(Action::Kind::SelectClear);
		return;
	}
	const float x0 = (std::min)(st.pressX, tx), x1 = (std::max)(st.pressX, tx);
	const float y0 = (std::min)(st.pressY, gy), y1 = (std::max)(st.pressY, gy);
	std::vector<std::pair<int, int>> picked;
	for (size_t i = 0; i < rows.size(); ++i) {
		const Row& r = rows[i];
		if (kNodes[r.node].kind != NodeKind::Prop || IsSplit(st, r.node)) continue;
		const float ry0 = i * kRowH - st.rowScroll, ry1 = ry0 + kRowH;
		if (ry1 < y0 || ry0 > y1) continue;
		for (const MergedKey& k : MergedOf(d.keys, r.node)) {
			const float x = (float)TickToX(st.window, d.g.cw, k.t);
			if (x >= x0 - 4 && x <= x1 + 4)
				for (const auto& m : MembersOf(k)) picked.push_back(m);
		}
	}
	if (!picked.empty()) SelectMembers(d, picked, shift);
	else if (!shift) d.Emit(Action::Kind::SelectClear);
}

void ScrubTo(D& d, float tx) {
	TimelineDockState& st = d.st;
	const double step = SnapStep(st.snap, st.window);
	const bool alt = ImGui::GetIO().KeyAlt;
	const float lo = kPad, hi = kPad + d.g.cw;
	if (tx > hi || tx < lo) {
		// Past the track's time edges the window creeps and the playhead stays on its edge.
		const float over = tx > hi ? tx - hi : tx - lo;
		const double span = st.window.Len();
		st.window = Pan(st.window, d.domain, (over > 0 ? 1 : -1) * span * (std::min)(0.012, std::fabs(over) * 0.00012) * 0.6);
		d.Emit(Action::Kind::Playhead, 0, 0, SnapTick(over > 0 ? st.window.t1 : st.window.t0, step, alt));
		return;
	}
	d.Emit(Action::Kind::Playhead, 0, 0, SnapTick(XToTick(st.window, d.g.cw, tx), step, alt));
}

void ReleaseScrub(D& d, float tx) {
	const TimelineDockState& st = d.st;
	const double t = SnapTick(XToTick(st.window, d.g.cw, (std::max)(kPad, (std::min)(kPad + d.g.cw, tx))),
		SnapStep(st.snap, st.window), ImGui::GetIO().KeyAlt);
	const double at = (std::max)(0.0, std::round(t));
	d.Emit(Action::Kind::Playhead, 0, 0, at);
	d.Emit(Action::Kind::PlayheadRelease, 0, 0, at);
}

void DrawRows(D& d, const std::vector<Row>& rows) {
	TimelineDockState& st = d.st;
	const float contentH = rows.size() * kRowH;
	st.rowScroll = ClampRowScroll(st.rowScroll, contentH);
	d.l->PushClipRect(d.P(0, kRowsY), d.P(d.w, kRowsY + kRowsH), true);
	for (size_t i = 0; i < rows.size(); ++i) {
		const float y = kRowsY + i * kRowH - st.rowScroll;
		if (y + kRowH < kRowsY || y > kRowsY + kRowsH) continue;
		const Row& r = rows[i];
		const bool sel = st.selRow == r.node;
		const bool layer = kNodes[r.node].kind == NodeKind::Layer;
		d.Fill(0, y, d.w, y + kRowH, layer ? (sel ? Paint::LayerSel : Paint::LayerBg) : (sel ? Paint::RowSel : Rgba(0, 0, 0, 0)));
		d.HLine(0, d.w, y + kRowH - 1, Paint::RowLine);
		d.VLine(kLeftW - 1, y, y + kRowH, Paint::Border);
	}
	// The track side, clipped to the track.
	d.l->PushClipRect(d.P(kLeftW, kRowsY), d.P(kLeftW + d.g.tw, kRowsY + kRowsH), true);
	for (size_t i = 0; i < rows.size(); ++i) {
		const float y = kRowsY + i * kRowH - st.rowScroll;
		if (y + kRowH < kRowsY || y > kRowsY + kRowsH) continue;
		DrawTrackCell(d, rows[i], y);
	}
	d.l->PopClipRect();
	for (size_t i = 0; i < rows.size(); ++i) {
		const float y = kRowsY + i * kRowH - st.rowScroll;
		if (y + kRowH < kRowsY || y > kRowsY + kRowsH) continue;
		DrawLeftCell(d, rows[i], (int)i, y);
	}
	d.l->PopClipRect();

	// The rows' scrollbar, only when they overflow.
	if (contentH > kRowsH) {
		float ty = 0, th = 0;
		RowsThumb(st.rowScroll, contentH, ty, th);
		d.Fill(d.w - kScrollW + 3, kRowsY + ty, d.w - kScrollW + 9, kRowsY + ty + th, Paint::ScrollThumb, 3);
	}
}

// ---- the footer (y 414..440) ----------------------------------------------------------------------

void FooterButton(D& d, const char* id, float x, float w, const char* label, const char* tip, bool& clicked) {
	const float y0 = kDockH - kFooterH + (kFooterH - 18) * 0.5f;
	const Hit h = HitBox(id, d, x, y0, x + w, y0 + 18);
	d.Tip(tip);
	d.Fill(x, y0, x + w, y0 + 18, h.held ? Paint::BtnDown : (h.hovered ? Paint::BtnHover : Paint::BtnBg));
	const bool word = std::strlen(label) > 1 && (unsigned char)label[0] < 0x80;
	d.TextIn(word ? FontRole::Caption : FontRole::Label, x, y0, x + w, y0 + 18, Paint::Value, label, word ? 1.0f : 0.0f);
	clicked = h.clicked;
}

void DrawFooter(D& d) {
	TimelineDockState& st = d.st;
	const float y0 = kDockH - kFooterH;
	d.HLine(0, d.w, y0, Paint::Border);
	d.VLine(kLeftW - 1, y0, kDockH, Paint::Border);
	const float textY = y0 + (kFooterH - d.Size(FontRole::Hint)) * 0.5f;
	d.Text(FontRole::Hint, kLeftW + 12, textY, Paint::Hint, "RENDER RANGE  whole demo");

	float x = d.w - 14;
	bool zoomIn = false, zoomOut = false, fit = false;
	x -= 24;
	FooterButton(d, "##tlzoomin", x, 24, "+", "Zoom in (Ctrl+Wheel)", zoomIn);
	x -= 6 + kFooterTrackW;
	const float trackX = x, trackY = y0 + (kFooterH - 12) * 0.5f;
	{
		float thumbX = 0, thumbW = 0;
		FooterThumb(st.window, d.domain, kFooterTrackW, thumbX, thumbW);
		d.Fill(trackX, trackY, trackX + kFooterTrackW, trackY + 12, Paint::ScrollTrack, 7);
		d.Fill(trackX + thumbX, trackY, trackX + thumbX + thumbW, trackY + 12, Paint::ScrollThumb, 7);
		const Hit h = HitBox("##tlfootbar", d, trackX, trackY, trackX + kFooterTrackW, trackY + 12);
		if (ImGui::IsItemActivated()) {
			const float fx = d.mouse.x - trackX;
			if (fx < thumbX || fx > thumbX + thumbW) {
				// A press beside the thumb centres the window there.
				const double c = d.domain.t0 + fx / kFooterTrackW * (std::max)(1.0, d.domain.Len());
				const double span = st.window.Len();
				st.window = Normalize(MakeSpan(c - span / 2, c + span / 2), d.domain);
				ManualView(d);
			}
			st.gesture = Gesture::FooterBar;
			st.pressX = d.mouse.x;
			st.grabT0 = st.window.t0;
			st.grabT1 = st.window.t1;
		}
		(void)h;
	}
	x -= 6 + 24;
	FooterButton(d, "##tlzoomout", x, 24, "\xE2\x80\x93", "Zoom out (Ctrl+Wheel)", zoomOut);
	x -= 6 + 34;
	FooterButton(d, "##tlfit", x, 34, "FIT", "Fit the selected keys, else the selected layer or row; press again for every key", fit);
	{
		char a[24], b[24], e[24], text[96];
		TimeText(d.m, st.window.t0, a, sizeof(a), true);
		TimeText(d.m, st.window.t1, b, sizeof(b), true);
		TimeText(d.m, d.domain.t1, e, sizeof(e), true);
		std::snprintf(text, sizeof(text), "Showing %s \xE2\x80\x93 %s of %s", a, b, e);
		d.Text(FontRole::Hint, x - 6 - d.TextW(FontRole::Hint, text), textY, Paint::Hint, text);
	}
	if (zoomIn) { st.window = ZoomStep(st.window, d.domain, 1 / 1.5, d.m.playhead); ManualView(d); }
	if (zoomOut) { st.window = ZoomStep(st.window, d.domain, 1.5, d.m.playhead); ManualView(d); }
	if (fit) FitAll(d);
}

// ---- input that is not a button ----------------------------------------------------------------

void HandleRegions(D& d, const std::vector<Row>& rows) {
	TimelineDockState& st = d.st;
	const float tx = d.mouse.x - kLeftW;
	// The overview.
	HitBox("##tloverview", d, kLeftW, kHeaderH, kLeftW + d.g.ow, kHeaderH + kOverviewH);
	if (ImGui::IsItemActivated()) {
		float bx0 = 0, bx1 = 0;
		OverviewBox(st.window, d.domain, d.g.ow, d.g.tw, bx0, bx1);
		if (std::fabs(tx - bx0) <= 6) st.overviewPart = 1;
		else if (std::fabs(tx - bx1) <= 6) st.overviewPart = 2;
		else {
			st.overviewPart = 3;
			if (tx < bx0 || tx > bx1) {
				const double c = XToDom(d.domain, d.g.ow, tx), span = st.window.Len();
				st.window = Normalize(MakeSpan(c - span / 2, c + span / 2), d.domain);
				ManualView(d);
			}
		}
		st.gesture = Gesture::Overview;
		st.pressX = tx;
		st.grabT0 = st.window.t0;
		st.grabT1 = st.window.t1;
	}
	// The ruler: a press scrubs from where it lands.
	HitBox("##tlruler", d, kLeftW, kHeaderH + kOverviewH, kLeftW + d.g.tw, kRowsY);
	if (ImGui::IsItemActivated()) {
		st.gesture = Gesture::Scrub;
		ScrubTo(d, (std::max)(kPad, (std::min)(kPad + d.g.cw, tx)));
	}
	// The rows' scrollbar column.
	const float contentH = rows.size() * kRowH;
	if (contentH > kRowsH) {
		HitBox("##tlrowsbar", d, kLeftW + d.g.tw, kRowsY, d.w, kRowsY + kRowsH);
		if (ImGui::IsItemActivated()) {
			st.gesture = Gesture::RowsBar;
			st.pressY = d.mouse.y;
			st.grabScroll = st.rowScroll;
		}
	}
	// The track: keys, a layer's bar, the playhead, else a box selection.
	HitBox("##tltrack", d, kLeftW, kRowsY, kLeftW + d.g.tw, kRowsY + kRowsH);
	if (ImGui::IsItemActivated()) PressTrack(d, rows, tx, d.mouse.y - kRowsY);

	// The gesture in progress.
	const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
	switch (st.gesture) {
	case Gesture::Scrub:
		if (down) {
			ScrubTo(d, tx);
		} else {
			ReleaseScrub(d, tx);
			st.gesture = Gesture::None;
		}
		break;
	case Gesture::Keys:
		if (down) DragKeys(d);
		else {
			if (!st.dragStarted && st.narrow) d.EmitSelectSet(st.narrowTo);
			st.gesture = Gesture::None;
		}
		break;
	case Gesture::Box:
		if (!down) {
			ReleaseBox(d, rows);
			st.gesture = Gesture::None;
		}
		break;
	case Gesture::Overview:
		if (down) {
			if (st.overviewPart == 1) st.window = Normalize(MakeSpan(XToDom(d.domain, d.g.ow, tx), st.window.t1), d.domain);
			else if (st.overviewPart == 2) st.window = Normalize(MakeSpan(st.window.t0, XToDom(d.domain, d.g.ow, tx)), d.domain);
			else {
				const double dt = XToDom(d.domain, d.g.ow, tx) - XToDom(d.domain, d.g.ow, st.pressX);
				st.window = Normalize(MakeSpan(st.grabT0 + dt, st.grabT1 + dt), d.domain);
			}
			ManualView(d);
		} else {
			st.gesture = Gesture::None;
		}
		break;
	case Gesture::FooterBar:
		if (down) {
			const double dt = (d.mouse.x - st.pressX) * (std::max)(1.0, d.domain.Len()) / kFooterTrackW;
			st.window = Normalize(MakeSpan(st.grabT0 + dt, st.grabT1 + dt), d.domain);
			ManualView(d);
		} else {
			st.gesture = Gesture::None;
		}
		break;
	case Gesture::RowsBar:
		if (down) st.rowScroll = ClampRowScroll(st.grabScroll + (d.mouse.y - st.pressY) * contentH / kRowsH, contentH);
		else st.gesture = Gesture::None;
		break;
	default:
		break;
	}

	// The wheel over the dock: Ctrl zooms about the pointer, Shift pans, plain scrolls the rows.
	const ImGuiIO& io = ImGui::GetIO();
	const float wheel = io.MouseWheel != 0 ? io.MouseWheel : io.MouseWheelH;
	if (wheel != 0 && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
		if (io.KeyCtrl) {
			const bool overTrack = tx >= 0 && tx <= d.g.tw;
			const double anchor = overTrack ? XToTick(st.window, d.g.cw, tx) : (st.window.t0 + st.window.t1) * 0.5;
			st.window = ZoomAt(st.window, d.domain, std::pow(1.25, -wheel), anchor);
			ManualView(d);
		} else if (io.KeyShift) {
			st.window = Pan(st.window, d.domain, -wheel * 0.1 * st.window.Len());
			ManualView(d);
		} else {
			st.rowScroll = ClampRowScroll(st.rowScroll - wheel * 3 * kRowH, contentH);
		}
	}

	// The keys CS2 forwards to its dock: undo, redo, select all, delete and the eases.
	if (!io.WantTextInput) {
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) d.Emit(io.KeyShift ? Action::Kind::Redo : Action::Kind::Undo);
		else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) d.Emit(Action::Kind::SelectAll);
		else if ((ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) && d.keys.SelectionCount() > 0)
			d.Emit(Action::Kind::DeleteSelected);
		else if (ImGui::IsKeyPressed(ImGuiKey_F9, false))
			ApplyEase(d, io.KeyCtrl && io.KeyShift ? EaseKind::Out : (io.KeyShift ? EaseKind::In : EaseKind::InOut));
	}
}

void DrawOverlays(D& d) {
	TimelineDockState& st = d.st;
	// The box selection.
	if (st.gesture == Gesture::Box) {
		const float tx = d.mouse.x - kLeftW, gy = d.mouse.y - kRowsY;
		const float x0 = kLeftW + (std::min)(st.pressX, tx), x1 = kLeftW + (std::max)(st.pressX, tx);
		const float y0 = kRowsY + (std::min)(st.pressY, gy), y1 = kRowsY + (std::max)(st.pressY, gy);
		d.l->PushClipRect(d.P(kLeftW, kRowsY), d.P(kLeftW + d.g.tw, kRowsY + kRowsH), true);
		d.Fill(x0, y0, x1, y1, Paint::BoxFill);
		d.Outline(x0, y0, x1 + 1, y1 + 1, Paint::Accent);
		d.l->PopClipRect();
	}
	// The playhead, from under the ruler's range bar down through the rows.
	const float x = (float)TickToX(st.window, d.g.cw, d.m.playhead);
	if (x >= 0 && x <= d.g.tw) {
		const float top = kHeaderH + kOverviewH + 8;
		d.Fill(kLeftW + x - 1, top, kLeftW + x + 1, kRowsY + kRowsH, Paint::Playhead);
		d.Fill(kLeftW + x - 6, top, kLeftW + x + 6, top + 10, Paint::Playhead, 3);
	}
}

void DrawTip(D& d) {
	TimelineDockState& st = d.st;
	if (!d.tip || st.gesture != Gesture::None) {
		st.tipId = 0;
		return;
	}
	const double now = ImGui::GetTime();
	if (st.tipId != d.tipItem) {
		st.tipId = d.tipItem;
		st.tipSince = now;
		return;
	}
	if (now - st.tipSince < kTipDelay) return;
	const float tw = d.TextW(FontRole::Hint, d.tip) + 16, th = d.Size(FontRole::Hint) + 8;
	float x = d.tipX0;
	if (x + tw > d.w - 2) x = d.w - 2 - tw;
	if (x < 2) x = 2;
	float y = d.tipY0 - 4 - th;
	if (y < 0) y = d.tipY0 + 24;
	ImDrawList* fg = ImGui::GetForegroundDrawList();
	ImDrawList* keep = d.l;
	d.l = fg;
	d.Fill(x, y, x + tw, y + th, Paint::TipBg, 3);
	d.Outline(x, y, x + tw, y + th, Paint::Border, 3);
	d.Text(FontRole::Hint, x + 8, y + 4, Paint::Value, d.tip);
	d.l = keep;
}

} // namespace

std::vector<TimelineDockAction> DrawTimelineDock(TimelineDockState& state, const TimelineDockModel& model,
	const FontSet& fonts, const Rect& dock, float scale) {
	std::vector<Action> out;
	if (!model.keys || !(scale > 0) || !dock.Valid()) return out;
	const ImVec2 mouse = ImGui::GetIO().MousePos;
	D d{ state, model, *model.keys, fonts, ImGui::GetWindowDrawList(), scale, dock.x0, dock.y0, dock.W() / scale,
		WidthsFor(dock.W() / scale), Span(), out, ImVec2((mouse.x - dock.x0) / scale, (mouse.y - dock.y0) / scale) };

	// The window: seeded from C++'s suggestion when it changes, then the dock's own.
	d.domain = DomainFor(model.domainT0, model.domainT1, state.window, model.playhead);
	if (!state.seeded || state.seededEpoch != model.viewEpoch) {
		state.window = SeedView(model.viewT0, model.viewT1, model.playhead, d.domain);
		state.seeded = true;
		state.seededEpoch = model.viewEpoch;
		state.paging = Paging();
		state.knownValid = false;
	}
	state.window = Normalize(state.window, d.domain);
	FollowPlayback(state.window, d.domain, state.paging, model.playhead, model.playing,
		model.scrubbing || state.gesture == Gesture::Scrub);
	RevealNewKeys(d);

	const std::vector<Row> rows = Flatten(state);
	d.Fill(0, 0, d.w, kDockH, Paint::Panel);
	const float snapRight = DrawToolbar(d);
	DrawHeaderBand(d);
	DrawRows(d, rows);
	HandleRegions(d, rows);
	DrawOverlays(d);
	DrawFooter(d);
	DrawTip(d);
	DrawSnapMenu(d, snapRight); // over everything, with a catcher that closes it
	return out;
}

bool TimelineDockEscape(TimelineDockState& state) {
	if (!state.snapMenu) return false;
	state.snapMenu = false;
	return true;
}

std::string TimelineDockRibbonMeta(const TimelineDockModel& model) {
	if (!model.keys) return std::string();
	std::vector<KeyRef> keys;
	for (int c = 0; c < GraphExpModel::kChannelCount; ++c)
		for (const GraphExpModel::Keyframe& k : model.keys->GetChannel(c).keys) {
			KeyRef r;
			r.ch = c;
			r.id = k.id;
			r.t = k.tick;
			keys.push_back(r);
		}
	const int n = (int)MergeKeys(keys).size();
	char text[48];
	std::snprintf(text, sizeof(text), "1 layer \xC2\xB7 %d %s", n, n == 1 ? "key" : "keys");
	return text;
}

} // namespace Ui
} // namespace SourceFilmmaker
