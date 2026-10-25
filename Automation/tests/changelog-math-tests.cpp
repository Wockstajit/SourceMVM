// Behavior tests for Community/ChangelogMath.h: CHANGELOG.md read as release sections, and the
// sections the What's new popup opens with for a given installed and last seen version.
#include "../../AfxHookSource2/Filmmaker/Community/ChangelogMath.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace Filmmaker;

namespace {

int g_failed = 0;

#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED line " << __LINE__ << ": " #cond "\n"; ++g_failed; } } while (0)

const char* kLog =
	"# Changelog\r\n"
	"\r\n"
	"Intro text with a - dash and ## not a heading.\r\n"
	"- an intro bullet that belongs to no release\r\n"
	"\r\n"
	"## Unreleased\r\n"
	"\r\n"
	"### Rendering\r\n"
	"\r\n"
	"- Green screen passes stay clean,\r\n"
	"  even with **depth of field** on.\r\n"
	"\r\n"
	"## 0.3.1 Hotfix 1 - 2026-10-21\r\n"
	"\r\n"
	"### Fixed\r\n"
	"\r\n"
	"- Tooltips draw on top of the `demo bar`.\r\n"
	"- See the [release](https://example.com/r) for more.\r\n"
	"\r\n"
	"## 0.3.1 - 2026-10-20\r\n"
	"\r\n"
	"- A bullet before any group.\r\n"
	"\r\n"
	"### Updates\r\n"
	"\r\n"
	"* Star bullets count too.\r\n"
	"\r\n"
	"## Notes\r\n"
	"\r\n"
	"- not a release, so not a section\r\n"
	"\r\n"
	"## 0.3.0 - 2026-10-20\r\n"
	"\r\n"
	"### Round heat map\r\n"
	"\r\n"
	"- A heat map.\r\n"
	"\r\n"
	"## 0.2.1 - 2026-10-16\r\n"
	"\r\n"
	"### Cameras\r\n"
	"\r\n"
	"- Wheel cycles cameras.\r\n";

void ParsesVersions() {
	ChangelogVersion v = ParseChangelogVersion("0.3.1-hotfix1");
	CHECK(v.Valid() && v.major == 0 && v.minor == 3 && v.patch == 1 && v.hotfix == 1);
	v = ParseChangelogVersion("0.3.1 Hotfix 12 - 2026-10-21");
	CHECK(v.hotfix == 12);
	v = ParseChangelogVersion("v10.20.300");
	CHECK(v.major == 10 && v.minor == 20 && v.patch == 300 && v.hotfix == 0);
	// A date is not a version; neither is a two-part number.
	CHECK(!ParseChangelogVersion("2026-10-21").Valid());
	CHECK(!ParseChangelogVersion("0.3").Valid());
	CHECK(!ParseChangelogVersion("").Valid());
	CHECK(!ParseChangelogVersion("none").Valid());

	CHECK(CompareChangelogVersion(ParseChangelogVersion("0.3.1"), ParseChangelogVersion("0.3.1-hotfix1")) < 0);
	CHECK(CompareChangelogVersion(ParseChangelogVersion("0.3.1-hotfix1"), ParseChangelogVersion("0.3.1 Hotfix 1")) == 0);
	CHECK(CompareChangelogVersion(ParseChangelogVersion("0.10.0"), ParseChangelogVersion("0.9.9-hotfix9")) > 0);
	CHECK(CompareChangelogVersion(ChangelogVersion{}, ParseChangelogVersion("0.0.1")) < 0);

	CHECK(ChangelogVersionLabel(ParseChangelogVersion("0.3.1-hotfix1")) == "0.3.1 hotfix 1");
	CHECK(ChangelogVersionLabel(ParseChangelogVersion("0.3.1")) == "0.3.1");
	CHECK(ChangelogVersionLabel(ChangelogVersion{}).empty());
}

void ReadsSections() {
	const std::vector<ChangelogSection> s = ParseChangelog(kLog);
	CHECK(s.size() == 5); // Unreleased, 0.3.1 Hotfix 1, 0.3.1, 0.3.0, 0.2.1 -- "Notes" is not a release
	if (s.size() != 5) return;

	CHECK(s[0].unreleased && !s[0].version.Valid() && s[0].title == "Unreleased" && s[0].date.empty());
	CHECK(s[0].groups.size() == 1 && s[0].groups[0].name == "Rendering");
	// The indented line carries on its bullet, and the bold marks are gone.
	CHECK(s[0].groups[0].items.size() == 1);
	CHECK(s[0].groups[0].items[0] == "Green screen passes stay clean, even with depth of field on.");
	// Unreleased may name the version it is meant for; the popup shows the whole heading.
	const std::vector<ChangelogSection> next = ParseChangelog("## Unreleased (0.4.0)\n\n### Maps\n\n- Another map.\n");
	CHECK(next.size() == 1 && next[0].unreleased && !next[0].version.Valid() && next[0].title == "Unreleased (0.4.0)");

	CHECK(s[1].title == "0.3.1 Hotfix 1" && s[1].date == "2026-10-21" && s[1].version.hotfix == 1);
	CHECK(s[1].groups.size() == 1 && s[1].groups[0].items.size() == 2);
	CHECK(s[1].groups[0].items[0] == "Tooltips draw on top of the demo bar.");
	CHECK(s[1].groups[0].items[1] == "See the release for more.");

	// Bullets before any ### land in an unnamed group; * bullets count.
	CHECK(s[2].title == "0.3.1" && s[2].groups.size() == 2);
	CHECK(s[2].groups[0].name.empty() && s[2].groups[0].items[0] == "A bullet before any group.");
	CHECK(s[2].groups[1].name == "Updates" && s[2].groups[1].items[0] == "Star bullets count too.");

	// The bullet under "## Notes" did not leak into 0.3.1 or 0.3.0.
	CHECK(s[3].title == "0.3.0" && s[3].groups.size() == 1 && s[3].groups[0].items.size() == 1);
}

std::vector<std::string> Titles(const std::vector<ChangelogSection>& s, const std::vector<size_t>& pick) {
	std::vector<std::string> out;
	for (size_t i : pick) out.push_back(s[i].title);
	return out;
}

void PicksWhatsNew() {
	const std::vector<ChangelogSection> s = ParseChangelog(kLog);
	using V = std::vector<std::string>;
	const ChangelogVersion none;
	const ChangelogVersion hotfix = ParseChangelogVersion("0.3.1-hotfix1");

	// Updated from 0.3.0: both 0.3.1 builds, newest first, never Unreleased or older ones.
	CHECK(Titles(s, ChangelogWhatsNew(s, hotfix, ParseChangelogVersion("0.3.0"))) == V({ "0.3.1 Hotfix 1", "0.3.1" }));
	// Updated from 0.2.1: everything since, up to the installed build.
	CHECK(Titles(s, ChangelogWhatsNew(s, ParseChangelogVersion("0.3.1"), ParseChangelogVersion("0.2.1")))
		== V({ "0.3.1", "0.3.0" }));
	// From 0.3.1 to its hotfix: only the hotfix.
	CHECK(Titles(s, ChangelogWhatsNew(s, hotfix, ParseChangelogVersion("0.3.1"))) == V({ "0.3.1 Hotfix 1" }));
	// Already seen: nothing new.
	CHECK(ChangelogWhatsNew(s, hotfix, hotfix).empty());
	// Never seen (first start with the popup): the installed release and its hotfixes so far.
	CHECK(Titles(s, ChangelogWhatsNew(s, hotfix, none)) == V({ "0.3.1 Hotfix 1", "0.3.1" }));
	CHECK(Titles(s, ChangelogWhatsNew(s, ParseChangelogVersion("0.3.1"), none)) == V({ "0.3.1" }));
	// A dev build: what is on main and the newest release.
	CHECK(Titles(s, ChangelogWhatsNew(s, none, none)) == V({ "Unreleased", "0.3.1 Hotfix 1" }));
	// A build newer than the file knows (its changelog was not updated): nothing to show.
	CHECK(ChangelogWhatsNew(s, ParseChangelogVersion("0.4.0"), ParseChangelogVersion("0.4.0")).empty());
}

// The CHANGELOG.md this checkout ships must keep reading as releases, newest first.
void ShippedChangelogParses(const char* path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) { std::cerr << "FAILED: cannot open " << path << "\n"; ++g_failed; return; }
	std::ostringstream text; text << in.rdbuf();
	const std::vector<ChangelogSection> s = ParseChangelog(text.str());
	CHECK(s.size() >= 2);
	ChangelogVersion previous;
	bool first = true;
	for (const ChangelogSection& section : s) {
		CHECK(!section.groups.empty());
		for (const ChangelogGroup& g : section.groups) CHECK(!g.items.empty());
		if (section.unreleased) { CHECK(first); first = false; continue; }
		CHECK(section.version.Valid() && section.date.size() == 10);
		if (previous.Valid()) CHECK(CompareChangelogVersion(section.version, previous) < 0);
		previous = section.version;
		first = false;
	}
}

} // namespace

int main(int argc, char** argv) {
	ParsesVersions();
	ReadsSections();
	PicksWhatsNew();
	ShippedChangelogParses(argc > 1 ? argv[1] : CHANGELOG_PATH);
	if (g_failed) { std::cerr << g_failed << " check(s) failed\n"; return 1; }
	std::cout << "ChangelogMathTests passed\n";
	return 0;
}
