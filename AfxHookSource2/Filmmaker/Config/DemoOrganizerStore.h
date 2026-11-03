#pragma once

// Persists the demo browser's user-side ORGANISATION to
//   %APPDATA%\HLAE\filmmaker_demo_organizer.txt
//
// Three things live here, all of them chosen by the user in the Downloaded page's list
// toolbar and none of them derivable from the demo files themselves:
//
//   * the selected sort mode (e.g. "recently played")
//   * user-created folders (name + collapsed state + display order)
//   * per-demo state: when it was last played, and which folder it sits in
//
// Same shape and file format style as DemoFolderStore / HiddenDemoStore (a small line-based
// UTF-8 text file, no JSON parser needed), kept as its own store because it is a different
// list with a different lifetime.
//
// File format (one record per line, unknown lines are ignored so old/newer files degrade
// instead of failing):
//
//   v 1
//   sort played
//   rootcollapsed 0
//   g <id> <collapsed 0|1> <name>
//   p <lastPlayedUnix> <groupId|-1> <normalised path>
//
// Demo paths are stored NORMALISED (lower-case, backslashes) because that is also the key
// the Panorama UI can rebuild from the payload it already has (folder + '\' + fileName),
// which is what lets the two sides join without a second path field in the demo payload.

#include <string>
#include <vector>
#include <cstdint>

namespace Filmmaker {

struct DemoOrganizerGroup {
	int id = -1;              // stable identity; what the UI sends back in commands
	std::string name;         // UTF-8 display name
	bool collapsed = false;   // folder header toggled shut
};

struct DemoOrganizerRecord {
	std::wstring key;         // normalised demo path (see NormalizeDemoKey)
	int64_t lastPlayed = 0;   // unix seconds, 0 = never played through this tool
	int groupId = -1;         // -1 = not in a user folder
};

// Lower-cases and switches separators to '\' so the engine's path form, our scan's path form
// and the UI's folder+fileName form all reduce to the same key.
std::wstring NormalizeDemoKey(const std::wstring& path);

class DemoOrganizerStore {
public:
	// fileName is the store's file in %APPDATA%\HLAE\; CS:S keeps its own beside CS2's.
	explicit DemoOrganizerStore(const wchar_t* fileName = L"filmmaker_demo_organizer.txt") : m_fileName(fileName) {}

	void Load();
	bool Save() const;

	const std::string& Sort() const { return m_sort; }
	bool SetSort(const std::string& mode);

	bool RootCollapsed() const { return m_rootCollapsed; }
	bool SetRootCollapsed(bool collapsed);

	const std::vector<DemoOrganizerGroup>& Groups() const { return m_groups; }
	const std::vector<DemoOrganizerRecord>& Records() const { return m_records; }

	// Returns the new folder id, or -1 when the name is empty/unusable.
	int CreateGroup(const std::string& name);
	bool RenameGroup(int id, const std::string& name);
	// Deleting a folder never touches demos: its members simply fall back to ungrouped.
	bool DeleteGroup(int id);
	bool SetGroupCollapsed(int id, bool collapsed);

	// groupId -1 removes the demo from whatever folder it was in.
	bool Assign(const std::wstring& path, int groupId);
	bool SetLastPlayed(const std::wstring& path, int64_t whenUnix);

	// A demo that MOVED keeps its record. The key is the path, so a rename would otherwise read
	// as "this demo is gone and a different one appeared": the new file would sit outside every
	// user folder and claim it had never been played. Returns false when there was nothing to
	// carry, or when something is already recorded at the new path (that record wins -- it
	// describes the file that is actually there now).
	bool Rekey(const std::wstring& oldPath, const std::wstring& newPath);

	int64_t LastPlayed(const std::wstring& path) const;

	// One organiser command, the part after "org ": sort <mode>, newfolder <name>, rename <id> <name>,
	// delfolder <id>, collapse <id|-1> <0|1> (-1 is the No folder bucket), assign <id|-1> <demo key>
	// (the key last, so it may hold spaces). True when it changed something; the caller saves.
	bool Apply(const std::string& command);

private:
	std::wstring FilePath() const;
	DemoOrganizerRecord* Find(const std::wstring& key);
	const DemoOrganizerRecord* Find(const std::wstring& key) const;
	// Drops records that carry no information (never played, no folder).
	void PruneEmpty();

	const wchar_t* m_fileName;
	std::string m_sort = "date";
	bool m_rootCollapsed = false;
	std::vector<DemoOrganizerGroup> m_groups;
	std::vector<DemoOrganizerRecord> m_records;
	int m_nextGroupId = 1;
};

} // namespace Filmmaker
