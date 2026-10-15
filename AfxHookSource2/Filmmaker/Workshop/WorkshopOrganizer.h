#pragma once

// Workshop map ORGANISER: the metadata CS2's own Play -> Workshop Maps page cannot show.
//
// The native page builds its tiles from WorkshopAPI.GetAvailableWorkshopMaps(), which reports
// title / map / tags / description and nothing else -- no "when did I install this", no "when
// did I last play this". Those two are what make a 180-item workshop list usable, so they are
// sourced here instead:
//
//   * installed/updated time  -- mtime of <steam library>\steamapps\workshop\content\730\<id>
//   * last played             -- our own history, stamped when the engine loads that map
//
// Deliberately separate from the Panorama side: the JS owns WHICH tiles exist (only the engine
// knows which subscribed items are valid CS2 maps); this owns the metadata keyed by workshop
// id, plus the user's sort mode and the "hide training maps" keyword list. The Play page works
// exactly the same with this module inert -- it just loses sorting and filtering.
//
// Wiring (three call sites, all one-liners):
//   Filmmaker::DoInit()             -> WorkshopOrganizer_Init()
//   Filmmaker::RunMainThreadFrame() -> WorkshopOrganizer_PollLevel(<engine level name>)
// plus PlayMenuHud, which pushes BuildJson() into the page and routes "ws ..." commands here.

#include <string>
#include <cstdint>

namespace Filmmaker {

// Loads the persisted state and kicks off the first workshop-folder scan. Safe to call more
// than once (only the first call does the work).
void WorkshopOrganizer_Init();

// Bumped on every change; PlayMenuHud pushes a new payload when it moves.
uint64_t WorkshopOrganizer_Version();

// Per-main-frame watcher: stamps "played just now" the moment the engine loads a map that
// belongs to a subscribed workshop item. Cheap -- a string compare against the last seen
// level, then a lookup in the scanned map-name table.
void WorkshopOrganizer_PollLevel(const std::string& levelNameShort);

// Handles one "ws ..." line from the Panorama command queue. Returns true when the line was
// ours (whether or not it changed anything).
bool WorkshopOrganizer_HandleCommand(const std::string& line);

// Payload for the UI:
//   { sort, hideTraining, root, words:[..], items:{ "<id>": {installed,played} } }
std::string WorkshopOrganizer_BuildJson();

// Re-reads the workshop content folder. Called by Init and by "ws rescan"; a Steam download
// that finishes while CS2 is open is otherwise invisible until the next launch.
void WorkshopOrganizer_Rescan();

// Diagnostic: one-line summary for `mirv_filmmaker play status`.
std::string WorkshopOrganizer_Describe();

} // namespace Filmmaker
