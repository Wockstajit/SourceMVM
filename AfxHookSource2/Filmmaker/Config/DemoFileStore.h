#pragma once

// Persists demos the user added ONE FILE AT A TIME to
//   %APPDATA%\HLAE\filmmaker_demo_files.txt
// (one UTF-8 absolute path per line).
//
// Separate from DemoFolderStore because it answers a different question. A folder is a
// standing subscription ("list whatever .dem files are in here, now and later"); an entry
// here is a single explicit file the user pointed at, which must appear even though nothing
// else in its directory should. Adding a demo's parent folder instead would drag in every
// other demo sitting next to it, which is exactly what this avoids.
//
// Paths whose file no longer exists are skipped at scan time, so a moved/deleted demo
// degrades to "not listed" instead of failing.

#include <string>
#include <vector>

namespace Filmmaker {

class DemoFileStore {
public:
	// Loads the saved paths from disk into memory.
	void Load();

	// Saves the current in-memory list to disk.
	bool Save() const;

	const std::vector<std::wstring>& Files() const { return m_files; }

	// Returns true if added (false if already present, case-insensitively). Does not auto-save.
	bool Add(const std::wstring& path);

	// A file that was RENAMED is still the file the user picked, so the entry follows it. Without
	// this an individually added demo drops out of the list the moment it is renamed, and looks
	// deleted. Returns true when an entry moved (the caller saves). Deliberately still no
	// Remove(): the browser's delete button hides a demo through HiddenDemoStore (which works for
	// individually added files too), and re-picking the file un-hides it.
	bool Rename(const std::wstring& oldPath, const std::wstring& newPath);

private:
	std::wstring FilePath() const;

	std::vector<std::wstring> m_files;
};

} // namespace Filmmaker
