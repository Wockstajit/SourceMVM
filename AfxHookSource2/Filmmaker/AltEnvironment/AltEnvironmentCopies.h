#pragma once

// ALTERNATIVE ENVIRONMENT, the copies on disk: environment.dem / environment-b.dem in the demo's
// project folder, each with a .src marker naming the file it was written from (and that file's size
// and write time), the original demo and everything the copy depends on (the "spec key").
//
// Kept apart from AltEnvironment.cpp, which reads and writes the demo's project settings: only this
// side ever looks at the raw file the engine plays, and project data is keyed by the original demo
// (Automation/tests/demo-project-keying-tests.mjs).

#include <string>

namespace Filmmaker::AltEnvironment::Copies {

// The engine's spelling of a path matches ours (separators and case aside).
bool SameFile(std::wstring a, std::wstring b);

std::wstring SlotPath(const std::wstring& original, int slot, bool forSave);
// The slot a new copy is written to: the one the engine is not playing.
int FreeSlot(const std::wstring& original);
// The slot that holds `original`'s copy of `source` written for `specKey`, or -1.
int CurrentSlot(const std::wstring& original, const std::wstring& source, const std::string& specKey);
void WriteMarker(const std::wstring& copy, const std::wstring& source, const std::wstring& original, const std::string& specKey);

// What a copy is written from: the scrub keyframe copy when it is ready (so scrubbing stays fast),
// else the repaired-or-original demo. Empty while the demo is still being checked.
std::wstring BaseFor(const std::wstring& original);

// The engine plays one of the environment slots / exactly this file.
bool PlayingCopy();
bool IsPlaying(const std::wstring& file);

// Deletes every other demo's copies, and this demo's slots other than `keepSlot` and the playing one.
void KeepOnly(const std::wstring& original, const std::wstring& playingFile, int keepSlot);

} // namespace Filmmaker::AltEnvironment::Copies
