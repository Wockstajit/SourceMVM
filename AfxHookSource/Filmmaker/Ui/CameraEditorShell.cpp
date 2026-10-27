#include "CameraEditorShell.h"

#include "UiWidgets.h"

#include <cfloat>
#include <cmath>
#include <string>

namespace SourceFilmmaker {
namespace Ui {

namespace {

const char* kInspectorTabs[] = { "PATH CAMERA", "FOLLOW CAMERA", "SCENE KEYS" };
const char* kInspectorEmpty[] = { "No cameras yet.", "No follow target.", "No scene keys." };
const char* kDockTabs[] = { "REGULAR TIMELINE", "TIMELINE" };

} // namespace

float DockEditorHeight(DockTab dock) {
	return dock == DockTab::Timeline ? kTimelineEditorHeight : kDemoBarHeight;
}

CameraEditorFrame DrawCameraEditor(CameraEditorState& state, const FontSet& fonts, float clientW, float clientH,
	const DemoBarModel& demo, const PathInspectorModel& path, const TimelineDockModel& timeline) {
	CameraEditorFrame frame;
	frame.layout = CameraEditorLayout(clientW, clientH, state.inspectorW, DockEditorHeight(state.dock));
	const WorkspaceLayout& l = frame.layout;
	const Stage& stage = l.stage;
	const float s = stage.scale;
	if (!(stage.width > 0)) return frame;
	frame.previewClient = ToClient(l.preview, stage);

	ImDrawList* back = ImGui::GetBackgroundDrawList();

	// The preview frame and its tag. The renderer has already letterboxed the game into
	// previewClient; this only outlines it, as CS2's shell does.
	const Rect pc = frame.previewClient;
	if (pc.Valid()) {
		back->AddRect(ImVec2(pc.x0, pc.y0), ImVec2(pc.x1, pc.y1), ToU32(Theme::Accent), 0, 0, 1.0f);
		ImFont* tag = fonts.Get(FontRole::Label);
		const float pad = 6 * s;
		const ImVec2 tagPos(pc.x0 + 12 * s, pc.y0 + 10 * s);
		const float tagW = SpacedTextWidth(tag, "PREVIEW", 1.4f * s);
		back->AddRectFilled(ImVec2(tagPos.x - pad, tagPos.y - pad * 0.6f),
			ImVec2(tagPos.x + tagW + pad, tagPos.y + tag->FontSize + pad * 0.6f), ToU32(Rgba(0, 0, 0, 170)));
		DrawSpacedText(back, tag, tagPos, ToU32(Theme::Accent), "PREVIEW", 1.4f * s);
	}

	// The workspace's name in the letterbox under the preview (CS2: "CAMERA / EDITOR" with its
	// code line), dim so it reads as a watermark rather than a control.
	{
		ImFont* display = fonts.Get(FontRole::Display);
		ImFont* small = fonts.Get(FontRole::Small);
		const Rect area = ToClient(l.area, stage);
		const float bandTop = pc.Valid() ? pc.y1 : area.y0;
		const float x = area.x0 + Metrics::col.pad * s;
		const float y = area.y1 - display->FontSize - small->FontSize - 14 * s;
		if (y > bandTop) {
			DrawSpacedText(back, display, ImVec2(x, y), ToU32(Faded(Theme::Accent, 0.32f)), "CAMERA / EDITOR", 2.0f * s);
			DrawSpacedText(back, small, ImVec2(x + 2 * s, y + display->FontSize + 4 * s),
				ToU32(Faded(Theme::Accent, 0.55f)), "SOURCE // MVM FILMMAKER", 1.6f * s);
		}
	}

	// Inspector column.
	const Rect inspector = ToClient(l.inspector, stage);
	BeginPanel("##inspector", inspector, Theme::PanelBg);
	{
		ImDrawList* draw = ImGui::GetWindowDrawList();
		draw->AddLine(ImVec2(inspector.x0, inspector.y0), ImVec2(inspector.x0, inspector.y1), ToU32(Theme::Border), 1.0f);
		const float pad = Metrics::col.pad * s;
		const float innerW = inspector.W() - 2 * pad;
		ImFont* title = fonts.Get(FontRole::Title);
		ImFont* label = fonts.Get(FontRole::Label);

		// Header: the workspace title and ESC EXIT.
		const ImVec2 head(inspector.x0 + pad, inspector.y0 + pad);
		DrawSpacedText(draw, title, head, ToU32(Theme::Accent), "CAMERA EDITOR", 1.2f * s);
		const float chipW = ChipWidth("ESC EXIT", label, s);
		if (Chip("##exit", "ESC EXIT", ImVec2(inspector.x1 - pad - chipW, head.y - Metrics::chip.padV * s), label, s))
			frame.exitClicked = true;

		// The inspector's own tabs.
		ImGui::SetCursorScreenPos(ImVec2(head.x, head.y + title->FontSize + 18 * s));
		int tab = (int)state.tab;
		if (TextTabs("##inspectortabs", kInspectorTabs, (int)InspectorTab::Count, tab, fonts.Get(FontRole::Label),
			Metrics::tabTop, s, innerW))
			state.tab = (InspectorTab)tab;

		// The body: the rail on the left, the selected card beside it (CS2's edBodyRow), with PATH
		// CAMERA's keys pinned under it.
		const ImVec2 body = ImGui::GetCursorScreenPos();
		if (state.tab == InspectorTab::PathCamera) {
			const float stripH = PathKeyStripHeight(fonts, s, innerW);
			const float bottom = inspector.y1 - 10 * s;
			const float bodyTop = body.y + 14 * s, bodyBottom = bottom - stripH - 8 * s;
			const Rect rail = MakeRect(body.x, bodyTop, body.x + Metrics::rail.w * s, bodyBottom);
			const Rect pane = MakeRect(rail.x1 + Metrics::rail.gap * s, bodyTop, inspector.x1 - pad, bodyBottom);
			DrawPathInspector(state.path, path, fonts, s, rail, pane, frame.pathActions);
			DrawPathKeyStrip(draw, fonts, s, MakeRect(body.x, bottom - stripH, inspector.x1 - pad, bottom));
		} else {
			draw->AddText(fonts.Get(FontRole::Body), fonts.Get(FontRole::Body)->FontSize,
				ImVec2(body.x, body.y + 16 * s), ToU32(Theme::TextDim), kInspectorEmpty[(int)state.tab]);
		}
	}
	ImGui::End();

	// Dock ribbon: REGULAR TIMELINE / TIMELINE, left of the inspector (TimelineDockRibbonJs.h): two
	// 150-wide tabs, and on TIMELINE how many layers and keys it holds.
	const Rect ribbon = ToClient(l.ribbon, stage);
	const bool onTimeline = state.dock == DockTab::Timeline;
	BeginPanel("##ribbon", ribbon, onTimeline ? Theme::DockBg : Rgba(0, 0, 0, 150));
	{
		ImDrawList* draw = ImGui::GetWindowDrawList();
		draw->AddRectFilled(ImVec2(ribbon.x0, ribbon.y0), ImVec2(ribbon.x1, ribbon.y0 + 1), ToU32(Theme::AccentDim));
		draw->AddRectFilled(ImVec2(ribbon.x0, ribbon.y1 - 1), ImVec2(ribbon.x1, ribbon.y1), ToU32(Theme::AccentDim));
		ImFont* label = fonts.Get(FontRole::Label);
		float x = ribbon.x0 + 12 * s;
		const float tabW = 150 * s, tabH = label->FontSize + 18 * s;
		for (int i = 0; i < 2; ++i) {
			const Rect tab = MakeRect(x, ribbon.y0 + (ribbon.H() - tabH) * 0.5f, x + tabW, ribbon.y0 + (ribbon.H() + tabH) * 0.5f);
			ImGui::SetCursorScreenPos(ImVec2(tab.x0, tab.y0));
			if (ImGui::InvisibleButton(i == 0 ? "##dockregular" : "##docktimeline", ImVec2(tab.W(), tab.H()))) state.dock = (DockTab)i;
			const bool hovered = ImGui::IsItemHovered();
			const bool on = (int)state.dock == i;
			const Color bg = on ? Rgba(255, 176, 0, 51) : (hovered ? Rgba(255, 255, 255, 38) : Rgba(255, 255, 255, 20));
			const Color edge = on ? Theme::Accent : Rgba(255, 255, 255, 26);
			draw->AddRectFilled(ImVec2(tab.x0, tab.y0), ImVec2(tab.x1, tab.y1), ToU32(bg), 3 * s);
			draw->AddRect(ImVec2(tab.x0, tab.y0), ImVec2(tab.x1, tab.y1), ToU32(edge), 3 * s);
			const float tw = SpacedTextWidth(label, kDockTabs[i], 1.4f * s);
			DrawSpacedText(draw, label, ImVec2(std::round(tab.x0 + (tab.W() - tw) * 0.5f), std::round(tab.y0 + (tab.H() - label->FontSize) * 0.5f)),
				ToU32(on ? Theme::Accent : Theme::Text), kDockTabs[i], 1.4f * s);
			x += tabW + 4 * s;
		}
		if (onTimeline) {
			ImFont* meta = fonts.Get(FontRole::Note);
			const std::string text = TimelineDockRibbonMeta(timeline);
			const float mw = meta->CalcTextSizeA(meta->FontSize, FLT_MAX, 0, text.c_str()).x;
			draw->AddText(meta, meta->FontSize, ImVec2(std::round(ribbon.x1 - 12 * s - mw), std::round(ribbon.y0 + (ribbon.H() - meta->FontSize) * 0.5f)),
				ToU32(Theme::TextDim), text.c_str());
		}
	}
	ImGui::End();

	// The docked editor: the demo bar on REGULAR TIMELINE, opaque as CS2's docked bar is, and the
	// keyframe editor on TIMELINE.
	const Rect dock = ToClient(l.dock, stage);
	BeginPanel("##dock", dock, Theme::DockBg);
	if (state.dock == DockTab::RegularTimeline) {
		DemoBarModel inEditor = demo;
		inEditor.showCamEditorButton = false; // CS2 swaps it for CONFIG inside the editor
		frame.demoActions = DrawDemoBar(state.demoBar, inEditor, fonts, dock, s);
	} else {
		frame.timelineActions = DrawTimelineDock(state.timeline, timeline, fonts, dock, s);
	}
	ImGui::End();

	DrawPathClearConfirm(state.path, fonts, s, clientW, clientH, frame.pathActions); // over everything
	frame.modal = state.path.confirmClear;
	return frame;
}

} // namespace Ui
} // namespace SourceFilmmaker
