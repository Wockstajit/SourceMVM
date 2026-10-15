// Behavior tests for Community/HcTrackerFeedMath.h: reading HC Tracker's /api/explore answer.
#include "../../AfxHookSource2/Filmmaker/Community/HcTrackerFeedMath.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

using namespace Filmmaker::HcTracker;

// The shape hctracker.com answered with on 2026-10-10 (trimmed).
const char* kOpen = R"({"filters":{"type":"contests","status":"open","page":1,"pageSize":3},
 "results":{"contests":{"items":[{"id":"e44c5933","shareCode":"H9FXZF","title":"Droid 3hc","description":"have fun ",
 "lifecycleStatus":"open","scheduledStartAt":null,"submissionsCloseAt":"2026-10-12T20:00:00.000Z",
 "host":{"username":"tinypieces","isVerifiedHost":false},"entryCount":9,"rsvpCount":0}],
 "pagination":{"page":1,"pageSize":3,"total":1,"totalPages":1,"hasNext":false}},
 "entries":{"items":[],"pagination":{"total":0}}}})";

void ReadsAnOpenHc() {
	std::vector<Contest> list;
	int total = -1;
	CHECK(ParseContests(kOpen, list, total));
	CHECK(total == 1 && list.size() == 1);
	CHECK(list[0].title == "Droid 3hc" && list[0].host == "tinypieces" && list[0].code == "H9FXZF");
	CHECK(list[0].entries == 9);
	CHECK(list[0].closesAt == "2026-10-12T20:00:00.000Z" && list[0].startsAt.empty()); // null reads as unset
}

void CountsBeyondThePage() {
	// pageSize 1 but 3 live: the badge must say 3, the list holds what was returned.
	const char* json = R"({"results":{"contests":{"items":[{"shareCode":"AAAA11","title":"a"}],"pagination":{"total":3}}}})";
	std::vector<Contest> list;
	int total = 0;
	CHECK(ParseContests(json, list, total) && total == 3 && list.size() == 1);
}

void EmptyAndBrokenAnswers() {
	std::vector<Contest> list;
	int total = 7;
	CHECK(ParseContests(R"({"results":{"contests":{"items":[],"pagination":{"total":0}}}})", list, total));
	CHECK(list.empty() && total == 0);
	CHECK(ParseContests(R"({"somethingElse":true})", list, total) && list.empty() && total == 0);
	CHECK(!ParseContests("<html>502 Bad Gateway</html>", list, total));
}

void NeverLinksABadCode() {
	CHECK(IsShareCode("H9FXZF") && IsShareCode("abcd") && !IsShareCode("abc") && !IsShareCode(""));
	CHECK(!IsShareCode("../../x") && !IsShareCode("H9 FXZ") && !IsShareCode("H9FXZF?x=1"));
	const char* json = R"({"results":{"contests":{"items":[{"shareCode":"../evil","title":"x"},{"shareCode":"GOOD12","title":"y"}]}}})";
	std::vector<Contest> list;
	int total = 0;
	CHECK(ParseContests(json, list, total) && list.size() == 1 && list[0].code == "GOOD12");
}

void LongTitlesAreCut() {
	std::string title(200, 'x');
	const std::string json = std::string(R"({"results":{"contests":{"items":[{"shareCode":"ABCD12","title":")") + title + R"("}]}}})";
	std::vector<Contest> list;
	int total = 0;
	CHECK(ParseContests(json, list, total) && list.size() == 1 && list[0].title.size() <= kMaxTitleBytes);
}

} // namespace

int main() {
	ReadsAnOpenHc();
	CountsBeyondThePage();
	EmptyAndBrokenAnswers();
	NeverLinksABadCode();
	LongTitlesAreCut();
	if (g_failures) { std::printf("%d failure(s)\n", g_failures); return EXIT_FAILURE; }
	std::printf("hc tracker feed math tests passed\n");
	return EXIT_SUCCESS;
}
