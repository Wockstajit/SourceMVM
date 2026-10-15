#pragma once

// THE ONE OWNER of "which editor is docked along the bottom".
//
// THE LAYOUT THIS BELONGS TO. The workspace has two independent axes, and conflating them is what
// produced a panel with everything stacked down the right-hand side:
//
//   * WHICH WORKSPACE is open -- Camera Editor, Config, Render. That chooses what the RIGHT column
//     shows, and the column is an INSPECTOR: the properties of whatever is selected.
//   * WHICH EDITOR IS DOCKED at the bottom -- the game's own replay bar, the camera timeline, or
//     the graph. That is this file, and it is a property of the screen, not of the workspace.
//
// So the timeline, the camera editor's strip and the graph all live UNDER the preview, switched by
// a ribbon of tabs at the top of the dock. None of them belongs in the right column. The CS2 editor
// has always been shaped this way; the first Deadlock port was not, which is the bug this fixes.
//
//     +------------------------------------------+------------------+
//     |      +--------------------------+        |  INSPECTOR       |
//     |      |   preview (the world)    |        |  COLUMN          |
//     |      +--------------------------+        |                  |
//     +------------------------------------------+------------------+
//     |  [REGULAR TIMELINE] [CAMERA] [GRAPH]           meta     (*) |   <- ribbon, 36px
//     +-------------------------------------------------------------+
//     |  the active docked editor                                    |
//     +-------------------------------------------------------------+
//
// A TAB IS A SWITCH, NOT A TOGGLE. It always selects its own mode, so the dock can never end up
// showing nothing -- which is the state a toggle reaches the second time you press it.
//
// REGULAR TIMELINE IS NOT AN EMPTY DOCK. It is Deadlock's own replay bar, left alone and drawn by
// the game. The dock reserves the height and draws no editor of its own, which is why the ribbon is
// translucent there: it overlays the game's render instead of sitting under it.
//
// Main/UI thread only. Every caller is a HUD pump or the console dispatch.

#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

enum class DockTab {
	Native = 0, // Deadlock's own CitadelHudReplay bar; no overlay of ours
	Camera = 1, // our camera timeline: markers, playhead, loop region, transport
	Graph = 2,  // the curve editor
};

void Dock_SetTab(DockTab tab);
DockTab Dock_Tab();

// "native" / "camera" / "graph" -- the same spellings the console command and the panel state use,
// so there is one vocabulary end to end and no translation table anywhere.
const char* Dock_TabName();
bool Dock_ParseTab(const char* name, DockTab* out);

// The ribbon's own height, and the height of the editor docked below it. Any column must stop at
// screenHeight - Dock_Height(), because a control under the dock cannot be clicked.
int Dock_RibbonHeight();
int Dock_EditorHeight();
int Dock_EditorHeightFor(DockTab tab);
int Dock_Height(); // ribbon + editor

// The measured height of the game's own replay bar, reported by the panel once it has laid out.
// Until then Dock_EditorHeightFor(Native) returns a fallback. Measuring rather than assuming is
// what stops the preview overlapping a bar that a game update made taller.
// Deadlock's own replay bar is what the NATIVE tab hosts, so the band has to be the bar's real
// height -- and only Panorama can measure that. Reported once per change and clamped here before
// it is believed.
void Dock_SetMeasuredNativeHeight(int pixels);
int Dock_MeasuredNativeHeight();

// The band the workspace laid out for the docked editor, in window-client pixels: the ribbon's
// top-left and the size of everything below it. The graph solves every cursor hit test against
// this, so it has to be the rectangle the panel actually drew into rather than one derived a
// second time from the screen size.
void Dock_SetRect(int x, int y, int w, int h);
void Dock_GetRect(int& x, int& y, int& w, int& h);

// mirv_filmmaker dock [native|camera|graph]
void Dock_RunCommand(int argc, advancedfx::ICommandArgs* args);

void Dock_AppendStateJson(std::string& out);
void Dock_Report();

} // namespace Filmmaker
