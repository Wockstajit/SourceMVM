#pragma once

// The CS:S Camera Editor workspace, CS2's layout in CS:S paint: the game framed as a preview, the
// inspector column on the right (PATH CAMERA / FOLLOW CAMERA / SCENE KEYS), the dock ribbon
// (REGULAR TIMELINE / TIMELINE) and the docked editor under the preview, ESC EXIT.
//
// Engine-free: the game host and the browser mockup both call Draw inside an ImGui frame. The panes
// are empty until the steps that fill them; this is the frame they plug into.

#include "UiStyle.h"
#include "WorkspaceLayoutMath.h"

namespace SourceFilmmaker {
namespace Ui {

enum class InspectorTab { PathCamera = 0, FollowCamera = 1, SceneKeys = 2, Count };
enum class DockTab { RegularTimeline = 0, Timeline = 1 };

struct CameraEditorState {
	bool open = false;
	InspectorTab tab = InspectorTab::PathCamera;
	DockTab dock = DockTab::RegularTimeline;
	double inspectorW = Filmmaker::InspectorWidthMath::kDefault; // the operator's stored width
};

struct CameraEditorFrame {
	WorkspaceLayout layout;  // stage units
	Rect previewClient;      // where the game is drawn, client px
	bool exitClicked = false;
};

// Lays out and draws the workspace for a clientW x clientH window. Call between ImGui::NewFrame and
// ImGui::Render; fonts must have been built for layout scale (FontSet::scale).
CameraEditorFrame DrawCameraEditor(CameraEditorState& state, const FontSet& fonts, float clientW, float clientH);

// Stage height of the docked editor for a dock tab.
float DockEditorHeight(DockTab dock);

} // namespace Ui
} // namespace SourceFilmmaker
