#pragma once

// Persists the workshop map browser's user-side ORGANISATION to
//   %APPDATA%\HLAE\filmmaker_workshop_organizer.txt
//
// Three things live here, none of them derivable from the workshop items themselves:
//
//   * the selected sort mode ("name" / "installed" / "played")
//   * the "hide training maps" toggle and the keyword list behind it (user-editable)
//   * per-map state: when a workshop map was last played
//
// ONE keyword list, not a set of categories. Categories were tried first and were worse on
// both counts: they cluttered a narrow sidebar with seven toggles, and splitting the words up
// did nothing to improve what actually matched -- the accuracy problem was WHERE the words were
// matched (see WorkshopOrganizer's note on name-only matching), not how they were grouped.
//
// Same shape and file-format style as DemoOrganizerStore (a small line-based UTF-8 text file,
// no JSON parser needed), kept as its own store because it is a different list with a different
// lifetime -- workshop maps have nothing to do with the demo browser.
//
// File format (one record per line, unknown lines are ignored so old/newer files degrade
// instead of failing):
//
//   v 2
//   sort installed
//   hidetraining 0
//   words aim,training,surf,...
//   p <lastPlayedUnix> <workshopId>
//
// The "installed" timestamp is deliberately NOT persisted: it is read from the workshop
// content folder's mtime on every scan, so a Steam update is picked up for free.

#include <string>
#include <vector>
#include <cstdint>

namespace Filmmaker {

struct WorkshopPlayRecord {
	std::string id;                    // workshop item id, as text (it does not fit an int)
	int64_t lastPlayed = 0;            // unix seconds
};

class WorkshopOrganizerStore {
public:
	void Load();
	bool Save() const;

	const std::string& Sort() const { return m_sort; }
	bool SetSort(const std::string& mode);

	bool HideTraining() const { return m_hideTraining; }
	bool SetHideTraining(bool hide);

	const std::vector<std::string>& Words() const { return m_words; }
	// Replaces the keyword list from a comma-separated string. An empty list is allowed: it
	// turns the hide toggle into a no-op without losing the toggle itself.
	bool SetWords(const std::string& commaSeparated);
	// Restores the built-in keywords.
	bool ResetWords();
	// Comma-joined, for the payload and the file.
	std::string WordsJoined() const;

	const std::vector<WorkshopPlayRecord>& PlayRecords() const { return m_played; }
	bool SetLastPlayed(const std::string& workshopId, int64_t whenUnix);

private:
	std::wstring FilePath() const;

	std::string m_sort = "name";
	bool m_hideTraining = false;
	std::vector<std::string> m_words;
	std::vector<WorkshopPlayRecord> m_played;
};

} // namespace Filmmaker
