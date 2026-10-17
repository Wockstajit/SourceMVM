#pragma once

// <sstream> in full, not a forward declaration: std::ostringstream is a TYPEDEF, so declaring it
// by hand is ill-formed.
#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Persistent, reusable Filmmaker preferences. Shot-specific camera/path/follow data deliberately
// stays in its owning subsystem and is never copied into this global store.
void FilmmakerSettings_Load();
void FilmmakerSettings_RunFrame();
void FilmmakerSettings_Flush();
// Explicit user resets/loads replace saved preferences even during an automatic quiet reset.
void FilmmakerSettings_AcceptCurrent();

bool FilmmakerSettings_UiBool(const char* key, bool fallback);
std::string FilmmakerSettings_UiString(const char* key, const char* fallback);
void FilmmakerSettings_SetUi(const char* key, const char* value);
// Bumped whenever a persistent setting changes, UI-only keys excepted: what a frame looks like may
// have changed (the scrub frame cache drops older frames, ScrubFrameCache.cpp).
uint64_t FilmmakerSettings_PictureRevision();

// Appends ",\"ui\":{...}" -- every UI key that has actually been stored, as one object.
//
// A panel that wants to restore N pieces of disclosure state does NOT need N fields added to its
// own state JSON, and more importantly a key that is ABSENT here is distinguishable from one
// stored as "0". That is what lets a panel leave a control the operator has never touched at its
// build-time default instead of forcing every default to be duplicated on the C++ side.
void FilmmakerSettings_AppendUiJson(std::ostringstream& o);

// Restore one group of settings to the values the modules were CONSTRUCTED with.
//
// The defaults are not a hand-written table that can drift: a snapshot is taken at the top of
// Load(), before the saved file is parsed, when every module still holds its own member
// initialisers. That snapshot IS the defaults, by construction. Resetting re-runs the same Load*
// path against it, so a reset and a fresh install land on exactly the same state.
//
// group: "viewFx" | "smoothView" | "handheld" | "viewmodel" | "camFov" | "lighting" | "flags" | "all".
// Unknown names are ignored and reported false.
bool FilmmakerSettings_ResetGroup(const char* group);

// A reset that MUST NOT reach the settings file.
//
// "Keep between demos = off" restores modules to their defaults on every level change. Those are
// the same modules this store snapshots every frame, so the autosave would take the level-local
// reset and overwrite the operator's saved tint / sun / camera feel on disk -- switching a
// preference off would silently delete the preferences. (That is exactly why the destructive
// branch was removed from EffectsPersistence once already; it is the reason this hook exists.)
//
// Call it immediately BEFORE issuing such a reset. The store then holds the file at its current
// contents and stops tracking changes until the live state moves AWAY from the post-reset state --
// i.e. until the operator actually edits something, which is saved normally. Idempotent; calling
// it again just restarts the window.
void FilmmakerSettings_BeginQuietReset();

// A practice session (Clippy's tutorial, Config/PracticeSessionFlag.h). Begin writes the user's
// pending edits, then puts every setting back to its construction default (the UI keys and the
// editor layout included; the render output folder is kept) and saves nothing until End. End
// applies the settings file again, so the user's settings are exactly what they were. Main thread.
void FilmmakerSettings_BeginPractice();
void FilmmakerSettings_EndPractice();

// Console entry used by Panorama to persist UI-only state:
//   mirv_filmmaker settings ui <key> <value>
void FilmmakerSettings_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
