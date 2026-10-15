#pragma once

// Native Windows "Select Folder" dialog (IFileOpenDialog with FOS_PICKFOLDERS).
// Must be called from a dedicated worker thread (it initializes its own COM
// apartment), never from the render thread.

#include <string>

namespace Filmmaker {

// Shows the folder picker (blocking). Returns true and fills `out` if the user
// chose a folder; false if cancelled or on error.
bool PickFolderBlocking(std::wstring& out, const wchar_t* title = L"Select a folder");

} // namespace Filmmaker
