#pragma once

// Blocking "pick one or more .dem files" shell dialog.
//
// Deliberately a separate file from FolderPicker.h: that one answers "which directory should
// be scanned", this one answers "which individual demo files should be listed". They are
// different questions with different dialog options (multi-select + a file-type filter here),
// and the demo browser now offers both as distinct actions.
//
// Same threading contract as PickFolderBlocking: this BLOCKS and pumps a modal Win32 dialog,
// so it must never be called from the render thread or the main game frame. Call it from a
// detached worker (see Filmmaker::RequestAddDemo).

#include <string>
#include <vector>

namespace Filmmaker {

// Shows a multi-select open dialog filtered to *.dem. Returns true and fills `out` with the
// chosen absolute paths when the user confirms; returns false (leaving `out` empty) when the
// user cancels or the dialog cannot be created.
bool PickDemoFilesBlocking(std::vector<std::wstring>& out, const wchar_t* title = L"Select demo files");

} // namespace Filmmaker
