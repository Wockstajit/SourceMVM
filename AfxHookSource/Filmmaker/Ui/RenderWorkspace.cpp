#include "RenderWorkspace.h"

#include "InspectorCards.h"
#include "UiWidgets.h"
#include "../SourceRenderPassTable.h"
#include "../../../AfxHookSource2/Filmmaker/Render/RenderPassPlan.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace SourceFilmmaker {
namespace Ui {

namespace {

constexpr Color kError = Rgba(255, 110, 92);   // CS2's RENDER_ERROR_TEXT, in CS:S's warmer red
constexpr Color kReady = Theme::Loop;          // "Ready to render"
constexpr Color kRunning = Rgba(78, 159, 255); // CS2's running blue

std::string Format(const char* fmt, ...) {
	char buf[512];
	va_list va;
	va_start(va, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va);
	va_end(va);
	return buf;
}

// "0:03.00"
std::string Duration(double seconds) {
	if (!(seconds > 0)) seconds = 0;
	const int m = (int)(seconds / 60);
	return Format("%d:%05.2f", m, seconds - 60.0 * m);
}

const char* const kTabs[3] = { "RANGE & CAMERA", "PASSES", "OUTPUT" };
// `render tab` keeps CS2's four slots: RANGE & CAMERA answers 0 and 1, PASSES is 2, OUTPUT 3.
const int kTabSlots[3] = { 0, 2, 3 };

const char* const kRangeModes[5] = { "Full Demo", "Round", "Camera Path", "Segment", "Custom In/Out" };
const char* const kRangeTokens[5] = { "full", "round", "path", "segment", "custom" };
const char* const kRangeHelp[5] = {
	"Render the entire demo from its first to last tick.",
	"Render only the round containing the playhead.",
	"Render from the first camera key through the last camera key.",
	"Render from the selected camera key to the next key.",
	"Render between the In and Out flags on the timeline. The Out frame is excluded. Change the span from the playhead bar below.",
};

const char* const kCameras[6] = { "Current Viewer", "First-Person POV", "Third-Person", "Free Camera", "Camera Path", "Follow Camera" };
const char* const kCameraTokens[6] = { "current", "pov", "thirdperson", "free", "path", "follow" };
const char* const kCameraSubs[6] = { "What's on screen now", "In-eye on the spectated player", "Chase cam on the spectated player",
	"Free fly (WASD + mouse)", "Plays the path", "Tracks the follow target" };

const char* const kExtensions[4] = { "Fail (safe)", "Hold first", "Hold last", "Clamp range" };
const char* const kExtensionTokens[4] = { "fail", "holdfirst", "holdlast", "clamp" };

// The format grid in CS2's order, with each format's RenderOutputFormat index.
struct FormatCell {
	const char* token;
	const char* label;
	const char* kind;
	int format;
};
const FormatCell kFormats[8] = { { "mp4", "MP4", "VIDEO", 0 }, { "avi", "AVI", "VIDEO", 1 }, { "gif", "GIF", "VIDEO", 2 },
	{ "exr", "EXR", "SEQ", 7 }, { "tga", "TGA", "SEQ", 5 }, { "bmp", "BMP", "SEQ", 6 }, { "png", "PNG", "SEQ", 3 },
	{ "tiff", "TIFF", "SEQ", 4 } };

const int kFpsChoices[5] = { 24, 30, 50, 60, 120 };
const char* const kFpsCaptions[6] = { "24", "30", "50", "60", "120", "Custom" };
const int kCrfChoices[4] = { 14, 18, 23, 28 };
const char* const kQualityCaptions[5] = { "Highest", "High", "Medium", "Small file", "Bitrate" };
const char* const kEncoders[3] = { "Auto", "CPU", "GPU" };
const char* const kEncoderTokens[3] = { "auto", "cpu", "gpu" };
const char* const kSpeeds[3] = { "Best", "Balanced", "Fastest" };
const char* const kSpeedTokens[3] = { "quality", "balanced", "fast" };

const char* const kHudTitles[2] = { "No HUD", "Game HUD" };
const char* const kHudSubs[2] = { "Clean plate: nothing but the world.", "Health, ammo, radar and the kill feed, as the game draws them." };
const char* const kHudTokens[2] = { "none", "game" };

// CS2's rsSection head: an accent bar and the caption.
void Section(CardColumn& c, const char* title, const char* aside = nullptr) {
	c.y += 16 * c.s;
	ImDrawList* draw = ImGui::GetWindowDrawList();
	ImFont* font = c.Font(FontRole::Label);
	draw->AddRectFilled(ImVec2(c.x0, c.y), ImVec2(c.x0 + 3 * c.s, c.y + font->FontSize), ToU32(Theme::Accent));
	DrawSpacedText(draw, font, ImVec2(c.x0 + 11 * c.s, c.y), ToU32(Theme::Text), title, 1.2f * c.s);
	if (aside) {
		ImFont* small = c.Font(FontRole::Note);
		const float w = SpacedTextWidth(small, aside, 0.6f * c.s);
		DrawSpacedText(draw, small, ImVec2(c.x0 + 11 * c.s + SpacedTextWidth(font, title, 1.2f * c.s) + 12 * c.s,
			c.y + font->FontSize - small->FontSize), ToU32(Theme::TextDim), aside, 0.6f * c.s);
		(void)w;
	}
	c.y += font->FontSize + 10 * c.s;
}

// Text wrapped to the column; moves c.y below it.
void Wrapped(CardColumn& c, ImFont* font, Color color, const std::string& text, float indent = 0) {
	if (text.empty()) return;
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const float width = c.W() - indent;
	const ImVec2 size = font->CalcTextSizeA(font->FontSize, FLT_MAX, width, text.c_str());
	draw->AddText(font, font->FontSize, ImVec2(c.x0 + indent, c.y), ToU32(color), text.c_str(), nullptr, width);
	c.y += size.y + 4 * c.s;
}

// A small square check (selected) as CS2's tiles carry.
void CheckBox(ImDrawList* draw, float x, float y, float size, bool on) {
	draw->AddRect(ImVec2(x, y), ImVec2(x + size, y + size), ToU32(on ? Theme::Text : Theme::TextDim));
	if (on) draw->AddRectFilled(ImVec2(x + size * 0.3f, y + size * 0.3f), ImVec2(x + size * 0.7f, y + size * 0.7f), ToU32(Theme::Text));
}

// CS2's pick-one tiles in a grid: the check, the title, and a line under it (in red when the tile
// cannot be used now; it then takes no click). Returns the clicked tile, -1 for none.
int Tiles(CardColumn& c, const char* id, const char* const* titles, const std::string* subs, const bool* blocked, int count,
	int columns, int selected) {
	const float s = c.s, gap = 6 * s;
	ImFont* title = c.Font(FontRole::Button);
	ImFont* sub = c.Font(FontRole::Note);
	const float w = (c.W() - gap * (columns - 1)) / columns;
	// One height for every tile, the tallest wrapped line deciding.
	float subH = sub->FontSize;
	for (int i = 0; i < count; ++i)
		subH = (std::max)(subH, sub->CalcTextSizeA(sub->FontSize, FLT_MAX, w - 34 * s, subs[i].c_str()).y);
	const float h = 12 * s + title->FontSize + 4 * s + subH + 12 * s;
	int clicked = -1;
	ImDrawList* draw = ImGui::GetWindowDrawList();
	for (int i = 0; i < count; ++i) {
		const int col = i % columns, row = i / columns;
		const Rect r = MakeRect(c.x0 + col * (w + gap), c.y + row * (h + gap), c.x0 + col * (w + gap) + w, c.y + row * (h + gap) + h);
		ImGui::SetCursorScreenPos(ImVec2(r.x0, r.y0));
		ImGui::PushID(id);
		ImGui::PushID(i);
		const bool hit = ImGui::InvisibleButton("##tile", ImVec2(r.W(), r.H()));
		const bool hot = ImGui::IsItemHovered();
		ImGui::PopID();
		ImGui::PopID();
		const bool on = i == selected;
		const bool live = !(blocked && blocked[i]);
		draw->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1),
			ToU32(on ? Rgba(255, 176, 0, 34) : (hot && live ? Rgba(255, 255, 255, 18) : kCardTileBg)));
		if (on) draw->AddRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), ToU32(Theme::Accent));
		CheckBox(draw, r.x0 + 12 * s, r.y0 + 12 * s + (title->FontSize - 12 * s) * 0.5f, 12 * s, on);
		draw->AddText(title, title->FontSize, ImVec2(r.x0 + 32 * s, r.y0 + 12 * s), ToU32(live ? Theme::Text : Theme::TextDim), titles[i]);
		draw->AddText(sub, sub->FontSize, ImVec2(r.x0 + 32 * s, r.y0 + 16 * s + title->FontSize), ToU32(live ? Theme::TextDim : kError),
			subs[i].c_str(), nullptr, w - 34 * s);
		if (hit && live) clicked = i;
	}
	c.y += ((count + columns - 1) / columns) * (h + gap) - gap + 4 * s;
	return clicked;
}

// A labelled line of the JOB column: the caption, and its value under it.
void JobRow(CardColumn& c, const char* caption, const std::string& value, Color color) {
	ImDrawList* draw = ImGui::GetWindowDrawList();
	ImFont* cap = c.Font(FontRole::Caption);
	DrawSpacedText(draw, cap, ImVec2(c.x0, c.y), ToU32(Theme::TextDim), caption, 1.0f * c.s);
	c.y += cap->FontSize + 4 * c.s;
	Wrapped(c, c.Font(FontRole::BodyBold), color, value);
	c.y += 8 * c.s;
}

// An input field over r: Enter (or leaving it) hands back its number; false otherwise.
bool NumberField(const char* id, char* text, size_t size, const Rect& r, int& value) {
	ImGui::SetCursorScreenPos(ImVec2(r.x0, r.y0));
	ImGui::PushItemWidth(r.W());
	const bool done = ImGui::InputText(id, text, size, ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);
	const bool left = ImGui::IsItemDeactivatedAfterEdit();
	ImGui::PopItemWidth();
	if (!done && !left) return false;
	value = std::atoi(text);
	return true;
}

std::string PassSummary(const RenderWorkspaceModel& m) {
    std::string text;
    const char* groups[5] = {"Everything", "No gun", "World", "Players", "Gun"};
    for (int t = 0; t < kPassTypes; ++t) {
        std::string row;
        for (int g = 0; g < kPassGroups; ++g) if (m.selectedPass[t][g]) {
            if (!row.empty()) row += ", ";
            row += groups[g];
        }
        if (row.empty()) continue;
        if (!text.empty()) text += "\n";
        text += Filmmaker::PassTypeLabel((Filmmaker::PassType)t);
        text += ": " + row;
    }
    return text.empty() ? "No passes selected" : text;
}

std::string Plural(int n, const char* one, const char* many) { return Format("%d %s", n, n == 1 ? one : many); }

// ---- the JOB column -------------------------------------------------------------------------------

void JobColumn(RenderWorkspaceState&, const RenderWorkspaceModel& m, const FontSet& fonts, float s, const Rect& col,
	std::vector<std::string>& out) {
	BeginPanel("##renderjob", col, Rgba(15, 17, 20, 245));
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const float pad = Metrics::col.pad * s;
	ImFont* title = fonts.Get(FontRole::Title);
	DrawSpacedText(draw, title, ImVec2(col.x0 + pad, col.y0 + pad), ToU32(Theme::Accent), "JOB", 1.6f * s);
	const float ruleY = col.y0 + pad + title->FontSize + 14 * s;
	draw->AddLine(ImVec2(col.x0, ruleY), ImVec2(col.x1, ruleY), ToU32(Theme::Border), 1.0f);

	// START RENDER (or CANCEL RENDER) pinned at the bottom.
	ImFont* button = fonts.Get(FontRole::Button);
	const float btnH = button->FontSize + 30 * s;
	const Rect btn = MakeRect(col.x0 + pad, col.y1 - pad - btnH, col.x1 - pad, col.y1 - pad);
	if (m.running) {
		if (CardButton("##rendercancel", "CANCEL RENDER", btn, ButtonKind::Danger, true, button)) out.push_back("mirv_filmmaker render cancel");
	} else if (CardButton("##renderstart", "START RENDER", btn, ButtonKind::Primary, m.fatalCount == 0 && m.demo, button)) {
		out.push_back("mirv_filmmaker render start");
	}

	float width = col.W() - 2 * pad;
	const Rect body = MakeRect(col.x0 + pad, ruleY + 14 * s, col.x1 - pad, btn.y0 - 14 * s);
	CardColumn c;
	c.fonts = &fonts;
	c.s = s;
	c.x0 = body.x0;
	c.y = BeginCardScroll("##renderjobbody", body, width);
	c.x1 = body.x0 + width;

	const int ticks = m.rangeOk ? m.out - m.in : 0;
	JobRow(c, "RANGE", ticks > 0 ? Format("%d  \xE2\x86\x92  %d", m.in, m.out - 1) : "not set", ticks > 0 ? Theme::Text : kError);
	JobRow(c, "LENGTH", ticks > 0 ? Duration(ticks / m.tickRate) + "   \xC2\xB7   " + Plural(ticks, "tick", "ticks") : "\xE2\x80\x94", Theme::Text);
	JobRow(c, "FRAMES", m.frames > 0 ? Format("%d   \xC2\xB7   %d fps", m.frames, m.fps) : "\xE2\x80\x94", Theme::Text);
	JobRow(c, "CAMERA", kCameras[m.camera >= 0 && m.camera < 6 ? m.camera : 0], Theme::Text);
	JobRow(c, "PASSES", PassSummary(m), Theme::Text);
	JobRow(c, "CAPTURE", std::string(m.hud == 0 ? "No HUD" : "Game HUD") + "   \xC2\xB7   "
		+ (m.cameraData ? "camera metadata" : "no camera metadata") + (m.blood ? "" : "   \xC2\xB7   no blood"), Theme::Text);
	JobRow(c, "OUTPUT", m.formatName + Format("   \xC2\xB7   %d\xC3\x97%d", m.width, m.height), m.formatOk[m.format & 7] ? Theme::Text : kError);
	JobRow(c, "FOLDER", m.folder.empty() ? "not chosen" : m.folder, m.folder.empty() ? kError : Theme::Text);

	draw = ImGui::GetWindowDrawList();
	draw->AddLine(ImVec2(c.x0, c.y), ImVec2(c.x1, c.y), ToU32(Rgba(255, 255, 255, 30)), 1.0f);
	c.y += 14 * s;

	// What happens if START RENDER is pressed now, or what the job is doing.
	ImFont* status = fonts.Get(FontRole::Title);
	ImFont* note = fonts.Get(FontRole::Note);
	std::string head, line;
	Color headColor = Theme::Text, lineColor = Theme::TextDim;
	if (m.running) {
		head = Format("RENDERING  %d%%", m.percent);
		headColor = kRunning;
		line = Format("%s   \xC2\xB7   frame %d / %d   \xC2\xB7   tick %d", m.phase.c_str(), m.framesDone, m.framesTotal, m.curTick);
	} else if (m.sessionState == 12 && !m.lastError.empty()) {
		head = "Render FAILED";
		headColor = kError;
		line = m.lastError;
		lineColor = kError;
	} else if (!m.demo) {
		head = "No demo playing";
		headColor = kError;
		line = "Play a demo to render from it.";
		lineColor = kError;
	} else if (m.fatalCount > 0) {
		head = Format("%d problem%s \xE2\x80\x94 cannot start", m.fatalCount, m.fatalCount == 1 ? "" : "s");
		headColor = kError;
		line = !m.rangeOk ? "Choose a range in RANGE, or set In and Out at the playhead."
			: (m.folder.empty() ? "Choose an output folder in OUTPUT." : m.firstFatal);
		if (!m.rangeOk && m.folder.empty()) line += "\nChoose an output folder in OUTPUT.";
		lineColor = kError;
	} else {
		head = "Ready to render";
		headColor = kReady;
		if (m.sessionState == 11 && m.framesDone > 0) line = Format("Last render: %d frame(s) \xE2\x86\x92 %s.   ", m.framesDone, m.takeDir.c_str());
		if (m.warnCount > 0) line += Format("%d warning(s): see mirv_filmmaker render validate", m.warnCount);
	}
	draw->AddText(status, status->FontSize, ImVec2(c.x0, c.y), ToU32(headColor), head.c_str());
	c.y += status->FontSize + 6 * s;
	Wrapped(c, note, lineColor, line);

	// PREVIOUS RENDERS: Use starts the next job from that take's settings.
	if (!m.takes.empty()) {
		c.y += 10 * s;
		ImFont* cap = fonts.Get(FontRole::Caption);
		draw = ImGui::GetWindowDrawList();
		DrawSpacedText(draw, cap, ImVec2(c.x0, c.y), ToU32(Theme::TextDim), "PREVIOUS RENDERS", 1.0f * s);
		c.y += cap->FontSize + 8 * s;
		ImFont* when = fonts.Get(FontRole::Button);
		const int shown = (std::min)((int)m.takes.size(), 3);
		for (int i = 0; i < shown; ++i) {
			const RenderTake& t = m.takes[i];
			const float useW = CardButtonWidth("Use", button, s) + 14 * s;
			const float rowTop = c.y;
			const std::string first = t.when + (t.status == "completed" || t.status.empty() ? "" : "   \xC2\xB7   " + t.status);
			draw->AddText(when, when->FontSize, ImVec2(c.x0, c.y), ToU32(Theme::Text), first.c_str());
			c.y += when->FontSize + 3 * s;
			CardColumn detail = c;
			detail.x1 = c.x1 - useW - 8 * s;
			Wrapped(detail, note, Theme::TextDim, t.detail);
			c.y = detail.y;
			const float h = (std::max)(c.y - rowTop, button->FontSize + 14 * s);
			ImGui::PushID(i);
			if (CardButton("##use", "Use", MakeRect(c.x1 - useW, rowTop, c.x1, rowTop + (std::min)(h, button->FontSize + 16 * s)),
				ButtonKind::Normal, !m.running, button))
				out.push_back(Format("mirv_filmmaker render import %d", i));
			ImGui::PopID();
			c.y = rowTop + h + 8 * s;
		}
	}
	EndCardScroll(c.x0, c.y + 10 * s);
	ImGui::End();
}

// ---- the RENDER column ----------------------------------------------------------------------------

void RangeAndCamera(RenderWorkspaceState&, const RenderWorkspaceModel& m, CardColumn& c, std::vector<std::string>& out) {
	const float s = c.s;
	Section(c, "RANGE");
	const int mode = Segmented(c, "##rangemode", kRangeModes, 5, m.rangeMode, m.modeLive);
	if (mode >= 0) out.push_back(std::string("mirv_filmmaker render range ") + kRangeTokens[mode]);
    if (m.rangeOk) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImFont* cap = c.Font(FontRole::Caption);
        ImFont* value = c.Font(FontRole::SpanValue);
        ImFont* button = c.Font(FontRole::Button);
        const float top = c.y + 8 * s, pad = 14 * s;
        const float left = c.x0 + pad, right = c.x1 - pad;
        const float innerW = right - left;
        const float statW = innerW * 0.22f, divider = right - statW * 2 - 14 * s;
        const float endpointW = (divider - left - 34 * s) / 2;
        const float x[4] = { left, left + endpointW + 34 * s, divider + 14 * s, right - statW };
        const int ticks = m.out - m.in;
        const std::string caps[4] = { "IN", "LAST FRAME", "LENGTH", Format("TICKS @ %.4g", m.tickRate) };
        const std::string values[4] = { Format("%d", m.in), Format("%d", m.out - 1), Duration(ticks / m.tickRate), Format("%d", ticks) };
        const float labelY = top + pad, valueY = labelY + cap->FontSize + 4 * s;
        const float nudgeY = valueY + value->FontSize + 6 * s, bw = 30 * s, bh = button->FontSize + 8 * s;
        const float barY = nudgeY + bh + 10 * s, barH = 18 * s;
        const float bottom = barY + barH + cap->FontSize + 4 * s + pad;
        draw->AddRectFilled(ImVec2(c.x0, top), ImVec2(c.x1, bottom), ToU32(kCardTileBg));
        draw->AddRect(ImVec2(c.x0, top), ImVec2(c.x1, bottom), ToU32(Rgba(255,255,255,20)));
        for (int i = 0; i < 4; ++i) {
            const float cellW = i < 2 ? endpointW : statW;
            const float cw = SpacedTextWidth(cap, caps[i].c_str(), 1.0f * s);
            const float vw = value->CalcTextSizeA(value->FontSize, FLT_MAX, 0, values[i].c_str()).x;
            const float align = i == 1 ? 0.5f : (i >= 2 ? 1.0f : 0.0f);
            DrawSpacedText(draw, cap, ImVec2(x[i] + (cellW-cw)*align, labelY), ToU32(Theme::TextDim), caps[i].c_str(), 1.0f*s);
            draw->AddText(value, value->FontSize, ImVec2(x[i] + (cellW-vw)*align, valueY), ToU32(Theme::Text), values[i].c_str());
        }
        const float arrowX = left + endpointW + 17*s, arrowY = valueY + value->FontSize*0.6f;
        draw->AddLine(ImVec2(arrowX-7*s,arrowY),ImVec2(arrowX+7*s,arrowY),ToU32(Theme::TextDim));
        draw->AddLine(ImVec2(arrowX+3*s,arrowY-4*s),ImVec2(arrowX+7*s,arrowY),ToU32(Theme::TextDim));
        draw->AddLine(ImVec2(arrowX+3*s,arrowY+4*s),ImVec2(arrowX+7*s,arrowY),ToU32(Theme::TextDim));
        draw->AddLine(ImVec2(divider,labelY),ImVec2(divider,nudgeY+bh),ToU32(Theme::Border));
        for (int end = 0; end < 2; ++end) for (int d = 0; d < 2; ++d) {
            const int delta = d == 0 ? -1 : 1;
            int a = m.in, b = m.out;
            if (end == 0) a += delta; else b += delta;
            const bool ok = !m.running && a >= m.demoStart && b - 1 <= m.demoEnd && b > a;
            const float nx = x[end] + (end ? (endpointW - 2*bw - 4*s)*0.5f : 0) + d*(bw+4*s);
            ImGui::PushID(end*2+d);
            if (CardButton("##nudge",delta<0?"-1":"+1",MakeRect(nx,nudgeY,nx+bw,nudgeY+bh),ButtonKind::Normal,ok,button))
                out.push_back(Format("mirv_filmmaker render range custom %d %d",a,b));
            ImGui::PopID();
        }
        const float span = (float)(std::max)(1,m.demoEnd-m.demoStart);
        auto at = [&](int tick) { return left + innerW*(std::max)(0.0f,(std::min)(1.0f,(tick-m.demoStart)/span)); };
        draw->AddRectFilled(ImVec2(left,barY+4*s),ImVec2(right,barY+14*s),ToU32(Rgba(255,255,255,15)));
        draw->AddRectFilled(ImVec2(at(m.in),barY+4*s),ImVec2((std::min)(right,(std::max)(at(m.out),at(m.in)+3*s)),barY+14*s),ToU32(Theme::RenderRange));
        const float head = (std::min)(right-2*s,at(m.playhead));
        draw->AddRectFilled(ImVec2(head,barY),ImVec2(head+2*s,barY+barH),ToU32(Theme::Accent));
        draw->AddText(cap,cap->FontSize,ImVec2(left,barY+barH+3*s),ToU32(Theme::TextDim),"0:00");
        const std::string duration = Duration((m.demoEnd-m.demoStart)/m.tickRate);
        const float dw = cap->CalcTextSizeA(cap->FontSize,FLT_MAX,0,duration.c_str()).x;
        draw->AddText(cap,cap->FontSize,ImVec2(right-dw,barY+barH+3*s),ToU32(Theme::TextDim),duration.c_str());
        c.y = bottom + 8*s;
    } else {
        const char* why = m.rangeMode == 4 ? "No In / Out set. Move the playhead and use Set In and Set Out below."
            : (m.rangeMode == 2 ? "Camera Path needs 2+ camera keys. Place them with K."
            : (m.rangeMode == 3 ? "Segment needs a camera key selected that has a NEXT key."
            : (m.rangeMode == 1 ? "No round contains the playhead." : "The demo's length is not known.")));
        const float top = c.y + 8*s;
        CardColumn empty = c; empty.x0 += 14*s; empty.x1 -= 14*s; empty.y = top+14*s;
        Wrapped(empty,c.Font(FontRole::Note),kError,why);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRect(ImVec2(c.x0,top),ImVec2(c.x1,empty.y+10*s),ToU32(kError));
        c.y = empty.y+18*s;
    }
    Note(c,kRangeHelp[m.rangeMode >= 0 && m.rangeMode < 5 ? m.rangeMode : 4]);

	Section(c, "CAMERA");
	std::string subs[6];
	bool blocked[6];
	for (int i = 0; i < 6; ++i) {
		blocked[i] = !m.cameraBlock[i].empty();
		subs[i] = blocked[i] ? m.cameraBlock[i] : kCameraSubs[i];
	}
	const int cam = Tiles(c, "##camera", kCameras, subs, blocked, 6, 3, m.camera);
	if (cam >= 0) out.push_back(std::string("mirv_filmmaker render camera ") + kCameraTokens[cam]);
	if (m.camera == 4) {
		const int ext = ModeRow(c, "##extension", "If the path is shorter than the range", kExtensions, 4, m.extension, true);
		if (ext >= 0) out.push_back(std::string("mirv_filmmaker render extension ") + kExtensionTokens[ext]);
		Note(c, "Default is Fail. Silent clamping renders the wrong clip without telling you.");
	}
}

void Passes(RenderWorkspaceState& st, const RenderWorkspaceModel& m, CardColumn& c, std::vector<std::string>& out) {
    const float s = c.s;
    Section(c, "PASSES");
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImFont* caption = c.Font(FontRole::Caption);
    ImFont* label = c.Font(FontRole::Button);
    const bool viewing = m.previewType != 0 || m.previewGroup != 0;
    const float viewH = label->FontSize + 14*s, viewW = 116*s;
    const Rect view = MakeRect(c.x1-viewW,c.y-caption->FontSize-18*s,c.x1,c.y-caption->FontSize-18*s+viewH);
    if (CardButton("##passview",viewing ? "Viewing live" : "View",view,viewing ? ButtonKind::Primary : ButtonKind::Normal,!m.running,label)) {
        if (viewing) out.push_back("mirv_filmmaker render pass preview off");
        else {
            bool picked=false;
            for (int t=0;t<kPassTypes && !picked;++t) for (int g=0;g<kPassGroups && !picked;++g)
                if (m.selectedPass[t][g] && (t!=0 || g!=0)) {
                    out.push_back(std::string("mirv_filmmaker render pass preview ")+Filmmaker::PassTypeToken((Filmmaker::PassType)t)+" "+Filmmaker::VisibleElementsToken((Filmmaker::VisibleElements)g)); picked=true;
                }
        }
    }
    c.y += 8*s;
    if (viewing) {
        Note(c,"LIVE IN CAMERA",Theme::Accent);
        float x=c.x0;
        for(int t=0;t<kPassTypes;++t) for(int g=0;g<kPassGroups;++g) if(m.selectedPass[t][g] && (t!=0 || g!=0)) {
            const char* groups[5]={"All","No gun","World","Players","Gun"};
            const std::string text=std::string(Filmmaker::PassTypeLabel((Filmmaker::PassType)t))+" / "+groups[g];
            const float w=CardButtonWidth(text.c_str(),label,s)+12*s;
            if(x+w>c.x1 && x>c.x0) {x=c.x0;c.y+=viewH+5*s;}
            if(CardButton(Format("##view%d_%d",t,g).c_str(),text.c_str(),MakeRect(x,c.y,x+w,c.y+viewH),
                m.previewType==t && m.previewGroup==g ? ButtonKind::Primary : ButtonKind::Normal,!m.running,label))
                out.push_back(std::string("mirv_filmmaker render pass preview ")+Filmmaker::PassTypeToken((Filmmaker::PassType)t)+" "+Filmmaker::VisibleElementsToken((Filmmaker::VisibleElements)g));
            x+=w+5*s;
        }
        c.y+=viewH+12*s;
    }
    const float rowLabel = 158 * s, gap = 5 * s, colW = (c.W() - rowLabel - gap * 3) / 4;
    const float rowH = label->FontSize + 20 * s;
    for (int t = 0; t < kPassTypes; ++t) {
        const char* title = Filmmaker::PassTypeLabel((Filmmaker::PassType)t);
        const float tw = caption->CalcTextSizeA(caption->FontSize, FLT_MAX, 0, title).x;
        draw->AddText(caption, caption->FontSize, ImVec2(c.x0 + rowLabel + t * (colW + gap) + (colW - tw) / 2, c.y), ToU32(Theme::TextDim), title);
    }
    c.y += caption->FontSize + 8 * s;
    const char* const rows[5] = { "Everything", "World + players", "World only", "Players only", "Gun only" };
    for (int g = 0; g < kPassGroups; ++g) {
        draw->AddText(label, label->FontSize, ImVec2(c.x0, c.y + (rowH - label->FontSize) / 2), ToU32(Theme::Text), rows[g]);
        for (int t = 0; t < kPassTypes; ++t) {
            const PassRoute route = RoutePass(t, g);
            const Rect r = MakeRect(c.x0 + rowLabel + t * (colW + gap), c.y,
                c.x0 + rowLabel + t * (colW + gap) + colW, c.y + rowH);
            if (route.way == PassWay::NotAPass) continue;
            const bool available = route.way == PassWay::Main || route.way == PassWay::Extra;
            const std::string id = Format("##pass_%d_%d", t, g);
            if (CardButton(id.c_str(), available ? "" : "-", r,
                m.selectedPass[t][g] ? ButtonKind::Primary : ButtonKind::Normal, available && !m.running, label))
                out.push_back(std::string("mirv_filmmaker render pass toggle ") + Filmmaker::PassTypeToken((Filmmaker::PassType)t)
                    + " " + Filmmaker::VisibleElementsToken((Filmmaker::VisibleElements)g));
            if (available) CheckBox(draw, r.x0+(r.W()-12*s)*0.5f, r.y0+(r.H()-12*s)*0.5f, 12*s, m.selectedPass[t][g]);
            if (!available && route.why && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", route.why);
        }
        c.y += rowH + gap;
    }
    Section(c, "KEYING & DEPTH");
    const int colors[4][3] = { {0,205,0}, {0,0,255}, {255,0,255}, {0,0,0} };
    const char* const names[4] = { "Green", "Blue", "Magenta", "Black" };
    const float chipW = (c.W() - gap * 3) / 4;
    for (int i = 0; i < 4; ++i) {
        const bool on = m.keyColor[0] == colors[i][0] && m.keyColor[1] == colors[i][1] && m.keyColor[2] == colors[i][2];
        const Rect r = MakeRect(c.x0 + i * (chipW + gap), c.y, c.x0 + i * (chipW + gap) + chipW, c.y + rowH);
        draw->AddRectFilled(ImVec2(r.x0+10*s,r.y0+10*s),ImVec2(r.x0+18*s,r.y1-10*s),IM_COL32(colors[i][0],colors[i][1],colors[i][2],255));
        if (CardButton(Format("##key%d", i).c_str(), names[i], r, on ? ButtonKind::Primary : ButtonKind::Normal, !m.running, label))
            out.push_back(Format("mirv_filmmaker render pass key %d %d %d", colors[i][0], colors[i][1], colors[i][2]));
    }
    c.y += rowH + 10 * s;
    // Keep text edits stable while the model refreshes every frame; reseed only when the live
    // setting changed and no field is being edited. Enter or leaving a field commits the pair.
    if (!ImGui::IsAnyItemActive()) {
        if (st.depthNearSeed != m.depthNear) { st.depthNearSeed = m.depthNear; std::snprintf(st.depthNearText, sizeof(st.depthNearText), "%g", m.depthNear); }
        if (st.depthFarSeed != m.depthFar) { st.depthFarSeed = m.depthFar; std::snprintf(st.depthFarText, sizeof(st.depthFarText), "%g", m.depthFar); }
    }
    bool commit = false;
    ImGui::BeginDisabled(m.running);
    for (int i = 0; i < 2; ++i) {
        const float x = c.x0 + i * (c.W() + gap) / 2;
        draw->AddText(caption, caption->FontSize, ImVec2(x, c.y), ToU32(Theme::TextDim), i == 0 ? "NEAR · WHITE" : "FAR · BLACK");
        ImGui::SetCursorScreenPos(ImVec2(x, c.y + caption->FontSize + 5 * s));
        ImGui::SetNextItemWidth((c.W() - gap) / 2);
        commit |= ImGui::InputText(i == 0 ? "##depthnear" : "##depthfar", i == 0 ? st.depthNearText : st.depthFarText,
            sizeof(st.depthNearText), ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);
        commit |= ImGui::IsItemDeactivatedAfterEdit();
    }
    ImGui::EndDisabled();
    const float nearDistance = (float)std::atof(st.depthNearText), farDistance = (float)std::atof(st.depthFarText);
    if (commit && std::isfinite(nearDistance) && std::isfinite(farDistance) && nearDistance >= 0 && farDistance > nearDistance)
        out.push_back(Format("mirv_filmmaker render pass depth %g %g", nearDistance, farDistance));
    c.y += caption->FontSize + ImGui::GetFrameHeight() + 12 * s;
    if (!(nearDistance >= 0 && farDistance > nearDistance)) Note(c, "Depth needs 0 ≤ near < far.", kError);

	Section(c, "GAME HUD", "Beauty \xC2\xB7 Everything");
	const std::string subs[2] = { kHudSubs[0], kHudSubs[1] };
	const int hud = Tiles(c, "##hud", kHudTitles, subs, nullptr, 2, 2, m.hud == 0 ? 0 : 1);
	if (hud >= 0) out.push_back(std::string("mirv_filmmaker render pass hud ") + kHudTokens[hud]);
	if (SwitchRow(c, "##cameradata", "Camera metadata sidecar", m.cameraData, true))
		out.push_back(m.cameraData ? "mirv_filmmaker render pass cameradata 0" : "mirv_filmmaker render pass cameradata 1");
	if (SwitchRow(c, "##blood", "Blood", m.blood, true))
		out.push_back(m.blood ? "mirv_filmmaker render pass blood 0" : "mirv_filmmaker render pass blood 1");
}

void Output(RenderWorkspaceState& st, const RenderWorkspaceModel& m, CardColumn& c, std::vector<std::string>& out) {
	const float s = c.s;
	ImFont* button = c.Font(FontRole::Button);
	ImFont* note = c.Font(FontRole::Note);

	// DESTINATION: the folder and Choose folder..., the take's name, the format grid.
	Section(c, "DESTINATION");
	{
		const char* caption = m.folderPending ? "Choosing..." : "Choose folder...";
		const float bw = CardButtonWidth(caption, button, s) + 16 * s;
		CardColumn box = c;
		box.x0 = c.x0 + 10 * s;
		box.x1 = c.x1 - bw - 18 * s;
		const float top = c.y;
		box.y = top + 9 * s;
		const std::string text = m.folder.empty() ? "No folder chosen yet." : m.folder;
		const ImVec2 size = note->CalcTextSizeA(note->FontSize, FLT_MAX, box.W(), text.c_str());
		const float h = (std::max)(size.y + 18 * s, button->FontSize + 18 * s);
		ImDrawList* draw = ImGui::GetWindowDrawList();
		draw->AddRectFilled(ImVec2(c.x0, top), ImVec2(c.x1 - bw - 8 * s, top + h), ToU32(kCardTileBg));
		draw->AddRect(ImVec2(c.x0, top), ImVec2(c.x1 - bw - 8 * s, top + h), ToU32(m.folder.empty() ? kError : Theme::Border));
		Wrapped(box, note, m.folder.empty() ? kError : Theme::Text, text);
		if (CardButton("##folder", caption, MakeRect(c.x1 - bw, top, c.x1, top + h), ButtonKind::Primary, !m.folderPending && !m.running, button))
			out.push_back("mirv_filmmaker render output folder");
		c.y = top + h + 6 * s;
		Note(c, ("Take: " + (m.project.empty() ? std::string("render") : m.project) + "_<timestamp>").c_str());
	}
	{
		const float gap = 4 * s, w = (c.W() - gap * 7) / 8;
		ImFont* label = c.Font(FontRole::Button);
		ImFont* kind = c.Font(FontRole::Caption);
		const float h = label->FontSize + kind->FontSize + 18 * s;
		ImDrawList* draw = ImGui::GetWindowDrawList();
		for (int i = 0; i < 8; ++i) {
			const FormatCell& f = kFormats[i];
			const Rect r = MakeRect(c.x0 + i * (w + gap), c.y, c.x0 + i * (w + gap) + w, c.y + h);
			ImGui::SetCursorScreenPos(ImVec2(r.x0, r.y0));
			ImGui::PushID(i);
			const bool hit = ImGui::InvisibleButton("##format", ImVec2(r.W(), r.H()));
			const bool hot = ImGui::IsItemHovered();
			ImGui::PopID();
			const bool on = f.format == m.format, ok = m.formatOk[f.format];
			draw->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1),
				ToU32(on ? Rgba(255, 176, 0, 34) : (hot ? Rgba(255, 255, 255, 18) : kCardTileBg)));
			if (on) draw->AddRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), ToU32(Theme::Accent));
			const Color text = ok ? Theme::Text : Theme::TextDisabled;
			const float lw = label->CalcTextSizeA(label->FontSize, FLT_MAX, 0, f.label).x;
			const float kw = SpacedTextWidth(kind, f.kind, 0.8f * s);
			draw->AddText(label, label->FontSize, ImVec2(std::round(r.x0 + (w - lw) * 0.5f), r.y0 + 7 * s), ToU32(text), f.label);
			DrawSpacedText(draw, kind, ImVec2(std::round(r.x0 + (w - kw) * 0.5f), r.y0 + 10 * s + label->FontSize),
				ToU32(ok ? Theme::TextDim : Theme::TextDisabled), f.kind, 0.8f * s);
			if (hit && !m.running) out.push_back(std::string("mirv_filmmaker render output format ") + f.token);
		}
		c.y += h + 8 * s;
		Note(c, m.formatNote.c_str(), m.formatOk[m.format & 7] ? Theme::TextDim : kError);
	}

	// PICTURE: the game's resolution and the frame rate.
	Section(c, "PICTURE");
	{
		ImDrawList* draw = ImGui::GetWindowDrawList();
		ImFont* value = c.Font(FontRole::Value);
		ImFont* cap = c.Font(FontRole::Caption);
		const std::string dims = Format("%d \xC3\x97 %d", m.width, m.height);
		const char* tag = "GAME RESOLUTION";
		const float tagW = SpacedTextWidth(cap, tag, 1.0f * s);
		const float dimsW = value->CalcTextSizeA(value->FontSize, FLT_MAX, 0, dims.c_str()).x;
		const float y = c.y - value->FontSize - 10 * s;
		DrawSpacedText(draw, cap, ImVec2(c.x1 - tagW, y + value->FontSize - cap->FontSize), ToU32(Theme::Loop), tag, 1.0f * s);
		draw->AddText(value, value->FontSize, ImVec2(c.x1 - tagW - 12 * s - dimsW, y), ToU32(Theme::Text), dims.c_str());
	}
	int fpsCell = 5;
	for (int i = 0; i < 5; ++i)
		if (m.fps == kFpsChoices[i]) fpsCell = i;
	if (st.fpsCustom) fpsCell = 5;
	const int fps = ModeRow(c, "##fps", "Frame rate", kFpsCaptions, 6, fpsCell, !m.running);
	if (fps >= 0 && fps < 5) {
		st.fpsCustom = false;
		out.push_back(Format("mirv_filmmaker render output fps %d", kFpsChoices[fps]));
	} else if (fps == 5) {
		st.fpsCustom = true;
		std::snprintf(st.fpsText, sizeof(st.fpsText), "%d", m.fps);
	}
	if (fpsCell == 5) {
		const Rect field = MakeRect(c.x0 + c.W() * 0.5f, c.y, c.x1, c.y + button->FontSize + 12 * s);
		ImGui::GetWindowDrawList()->AddText(note, note->FontSize, ImVec2(c.x0, c.y + 4 * s), ToU32(Theme::TextDim), "Frames a second (1-1000)");
		if (!st.fpsCustom) std::snprintf(st.fpsText, sizeof(st.fpsText), "%d", m.fps);
		int value = 0;
		if (NumberField("##fpsfield", st.fpsText, sizeof(st.fpsText), field, value) && value >= 1 && value <= 1000)
			out.push_back(Format("mirv_filmmaker render output fps %d", value));
		c.y = field.y1 + 6 * s;
	}
	Note(c, "Render sets host_framerate to the frame rate automatically.");

	// ENCODING: MP4's quality, encoder and speed.
	const bool mp4 = m.format == 0;
	Section(c, "ENCODING", mp4 ? nullptr : "MP4 ONLY");
	int quality = 1;
	if (m.bitrateKbps > 0 || st.bitrateCustom) quality = 4;
	else {
		quality = -1;
		for (int i = 0; i < 4; ++i)
			if (m.crf == kCrfChoices[i]) quality = i;
	}
	const int q = ModeRow(c, "##quality", "Quality", kQualityCaptions, 5, quality, mp4 && !m.running);
	if (q >= 0 && q < 4) {
		st.bitrateCustom = false;
		out.push_back("mirv_filmmaker render output bitrate 0");
		out.push_back(Format("mirv_filmmaker render output quality %d", kCrfChoices[q]));
	} else if (q == 4) {
		st.bitrateCustom = true;
		if (m.bitrateKbps <= 0) out.push_back("mirv_filmmaker render output bitrate 8000");
		std::snprintf(st.bitrateText, sizeof(st.bitrateText), "%d", m.bitrateKbps > 0 ? m.bitrateKbps : 8000);
	}
	if (quality == 4 && mp4) {
		const Rect field = MakeRect(c.x0 + c.W() * 0.5f, c.y, c.x1, c.y + button->FontSize + 12 * s);
		ImGui::GetWindowDrawList()->AddText(note, note->FontSize, ImVec2(c.x0, c.y + 4 * s), ToU32(Theme::TextDim), "Bitrate (kbps)");
		if (!st.bitrateCustom) std::snprintf(st.bitrateText, sizeof(st.bitrateText), "%d", m.bitrateKbps);
		int value = 0;
		if (NumberField("##bitratefield", st.bitrateText, sizeof(st.bitrateText), field, value) && value >= 0)
			out.push_back(Format("mirv_filmmaker render output bitrate %d", value));
		c.y = field.y1 + 6 * s;
	}
	const int encoderCell = m.encoder == 2 ? 0 : (m.encoder == 0 ? 1 : 2);
	const int enc = ModeRow(c, "##encoder", "Encoder", kEncoders, 3, encoderCell, mp4 && !m.running);
	if (enc >= 0) out.push_back(std::string("mirv_filmmaker render output encoder ") + kEncoderTokens[enc]);
	const bool gpu = m.encoder == 1 || (m.encoder == 2 && m.nvenc == 2);
	if (!gpu) {
		const int speed = ModeRow(c, "##speed", "Speed", kSpeeds, 3, m.speed, mp4 && !m.running);
		if (speed >= 0) out.push_back(std::string("mirv_filmmaker render output speed ") + kSpeedTokens[speed]);
	}
	Note(c, (m.encoder == 2 && m.nvenc == 2) ? "Auto \xE2\x86\x92 GPU (NVENC). GPU encoding never slows a render down; CPU encoding can."
		: m.nvencNote.c_str(), (m.encoder == 1 && m.nvenc == 3) ? kError : Theme::TextDim);

	// WHEN THE RENDER ENDS: the chime.
	Section(c, "WHEN THE RENDER ENDS");
	if (SwitchRow(c, "##sound", "Play a sound", m.soundOn, true)) out.push_back("mirv_filmmaker render sound toggle");
	const double volume = st.volumeDrag >= 0 ? st.volumeDrag : m.volume;
	const ValueEdit v = ValueRow(c, "##volume", "Volume", volume, 0, 100, 0, "%", Theme::Accent, m.soundOn);
	if (v.changed) {
		st.volumeDrag = (int)std::lround(v.value);
		out.push_back(Format("mirv_filmmaker render sound volume %d", st.volumeDrag));
	}
	if (v.ended) {
		st.volumeDrag = -1;
		out.push_back("mirv_filmmaker render sound test"); // hear the new level
	}
	{
		const char* caption = "Play the sound";
		const float bw = CardButtonWidth(caption, button, s) + 16 * s;
		if (CardButton("##soundtest", caption, MakeRect(c.x0, c.y + 4 * s, c.x0 + bw, c.y + 4 * s + button->FontSize + 14 * s),
			ButtonKind::Normal, true, button))
			out.push_back("mirv_filmmaker render sound test");
		c.y += button->FontSize + 22 * s;
	}
}

void SettingsColumn(RenderWorkspaceState& st, const RenderWorkspaceModel& m, const FontSet& fonts, float s, const Rect& col,
	std::vector<std::string>& out) {
	BeginPanel("##rendersettings", col, Rgba(15, 17, 20, 245));
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const float pad = Metrics::col.pad * s;
	ImFont* title = fonts.Get(FontRole::Title);
	ImFont* label = fonts.Get(FontRole::Label);

	// Header: RENDER (RENDER · RUNNING while a job runs), SOUND: ON | OFF and BACK.
	const ImVec2 head(col.x0 + pad, col.y0 + pad);
	DrawSpacedText(draw, title, head, ToU32(m.running ? kRunning : Theme::Accent),
		m.running ? "RENDER \xC2\xB7 RUNNING" : "RENDER", 1.6f * s);
	const float backW = ChipWidth("BACK", label, s);
	const float chipY = head.y - Metrics::chip.padV * s;
	if (Chip("##renderback", "BACK", ImVec2(col.x1 - pad - backW, chipY), label, s)) out.push_back("mirv_filmmaker render close");
	const char* sound = m.soundOn ? "SOUND: ON" : "SOUND: OFF";
	const float soundW = ChipWidth(sound, label, s);
	if (Chip("##rendersound", sound, ImVec2(col.x1 - pad - backW - 8 * s - soundW, chipY), label, s))
		out.push_back("mirv_filmmaker render sound toggle");

	// The tabs.
	ImGui::SetCursorScreenPos(ImVec2(head.x, head.y + title->FontSize + 18 * s));
	int tab = m.tab >= 0 && m.tab < 3 ? m.tab : 0;
	if (TextTabs("##rendertabs", kTabs, 3, tab, label, Metrics::tabTop, s, col.W() - 2 * pad))
		out.push_back(Format("mirv_filmmaker render tab %d", kTabSlots[tab]));
	const float bodyTop = ImGui::GetCursorScreenPos().y + 4 * s;

	// The playhead bar pinned at the bottom: PLAYHEAD, Set In, Set Out, Clear.
	ImFont* button = fonts.Get(FontRole::Button);
	ImFont* cap = fonts.Get(FontRole::Caption);
	ImFont* value = fonts.Get(FontRole::Value);
	const float footH = button->FontSize + 26 * s;
	const Rect foot = MakeRect(col.x0 + pad, col.y1 - pad - footH, col.x1 - pad, col.y1 - pad);
	draw->AddLine(ImVec2(col.x0, foot.y0 - 12 * s), ImVec2(col.x1, foot.y0 - 12 * s), ToU32(Rgba(255, 255, 255, 30)), 1.0f);
	DrawSpacedText(draw, cap, ImVec2(foot.x0, foot.y0 + 2 * s), ToU32(Theme::TextDim), "PLAYHEAD", 1.0f * s);
	draw->AddText(value, value->FontSize, ImVec2(foot.x0, foot.y0 + 6 * s + cap->FontSize), ToU32(Theme::Accent),
		Format("%d", m.playhead).c_str());
	{
		const float gap = 6 * s, btnW = (foot.W() * 0.72f - 2 * gap) / 3;
		const float x0 = foot.x1 - 3 * btnW - 2 * gap;
		const bool live = m.demo && !m.running;
		if (CardButton("##setin", "Set In", MakeRect(x0, foot.y0, x0 + btnW, foot.y1), ButtonKind::Normal, live, button))
			out.push_back(Format("mirv_filmmaker render in %d", m.playhead));
		if (CardButton("##setout", "Set Out", MakeRect(x0 + btnW + gap, foot.y0, x0 + 2 * btnW + gap, foot.y1), ButtonKind::Normal, live, button))
			out.push_back(Format("mirv_filmmaker render out %d", m.playhead + 1));
		if (CardButton("##clearrange", "Clear", MakeRect(x0 + 2 * (btnW + gap), foot.y0, foot.x1, foot.y1), ButtonKind::Normal, !m.running, button))
			out.push_back("mirv_filmmaker render clearrange");
	}

	float width = col.W() - 2 * pad;
	const Rect body = MakeRect(col.x0 + pad, bodyTop, col.x1 - pad, foot.y0 - 22 * s);
	CardColumn c;
	c.fonts = &fonts;
	c.s = s;
	c.x0 = body.x0;
	c.y = BeginCardScroll("##renderbody", body, width);
	c.x1 = body.x0 + width;
	ImGui::PushID(tab);
	if (tab == 1) Passes(st, m, c, out);
	else if (tab == 2) Output(st, m, c, out);
	else RangeAndCamera(st, m, c, out);
	ImGui::PopID();
	EndCardScroll(c.x0, c.y + 10 * s);
	ImGui::End();
}

} // namespace

void DrawRenderWorkspace(RenderWorkspaceState& state, const RenderWorkspaceModel& model, const FontSet& fonts, float s,
	const Rect& job, const Rect& settings, std::vector<std::string>& out) {
	if (job.Valid()) JobColumn(state, model, fonts, s, job, out);
	if (settings.Valid()) SettingsColumn(state, model, fonts, s, settings, out);
}

} // namespace Ui
} // namespace SourceFilmmaker
