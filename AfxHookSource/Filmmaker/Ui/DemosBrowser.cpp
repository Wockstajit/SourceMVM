#include "DemosBrowser.h"

#include "InspectorCards.h"
#include "UiWidgets.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace SourceFilmmaker {
namespace Ui {

namespace {

using namespace DemosBrowserMath;

#define MIDDOT "\xC2\xB7"
#define ELLIPSIS "\xE2\x80\xA6"

// Stage px (1080 tall). CS2's page: a 40 px list toolbar, 30 px folder headers, a 96 px action row
// under the details, the title 22 px bold 18 px from the top; the rail fits CS2's 340 px tiles with
// CS:S's wider Verdana.
constexpr float kMargin = 32, kHeaderH = 72, kRailW = 420, kGap = 16;
constexpr float kToolbarH = 40, kFolderH = 30, kFolderGap = 6, kTileH = 78, kTileGap = 4, kBadge = 56;
constexpr float kActionH = 96, kSearchW = 380;

// CS2's page colours in CS:S paint: its gold is the spectator orange, its status greens and oranges
// stay, so a demo's state reads the same in both games.
constexpr Color kPage = Rgba(0, 0, 0, 232);
constexpr Color kRail = Rgba(0, 0, 0, 120);
constexpr Color kRule = Rgba(255, 255, 255, 18);
constexpr Color kHeaderBg = Rgba(255, 255, 255, 10);
constexpr Color kHeaderHover = Rgba(255, 176, 0, 26);
constexpr Color kDropTarget = Rgba(255, 176, 0, 56);
constexpr Color kTileHover = Rgba(255, 255, 255, 12);
constexpr Color kTileSel = Rgba(255, 176, 0, 28);
constexpr Color kTileSelBorder = Rgba(255, 176, 0, 110);
constexpr Color kField = Rgba(5, 7, 10);
constexpr Color kOk = Rgba(101, 217, 149);
constexpr Color kGold = Rgba(240, 179, 35);
constexpr Color kUnsupported = Rgba(240, 154, 74);
constexpr Color kGrey = Rgba(174, 184, 194);
constexpr Color kTitleHover = Rgba(255, 215, 102);
constexpr Color kDanger = Rgba(255, 96, 72);

struct Hit {
	bool clicked = false, hovered = false, held = false, right = false;
};

Hit HitArea(const char* id, const Rect& r) {
	Hit h;
	if (!r.Valid()) return h;
	ImGui::SetCursorScreenPos(ImVec2(r.x0, r.y0));
	h.clicked = ImGui::InvisibleButton(id, ImVec2(r.W(), r.H()));
	h.hovered = ImGui::IsItemHovered();
	h.right = h.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
	h.held = ImGui::IsItemActive();
	return h;
}

float TextW(ImFont* font, const std::string& text) { return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, text.c_str()).x; }

void Text(ImDrawList* draw, ImFont* font, float x, float y, Color c, const std::string& text, float wrap = 0) {
	draw->AddText(font, font->FontSize, ImVec2(std::floor(x), std::floor(y)), ToU32(c), text.c_str(), nullptr, wrap);
}

// The text cut to width with an ellipsis at its end, never mid-character.
std::string Fit(ImFont* font, const std::string& text, float width) {
	if (TextW(font, text) <= width) return text;
	std::string out;
	for (size_t i = 0; i < text.size(); i += CharBytes(text, i)) {
		const std::string next = text.substr(0, i + CharBytes(text, i));
		if (TextW(font, next + ELLIPSIS) > width) break;
		out = next;
	}
	return out + ELLIPSIS;
}

void Box(ImDrawList* draw, const Rect& r, Color bg, Color border) {
	if (bg.a) draw->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), ToU32(bg));
	if (border.a) draw->AddRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), ToU32(border));
}

void Centred(ImDrawList* draw, ImFont* font, const Rect& r, Color c, const std::string& text) {
	const ImVec2 size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, text.c_str());
	Text(draw, font, r.x0 + (r.W() - size.x) * 0.5f, r.y0 + (r.H() - size.y) * 0.5f, c, text);
}

std::string DateText(int64_t t) {
	if (t <= 0) return std::string();
	const time_t tt = (time_t)t;
	std::tm tm = {};
#ifdef _WIN32
	if (localtime_s(&tm, &tt) != 0) return std::string();
#else
	if (!localtime_r(&tt, &tm)) return std::string();
#endif
	char text[40];
	std::strftime(text, sizeof(text), "%d %b %Y, %H:%M", &tm);
	return text;
}

std::string Upper(std::string text) {
	for (char& c : text)
		if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
	return text;
}

std::string Trim(const char* text) {
	std::string out = text ? text : "";
	for (char& c : out)
		if (c == '\r' || c == '\n' || c == '\t') c = ' ';
	size_t first = 0;
	while (first < out.size() && out[first] == ' ') ++first;
	out = out.substr(first);
	while (!out.empty() && out.back() == ' ') out.pop_back();
	return out;
}

void Copy(char* dst, size_t size, const std::string& text) {
	std::snprintf(dst, size, "%s", text.c_str());
}

// A text field over r in CS:S paint, with an accent outline. Enter commits (true); left reports the
// field losing focus.
bool Field(const char* id, const Rect& r, char* buf, size_t size, const char* hint, ImFont* font, float s, bool& focus,
	bool& left) {
	ImGui::SetCursorScreenPos(ImVec2(r.x0, r.y0));
	ImGui::PushFont(font);
	ImGui::PushStyleColor(ImGuiCol_FrameBg, ToU32(kField));
	ImGui::PushStyleColor(ImGuiCol_Border, ToU32(Theme::Accent));
	ImGui::PushStyleColor(ImGuiCol_Text, ToU32(Theme::Text));
	ImGui::PushStyleColor(ImGuiCol_TextDisabled, ToU32(Theme::TextDim));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * s, std::max(0.0f, (r.H() - font->FontSize) * 0.5f)));
	ImGui::SetNextItemWidth(r.W());
	if (focus) {
		ImGui::SetKeyboardFocusHere();
		focus = false;
	}
	const bool enter = ImGui::InputTextWithHint(id, hint, buf, size, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
	left = ImGui::IsItemDeactivated() && !enter;
	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor(4);
	ImGui::PopFont();
	return enter;
}

// Small drawn icons, so the page needs no image files.
void FolderIcon(ImDrawList* draw, float cx, float cy, float k, Color c) {
	const ImU32 ink = ToU32(c);
	const float w = 9 * k, h = 7 * k;
	draw->AddRect(ImVec2(cx - w, cy - h + 2 * k), ImVec2(cx + w, cy + h), ink, 0, 0, 1.5f * k);
	draw->AddLine(ImVec2(cx - w, cy - h + 2 * k), ImVec2(cx - w, cy - h - 1 * k), ink, 1.5f * k);
	draw->AddLine(ImVec2(cx - w, cy - h - 1 * k), ImVec2(cx - 2 * k, cy - h - 1 * k), ink, 1.5f * k);
	draw->AddLine(ImVec2(cx - 2 * k, cy - h - 1 * k), ImVec2(cx, cy - h + 2 * k), ink, 1.5f * k);
}

void TrashIcon(ImDrawList* draw, float cx, float cy, float k, Color c) {
	const ImU32 ink = ToU32(c);
	draw->AddLine(ImVec2(cx - 8 * k, cy - 6 * k), ImVec2(cx + 8 * k, cy - 6 * k), ink, 1.5f * k);
	draw->AddLine(ImVec2(cx - 3 * k, cy - 9 * k), ImVec2(cx + 3 * k, cy - 9 * k), ink, 1.5f * k);
	draw->AddRect(ImVec2(cx - 6 * k, cy - 6 * k), ImVec2(cx + 6 * k, cy + 9 * k), ink, 0, 0, 1.5f * k);
	for (int i = -1; i <= 1; ++i) draw->AddLine(ImVec2(cx + i * 3 * k, cy - 3 * k), ImVec2(cx + i * 3 * k, cy + 6 * k), ink, 1.2f * k);
}

void Chevron(ImDrawList* draw, float cx, float cy, float k, bool open, Color c) {
	if (open) draw->AddTriangleFilled(ImVec2(cx - 4 * k, cy - 2 * k), ImVec2(cx + 4 * k, cy - 2 * k), ImVec2(cx, cy + 3 * k), ToU32(c));
	else draw->AddTriangleFilled(ImVec2(cx - 2 * k, cy - 4 * k), ImVec2(cx - 2 * k, cy + 4 * k), ImVec2(cx + 3 * k, cy), ToU32(c));
}

void Plus(ImDrawList* draw, float cx, float cy, float k, Color c) {
	draw->AddLine(ImVec2(cx - 6 * k, cy), ImVec2(cx + 6 * k, cy), ToU32(c), 1.5f * k);
	draw->AddLine(ImVec2(cx, cy - 6 * k), ImVec2(cx, cy + 6 * k), ToU32(c), 1.5f * k);
}

// What the selected demo's header says about playing it, as CS2's compatibility readout.
struct Status {
	const char* title;
	std::string detail;
	Color color;
	bool bar = false;
};

Status StatusFor(const DemoRow& d, bool converting) {
	char text[96];
	if (converting) return { "CONVERTING", "Old CS:S demo: it plays when its copy is ready", kGold, true };
	switch (d.compat) {
	case Compat::Current: return { "SUPPORTED", "Current demo version", kOk };
	case Compat::Old: return { "OLD CS:S DEMO", "Converted for this game the first time it plays", kGold };
	case Compat::Converted: return { "OLD CS:S DEMO", "Converted and ready to watch", kOk };
	case Compat::Other:
		std::snprintf(text, sizeof(text), "Protocol %d " MIDDOT " this game plays 24 and old CS:S 7", d.protocol);
		return { "UNSUPPORTED DEMO", text, kUnsupported };
	default: return { "UNREADABLE DEMO", "No Source demo header in this file", kGrey };
	}
}

bool Playable(const DemoRow& d) { return d.compat == Compat::Current || d.compat == Compat::Old || d.compat == Compat::Converted; }

// Everything one frame of the page needs.
struct Page {
	DemosBrowserState& st;
	const FontSet& fonts;
	const DemosBrowserModel& m;
	DemosBrowserFrame& frame;
	float s, w, h;
	const char* tip = nullptr;
	std::string tipOwned;
	Rect tipRect;
	int dropGroup = -2; // the folder header under a dragged tile this frame; -2 none
	ImFont* F(FontRole role) const { return fonts.Get(role); }
	float Px(float stage) const { return stage * s; }
	void Send(const std::string& command) { frame.commands.push_back(command); }
	void Tip(const char* text, const Rect& r) { tip = text; tipRect = r; }
	void Tip(const std::string& text, const Rect& r) { tipOwned = text; tip = tipOwned.c_str(); tipRect = r; }
};

const DemoRow* Find(const DemosBrowserModel& m, const std::string& key) {
	for (const auto& d : m.demos)
		if (d.key == key) return &d;
	return nullptr;
}

const DemoGroup* FindGroup(const DemosBrowserModel& m, int id) {
	for (const auto& g : m.groups)
		if (g.id == id) return &g;
	return nullptr;
}

// The page's header: the title and count, and SEARCH, REFRESH, CLEAR ALL and ESC CLOSE at the right.
void Header(Page& p) {
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const float s = p.s, x0 = p.Px(kMargin), x1 = p.w - p.Px(kMargin);
	ImFont* title = p.F(FontRole::CardTitle);
	ImFont* note = p.F(FontRole::Note);
	ImFont* label = p.F(FontRole::Label);
	const float cy = p.Px(kHeaderH) * 0.5f;
	DrawSpacedText(draw, title, ImVec2(x0, std::floor(cy - title->FontSize * 0.5f)), ToU32(Theme::Accent), "DEMOS", 2.0f * s);
	const float tw = SpacedTextWidth(title, "DEMOS", 2.0f * s);
	char count[64];
	if (p.m.scanning) std::snprintf(count, sizeof(count), "Looking for demos" ELLIPSIS);
	else std::snprintf(count, sizeof(count), p.m.demos.size() == 1 ? "%d demo" : "%d demos", (int)p.m.demos.size());
	Text(draw, note, x0 + tw + p.Px(14), cy - note->FontSize * 0.5f + p.Px(1), Theme::TextDim, count);
	if (!p.m.message.empty()) {
		const float mx = x0 + tw + p.Px(14) + TextW(note, count) + p.Px(24);
		Text(draw, note, mx, cy - note->FontSize * 0.5f + p.Px(1), kUnsupported, Fit(note, p.m.message, x1 - p.Px(480) - mx));
	}

	const char* chips[] = { "ESC CLOSE", "CLEAR ALL", "REFRESH", "SEARCH" };
	float x = x1;
	const float chipH = label->FontSize + 2 * Metrics::chip.padV * s;
	for (int i = 0; i < 4; ++i) {
		const float cw = ChipWidth(chips[i], label, s);
		x -= cw;
		ImGui::PushID(i);
		const bool clicked = Chip("##chip", chips[i], ImVec2(x, std::floor(cy - chipH * 0.5f)), label, s, i == 3 && p.st.searchOpen);
		ImGui::PopID();
		if (clicked) {
			if (i == 0) p.frame.close = true;
			else if (i == 1) {
				if (!p.m.demos.empty()) p.st.confirm = DemosBrowserState::Confirm::ClearAll;
			} else if (i == 2) p.Send("refresh");
			else {
				p.st.searchOpen = !p.st.searchOpen;
				p.st.search[0] = 0;
				p.st.searchFocus = p.st.searchOpen;
			}
		}
		x -= p.Px(i == 0 ? 18 : 6);
	}
	draw->AddLine(ImVec2(x0, p.Px(kHeaderH) - 1), ImVec2(x1, p.Px(kHeaderH) - 1), ToU32(Theme::Border));
}

// The rail's toolbar: the sort as a dropdown field and the + menu.
void Toolbar(Page& p, const Rect& r) {
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const float s = p.s;
	Box(draw, r, Rgba(0, 0, 0, 56), Rgba(0, 0, 0, 0));
	draw->AddLine(ImVec2(r.x0, r.y1 - 1), ImVec2(r.x1, r.y1 - 1), ToU32(kRule));
	ImFont* caption = p.F(FontRole::Caption);
	ImFont* value = p.F(FontRole::Value);
	const float cy = (r.y0 + r.y1) * 0.5f;
	DrawSpacedText(draw, caption, ImVec2(r.x0 + p.Px(10), std::floor(cy - caption->FontSize * 0.5f)), ToU32(Theme::TextDim), "SORT", 1.5f * s);
	const float fx0 = r.x0 + p.Px(10) + SpacedTextWidth(caption, "SORT", 1.5f * s) + p.Px(10);
	const Rect field = MakeRect(fx0, cy - p.Px(13), fx0 + p.Px(190), cy + p.Px(13));
	const Hit f = HitArea("##sort", field);
	Box(draw, field, f.hovered ? Rgba(255, 255, 255, 13) : Theme::ButtonBg, p.st.menu == DemosBrowserState::Menu::Sort || f.hovered ? Theme::Accent : Theme::Border);
	Text(draw, value, field.x0 + p.Px(8), cy - value->FontSize * 0.5f, Theme::Text, SortCaption(SortFromName(p.m.sort)));
	Chevron(draw, field.x1 - p.Px(12), cy, s, true, Theme::Accent);
	if (f.clicked) {
		p.st.menu = p.st.menu == DemosBrowserState::Menu::Sort ? DemosBrowserState::Menu::None : DemosBrowserState::Menu::Sort;
		p.st.menuAnchor = field;
	}
	const Rect add = MakeRect(r.x1 - p.Px(8) - p.Px(30), cy - p.Px(13), r.x1 - p.Px(8), cy + p.Px(13));
	const Hit a = HitArea("##add", add);
	Box(draw, add, a.hovered ? Theme::Accent : Rgba(255, 255, 255, 16), Rgba(0, 0, 0, 0));
	Plus(draw, (add.x0 + add.x1) * 0.5f, cy, s, a.hovered ? Palette::Black : Theme::Text);
	if (a.hovered) p.Tip("New folder, add a demo file or a demo folder", add);
	if (a.clicked) {
		p.st.menu = p.st.menu == DemosBrowserState::Menu::Add ? DemosBrowserState::Menu::None : DemosBrowserState::Menu::Add;
		p.st.menuAnchor = MakeRect(add.x1 - p.Px(240), add.y0, add.x1, add.y1);
	}
}

// CS2's new-folder block under the toolbar: a caption, the name field, confirm and cancel.
float NewFolderBlock(Page& p, const Rect& r) {
	if (!p.st.newFolder) return 0;
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const float s = p.s;
	ImFont* caption = p.F(FontRole::Caption);
	const float h = p.Px(7 + 11 + 5 + 34 + 8);
	const Rect block = MakeRect(r.x0, r.y0, r.x1, r.y0 + h);
	Box(draw, block, Rgba(255, 176, 0, 18), Rgba(0, 0, 0, 0));
	draw->AddLine(ImVec2(block.x0, block.y1 - 1), ImVec2(block.x1, block.y1 - 1), ToU32(kRule));
	DrawSpacedText(draw, caption, ImVec2(block.x0 + p.Px(10), block.y0 + p.Px(7)), ToU32(Theme::TextDim), "NEW FOLDER", 1.5f * s);
	const float fy = block.y0 + p.Px(7 + 11 + 5);
	const float bw = p.Px(34);
	const Rect cancel = MakeRect(block.x1 - p.Px(8) - bw, fy, block.x1 - p.Px(8), fy + bw);
	const Rect ok = MakeRect(cancel.x0 - p.Px(5) - bw, fy, cancel.x0 - p.Px(5), fy + bw);
	const Rect field = MakeRect(block.x0 + p.Px(10), fy, ok.x0 - p.Px(5), fy + bw);
	// The buttons first, so a press on them is not taken for leaving the field.
	const Hit okHit = HitArea("##nfok", ok);
	const Hit cancelHit = HitArea("##nfcancel", cancel);
	Box(draw, ok, okHit.hovered ? Theme::Accent : Rgba(255, 176, 0, 210), Rgba(0, 0, 0, 0));
	{
		const float cx = (ok.x0 + ok.x1) * 0.5f, cy = (ok.y0 + ok.y1) * 0.5f;
		const ImU32 ink = ToU32(Palette::Black);
		draw->AddLine(ImVec2(cx - 6 * s, cy), ImVec2(cx - 2 * s, cy + 4 * s), ink, 2 * s);
		draw->AddLine(ImVec2(cx - 2 * s, cy + 4 * s), ImVec2(cx + 6 * s, cy - 5 * s), ink, 2 * s);
	}
	Box(draw, cancel, cancelHit.hovered ? Rgba(255, 255, 255, 40) : Rgba(255, 255, 255, 16), Rgba(0, 0, 0, 0));
	{
		const float cx = (cancel.x0 + cancel.x1) * 0.5f, cy = (cancel.y0 + cancel.y1) * 0.5f, k = 5 * s;
		const ImU32 ink = ToU32(cancelHit.hovered ? Theme::Text : Theme::TextDim);
		draw->AddLine(ImVec2(cx - k, cy - k), ImVec2(cx + k, cy + k), ink, 1.5f * s);
		draw->AddLine(ImVec2(cx - k, cy + k), ImVec2(cx + k, cy - k), ink, 1.5f * s);
	}
	bool left = false;
	const bool enter = Field("##newfolder", field, p.st.newFolderText, sizeof(p.st.newFolderText), "Name this folder", p.F(FontRole::Body), s,
		p.st.newFolderFocus, left);
	const std::string name = Trim(p.st.newFolderText);
	if ((enter || okHit.clicked) && !name.empty()) {
		p.Send("org newfolder " + name);
		p.st.newFolder = false;
		p.st.newFolderText[0] = 0;
	} else if (cancelHit.clicked) {
		p.st.newFolder = false;
		p.st.newFolderText[0] = 0;
	} else if (okHit.clicked) {
		p.st.newFolderFocus = true; // nothing typed yet
	}
	return h;
}

// A folder header: chevron, name, count. Click collapses it, right-click opens its menu, and it takes
// a dragged tile.
void FolderHeader(Page& p, const PlanItem& item, float x0, float x1, float& y) {
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const float s = p.s;
	y += p.Px(kFolderGap);
	const Rect r = MakeRect(x0, y, x1, y + p.Px(kFolderH));
	const DemoGroup* g = FindGroup(p.m, item.group);
	const bool collapsed = g ? g->collapsed : p.m.rootCollapsed;
	const std::string name = g ? g->name : std::string("No folder");
	ImGui::PushID(item.group);
	const bool renaming = g && p.st.renamingGroup == g->id;
	Hit h;
	if (!renaming) h = HitArea("##head", r);
	const bool dragging = !p.st.dragKey.empty();
	const bool dropHere = dragging && ImGui::IsMouseHoveringRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1));
	if (dropHere) p.dropGroup = item.group;
	Box(draw, r, dropHere ? kDropTarget : (h.hovered ? kHeaderHover : kHeaderBg), dropHere ? Theme::Accent : Rgba(0, 0, 0, 0));
	const float cy = (r.y0 + r.y1) * 0.5f;
	Chevron(draw, r.x0 + p.Px(14), cy, s, !collapsed, h.hovered || dropHere ? Theme::Accent : Theme::TextDim);
	ImFont* label = p.F(FontRole::Label);
	ImFont* note = p.F(FontRole::Note);
	char count[16];
	std::snprintf(count, sizeof(count), "%d", item.count);
	const float countW = TextW(note, count);
	const float nameX = r.x0 + p.Px(28), countX = r.x1 - p.Px(12) - countW;
	if (renaming) {
		bool left = false;
		const Rect field = MakeRect(nameX, r.y0 + p.Px(4), countX - p.Px(8), r.y1 - p.Px(4));
		const bool enter = Field("##renamefolder", field, p.st.groupText, sizeof(p.st.groupText), "Folder name", p.F(FontRole::RowLabel), s,
			p.st.groupFocus, left);
		if (enter || left) {
			const std::string typed = Trim(p.st.groupText);
			if (!typed.empty() && typed != g->name) p.Send("org rename " + std::to_string(g->id) + " " + typed);
			p.st.renamingGroup = -1;
		}
	} else {
		Text(draw, label, nameX, cy - label->FontSize * 0.5f, Theme::Text, Fit(label, Upper(name), countX - p.Px(8) - nameX));
	}
	Text(draw, note, countX, cy - note->FontSize * 0.5f, Theme::TextDim, count);
	if (h.clicked && !dragging) p.Send("org collapse " + std::to_string(item.group) + (collapsed ? " 0" : " 1"));
	if (h.right && g) {
		p.st.menu = DemosBrowserState::Menu::Folder;
		p.st.menuGroup = g->id;
		const ImVec2 mouse = ImGui::GetMousePos();
		p.st.menuAnchor = MakeRect(mouse.x, mouse.y, mouse.x + p.Px(220), mouse.y);
	}
	ImGui::PopID();
	y = r.y1;
}

// The map's badge: a square with the map kind (DE, CS, AIM), CS2's map icon in CS:S paint.
void MapBadge(ImDrawList* draw, const FontSet& fonts, const Rect& r, const DemoRow& d, float alpha) {
	Box(draw, r, Faded(Rgba(255, 255, 255, 14), alpha), Faded(Theme::Border, alpha));
	const std::string prefix = MapPrefix(d.map);
	ImFont* font = fonts.Get(prefix.size() > 2 ? FontRole::Value : FontRole::CardTitle);
	Centred(draw, font, r, Faded(Theme::Accent, alpha), prefix.empty() ? "?" : prefix);
}

// The tile's compatibility tag at its top right: an old demo still to convert in gold, a converted one
// in green, an unknown protocol in orange; none for a current demo.
void Tag(Page& p, ImDrawList* draw, const DemoRow& d, float xRight, float y, float alpha) {
	const char* text = nullptr;
	Color c = kGold;
	const char* tip = nullptr;
	if (d.compat == Compat::Old) {
		text = "V34";
		tip = "Old CS:S (v34) demo: converted for this game the first time it plays";
	} else if (d.compat == Compat::Converted) {
		text = "V34";
		c = kOk;
		tip = "Old CS:S (v34) demo, already converted for this game";
	} else if (d.compat == Compat::Other || d.compat == Compat::Unknown) {
		text = "?";
		c = kUnsupported;
		tip = d.compat == Compat::Other ? "A demo protocol this game does not play" : "No Source demo header in this file";
	}
	if (!text) return;
	ImFont* font = p.F(FontRole::Caption);
	const float tw = TextW(font, text);
	const Rect r = MakeRect(xRight - tw - p.Px(10), y, xRight, y + font->FontSize + p.Px(6));
	Box(draw, r, Faded(Rgba(0, 0, 0, 120), alpha), Faded(c, alpha));
	Centred(draw, font, r, Faded(c, alpha), text);
	if (ImGui::IsMouseHoveringRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1)) && ImGui::IsWindowHovered()) p.Tip(tip, r);
}

void Tile(Page& p, const DemoRow& d, bool lastPlayed, float x0, float x1, float& y) {
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const float s = p.s;
	const Rect r = MakeRect(x0, y, x1, y + p.Px(kTileH));
	const bool selected = d.key == p.st.selected;
	if (selected && p.st.scrollToSelected) {
		ImGui::SetCursorScreenPos(ImVec2(r.x0, (r.y0 + r.y1) * 0.5f));
		ImGui::SetScrollHereY(0.5f);
		p.st.scrollToSelected = false;
	}
	ImGui::PushID(d.key.c_str());
	const Hit h = HitArea("##tile", r);
	ImGui::PopID();
	const bool dragged = d.key == p.st.dragKey;
	const float alpha = dragged ? 0.32f : 1.0f;
	Box(draw, r, selected ? kTileSel : (h.hovered && p.st.dragKey.empty() ? kTileHover : Rgba(255, 255, 255, 5)),
		selected ? kTileSelBorder : Rgba(0, 0, 0, 0));
	// CS2's gold edge on the one demo played last; every other tile keeps the space, so nothing shifts.
	if (lastPlayed) draw->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x0 + p.Px(3), r.y1), ToU32(Faded(Theme::Accent, alpha)));
	const float pad = p.Px(12);
	const Rect badge = MakeRect(r.x0 + pad, r.y0 + (r.H() - p.Px(kBadge)) * 0.5f, r.x0 + pad + p.Px(kBadge), r.y0 + (r.H() + p.Px(kBadge)) * 0.5f);
	MapBadge(draw, p.fonts, badge, d, alpha);
	ImFont* value = p.F(FontRole::Value);
	ImFont* row = p.F(FontRole::RowLabel);
	ImFont* note = p.F(FontRole::Note);
	const float tx = badge.x1 + p.Px(12), tw = r.x1 - pad - tx;
	const float lineGap = p.Px(4);
	const float blockH = value->FontSize + row->FontSize + note->FontSize + 2 * lineGap;
	float ty = r.y0 + (r.H() - blockH) * 0.5f;
	Text(draw, value, tx, ty, Faded(Theme::Text, alpha), Fit(value, MapDisplay(d.map), tw - p.Px(44)));
	Tag(p, draw, d, r.x1 - pad, ty - p.Px(1), alpha);
	ty += value->FontSize + lineGap;
	Text(draw, row, tx, ty, Faded(Theme::Text, alpha * 0.85f), Fit(row, DemoName(d), tw));
	ty += row->FontSize + lineGap;
	std::string line = DateText(d.date);
	const std::string length = Duration(d.seconds);
	if (!length.empty()) line += (line.empty() ? "" : "  " MIDDOT "  ") + length;
	Text(draw, note, tx, ty, Faded(Theme::TextDim, alpha), Fit(note, line, tw));

	if (h.clicked && p.st.dragKey.empty()) {
		if (p.st.selected != d.key) p.st.renaming = false;
		p.st.selected = d.key;
	}
	// A press that moves past the drag threshold picks the tile up for a folder header.
	if (h.held && p.st.dragKey.empty() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6 * s)) p.st.dragKey = d.key;
	if (h.right) {
		p.st.menu = DemosBrowserState::Menu::Tile;
		p.st.menuKey = d.key;
		const ImVec2 mouse = ImGui::GetMousePos();
		p.st.menuAnchor = MakeRect(mouse.x, mouse.y, mouse.x + p.Px(260), mouse.y);
	}
	y = r.y1 + p.Px(kTileGap);
}

void Rail(Page& p, const Rect& rail) {
	ImDrawList* draw = ImGui::GetWindowDrawList();
	Box(draw, rail, kRail, Theme::Border);
	const Rect toolbar = MakeRect(rail.x0 + 1, rail.y0 + 1, rail.x1 - 1, rail.y0 + p.Px(kToolbarH));
	Toolbar(p, toolbar);
	const float blockH = NewFolderBlock(p, MakeRect(rail.x0 + 1, toolbar.y1, rail.x1 - 1, rail.y1));
	const Rect list = MakeRect(rail.x0 + 1, toolbar.y1 + blockH, rail.x1 - 1, rail.y1 - 1);

	const std::vector<PlanItem> plan = Plan(p.m.demos, p.m.groups, SortFromName(p.m.sort), p.m.rootCollapsed);
	// The gold edge: the most recently played demo that is listed.
	std::string lastKey;
	int64_t lastPlayed = 0;
	for (const auto& d : p.m.demos)
		if (d.played > lastPlayed) {
			lastPlayed = d.played;
			lastKey = d.key;
		}

	ImGui::SetCursorScreenPos(ImVec2(list.x0, list.y0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::BeginChild("##demolist", ImVec2(list.W(), list.H()), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
	ImGui::PopStyleVar();
	const float scrollbar = ImGui::GetScrollMaxY() > 0 ? ImGui::GetStyle().ScrollbarSize : 0.0f;
	const float x0 = list.x0 + p.Px(8), x1 = list.x1 - p.Px(8) - scrollbar;
	float y = ImGui::GetCursorScreenPos().y + p.Px(6);
	for (const PlanItem& item : plan) {
		if (item.header) FolderHeader(p, item, x0, x1, y);
		else Tile(p, p.m.demos[(size_t)item.row], p.m.demos[(size_t)item.row].key == lastKey, x0, x1, y);
	}
	p.st.scrollToSelected = false; // a demo in a shut folder has no tile to bring into view
	if (plan.empty()) {
		ImFont* note = p.F(FontRole::Note);
		Text(draw, note, x0 + p.Px(6), y + p.Px(6), Theme::TextDim,
			p.m.scanning ? "Looking for demos" ELLIPSIS : "No demos in the list.");
		y += note->FontSize + p.Px(12);
	}
	ImGui::SetCursorScreenPos(ImVec2(x0, y + p.Px(6)));
	ImGui::Dummy(ImVec2(1, 1));
	ImGui::EndChild();
}

// One "label    value" line of the details card.
void InfoRow(Page& p, const Rect& card, float& y, const char* label, const std::string& value, Color valueColor = Theme::Text) {
	ImDrawList* draw = ImGui::GetWindowDrawList();
	ImFont* l = p.F(FontRole::RowLabel);
	ImFont* v = p.F(FontRole::Value);
	const float rowH = p.Px(34);
	const Rect r = MakeRect(card.x0, y, card.x1, y + rowH);
	Box(draw, r, kCardTileBg, Rgba(0, 0, 0, 0));
	const float cy = (r.y0 + r.y1) * 0.5f;
	Text(draw, l, r.x0 + p.Px(14), cy - l->FontSize * 0.5f, Theme::TextDim, label);
	const float vx = r.x0 + p.Px(170);
	const std::string shown = value.empty() ? "-" : value;
	const std::string fit = Fit(v, shown, r.x1 - p.Px(14) - vx);
	Text(draw, v, vx, cy - v->FontSize * 0.5f, valueColor, fit);
	if (fit != shown && ImGui::IsMouseHoveringRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1))) p.Tip(shown, r);
	y = r.y1 + p.Px(4);
}

// A square icon button of the action row.
bool IconButton(Page& p, const char* id, const Rect& r, bool danger, bool folder, const char* tip) {
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const Hit h = HitArea(id, r);
	Color bg = h.held ? Theme::ButtonArmedBg : (h.hovered ? (danger ? Rgba(192, 28, 0, 60) : Theme::ButtonHoverBg) : Rgba(255, 255, 255, 16));
	Box(draw, r, bg, h.hovered ? (danger ? Palette::Red : Theme::Accent) : Theme::Border);
	const float cx = (r.x0 + r.x1) * 0.5f, cy = (r.y0 + r.y1) * 0.5f;
	const Color ink = h.hovered && danger ? kDanger : Theme::Text;
	if (folder) FolderIcon(draw, cx, cy, p.s, ink);
	else TrashIcon(draw, cx, cy, p.s, ink);
	if (h.hovered) p.Tip(tip, r);
	return h.clicked;
}

void Details(Page& p, const Rect& panel) {
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const float s = p.s;
	Box(draw, panel, kRail, Theme::Border);
	const DemoRow* d = Find(p.m, p.st.selected);
	if (!d) {
		ImFont* body = p.F(FontRole::Body);
		const char* text = p.m.scanning ? "Looking for demos" ELLIPSIS
			: "No demos found. Use the + button above the list to add a demo folder.";
		Centred(draw, body, panel, Theme::TextDim, text);
		return;
	}

	// The name: click to rename the file, as CS2's title.
	ImFont* title = p.F(FontRole::Timecode);
	const float titleY = panel.y0 + p.Px(18);
	const float titleW = panel.W() * 0.8f;
	const Rect titleRect = MakeRect(panel.x0 + (panel.W() - titleW) * 0.5f, titleY - p.Px(4), panel.x0 + (panel.W() + titleW) * 0.5f, titleY + p.Px(34));
	if (p.st.renaming) {
		bool left = false;
		const bool enter = Field("##renamedemo", titleRect, p.st.renameText, sizeof(p.st.renameText), "Demo name", title, s, p.st.renameFocus, left);
		if (enter || left) {
			const std::string typed = Trim(p.st.renameText);
			if (!typed.empty() && typed != DemoName(*d)) {
				p.Send("renamedemo " + std::to_string(d->index) + " " + typed);
				// Select the renamed file once the list has it.
				const std::string file = DemoFileName(typed);
				if (!file.empty()) p.st.selected = Lower(d->folder + "\\" + file);
				for (char& c : p.st.selected)
					if (c == '/') c = '\\';
			}
			p.st.renaming = false;
		}
	} else {
		const Hit h = HitArea("##title", titleRect);
		const std::string name = Fit(title, DemoName(*d), titleW);
		Centred(draw, title, titleRect, h.hovered ? kTitleHover : Rgba(255, 255, 255), name);
		if (h.hovered) p.Tip("Click to rename this demo file", titleRect);
		if (h.clicked) {
			p.st.renaming = true;
			p.st.renameFocus = true;
			p.st.renameKey = d->key;
			Copy(p.st.renameText, sizeof(p.st.renameText), DemoName(*d));
		}
	}
	ImFont* note = p.F(FontRole::Note);
	const Rect folderLine = MakeRect(panel.x0 + p.Px(24), titleRect.y1 + p.Px(4), panel.x1 - p.Px(24), titleRect.y1 + p.Px(4) + note->FontSize);
	Centred(draw, note, folderLine, Theme::TextDim, Fit(note, d->folder, folderLine.W()));

	// CS2's meta row: Map, Duration, Date.
	ImFont* caption = p.F(FontRole::Caption);
	ImFont* metaValue = p.F(FontRole::CardTitle);
	const float metaY = folderLine.y1 + p.Px(30);
	const char* captions[] = { "MAP", "LENGTH", "DATE" };
	std::string values[] = { MapDisplay(d->map), Duration(d->seconds), DateText(d->date) };
	const float colW = std::min(p.Px(260), (panel.W() - p.Px(48)) / 3);
	const float metaX = panel.x0 + (panel.W() - 3 * colW) * 0.5f;
	for (int i = 0; i < 3; ++i) {
		const float cx = metaX + colW * (i + 0.5f);
		const float cw = SpacedTextWidth(caption, captions[i], 1.5f * s);
		DrawSpacedText(draw, caption, ImVec2(std::floor(cx - cw * 0.5f), metaY), ToU32(Theme::TextDim), captions[i], 1.5f * s);
		const std::string v = Fit(metaValue, values[i].empty() ? "-" : values[i], colW - p.Px(12));
		Text(draw, metaValue, cx - TextW(metaValue, v) * 0.5f, metaY + caption->FontSize + p.Px(8), Theme::Text, v);
	}

	// What the header says, as CS2's scoreboard area holds the match: a card of lines.
	const float cardW = std::min(p.Px(720), panel.W() - p.Px(64));
	const Rect card = MakeRect(panel.x0 + (panel.W() - cardW) * 0.5f, metaY + caption->FontSize + metaValue->FontSize + p.Px(40),
		panel.x0 + (panel.W() + cardW) * 0.5f, panel.y1 - p.Px(kActionH) - p.Px(16));
	float y = card.y0;
	char text[128];
	InfoRow(p, card, y, "Map file", d->map);
	InfoRow(p, card, y, "Recorded by", d->recorder);
	InfoRow(p, card, y, "Server", d->server);
	if (d->ticks > 0 && d->seconds > 0) {
		std::snprintf(text, sizeof(text), "%d at %.0f ticks a second", d->ticks, d->ticks / d->seconds);
		InfoRow(p, card, y, "Ticks", text);
	} else InfoRow(p, card, y, "Ticks", std::string());
	const char* kind = d->compat == Compat::Current ? "current" : (d->compat == Compat::Old || d->compat == Compat::Converted ? "old CS:S, v34" : "not played here");
	if (d->protocol > 0) std::snprintf(text, sizeof(text), "Protocol %d (%s)", d->protocol, kind);
	else text[0] = 0;
	InfoRow(p, card, y, "Version", text);
	InfoRow(p, card, y, "Last played", Ago(d->played, p.m.now));
	InfoRow(p, card, y, "File", d->fileName);

	// CS2's action row: the compatibility readout, WATCH, Show in Explorer, Remove.
	const float rowY = panel.y1 - p.Px(kActionH);
	draw->AddLine(ImVec2(panel.x0 + 1, rowY), ImVec2(panel.x1 - 1, rowY), ToU32(Rgba(255, 255, 255, 32)));
	const float bh = p.Px(48), by = rowY + (p.Px(kActionH) - bh) * 0.5f;
	const float right = panel.x1 - p.Px(48);
	const Rect trash = MakeRect(right - p.Px(64), by, right, by + bh);
	const Rect folder = MakeRect(trash.x0 - p.Px(8) - p.Px(64), by, trash.x0 - p.Px(8), by + bh);
	const Rect watch = MakeRect(folder.x0 - p.Px(14) - p.Px(200), by, folder.x0 - p.Px(14), by + bh);
	const bool converting = p.m.converting && p.st.watchKey == d->key;
	const bool busy = converting || ImGui::GetTime() < p.st.watchBusyUntil;
	const bool canWatch = Playable(*d) && !busy;
	ImGui::PushID("watch");
	const bool watchClicked = CardButton("##watch", "WATCH", watch, ButtonKind::Solid, canWatch, p.F(FontRole::BodyBold));
	ImGui::PopID();
	if (!canWatch && !busy && ImGui::IsMouseHoveringRect(ImVec2(watch.x0, watch.y0), ImVec2(watch.x1, watch.y1)))
		p.Tip("This game cannot play this demo", watch);
	if (watchClicked) {
		p.Send("watch " + std::to_string(d->index));
		p.st.watchKey = d->key;
		p.st.watchBusyUntil = ImGui::GetTime() + 1.5; // CS2's guard against a double click
	}
	if (IconButton(p, "##folder", folder, false, true, "Show the demo file in Explorer")) p.Send("openfolder " + std::to_string(d->index));
	if (IconButton(p, "##trash", trash, true, false, "Remove from the list. The file stays on disk.")) {
		p.st.confirm = DemosBrowserState::Confirm::Remove;
		p.st.confirmIndex = d->index;
		p.st.confirmName = d->fileName;
	}
	const Status status = StatusFor(*d, converting);
	ImFont* statusTitle = p.F(FontRole::Title);
	const float sw = std::min(p.Px(330), watch.x0 - p.Px(14) - (panel.x0 + p.Px(24)));
	const float sx = watch.x0 - p.Px(14) - sw;
	const float blockH = statusTitle->FontSize + p.Px(4) + note->FontSize + (status.bar ? p.Px(8) : 0);
	float sy = rowY + (p.Px(kActionH) - blockH) * 0.5f;
	if (sw > p.Px(80)) {
		DrawSpacedText(draw, statusTitle, ImVec2(sx, sy), ToU32(status.color), Fit(statusTitle, status.title, sw).c_str(), 0.5f * s);
		sy += statusTitle->FontSize + p.Px(4);
		Text(draw, note, sx, sy, Rgba(174, 184, 194), Fit(note, status.detail, sw));
		if (status.bar) {
			// CS2's indeterminate bar: a 78 px fill sliding along a 4 px track.
			sy += note->FontSize + p.Px(4);
			const Rect track = MakeRect(sx, sy, sx + sw, sy + p.Px(4));
			Box(draw, track, Rgba(255, 176, 0, 46), Rgba(0, 0, 0, 0));
			const float span = sw + p.Px(78);
			const float at = std::fmod((float)ImGui::GetTime() * p.Px(228), span) - p.Px(78);
			const float f0 = std::max(track.x0, track.x0 + at), f1 = std::min(track.x1, track.x0 + at + p.Px(78));
			if (f1 > f0) draw->AddRectFilled(ImVec2(f0, track.y0), ImVec2(f1, track.y1), ToU32(Theme::Accent));
		}
	}
}

// CS2's jump-to search: a field at the top right and up to 12 matches under it.
void SearchOverlay(Page& p) {
	if (!p.st.searchOpen) return;
	const float s = p.s;
	ImFont* body = p.F(FontRole::Body);
	ImFont* row = p.F(FontRole::RowLabel);
	ImFont* hint = p.F(FontRole::Note);
	const float x1 = p.w - p.Px(kMargin), x0 = x1 - p.Px(kSearchW);
	const float y0 = p.Px(kHeaderH) + p.Px(8);
	const float fieldH = p.Px(36);
	const std::vector<SearchHit> hits = Search(p.m.demos, p.st.search);
	const float rowH = row->FontSize + hint->FontSize + p.Px(7 * 2 + 3);
	const float listH = hits.empty() ? 0 : std::min(p.Px(340), rowH * hits.size()) + p.Px(2);
	BeginOverlay("##demosearch", MakeRect(x0, y0, x1, y0 + fieldH + (hits.empty() ? 0 : p.Px(4) + listH)), Rgba(0, 0, 0, 0));
	bool left = false;
	Field("##searchfield", MakeRect(x0, y0, x1, y0 + fieldH), p.st.search, sizeof(p.st.search), "Search map, demo name, recorder or server",
		body, s, p.st.searchFocus, left);
	if (!hits.empty()) {
		ImDrawList* draw = ImGui::GetWindowDrawList();
		const Rect list = MakeRect(x0, y0 + fieldH + p.Px(4), x1, y0 + fieldH + p.Px(4) + listH);
		draw->AddRectFilled(ImVec2(list.x0, list.y0), ImVec2(list.x1, list.y1), ToU32(Rgba(10, 10, 10)));
		draw->AddRect(ImVec2(list.x0, list.y0), ImVec2(list.x1, list.y1), ToU32(Theme::Border));
		ImGui::SetCursorScreenPos(ImVec2(list.x0 + 1, list.y0 + 1));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		const bool scrolls = rowH * hits.size() > p.Px(340);
		ImGui::BeginChild("##hits", ImVec2(list.W() - 2, list.H() - 2), ImGuiChildFlags_None,
			ImGuiWindowFlags_NoBackground | (scrolls ? 0 : ImGuiWindowFlags_NoScrollbar));
		ImGui::PopStyleVar();
		ImDrawList* inner = ImGui::GetWindowDrawList();
		float y = ImGui::GetCursorScreenPos().y;
		const float iw = list.W() - 2 - (ImGui::GetScrollMaxY() > 0 ? ImGui::GetStyle().ScrollbarSize : 0.0f);
		for (size_t i = 0; i < hits.size(); ++i) {
			const DemoRow& d = p.m.demos[(size_t)hits[i].row];
			const Rect r = MakeRect(list.x0 + 1, y, list.x0 + 1 + iw, y + rowH);
			ImGui::PushID((int)i);
			const Hit h = HitArea("##hit", r);
			ImGui::PopID();
			if (h.hovered) inner->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), ToU32(Rgba(255, 176, 0, 40)));
			inner->AddLine(ImVec2(r.x0, r.y1 - 1), ImVec2(r.x1, r.y1 - 1), ToU32(Rgba(255, 255, 255, 13)));
			Text(inner, row, r.x0 + p.Px(12), r.y0 + p.Px(7), Theme::Text,
				Fit(row, MapDisplay(d.map) + "  " MIDDOT "  " + CropName(DemoName(d)), r.W() - p.Px(24)));
			Text(inner, hint, r.x0 + p.Px(12), r.y0 + p.Px(10) + row->FontSize, Theme::TextDim, Fit(hint, CropName(hits[i].why, 30, 8), r.W() - p.Px(24)));
			if (h.clicked) {
				// A demo in a shut folder: open the folder, so its tile is there to show.
				const DemoGroup* g = FindGroup(p.m, d.group);
				if (g ? g->collapsed : (p.m.rootCollapsed && !p.m.groups.empty()))
					p.Send("org collapse " + std::to_string(g ? g->id : -1) + " 0");
				p.st.selected = d.key;
				p.st.scrollToSelected = true;
				p.st.renaming = false;
				p.st.searchOpen = false;
				p.st.search[0] = 0;
			}
			y = r.y1;
		}
		ImGui::SetCursorScreenPos(ImVec2(list.x0, y));
		ImGui::Dummy(ImVec2(1, 1));
		ImGui::EndChild();
	}
	ImGui::End();
}

void Menus(Page& p) {
	using Menu = DemosBrowserState::Menu;
	if (p.st.menu == Menu::None) return;
	std::vector<DropOption> options;
	auto add = [&options](const std::string& text, Color color = Theme::Text, bool selected = false) {
		DropOption o;
		o.text = text;
		o.color = color;
		o.selected = selected;
		options.push_back(o);
	};
	std::vector<std::string> commands; // per option; empty does nothing
	const DemoRow* d = p.st.menu == Menu::Tile ? Find(p.m, p.st.menuKey) : nullptr;
	switch (p.st.menu) {
	case Menu::Sort:
		for (int i = 0; i < (int)Sort::Count; ++i) {
			add(SortCaption((Sort)i), Theme::Text, SortFromName(p.m.sort) == (Sort)i);
			commands.push_back(std::string("org sort ") + SortName((Sort)i));
		}
		break;
	case Menu::Add:
		add("New folder");
		commands.push_back("#newfolder");
		add("Add demo file" ELLIPSIS);
		commands.push_back("adddemo");
		add("Add demo folder" ELLIPSIS);
		commands.push_back("addfolder");
		if (p.m.hidden > 0) {
			add("Show removed demos (" + std::to_string(p.m.hidden) + ")");
			commands.push_back("unhideall");
		}
		break;
	case Menu::Tile:
		if (!d) break;
		add("Rename demo" ELLIPSIS);
		commands.push_back("#rename");
		for (const auto& g : p.m.groups) {
			if (g.id == d->group) continue;
			add("Move to " + g.name);
			commands.push_back("org assign " + std::to_string(g.id) + " " + d->key);
		}
		if (FindGroup(p.m, d->group)) {
			add("Remove from folder");
			commands.push_back("org assign -1 " + d->key);
		}
		if (p.m.groups.empty()) {
			add("New folder" ELLIPSIS);
			commands.push_back("#newfolder");
		}
		add("Last played: " + Ago(d->played, p.m.now), Theme::TextDim);
		commands.push_back(std::string());
		break;
	case Menu::Folder:
		add("Rename folder");
		commands.push_back("#renamefolder");
		add("Delete folder", Rgba(255, 96, 72));
		commands.push_back("#deletefolder");
		break;
	default:
		break;
	}
	if (options.empty()) {
		p.st.menu = Menu::None;
		return;
	}
	bool closed = false;
	const int picked = DropList("##demosmenu", p.st.menuAnchor, options, p.fonts, p.s, p.w, p.h, closed);
	if (picked >= 0 && picked < (int)commands.size()) {
		const std::string& c = commands[(size_t)picked];
		if (c == "#newfolder") {
			p.st.newFolder = true;
			p.st.newFolderFocus = true;
			p.st.newFolderText[0] = 0;
		} else if (c == "#rename" && d) {
			p.st.selected = d->key;
			p.st.renaming = true;
			p.st.renameFocus = true;
			p.st.renameKey = d->key;
			Copy(p.st.renameText, sizeof(p.st.renameText), DemoName(*d));
		} else if (c == "#renamefolder") {
			if (const DemoGroup* g = FindGroup(p.m, p.st.menuGroup)) {
				p.st.renamingGroup = g->id;
				p.st.groupFocus = true;
				Copy(p.st.groupText, sizeof(p.st.groupText), g->name);
			}
		} else if (c == "#deletefolder") {
			if (const DemoGroup* g = FindGroup(p.m, p.st.menuGroup)) {
				p.st.confirm = DemosBrowserState::Confirm::DeleteFolder;
				p.st.confirmIndex = g->id;
				p.st.confirmName = g->name;
			}
		} else if (!c.empty()) {
			p.Send(c);
		}
		if (!c.empty()) p.st.menu = Menu::None;
	}
	if (closed) p.st.menu = Menu::None;
}

// CS2's OK / Cancel popups, over a dimmed page.
void ConfirmDialog(Page& p) {
	using Confirm = DemosBrowserState::Confirm;
	if (p.st.confirm == Confirm::None) return;
	const float s = p.s;
	std::string title, body, ok;
	if (p.st.confirm == Confirm::ClearAll) {
		title = "Clear all demos?";
		body = "Remove every demo from this list?\nDemo files stay on disk.";
		ok = "CLEAR ALL";
	} else if (p.st.confirm == Confirm::Remove) {
		title = "Remove demo?";
		body = "Remove \"" + p.st.confirmName + "\" from the list?\nThe demo file stays on disk.";
		ok = "REMOVE";
	} else {
		title = "Delete folder?";
		body = "Delete \"" + p.st.confirmName + "\"?\nIts demos stay in the list, just outside any folder. No demo files are touched.";
		ok = "DELETE";
	}
	BeginOverlay("##demosconfirm", MakeRect(0, 0, p.w, p.h), Rgba(0, 0, 0, 150));
	ImDrawList* draw = ImGui::GetWindowDrawList();
	ImFont* titleFont = p.F(FontRole::CardTitle);
	ImFont* bodyFont = p.F(FontRole::Body);
	ImFont* button = p.F(FontRole::Button);
	const float w = p.Px(480), pad = p.Px(22);
	const float bodyH = bodyFont->CalcTextSizeA(bodyFont->FontSize, FLT_MAX, w - 2 * pad, body.c_str()).y;
	const float h = pad + titleFont->FontSize + p.Px(14) + bodyH + p.Px(22) + p.Px(34) + pad;
	const Rect box = MakeRect((p.w - w) * 0.5f, (p.h - h) * 0.5f, (p.w + w) * 0.5f, (p.h + h) * 0.5f);
	Box(draw, box, Rgba(10, 10, 10), Theme::Border);
	draw->AddRectFilled(ImVec2(box.x0, box.y0), ImVec2(box.x0 + p.Px(3), box.y1), ToU32(Theme::Accent));
	Text(draw, titleFont, box.x0 + pad, box.y0 + pad, Theme::Accent, title);
	Text(draw, bodyFont, box.x0 + pad, box.y0 + pad + titleFont->FontSize + p.Px(14), Theme::Text, body, w - 2 * pad);
	const float by = box.y1 - pad - p.Px(34);
	const float okW = CardButtonWidth(ok.c_str(), button, s) + p.Px(20), cancelW = CardButtonWidth("CANCEL", button, s) + p.Px(20);
	const Rect okRect = MakeRect(box.x1 - pad - okW, by, box.x1 - pad, by + p.Px(34));
	const Rect cancelRect = MakeRect(okRect.x0 - p.Px(8) - cancelW, by, okRect.x0 - p.Px(8), by + p.Px(34));
	if (CardButton("##confirmcancel", "CANCEL", cancelRect, ButtonKind::Normal, true, button)) p.st.confirm = Confirm::None;
	if (CardButton("##confirmok", ok.c_str(), okRect, ButtonKind::Danger, true, button)) {
		if (p.st.confirm == Confirm::ClearAll) p.Send("clearall");
		else if (p.st.confirm == Confirm::Remove) p.Send("hide " + std::to_string(p.st.confirmIndex));
		else p.Send("org delfolder " + std::to_string(p.st.confirmIndex));
		p.st.confirm = Confirm::None;
	}
	// A click beside the buttons does nothing; Cancel or Esc closes it. Submitted after the buttons, so
	// they keep their own clicks.
	ImGui::SetCursorScreenPos(ImVec2(0, 0));
	ImGui::InvisibleButton("##modalcatch", ImVec2(p.w, p.h));
	ImGui::End();
}

// The picked-up tile follows the pointer as CS2's ghost card: map badge and cropped name.
void DragGhost(Page& p) {
	if (p.st.dragKey.empty()) return;
	const DemoRow* d = Find(p.m, p.st.dragKey);
	if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || !d) {
		if (d && p.dropGroup != -2 && p.dropGroup != d->group && (p.dropGroup >= 0 || FindGroup(p.m, d->group)))
			p.Send("org assign " + std::to_string(p.dropGroup) + " " + d->key);
		p.st.dragKey.clear();
		return;
	}
	ImDrawList* draw = ImGui::GetForegroundDrawList();
	const ImVec2 m = ImGui::GetMousePos();
	const Rect r = MakeRect(m.x + p.Px(18), m.y + p.Px(20), m.x + p.Px(18 + 220), m.y + p.Px(20 + 40));
	draw->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), ToU32(Rgba(14, 14, 14, 240)));
	draw->AddRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), ToU32(Theme::Accent));
	const Rect badge = MakeRect(r.x0 + p.Px(9), r.y0 + p.Px(9), r.x0 + p.Px(31), r.y0 + p.Px(31));
	ImFont* caption = p.F(FontRole::Caption);
	Box(draw, badge, Rgba(255, 255, 255, 14), Theme::Border);
	const std::string prefix = MapPrefix(d->map);
	Centred(draw, caption, badge, Theme::Accent, prefix.empty() ? "?" : prefix);
	ImFont* row = p.F(FontRole::RowLabel);
	Text(draw, row, badge.x1 + p.Px(9), (r.y0 + r.y1 - row->FontSize) * 0.5f, Theme::Text, Fit(row, CropName(DemoName(*d), 14, 6), r.x1 - p.Px(11) - badge.x1 - p.Px(9)));
}

void Tooltip(Page& p) {
	DemosBrowserState& st = p.st;
	const std::string tip = p.tip ? p.tip : "";
	if (tip != st.tipText) {
		st.tipText = tip;
		st.tipSince = tip.empty() ? -1 : ImGui::GetTime();
	}
	st.tipAnchor = p.tipRect;
	if (st.tipText.empty() || st.tipSince < 0 || st.menu != DemosBrowserState::Menu::None || !st.dragKey.empty()) return;
	if (ImGui::GetTime() - st.tipSince < 0.18 || ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
	ImFont* font = p.F(FontRole::Note);
	const float maxW = p.Px(300), padX = p.Px(12), padTop = p.Px(9), padBottom = p.Px(10);
	const ImVec2 size = font->CalcTextSizeA(font->FontSize, FLT_MAX, maxW - 2 * padX, st.tipText.c_str());
	const float w = size.x + 2 * padX, h = padTop + size.y + padBottom;
	const Rect& a = st.tipAnchor;
	float x = (a.x0 + a.x1 - w) * 0.5f, y = a.y1 + p.Px(8);
	if (y + h > p.h - 2) y = a.y0 - p.Px(8) - h;
	if (x + w > p.w - 2) x = p.w - 2 - w;
	if (x < 2) x = 2;
	ImDrawList* draw = ImGui::GetForegroundDrawList();
	draw->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), ToU32(Theme::DockBg));
	draw->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), ToU32(Theme::Border));
	draw->AddText(font, font->FontSize, ImVec2(x + padX, y + padTop), ToU32(Theme::Text), st.tipText.c_str(), nullptr, maxW - 2 * padX);
}

} // namespace

DemosBrowserFrame DrawDemosBrowser(DemosBrowserState& st, const FontSet& fonts, float clientW, float clientH, const DemosBrowserModel& model) {
	DemosBrowserFrame frame;
	const float s = clientH / 1080.0f;
	Page p{ st, fonts, model, frame, s, clientW, clientH };

	// The selection follows the list: a demo that left it (removed, renamed) gives way to the newest.
	if (!Find(model, st.selected)) {
		st.selected = model.demos.empty() ? std::string() : model.demos.front().key;
		st.renaming = false;
	}
	if (st.renaming && st.renameKey != st.selected) st.renaming = false;
	if (st.renamingGroup >= 0 && !FindGroup(model, st.renamingGroup)) st.renamingGroup = -1;

	BeginPanel("##demospage", MakeRect(0, 0, clientW, clientH), Rgba(0, 0, 0, 0));
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(ImVec2(0, 0), ImVec2(clientW, clientH), ToU32(kPage));
	Header(p);
	const float top = p.Px(kHeaderH) + p.Px(kGap), bottom = clientH - p.Px(kMargin);
	const Rect rail = MakeRect(p.Px(kMargin), top, p.Px(kMargin) + p.Px(kRailW), bottom);
	Rail(p, rail);
	Details(p, MakeRect(rail.x1 + p.Px(kGap), top, clientW - p.Px(kMargin), bottom));
	ImGui::End();

	SearchOverlay(p);
	Menus(p);
	ConfirmDialog(p);
	DragGhost(p);
	Tooltip(p);
	return frame;
}

bool DemosBrowserEscape(DemosBrowserState& st) {
	if (st.confirm != DemosBrowserState::Confirm::None) {
		st.confirm = DemosBrowserState::Confirm::None;
		return true;
	}
	if (st.menu != DemosBrowserState::Menu::None) {
		st.menu = DemosBrowserState::Menu::None;
		return true;
	}
	if (st.renaming) {
		st.renaming = false;
		return true;
	}
	if (st.renamingGroup >= 0) {
		st.renamingGroup = -1;
		return true;
	}
	if (st.newFolder) {
		st.newFolder = false;
		st.newFolderText[0] = 0;
		return true;
	}
	if (st.searchOpen) {
		st.searchOpen = false;
		st.search[0] = 0;
		return true;
	}
	if (!st.dragKey.empty()) {
		st.dragKey.clear();
		return true;
	}
	return false;
}

Rect DrawDemosMenuButton(const FontSet& fonts, float clientW, float clientH, bool hovered) {
	const float s = clientH / 1080.0f;
	ImFont* caption = fonts.Get(FontRole::Caption);
	ImFont* title = fonts.Get(FontRole::CardTitle);
	const float padH = 18 * s, padV = 10 * s;
	const float w = std::max(SpacedTextWidth(title, "DEMOS", 2.0f * s), SpacedTextWidth(caption, "SOURCE:MVM", 1.5f * s)) + 2 * padH;
	const float h = caption->FontSize + 4 * s + title->FontSize + 2 * padV;
	const Rect r = MakeRect(clientW - 40 * s - w, 40 * s, clientW - 40 * s, 40 * s + h);
	ImDrawList* draw = ImGui::GetBackgroundDrawList();
	Box(draw, r, hovered ? Rgba(30, 20, 0, 220) : Rgba(0, 0, 0, 180), hovered ? Theme::Accent : Theme::Border);
	draw->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x0 + 3 * s, r.y1), ToU32(Theme::Accent));
	DrawSpacedText(draw, caption, ImVec2(r.x0 + padH, r.y0 + padV), ToU32(Theme::TextDim), "SOURCE:MVM", 1.5f * s);
	DrawSpacedText(draw, title, ImVec2(r.x0 + padH, r.y0 + padV + caption->FontSize + 4 * s), ToU32(Theme::Accent), "DEMOS", 2.0f * s);
	return r;
}

} // namespace Ui
} // namespace SourceFilmmaker
