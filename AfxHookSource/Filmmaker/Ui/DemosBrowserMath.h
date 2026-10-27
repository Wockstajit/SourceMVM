#pragma once

// The demos browser's list logic, engine-free (Ui/DemosBrowser.h draws it): what each demo row
// holds, the order the rail shows rows and folder headers in, the jump-to search, and the text the
// page derives (map names, lengths, "last played" ages, cropped names, a typed file name). Rules are
// CS2's demos browser (FilmmakerGuiJs.h and DemoOrganizerJs.h, its organiser): sorts compare plain
// code points, ties fall back to newest first, a folder lists its demos in the active sort, and
// "No folder" heads the loose demos only while a folder exists.
// Automation/tests/source-demos-browser-math-tests.cpp covers it.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace SourceFilmmaker {
namespace Ui {
namespace DemosBrowserMath {

// What the header's network protocol says about playing the demo here.
enum class Compat {
	Current,   // 24: the current Orange Box format, plays as it is
	Old,       // 7: an old CS:S (v34) demo, converted the first time it is played
	Converted, // 7 with its converted copy ready
	Other,     // another protocol: not one the hook knows
	Unknown,   // no readable header
};

struct DemoRow {
	int index = -1;        // the library's slot: what watch, hide, renamedemo and openfolder take
	std::string fileName;  // "match.dem"
	std::string folder;    // its folder, UTF-8
	std::string key;       // the organiser's key: lower case, backslashes (NormalizeDemoKey)
	std::string map;       // "de_dust2", empty when the header has none
	std::string recorder;  // the header's client name
	std::string server;    // the header's server name
	float seconds = 0;     // playback time
	int ticks = 0;         // playback ticks
	int64_t date = 0;      // file time, unix s
	int protocol = 0;      // network protocol
	Compat compat = Compat::Unknown;
	int group = -1;        // the user's folder, -1 for none
	int64_t played = 0;    // last played, unix s, 0 never
};

struct DemoGroup {
	int id = -1;
	std::string name;
	bool collapsed = false;
};

enum class Sort { Played, Date, DateAsc, Map, Name, Count };

inline const char* SortName(Sort s) {
	switch (s) {
	case Sort::Played: return "played";
	case Sort::DateAsc: return "dateasc";
	case Sort::Map: return "map";
	case Sort::Name: return "name";
	default: return "date";
	}
}

inline const char* SortCaption(Sort s) {
	switch (s) {
	case Sort::Played: return "Recently played";
	case Sort::DateAsc: return "Oldest first";
	case Sort::Map: return "Map name";
	case Sort::Name: return "Demo name";
	default: return "Newest first";
	}
}

inline Sort SortFromName(const std::string& name) {
	for (int i = 0; i < (int)Sort::Count; ++i)
		if (name == SortName((Sort)i)) return (Sort)i;
	return Sort::Date;
}

inline std::string Lower(const std::string& s) {
	std::string out = s;
	for (char& c : out)
		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
	return out;
}

// Counter-Strike: Source's own maps by their titles; any other map keeps its file name, and a demo
// with no map in its header shows "Demo".
inline std::string MapDisplay(const std::string& map) {
	static const char* const kNames[][2] = {
		{ "de_dust2", "Dust II" }, { "de_dust", "Dust" }, { "de_inferno", "Inferno" }, { "de_nuke", "Nuke" },
		{ "de_train", "Train" }, { "de_aztec", "Aztec" }, { "de_cbble", "Cobblestone" }, { "de_piranesi", "Piranesi" },
		{ "de_port", "Port" }, { "de_prodigy", "Prodigy" }, { "de_tides", "Tides" }, { "de_chateau", "Chateau" },
		{ "cs_office", "Office" }, { "cs_italy", "Italy" }, { "cs_assault", "Assault" }, { "cs_compound", "Compound" },
		{ "cs_havana", "Havana" }, { "cs_militia", "Militia" },
	};
	if (map.empty()) return "Demo";
	const std::string key = Lower(map);
	for (const auto& entry : kNames)
		if (key == entry[0]) return entry[1];
	return map;
}

// The map's kind for the tile's badge: "de", "cs", "aim" from "de_dust2"; empty without a prefix.
inline std::string MapPrefix(const std::string& map) {
	const size_t cut = map.find('_');
	if (cut == std::string::npos || cut == 0 || cut > 4) return std::string();
	std::string out = map.substr(0, cut);
	for (char& c : out)
		if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
	return out;
}

// The demo's name as the page shows it: its file name without ".dem", else the map's name.
inline std::string DemoName(const DemoRow& d) {
	std::string name = d.fileName;
	if (name.size() > 4 && Lower(name.substr(name.size() - 4)) == ".dem") name.resize(name.size() - 4);
	return name.empty() ? MapDisplay(d.map) : name;
}

// "41:02", or "1:02:03" past an hour; empty for no length.
inline std::string Duration(float seconds) {
	if (!(seconds > 0)) return std::string();
	const long total = (long)(seconds + 0.5f);
	char text[32];
	if (total >= 3600) std::snprintf(text, sizeof(text), "%ld:%02ld:%02ld", total / 3600, (total / 60) % 60, total % 60);
	else std::snprintf(text, sizeof(text), "%ld:%02ld", total / 60, total % 60);
	return text;
}

// CS2's "Last played: <ago>": never, just now (< 90 s), N min, N hr (< 2 days), N days (< 30 days),
// then N mo in 30-day months.
inline std::string Ago(int64_t played, int64_t now) {
	if (played <= 0) return "never";
	const int64_t d = now > played ? now - played : 0;
	char text[32];
	if (d < 90) return "just now";
	if (d < 3600) std::snprintf(text, sizeof(text), "%lld min ago", (long long)(d / 60));
	else if (d < 172800) std::snprintf(text, sizeof(text), "%lld hr ago", (long long)(d / 3600));
	else if (d < 2592000) std::snprintf(text, sizeof(text), "%lld days ago", (long long)(d / 86400));
	else std::snprintf(text, sizeof(text), "%lld mo ago", (long long)(d / 2592000));
	return text;
}

// The length of the UTF-8 character starting at s[i] (1 for a stray continuation byte).
inline size_t CharBytes(const std::string& s, size_t i) {
	const unsigned char c = (unsigned char)s[i];
	const size_t n = c < 0x80 ? 1 : ((c >> 5) == 6 ? 2 : ((c >> 4) == 14 ? 3 : ((c >> 3) == 30 ? 4 : 1)));
	return i + n <= s.size() ? n : 1;
}

// CS2's cropName: a name longer than head + tail + 1 characters keeps its first head and last tail
// characters around an ellipsis. Counts UTF-8 characters, so none is cut in half.
inline std::string CropName(const std::string& s, size_t head = 16, size_t tail = 7) {
	std::vector<size_t> starts;
	for (size_t i = 0; i < s.size(); i += CharBytes(s, i)) starts.push_back(i);
	if (starts.size() <= head + tail + 1) return s;
	return s.substr(0, starts[head]) + "\xE2\x80\xA6" + s.substr(starts[starts.size() - tail]);
}

// A typed demo name as a file name: characters Windows refuses (control characters and
// < > : " / \ | ? *) dropped, leading spaces and trailing spaces and dots trimmed, ".dem" added
// unless it already ends so. Empty when nothing usable is left.
inline std::string DemoFileName(const std::string& typed) {
	std::string out;
	for (char c : typed) {
		const unsigned char u = (unsigned char)c;
		if (u < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') continue;
		out += c;
	}
	size_t first = 0;
	while (first < out.size() && out[first] == ' ') ++first;
	out = out.substr(first);
	while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
	if (!(out.size() >= 4 && Lower(out.substr(out.size() - 4)) == ".dem")) out += ".dem";
	return out.size() > 4 ? out : std::string(); // ".dem" alone names nothing
}

// a before b in the sort; ties newest first, then by key so the order never flickers.
inline bool Before(const DemoRow& a, const DemoRow& b, Sort sort) {
	switch (sort) {
	case Sort::Played:
		if (a.played != b.played) return a.played > b.played;
		break;
	case Sort::DateAsc:
		if (a.date != b.date) return a.date < b.date;
		break;
	case Sort::Map: {
		const std::string ma = Lower(MapDisplay(a.map)), mb = Lower(MapDisplay(b.map));
		if (ma != mb) return ma < mb;
		break;
	}
	case Sort::Name: {
		const std::string na = Lower(DemoName(a)), nb = Lower(DemoName(b));
		if (na != nb) return na < nb;
		break;
	}
	default:
		break;
	}
	if (a.date != b.date) return a.date > b.date;
	return a.key < b.key;
}

// One line of the rail: a folder header (group, its demo count) or a demo (row, an index into the
// demos given).
struct PlanItem {
	bool header = false;
	int group = -1;  // the header's folder id, -1 for No folder
	int count = 0;   // a header's demos
	int row = -1;    // a demo's index in demos
};

// The rail, top to bottom: each folder in store order with its demos in the sort, then, while any
// folder exists, "No folder" with the loose demos; with no folders, the loose demos alone. A demo
// whose folder is gone counts as loose. Collapsed folders keep their header and hide their demos.
inline std::vector<PlanItem> Plan(const std::vector<DemoRow>& demos, const std::vector<DemoGroup>& groups, Sort sort,
	bool rootCollapsed) {
	auto known = [&groups](int id) {
		for (const auto& g : groups)
			if (g.id == id) return true;
		return false;
	};
	auto members = [&](int id) {
		std::vector<int> rows;
		for (int i = 0; i < (int)demos.size(); ++i) {
			const int g = known(demos[i].group) ? demos[i].group : -1;
			if (g == id) rows.push_back(i);
		}
		// Insertion sort keeps this header-only and is plenty for a demo list.
		for (size_t i = 1; i < rows.size(); ++i)
			for (size_t j = i; j > 0 && Before(demos[rows[j]], demos[rows[j - 1]], sort); --j) std::swap(rows[j], rows[j - 1]);
		return rows;
	};
	std::vector<PlanItem> plan;
	auto add = [&](int id, bool collapsed, bool withHeader) {
		const std::vector<int> rows = members(id);
		if (withHeader) {
			PlanItem h;
			h.header = true;
			h.group = id;
			h.count = (int)rows.size();
			plan.push_back(h);
			if (collapsed) return;
		}
		for (int r : rows) {
			PlanItem t;
			t.group = id;
			t.row = r;
			plan.push_back(t);
		}
	};
	for (const auto& g : groups) add(g.id, g.collapsed, true);
	add(-1, rootCollapsed, !groups.empty());
	return plan;
}

// One jump-to result: the demo (an index into demos) and why it matched ("map: Dust II").
struct SearchHit {
	int row = -1;
	std::string why;
};

// CS2's search: a case-insensitive substring of the trimmed query, in list order, at most max hits,
// checking the map's name, the demo's name, then who recorded it and the server (CS2 checks player
// names; a CS:S header has the recorder and server instead).
inline std::vector<SearchHit> Search(const std::vector<DemoRow>& demos, const std::string& query, size_t max = 12) {
	std::vector<SearchHit> hits;
	size_t first = 0, last = query.size();
	while (first < last && query[first] == ' ') ++first;
	while (last > first && query[last - 1] == ' ') --last;
	const std::string q = Lower(query.substr(first, last - first));
	if (q.empty()) return hits;
	for (int i = 0; i < (int)demos.size() && hits.size() < max; ++i) {
		const DemoRow& d = demos[i];
		const std::pair<const char*, std::string> fields[] = {
			{ "map", MapDisplay(d.map) }, { "map", d.map }, { "demo", DemoName(d) },
			{ "recorded by", d.recorder }, { "server", d.server },
		};
		for (const auto& f : fields) {
			if (!f.second.empty() && Lower(f.second).find(q) != std::string::npos) {
				hits.push_back({ i, std::string(f.first) + ": " + f.second });
				break;
			}
		}
	}
	return hits;
}

} // namespace DemosBrowserMath
} // namespace Ui
} // namespace SourceFilmmaker
