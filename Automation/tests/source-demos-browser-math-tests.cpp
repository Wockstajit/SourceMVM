// DemosBrowserMath, the CS:S demos browser's list rules: the rail's order with and without folders,
// each sort and its tie-break, the jump-to search, and the text the page derives (map names, lengths,
// ages, cropped names, a typed file name).

#include "../../AfxHookSource/Filmmaker/Ui/DemosBrowserMath.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace SourceFilmmaker::Ui::DemosBrowserMath;

static int g_failed = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n", __LINE__, #x); ++g_failed; } } while (0)

static DemoRow Row(const char* file, const char* map, int64_t date, int group = -1, int64_t played = 0) {
	DemoRow d;
	d.fileName = file;
	d.map = map;
	d.date = date;
	d.group = group;
	d.played = played;
	d.key = Lower(std::string("c:\\demos\\") + file);
	return d;
}

// The plan's demo rows, as their file names.
static std::vector<std::string> Files(const std::vector<DemoRow>& demos, const std::vector<PlanItem>& plan) {
	std::vector<std::string> out;
	for (const auto& p : plan) out.push_back(p.header ? "#" + std::to_string(p.group) + ":" + std::to_string(p.count) : demos[p.row].fileName);
	return out;
}

static void TestPlanWithoutFolders() {
	const std::vector<DemoRow> demos = { Row("b.dem", "de_nuke", 300), Row("a.dem", "cs_office", 100), Row("c.dem", "de_aztec", 200) };
	const std::vector<std::string> byDate = { "b.dem", "c.dem", "a.dem" };
	CHECK(Files(demos, Plan(demos, {}, Sort::Date, false)) == byDate);
	// No folder exists: no header, and the root's collapse is ignored.
	CHECK(Files(demos, Plan(demos, {}, Sort::Date, true)) == byDate);
	const std::vector<std::string> oldest = { "a.dem", "c.dem", "b.dem" };
	CHECK(Files(demos, Plan(demos, {}, Sort::DateAsc, false)) == oldest);
	// Map name sorts by the map's title: Aztec, Nuke, Office.
	const std::vector<std::string> byMap = { "c.dem", "b.dem", "a.dem" };
	CHECK(Files(demos, Plan(demos, {}, Sort::Map, false)) == byMap);
	const std::vector<std::string> byName = { "a.dem", "b.dem", "c.dem" };
	CHECK(Files(demos, Plan(demos, {}, Sort::Name, false)) == byName);
}

static void TestPlanWithFolders() {
	std::vector<DemoRow> demos = { Row("one.dem", "de_dust2", 500, 2, 10), Row("two.dem", "de_dust2", 400, 7),
		Row("three.dem", "de_dust2", 300, 2, 90), Row("four.dem", "de_dust2", 200), Row("five.dem", "de_dust2", 100, 9) };
	std::vector<DemoGroup> groups = { { 7, "Frags", false }, { 2, "Clips", false } };
	// Store order for folders, then No folder with the loose demos (five's folder 9 is gone: loose).
	const std::vector<std::string> plan = { "#7:1", "two.dem", "#2:2", "one.dem", "three.dem", "#-1:2", "four.dem", "five.dem" };
	CHECK(Files(demos, Plan(demos, groups, Sort::Date, false)) == plan);
	// Recently played inside a folder; ties (never played) fall back to newest first.
	const std::vector<std::string> played = { "#7:1", "two.dem", "#2:2", "three.dem", "one.dem", "#-1:2", "four.dem", "five.dem" };
	CHECK(Files(demos, Plan(demos, groups, Sort::Played, false)) == played);
	// A collapsed folder keeps its header and count; so does a collapsed No folder.
	groups[1].collapsed = true;
	const std::vector<std::string> shut = { "#7:1", "two.dem", "#2:2", "#-1:2" };
	CHECK(Files(demos, Plan(demos, groups, Sort::Date, true)) == shut);
}

static void TestSearch() {
	std::vector<DemoRow> demos = { Row("final.dem", "de_dust2", 3), Row("practice.dem", "cs_office", 2), Row("dust_night.dem", "de_inferno", 1) };
	demos[1].recorder = "SourceTV";
	demos[2].server = "Dust lovers";
	auto hits = Search(demos, "  DUST ");
	CHECK(hits.size() == 2);
	CHECK(hits.size() == 2 && hits[0].row == 0 && hits[0].why == "map: Dust II");
	CHECK(hits.size() == 2 && hits[1].row == 2 && hits[1].why == "demo: dust_night");
	hits = Search(demos, "sourcetv");
	CHECK(hits.size() == 1 && hits[0].row == 1 && hits[0].why == "recorded by: SourceTV");
	hits = Search(demos, "cs_off");
	CHECK(hits.size() == 1 && hits[0].why == "map: cs_office");
	CHECK(Search(demos, "   ").empty());
	CHECK(Search(demos, "e", 2).size() == 2);
}

static void TestText() {
	CHECK(MapDisplay("de_dust2") == "Dust II" && MapDisplay("DE_NUKE") == "Nuke");
	CHECK(MapDisplay("aim_map") == "aim_map" && MapDisplay("") == "Demo");
	CHECK(MapPrefix("de_dust2") == "DE" && MapPrefix("aim_map") == "AIM" && MapPrefix("dust") == "" && MapPrefix("_x") == "");
	DemoRow d = Row("match.DEM", "de_train", 0);
	CHECK(DemoName(d) == "match");
	d.fileName.clear();
	CHECK(DemoName(d) == "Train");
	CHECK(Duration(0) == "" && Duration(61.4f) == "1:01" && Duration(2462) == "41:02" && Duration(3723) == "1:02:03");
	CHECK(Ago(0, 1000) == "never" && Ago(1000, 1050) == "just now" && Ago(1000, 1000 + 600) == "10 min ago");
	CHECK(Ago(1000, 1000 + 7200) == "2 hr ago" && Ago(1000, 1000 + 3 * 86400) == "3 days ago");
	CHECK(Ago(1000, 1000 + 65 * 86400) == "2 mo ago" && Ago(2000, 1000) == "just now");
	CHECK(CropName("short") == "short");
	CHECK(CropName("abcdefghijklmnopqrstuvwxyz0123456789") == "abcdefghijklmnop\xE2\x80\xA6" "3456789");
	// UTF-8 counts characters, so the crop never splits one.
	const std::string accents = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";
	CHECK(CropName(accents, 1, 1) == "\xC3\xA9\xE2\x80\xA6\xC3\xA9");
	CHECK(DemoFileName("my demo") == "my demo.dem" && DemoFileName("  clip.DEM ") == "clip.DEM");
	CHECK(DemoFileName("a<b>:c\"d/e\\f|g?h*") == "abcdefgh.dem" && DemoFileName("name...") == "name.dem");
	CHECK(DemoFileName("   ") == "" && DemoFileName("??") == "" && DemoFileName(".dem") == "");
	CHECK(SortFromName("map") == Sort::Map && SortFromName("nonsense") == Sort::Date);
	CHECK(std::string(SortCaption(Sort::Played)) == "Recently played" && std::string(SortName(Sort::DateAsc)) == "dateasc");
}

int main() {
	TestPlanWithoutFolders();
	TestPlanWithFolders();
	TestSearch();
	TestText();
	if (g_failed) { std::printf("%d check(s) failed\n", g_failed); return 1; }
	std::printf("SourceDemosBrowserMathTests passed\n");
	return 0;
}
