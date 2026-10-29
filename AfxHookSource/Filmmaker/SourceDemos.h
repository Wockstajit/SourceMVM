#pragma once

// The demos browser's library (Ui/DemosBrowser.h draws the page, SourceUi.cpp hosts it): every demo
// under the game directory and the folders the user added, plus demos added one by one, each read
// for its header alone on a worker thread (SourceDemoFile.h ReadHeader). CS2's stores keep the
// user's side, under CS:S's own names in %APPDATA%\HLAE\ so the two games' lists stay apart:
// filmmaker_demo_folders_source1.txt, filmmaker_demo_files_source1.txt,
// filmmaker_hidden_demos_source1.txt and filmmaker_demo_organizer_source1.txt (sort, folders, last
// played). Old demos' converted copies (source_mvm/converted, SourceOldDemos.h) are not listed.
//
// Removing a demo hides it; no file is ever deleted. Renaming moves the .dem and carries its folder,
// last-played time and project folder (source_mvm\demos\css) to the new name.
//
// Main thread, except where noted.

#include "Ui/DemosBrowser.h"

#include <string>

namespace advancedfx { class ICommandArgs; }

namespace SourceFilmmaker {

// Loads the stores and starts the first scan. Safe to call again.
void Demos_Init();

// Once a frame: a folder or files picked in Windows' dialogs, and the playing demo's last-played time.
void Demos_Frame();

// Joins the scan worker.
void Demos_Shutdown();

// What the page draws.
void Demos_BuildModel(Ui::DemosBrowserModel& model);

// One page command (the lines Ui/DemosBrowser.h lists). True when the page should close now: a demo
// was sent to play. An old demo still to convert keeps it open until it plays (Demos_PageDone).
bool Demos_Command(const std::string& line);

// The old demo the page sent to play has started: close the page. True once.
bool Demos_PageDone();

// mirv_filmmaker demos list|folders|addfolder <path>|removefolder <i>|hidden|<page command>.
// Returns false when the subcommand is not the library's (on|off|toggle|state are the page's).
bool Demos_ConsoleCommand(int argc, advancedfx::ICommandArgs* args);

void Demos_PrintHealth();

} // namespace SourceFilmmaker
