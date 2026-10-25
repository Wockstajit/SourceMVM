#pragma once

// CHANGELOG.md read as release sections, and the pick the What's new window shows. Engine-free, so
// the parsing and the pick are unit-tested (Automation/tests/changelog-math-tests.cpp).
//
// The file is the one GitHub shows at the repository root, shipped beside the hook in
// resources/CHANGELOG.md. Its shape:
//
//   ## Unreleased                         work on main that no release carries yet
//   ## 0.3.1 Hotfix 1 - 2026-10-21        a release heading: version, optional hotfix, optional date
//   ### Fixed                             a group inside the release
//   - One change per bullet.              indented lines continue the bullet above them
//
// Versions compare as major.minor.patch, then hotfix. The installed runtime names itself
// "0.3.1-hotfix1" (installed.json); a heading names the same build "0.3.1 Hotfix 1".

#include <cctype>
#include <string>
#include <vector>

namespace Filmmaker {

struct ChangelogVersion {
	int major = -1, minor = 0, patch = 0, hotfix = 0;
	bool Valid() const { return major >= 0; }
};

struct ChangelogGroup {
	std::string name;               // "New", "Fixed", ...; empty for bullets before any ### heading
	std::vector<std::string> items; // plain text, Markdown marks removed
};

struct ChangelogSection {
	std::string title;              // "0.3.1 Hotfix 1" or "Unreleased"
	std::string date;               // YYYY-MM-DD, or empty
	ChangelogVersion version;       // invalid for Unreleased
	bool unreleased = false;
	std::vector<ChangelogGroup> groups;
};

namespace ChangelogDetail {

inline std::string Lower(std::string s) {
	for (char& c : s) c = (char)std::tolower((unsigned char)c);
	return s;
}

inline std::string Trim(const std::string& s) {
	size_t a = 0, b = s.size();
	while (a < b && std::isspace((unsigned char)s[a])) ++a;
	while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
	return s.substr(a, b - a);
}

// Reads digits at `i`; -1 when there are none.
inline int Number(const std::string& s, size_t& i) {
	if (i >= s.size() || !std::isdigit((unsigned char)s[i])) return -1;
	int v = 0;
	while (i < s.size() && std::isdigit((unsigned char)s[i]) && v < 100000) v = v * 10 + (s[i++] - '0');
	return v;
}

} // namespace ChangelogDetail

// The first "a.b.c" in `text`, plus "hotfix N" / "-hotfixN" after it. Invalid when there is none.
inline ChangelogVersion ParseChangelogVersion(const std::string& text) {
	using namespace ChangelogDetail;
	ChangelogVersion v;
	for (size_t start = 0; start < text.size(); ++start) {
		if (!std::isdigit((unsigned char)text[start]) || (start > 0 && std::isdigit((unsigned char)text[start - 1]))) continue;
		size_t i = start;
		const int a = Number(text, i);
		if (i >= text.size() || text[i] != '.') continue;
		++i;
		const int b = Number(text, i);
		if (b < 0 || i >= text.size() || text[i] != '.') continue;
		++i;
		const int c = Number(text, i);
		if (c < 0) continue;
		v.major = a; v.minor = b; v.patch = c;
		const std::string rest = Lower(text.substr(i));
		const size_t h = rest.find("hotfix");
		if (h != std::string::npos) {
			size_t j = h + 6;
			while (j < rest.size() && (rest[j] == ' ' || rest[j] == '-' || rest[j] == '_')) ++j;
			const int n = Number(rest, j);
			if (n > 0) v.hotfix = n;
		}
		return v;
	}
	return v;
}

// <0, 0, >0 like strcmp. An invalid version sorts before every valid one.
inline int CompareChangelogVersion(const ChangelogVersion& a, const ChangelogVersion& b) {
	if (a.Valid() != b.Valid()) return a.Valid() ? 1 : -1;
	if (a.major != b.major) return a.major < b.major ? -1 : 1;
	if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
	if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
	if (a.hotfix != b.hotfix) return a.hotfix < b.hotfix ? -1 : 1;
	return 0;
}

// "0.3.1" or "0.3.1 hotfix 1".
inline std::string ChangelogVersionLabel(const ChangelogVersion& v) {
	if (!v.Valid()) return std::string();
	std::string s = std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
	if (v.hotfix > 0) s += " hotfix " + std::to_string(v.hotfix);
	return s;
}

// Markdown marks a window cannot draw: **bold**, `code`, [text](url) -> text.
inline std::string ChangelogPlainText(const std::string& s) {
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		const char c = s[i];
		if (c == '*' && i + 1 < s.size() && s[i + 1] == '*') { ++i; continue; }
		if (c == '`') continue;
		if (c == '[') {
			const size_t close = s.find("](", i);
			const size_t end = close == std::string::npos ? close : s.find(')', close + 2);
			if (end != std::string::npos) {
				out += s.substr(i + 1, close - i - 1);
				i = end;
				continue;
			}
		}
		out += c;
	}
	return out;
}

inline std::vector<ChangelogSection> ParseChangelog(const std::string& markdown) {
	using namespace ChangelogDetail;
	std::vector<ChangelogSection> sections;
	bool inRelease = false; // false under a ## heading that names no release (and before the first)
	std::string* lastItem = nullptr;
	size_t pos = 0;
	while (pos <= markdown.size()) {
		size_t eol = markdown.find('\n', pos);
		if (eol == std::string::npos) eol = markdown.size();
		std::string line = markdown.substr(pos, eol - pos);
		pos = eol + 1;
		if (!line.empty() && line.back() == '\r') line.pop_back();
		const std::string trimmed = Trim(line);

		if (line.rfind("## ", 0) == 0) {
			ChangelogSection s;
			std::string heading = Trim(line.substr(3));
			const std::string lower = Lower(heading);
			s.unreleased = lower.rfind("unreleased", 0) == 0;
			if (!s.unreleased) s.version = ParseChangelogVersion(heading);
			// A YYYY-MM-DD anywhere in the heading is its date; the title is what comes before it.
			for (size_t i = 0; i + 10 <= heading.size(); ++i) {
				const std::string d = heading.substr(i, 10);
				bool ok = d[4] == '-' && d[7] == '-';
				for (size_t k = 0; ok && k < 10; ++k) if (k != 4 && k != 7 && !std::isdigit((unsigned char)d[k])) ok = false;
				if (!ok) continue;
				s.date = d;
				heading = heading.substr(0, i);
				while (!heading.empty() && (std::isspace((unsigned char)heading.back()) || heading.back() == '-' || heading.back() == '(')) heading.pop_back();
				break;
			}
			s.title = ChangelogPlainText(heading);
			inRelease = s.unreleased || s.version.Valid();
			if (inRelease) sections.push_back(std::move(s));
			lastItem = nullptr;
			continue;
		}
		if (!inRelease) continue;
		ChangelogSection& sec = sections.back();
		if (line.rfind("### ", 0) == 0) {
			ChangelogGroup g;
			g.name = ChangelogPlainText(Trim(line.substr(4)));
			sec.groups.push_back(std::move(g));
			lastItem = nullptr;
			continue;
		}
		if (trimmed.rfind("- ", 0) == 0 || trimmed.rfind("* ", 0) == 0) {
			if (sec.groups.empty()) sec.groups.push_back(ChangelogGroup{});
			sec.groups.back().items.push_back(ChangelogPlainText(Trim(trimmed.substr(2))));
			lastItem = &sec.groups.back().items.back();
			continue;
		}
		if (trimmed.empty() || line[0] == '#') { lastItem = nullptr; continue; }
		// An indented line carries on the bullet above it.
		if (lastItem && std::isspace((unsigned char)line[0])) *lastItem += " " + ChangelogPlainText(trimmed);
	}
	std::vector<ChangelogSection> releases;
	for (ChangelogSection& s : sections)
		if (!s.groups.empty()) releases.push_back(std::move(s));
	return releases;
}

// The sections the What's new window opens with, newest first, as indexes into `sections`.
//   current invalid (a dev build): Unreleased and the newest release.
//   seen invalid (first start with this window): every section of the current major.minor.patch
//     up to `current`, so a hotfix shows with its release.
//   otherwise: every release newer than `seen`, up to `current`.
inline std::vector<size_t> ChangelogWhatsNew(const std::vector<ChangelogSection>& sections,
	const ChangelogVersion& current, const ChangelogVersion& seen) {
	std::vector<size_t> out;
	if (!current.Valid()) {
		bool release = false;
		for (size_t i = 0; i < sections.size(); ++i) {
			if (sections[i].unreleased) out.push_back(i);
			else if (!release) { out.push_back(i); release = true; }
		}
		return out;
	}
	for (size_t i = 0; i < sections.size(); ++i) {
		const ChangelogSection& s = sections[i];
		if (s.unreleased || CompareChangelogVersion(s.version, current) > 0) continue;
		if (seen.Valid()) {
			if (CompareChangelogVersion(s.version, seen) <= 0) continue;
		} else if (s.version.major != current.major || s.version.minor != current.minor || s.version.patch != current.patch) {
			continue;
		}
		out.push_back(i);
	}
	return out;
}

} // namespace Filmmaker
