#include "WorkshopOrganizerStore.h"

#include "../../hlaeFolder.h"

#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cstdlib>
#include <cctype>

namespace Filmmaker {

namespace {

// The built-in "this is a training/utility map, not a map I want to play" keyword list.
// Matched as SUBSTRINGS against the map name and the workshop title only -- never the
// description -- so the words here have to be safe as substrings of a real map name.
//
// Two rules learned from getting this wrong in-game:
//   * No bare "train": it hides de_train. Hence "trainer"/"training" spelled out.
//   * No word that only ever appears in prose ("practice your aim"), because the description is
//     not matched at all any more.
const char* kDefaultWords =
	"aim,training,trainer,yprac,prac,crashz,"
	"movement,surf,bhop,kz,strafe,parkour,"
	"hub,benchmark,warmup,warm up,"
	"prefire,rush,recoil,spray,reflex,flick,"
	"config,generator,cfg";

// Keywords ride a single-line record file and a newline-separated command queue, so control
// characters and commas-in-words have to go. Also lower-cased, because every match this store
// feeds is case-insensitive.
std::string SanitizeWord(const std::string& in) {
	std::string out;
	out.reserve(in.size());
	for (char c : in) {
		if (c == '\r' || c == '\n' || c == '\t' || c == ',')
			continue;
		out.push_back((char)std::tolower((unsigned char)c));
	}
	size_t b = out.find_first_not_of(' ');
	size_t e = out.find_last_not_of(' ');
	if (b == std::string::npos)
		return std::string();
	out = out.substr(b, e - b + 1);
	if (out.size() > 32)
		out.resize(32);
	return out;
}

std::vector<std::string> SplitWords(const std::string& commaSeparated) {
	std::vector<std::string> words;
	size_t start = 0;
	while (start <= commaSeparated.size()) {
		const size_t comma = commaSeparated.find(',', start);
		const size_t end = (comma == std::string::npos) ? commaSeparated.size() : comma;
		const std::string word = SanitizeWord(commaSeparated.substr(start, end - start));
		// A duplicate word costs a wasted compare per tile and reads as a typo in the editor.
		if (!word.empty() && std::find(words.begin(), words.end(), word) == words.end())
			words.push_back(word);
		if (comma == std::string::npos)
			break;
		start = comma + 1;
		// A hand-edited file could carry hundreds of words; the editor cannot show them and
		// every one costs a substring search per tile.
		if (words.size() >= 80)
			break;
	}
	return words;
}

// Workshop ids are decimal digits only. Anything else in the file is a corrupt/hand-edited
// line and is dropped rather than trusted into a lookup key.
bool IsWorkshopId(const std::string& s) {
	if (s.empty() || s.size() > 20)
		return false;
	for (char c : s)
		if (c < '0' || c > '9')
			return false;
	return true;
}

// Splits "<token> <rest>" at the first space. Returns false when there is no token.
bool SplitFirst(const std::string& line, std::string& token, std::string& rest) {
	const size_t sp = line.find(' ');
	if (sp == std::string::npos) {
		token = line;
		rest.clear();
		return !token.empty();
	}
	token = line.substr(0, sp);
	rest = line.substr(sp + 1);
	return !token.empty();
}

} // namespace

std::wstring WorkshopOrganizerStore::FilePath() const {
	std::wstring path = GetHlaeRoamingAppDataFolderW();
	if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
		path += L'\\';
	path += L"filmmaker_workshop_organizer.txt";
	return path;
}

void WorkshopOrganizerStore::Load() {
	m_sort = "name";
	m_hideTraining = false;
	m_played.clear();
	m_words = SplitWords(kDefaultWords);

	std::ifstream f(std::filesystem::path(FilePath()), std::ios::binary);
	if (!f.is_open())
		return;

	std::string line;
	while (std::getline(f, line)) {
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
			line.pop_back();
		if (line.empty())
			continue;

		std::string tag, rest;
		if (!SplitFirst(line, tag, rest))
			continue;

		if (tag == "sort") {
			SetSort(rest);
		} else if (tag == "hidetraining") {
			m_hideTraining = (std::atoi(rest.c_str()) != 0);
		} else if (tag == "words") {
			m_words = SplitWords(rest);
		} else if (tag == "p") {
			// p <lastPlayed> <workshopId>
			std::string playedText, id;
			if (!SplitFirst(rest, playedText, id))
				continue;
			if (!IsWorkshopId(id))
				continue;
			WorkshopPlayRecord r;
			r.id = id;
			r.lastPlayed = (int64_t)_atoi64(playedText.c_str());
			if (r.lastPlayed <= 0)
				continue;
			m_played.push_back(r);
		}
		// "c <id> <on> <words>" lines are v1 category records. Ignored on purpose: the category
		// system is gone, and silently dropping them is what lets an older file still load.
	}
}

bool WorkshopOrganizerStore::Save() const {
	std::ofstream f(std::filesystem::path(FilePath()), std::ios::binary | std::ios::trunc);
	if (!f.is_open())
		return false;
	f << "v 2\n";
	f << "sort " << m_sort << "\n";
	f << "hidetraining " << (m_hideTraining ? 1 : 0) << "\n";
	f << "words " << WordsJoined() << "\n";
	for (const auto& r : m_played) {
		if (r.lastPlayed <= 0)
			continue;
		f << "p " << r.lastPlayed << " " << r.id << "\n";
	}
	return true;
}

std::string WorkshopOrganizerStore::WordsJoined() const {
	std::string out;
	for (size_t i = 0; i < m_words.size(); ++i) {
		if (i)
			out += ',';
		out += m_words[i];
	}
	return out;
}

bool WorkshopOrganizerStore::SetSort(const std::string& mode) {
	// Whitelist: an unknown mode from a stale file or a typo'd command must not leave the UI
	// with a sort it cannot render.
	static const char* kModes[] = { "name", "installed", "played" };
	const std::string m = SanitizeWord(mode);
	for (const char* k : kModes) {
		if (m == k) {
			if (m_sort == m)
				return false;
			m_sort = m;
			return true;
		}
	}
	return false;
}

bool WorkshopOrganizerStore::SetHideTraining(bool hide) {
	if (m_hideTraining == hide)
		return false;
	m_hideTraining = hide;
	return true;
}

bool WorkshopOrganizerStore::SetWords(const std::string& commaSeparated) {
	std::vector<std::string> words = SplitWords(commaSeparated);
	if (words == m_words)
		return false;
	m_words = words;
	return true;
}

bool WorkshopOrganizerStore::ResetWords() {
	std::vector<std::string> words = SplitWords(kDefaultWords);
	if (words == m_words)
		return false;
	m_words = words;
	return true;
}

bool WorkshopOrganizerStore::SetLastPlayed(const std::string& workshopId, int64_t whenUnix) {
	if (!IsWorkshopId(workshopId) || whenUnix <= 0)
		return false;
	for (auto& r : m_played) {
		if (r.id != workshopId)
			continue;
		if (r.lastPlayed == whenUnix)
			return false;
		r.lastPlayed = whenUnix;
		return true;
	}
	WorkshopPlayRecord r;
	r.id = workshopId;
	r.lastPlayed = whenUnix;
	m_played.push_back(r);
	return true;
}

} // namespace Filmmaker
