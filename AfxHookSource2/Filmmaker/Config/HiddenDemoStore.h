#pragma once

// Persists the demos the user has removed from the Downloaded list to
//   %APPDATA%\HLAE\filmmaker_hidden_demos.txt
// (one UTF-8 absolute path per line).
//
// "Remove" deliberately does NOT touch the .dem on disk -- the file is large and
// re-downloading a match is not always possible, so the delete button only hides the entry.
// Re-adding the containing folder (or the demo itself) un-hides it again, which is why this
// list is keyed on the demo PATH rather than on a list index.
//
// Same shape and file format as DemoFolderStore, kept as its own store because it is a
// different list with a different lifetime.

#include <string>
#include <vector>

namespace Filmmaker {

class HiddenDemoStore {
public:
	// fileName is the store's file in %APPDATA%\HLAE\; CS:S keeps its own beside CS2's.
	explicit HiddenDemoStore(const wchar_t* fileName = L"filmmaker_hidden_demos.txt") : m_fileName(fileName) {}

	// Loads the saved paths from disk into memory.
	void Load();

	// Saves the current in-memory list to disk.
	bool Save() const;

	const std::vector<std::wstring>& Paths() const { return m_paths; }

	// True if this exact demo path is hidden (case-insensitive).
	bool Contains(const std::wstring& path) const;

	// Returns true if added (false if already present). Does not auto-save.
	bool Add(const std::wstring& path);

	// Returns true if removed. Does not auto-save.
	bool Remove(const std::wstring& path);

	// Un-hides every demo that lives under `folder` (at any depth). Returns how many were
	// un-hidden. This is what makes "add the folder again and it comes back" work.
	size_t RemoveUnder(const std::wstring& folder);

	// Un-hides everything. Returns how many were un-hidden.
	size_t Clear();

private:
	std::wstring FilePath() const;

	const wchar_t* m_fileName;
	std::vector<std::wstring> m_paths;
};

} // namespace Filmmaker
