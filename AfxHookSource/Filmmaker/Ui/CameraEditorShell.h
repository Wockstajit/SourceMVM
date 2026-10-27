#pragma once

// The CS:S Camera Editor workspace, CS2's layout in CS:S paint: the game framed as a preview, the
// inspector column on the right (PATH CAMERA / FOLLOW CAMERA / SCENE KEYS), the dock ribbon
// (REGULAR TIMELINE / TIMELINE) and the docked editor under the preview, ESC EXIT.
//
// Engine-free: the game host and the browser mockup both call Draw inside an ImGui frame. PATH
// CAMERA is CS2's rail and card (PathInspector.h), FOLLOW CAMERA CS2's follow inspector
// (FollowInspector.h), the TIMELINE dock CS2's keyframe editor (TimelineDock.h); SCENE KEYS stays
// empty until the step that fills it.

#include "DemoBar.h"
#include "FollowInspector.h"
#include "PathInspector.h"
#include "TimelineDock.h"
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
	DemoBarUi demoBar;
	PathInspectorState path;
	FollowInspectorState follow;
	TimelineDockState timeline;
};

struct CameraEditorFrame {
	WorkspaceLayout layout;  // stage units
	Rect previewClient;      // where the game is drawn, client px
	bool modal = false;      // a dialog or a list is up: the preview takes no clicks until it is answered
	bool exitClicked = false;
	std::vector<DemoBarAction> demoActions; // from the demo bar in the REGULAR TIMELINE dock
	std::vector<PathInspectorAction> pathActions; // from the PATH CAMERA tab
	std::vector<std::string> followCommands;      // from the FOLLOW CAMERA tab: mirv_filmmaker follow ...
	std::vector<TimelineDockAction> timelineActions; // from the TIMELINE dock
};

// Lays out and draws the workspace for a clientW x clientH window, with the demo bar in the
// REGULAR TIMELINE dock, the keys in the TIMELINE dock, the camera path in the PATH CAMERA tab and
// the follow camera in the FOLLOW CAMERA tab.
// Call between ImGui::NewFrame and ImGui::Render; fonts must have been built for the layout scale
// (FontSet::scale).
CameraEditorFrame DrawCameraEditor(CameraEditorState& state, const FontSet& fonts, float clientW, float clientH,
	const DemoBarModel& demo, const PathInspectorModel& path, const FollowInspectorModel& follow, const TimelineDockModel& timeline);

// Stage height of the docked editor for a dock tab.
float DockEditorHeight(DockTab dock);

} // namespace Ui
} // namespace SourceFilmmaker
