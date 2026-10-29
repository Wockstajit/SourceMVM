#pragma once

// The CS:S workspaces, CS2's layout in CS:S paint: the game framed as a preview, the inspector
// column on the right, the dock ribbon (REGULAR TIMELINE / TIMELINE) and the docked editor under
// the preview, ESC EXIT. Three workspaces share it, as in CS2: the Camera Editor (PATH CAMERA /
// FOLLOW CAMERA / SCENE KEYS), Config (its CAMERA tab, ConfigInspector.h), which the demo bar's swap
// button changes to and back, and Render (RenderWorkspace.h), opened from the editor's RENDER button,
// with its JOB column at the left and the dock across the whole width.
//
// Engine-free: the game host and the browser mockup both call Draw inside an ImGui frame. PATH
// CAMERA is CS2's rail and card (PathInspector.h), FOLLOW CAMERA CS2's follow inspector
// (FollowInspector.h), the TIMELINE dock CS2's keyframe editor (TimelineDock.h); SCENE KEYS stays
// empty until the step that fills it.

#include "ConfigInspector.h"
#include "DemoBar.h"
#include "FollowInspector.h"
#include "PathInspector.h"
#include "RenderWorkspace.h"
#include "TimelineDock.h"
#include "UiStyle.h"
#include "WorkspaceLayoutMath.h"

namespace SourceFilmmaker {
namespace Ui {

enum class Workspace { Editor = 0, Config = 1, Render = 2 };
enum class InspectorTab { PathCamera = 0, FollowCamera = 1, SceneKeys = 2, Count };
enum class DockTab { RegularTimeline = 0, Timeline = 1 };

struct CameraEditorState {
	bool open = false;
	Workspace workspace = Workspace::Editor;
	InspectorTab tab = InspectorTab::PathCamera;
	DockTab dock = DockTab::RegularTimeline;
	double inspectorW = Filmmaker::InspectorWidthMath::kDefault; // the operator's stored width
	DemoBarUi demoBar;
	PathInspectorState path;
	FollowInspectorState follow;
	TimelineDockState timeline;
	ConfigState config;
	RenderWorkspaceState render;
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
	std::vector<std::string> configCommands;         // from Config: console commands, as CS2's panel sends them
	std::vector<std::string> renderCommands;         // from Render and the editor's RENDER button: mirv_filmmaker render ...
};

// Lays out and draws the open workspace for a clientW x clientH window, with the demo bar in the
// REGULAR TIMELINE dock, the keys in the TIMELINE dock, the camera path in the PATH CAMERA tab, the
// follow camera in the FOLLOW CAMERA tab and the camera feel in Config.
// Call between ImGui::NewFrame and ImGui::Render; fonts must have been built for the layout scale
// (FontSet::scale).
CameraEditorFrame DrawCameraEditor(CameraEditorState& state, const FontSet& fonts, float clientW, float clientH,
	const DemoBarModel& demo, const PathInspectorModel& path, const FollowInspectorModel& follow, const TimelineDockModel& timeline,
	const ConfigModel& config, const RenderWorkspaceModel& render);

// Stage height of the docked editor for a dock tab.
float DockEditorHeight(DockTab dock);

} // namespace Ui
} // namespace SourceFilmmaker
