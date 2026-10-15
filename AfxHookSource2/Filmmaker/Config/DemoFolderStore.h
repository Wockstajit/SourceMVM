#pragma once

// Persists the user's list of extra demo folders to
//   %APPDATA%\HLAE\filmmaker_demo_folders.txt
// (one UTF-8 path per line). Folders that no longer exist are simply ignored at
// scan time, so a deleted/moved folder degrades gracefully.

#include <string>
#include <vector>

namespace Filmmaker {

class DemoFolderStore {
public:
	// Loads the saved folders from disk into memory.
	void Load();

	// Saves the current in-memory list to disk.
	bool Save() const;

	const std::vector<std::wstring>& Folders() const { return m_folders; }

	// Returns true if added (false if already present). Does not auto-save.
	bool Add(const std::wstring& folder);

	// Returns true if removed. Does not auto-save.
	bool Remove(const std::wstring& folder);

private:
	std::wstring FilePath() const;

	std::vector<std::wstring> m_folders;
};

} // namespace Filmmaker
