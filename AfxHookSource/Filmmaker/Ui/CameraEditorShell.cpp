#include "CameraEditorShell.h"

#include "UiWidgets.h"

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
	const DemoBarModel& demo) {
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

		// The selected pane. Empty until the steps that fill it.
		const ImVec2 body = ImGui::GetCursorScreenPos();
		draw->AddText(fonts.Get(FontRole::Body), fonts.Get(FontRole::Body)->FontSize,
			ImVec2(body.x, body.y + 16 * s), ToU32(Theme::TextDim), kInspectorEmpty[(int)state.tab]);
	}
	ImGui::End();

	// Dock ribbon: REGULAR TIMELINE / TIMELINE, left of the inspector.
	const Rect ribbon = ToClient(l.ribbon, stage);
	BeginPanel("##ribbon", ribbon, Rgba(0, 0, 0, 150));
	{
		ImDrawList* draw = ImGui::GetWindowDrawList();
		draw->AddLine(ImVec2(ribbon.x0, ribbon.y0), ImVec2(ribbon.x1, ribbon.y0), ToU32(Theme::Border), 1.0f);
		ImFont* label = fonts.Get(FontRole::Label);
		float x = ribbon.x0 + Metrics::col.pad * s;
		for (int i = 0; i < 2; ++i) {
			const float w = ChipWidth(kDockTabs[i], label, s);
			const float h = label->FontSize + 2 * Metrics::chip.padV * s;
			const ImVec2 p(x, ribbon.y0 + (ribbon.H() - h) * 0.5f);
			const bool on = (int)state.dock == i;
			if (Chip(i == 0 ? "##dockregular" : "##docktimeline", kDockTabs[i], p, label, s, on))
				state.dock = (DockTab)i;
			x += w + Metrics::tabGap * s * 2;
		}
	}
	ImGui::End();

	// The docked editor: the demo bar on REGULAR TIMELINE, opaque as CS2's docked bar is; the
	// TIMELINE dock comes with its own step.
	const Rect dock = ToClient(l.dock, stage);
	BeginPanel("##dock", dock, state.dock == DockTab::Timeline ? Rgba(10, 10, 10, 240) : Theme::DockBg);
	if (state.dock == DockTab::RegularTimeline) {
		DemoBarModel inEditor = demo;
		inEditor.showCamEditorButton = false; // CS2 swaps it for CONFIG inside the editor
		frame.demoActions = DrawDemoBar(state.demoBar, inEditor, fonts, dock, s);
	}
	ImGui::End();

	return frame;
}

} // namespace Ui
} // namespace SourceFilmmaker
