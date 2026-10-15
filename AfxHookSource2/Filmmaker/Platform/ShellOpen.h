#pragma once

// Handing things to the Windows shell from inside the injected DLL: show a file in Explorer, open a
// web page in the user's browser, put text on the clipboard. Each shell call runs on its own
// worker thread with its own COM apartment (see ShellOpen.cpp for why the game's thread is not
// safe for it), so none of them block the frame.

#include <string>

namespace Filmmaker {

// Opens the file's folder in Explorer with the file selected, reusing an Explorer window already
// open (re-pointed at that folder) and raising it over the game. Path is a full file path.
void ShowInExplorerAsync(const std::wstring& filePath);

// Opens an http(s) URL in the user's default browser. Anything else is refused.
void OpenUrlAsync(const std::wstring& url);

// Replaces the clipboard's text with `text`. False when the clipboard could not be opened.
bool CopyTextToClipboard(const std::wstring& text);

} // namespace Filmmaker
