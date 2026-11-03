#pragma once

// The demos browser: CS2's demos page (the Watch page's Downloaded tab SOURCE:MVM takes over) as a
// full-screen CS:S page over the main menu. On the left the rail: a toolbar with the sort and the +
// menu (New folder, Add demo file, Add demo folder), the new-folder field, and the demos as tiles under
// the user's folder headers, which collapse, rename, delete and take dropped tiles. On the right the
// selected demo: its name (click to rename the file), map, length and date, what its header says, and
// WATCH, Show in Explorer and Remove. SEARCH jumps to a demo by map, name, recorder or server; REFRESH
// scans again; CLEAR ALL empties the list. Removing never deletes a file.
//
// The page sends what the user asked for as commands, the same lines `mirv_filmmaker demos <line>`
// takes: refresh, clearall, unhideall, adddemo, addfolder, watch <index>, hide <index>,
// openfolder <index>, renamedemo <index> <name>, and CS2's organiser lines (org sort|newfolder|rename|
// delfolder|collapse|assign). The game host (SourceDemos.cpp) and the browser mockup answer them.
// Engine-free; the list rules are DemosBrowserMath.h.

#include "DemosBrowserMath.h"
#include "UiStyle.h"

#include <string>
#include <vector>

namespace SourceFilmmaker {
namespace Ui {

struct DemosBrowserModel {
	std::vector<DemosBrowserMath::DemoRow> demos;    // the listed demos, newest first
	std::vector<DemosBrowserMath::DemoGroup> groups; // the user's folders, in their order
	std::string sort = "date";
	bool rootCollapsed = false;
	bool scanning = false;   // a scan is running: the empty list says so
	int hidden = 0;          // demos removed from the list, for "Show removed demos"
	int64_t now = 0;         // unix s, for "Last played"
	bool converting = false; // an old demo is being converted for playback
	std::string message;     // a note for the user (a rename that failed), shown for a few seconds
};

struct DemosBrowserState {
	std::string selected;           // the selected demo's key
	bool scrollToSelected = false;  // bring it into view on the next frame
	// The title being renamed in place.
	bool renaming = false, renameFocus = false;
	std::string renameKey;
	char renameText[100] = {};
	// The jump-to search.
	bool searchOpen = false, searchFocus = false;
	char search[96] = {};
	// The open menu and what it is for.
	enum class Menu { None, Sort, Add, Tile, Folder } menu = Menu::None;
	Rect menuAnchor;
	std::string menuKey; // the tile's demo
	int menuGroup = -1;  // the header's folder
	// The new-folder field under the toolbar, and a folder header being renamed.
	bool newFolder = false, newFolderFocus = false;
	char newFolderText[41] = {};
	int renamingGroup = -1;
	bool groupFocus = false;
	char groupText[41] = {};
	// A tile being dragged onto a folder header.
	std::string dragKey;
	// The question on screen.
	enum class Confirm { None, ClearAll, Remove, DeleteFolder } confirm = Confirm::None;
	int confirmIndex = -1;    // the demo or folder it is about
	std::string confirmName;
	// WATCH was pressed: which demo, and until when the button stays busy (ImGui time).
	std::string watchKey;
	double watchBusyUntil = 0;
	// The hover tooltip: its text, the row it belongs to, and since when (CS2 waits 0.18 s).
	std::string tipText;
	Rect tipAnchor;
	double tipSince = -1;
};

struct DemosBrowserFrame {
	std::vector<std::string> commands;
	bool close = false; // the page's ESC CLOSE was pressed
};

// Draws the page over the whole client area. Inside an ImGui frame.
DemosBrowserFrame DrawDemosBrowser(DemosBrowserState& state, const FontSet& fonts, float clientW, float clientH,
	const DemosBrowserModel& model);

// Esc: closes the innermost thing open on the page (a question, a menu, a field, the search). False
// when nothing was, and Esc closes the page.
bool DemosBrowserEscape(DemosBrowserState& state);

// The main menu's way in: a DEMOS button at the top right, drawn on the background draw list.
// Returns its rect (client px); hovered lights it.
Rect DrawDemosMenuButton(const FontSet& fonts, float clientW, float clientH, bool hovered);

} // namespace Ui
} // namespace SourceFilmmaker
