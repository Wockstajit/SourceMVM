#pragma once

// TimelineDock: the ONE owner of "which bottom editor is docked", shared by every workspace.
//
// WHY THIS EXISTS
//
// The dock (REGULAR TIMELINE / TIMELINE) is one surface that three workspaces show:
// the Camera Editor, the CONFIG panel and the Render workspace. `Docs/panorama/panorama_ui_guide.md`
// maintains this contract, originally phrased as "a single shared component imported by all three
// workspaces with an accent prop; change the timeline once, all three follow".
//
// It used to be a private member of the Camera Editor (`CameraEditorHud::m_bottomMode`), applied
// only inside that panel's RunFrame. Two consequences, both user-visible:
//
//   * CONFIG could not switch dock tabs at all. Its band was a dead LABEL reading "REGULAR
//     TIMELINE", because the camera timeline and graph were only ever made visible by the
//     Camera Editor's own frame pump.
//   * The tab a user picked in the editor was forgotten by every other workspace, so the
//     "same" bar disagreed with itself depending on which panel was open.
//
// So the mode moves here, with the host that is currently responsible for applying it. Panels
// READ this and DRAW it; they never keep their own copy.
//
// THE CONTRACT
//
//   * Exactly one host applies the dock per frame. `Apply(host)` is a no-op unless `host` is the
//     current owner, so two panels open at once can never fight over the overlays.
//   * The Camera Editor outranks CONFIG (opening the editor closes CONFIG anyway), and a host
//     that closes releases ownership, which parks the dock back on the native CS2 bar.
//   * `Height()` is the shared per-tab dock height (Docs/features/camera-editor-feature.md). Any workspace column must stop at
//     `screenHeight - Height()`, because a control under the bar is unclickable.
//
// Threading: main/UI thread only -- every caller is a HUD pump or the console dispatch, both of
// which run there. Mirrors Movie/TimelinePlayhead.h, which owns the cursor for the same two
// panels for the same reason.

namespace Filmmaker {

// Which bottom editor is docked. Native = CS2's own demo bar, no overlay of ours.
// Timeline = the unified TIMELINE dock (camera and scene keys, KEYFRAMES / GRAPH views), which
// replaced the separate CAMERA and GRAPH tabs.
enum class DockTab { Native = 0, Timeline = 1 };

// Who is currently responsible for showing the dock. None = nothing is open, so the dock is
// parked and the native bar is on its own.
enum class DockHost { None = 0, CameraEditor = 1, Config = 2 };

void TimelineDock_SetTab(DockTab tab);
DockTab TimelineDock_Tab();

// "native" / "timeline" -- the same spellings the console command and the panels' JSON state use,
// so there is one vocabulary end to end.
const char* TimelineDock_TabName();
bool TimelineDock_ParseTab(const char* name, DockTab& out);

// Dock design heights in the 1080-tall stage, ribbon included: 126 native (translucent, overlays
// the render), 476 timeline (the 440px dock under the 36px ribbon). Public so the CS:S hook's
// workspace layout uses the same numbers.
constexpr int kTimelineDockHeightNative = 126;
constexpr int kTimelineDockHeightTimeline = 476;
constexpr int kTimelineDockRibbonHeight = 36;
int TimelineDock_Height();
int TimelineDock_HeightFor(DockTab tab);

// Ownership. Claim returns false if another host already owns the dock this frame.
bool TimelineDock_Claim(DockHost host);
void TimelineDock_Release(DockHost host);
DockHost TimelineDock_Host();

// Push the current tab onto the overlay panels. Only the owning host's call does anything.
// Safe to call every frame; it is how a stray close or HUD recreation gets repaired.
void TimelineDock_Apply(DockHost host);

// Pick a tab by name, with "toggle" flipping Native <-> Timeline. Unknown names are ignored rather
// than throwing the dock to Native, so a typo in a bind cannot silently close the panel someone
// was working in.
void TimelineDock_Pick(const char* name);

}  // namespace Filmmaker

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {
// `mirv_filmmaker dock [native|timeline|toggle]`.
void TimelineDock_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);
}  // namespace Filmmaker
