#pragma once

// PLAY-SCREEN takeover: edits CS2's native main-menu "Play" page in place.
//
// Two jobs, both of them pure Panorama edits on native panels:
//
//   1. Strips the page down to the two tabs an offline movie/demo tool actually uses --
//      "Practice" (Play-listen) and "Workshop Maps" (PlayWorkshop). Official Matchmaking
//      (Play-official) and the community-server browser (PlayCommunity) are hidden, and the
//      page is bounced off Official onto Practice if CS2's saved setting lands it there.
//
//   2. Adds the workshop map sidebar next to the native "Filter maps" box: sort by name /
//      recently installed / recently played, keyword-derived category filters, a "hide
//      training maps" master toggle, and an editor for the keywords behind all of that.
//      The metadata comes from Workshop/WorkshopOrganizer; the tile SET stays native.
//
// DELIBERATELY SEPARATE from Panorama/FilmmakerMenu (the Watch/Demos tab). Different page,
// different lifetime, its own host panel and its own bridge -- neither knows the other exists.
//
// Same discipline as FilmmakerGuiJs: we never replace a native onactivate handler, so CS2's
// own tab bookkeeping stays correct; we only hide panels, relabel them, and add our own.
//
// A hidden host panel (#MvmPlayMenuHost) carries the C++<->JS bridge attributes:
//   C++ -> JS : sets host "ws" attribute (JSON), runs $.MvmPlay.renderWs().
//   JS  -> C++: appends to host "cmd": "ws sort <mode>", "ws cat <id> <0|1>", "ws hide <0|1>",
//               "ws words <id> <words>", "ws reset [id]", "ws rescan".

#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Per-main-thread-frame pump. Injects the page JS once the Panorama bridge and the main-menu
// context exist, pushes organiser state when it moves, and drains the JS->C++ command queue.
// Safe no-op while there is no main menu (in a demo/map) or the bridge has not resolved.
void PlayMenu_RunMainThreadFrame();

// Forces the injected JS to be rebuilt on the next frame.
void PlayMenu_RequestRebuild();

// Queues JS to run in the main-menu context next frame (live probe over netcon).
void PlayMenu_RequestEval(const std::string& js);

// One-line state summary for `mirv_filmmaker play status`.
std::string PlayMenu_Describe();

// "mirv_filmmaker play ..." console surface (implemented in PlayMenuCommand.cpp).
void PlayMenu_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
