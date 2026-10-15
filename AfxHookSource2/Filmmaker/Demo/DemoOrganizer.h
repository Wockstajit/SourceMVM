#pragma once

// Demo browser ORGANISER: the list toolbar's sort mode, the user's demo folders, and the
// "when did I last play this demo" history behind the Recently played sort.
//
// Deliberately separate from DemoLibrary: the library owns what is ON DISK (scan results,
// scoreboards, repair), this owns how the USER chose to arrange that list. It never reads or
// writes a .dem, and the library works exactly the same with this module inert.
//
// Wiring (three call sites, all one-liners):
//   Filmmaker::DoInit()             -> DemoOrganizer_Init()
//   Filmmaker::PlayResolvedDemo()   -> DemoOrganizer_NotePlayed(original)
//   Filmmaker::RunMainThreadFrame() -> DemoOrganizer_PollPlaying(PlayingDemoPath())
// plus FilmmakerMenu, which pushes BuildJson() into the page and routes "org ..." commands
// back here.
//
// The JS side joins this payload to its demo rows by rebuilding the same normalised key from
// the data it already has (folder + '\' + fileName, lower-cased), so no extra field had to be
// added to the demo payload itself.

#include <string>
#include <cstdint>

namespace Filmmaker {

// Loads the persisted state. Safe to call more than once (only the first call loads).
void DemoOrganizer_Init();

// Bumped on every change; FilmmakerMenu pushes a new payload when it moves.
uint64_t DemoOrganizer_Version();

// Records "played just now" for a demo path. Auto-saves.
void DemoOrganizer_NotePlayed(const std::wstring& demoPath);

// Carries a demo's folder membership and last-played time to its new path after the file has
// been renamed on disk. Auto-saves. No-op when the demo had no record of either.
void DemoOrganizer_NoteRenamed(const std::wstring& oldPath, const std::wstring& newPath);

// Per-main-frame watcher: notes a demo the moment the engine starts playing one, so demos
// opened from CS2's own Your Matches page or from a console `playdemo` also count as played.
// Cheap - a string compare against the last seen path.
void DemoOrganizer_PollPlaying(const std::wstring& playingPath);

// Handles one "org ..." line from the Panorama command queue. Returns true when the line was
// ours (whether or not it changed anything).
bool DemoOrganizer_HandleCommand(const std::string& line);

// Payload for the UI: { sort, rootCollapsed, groups:[...], demos:{ "<key>": {g,played} } }.
std::string DemoOrganizer_BuildJson();

} // namespace Filmmaker
