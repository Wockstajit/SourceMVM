#pragma once

// Engine-free half of the HC Tracker feed (Community/HcTrackerFeed): reading HC Tracker's
// /api/explore answer into the HCs the main-menu badge shows. Tested by
// Automation/tests/hc-tracker-feed-math-tests.cpp.
//
// The API is the one hctracker.com's own homepage reads ("1 HC LIVE NOW"); it is not a published
// API, so parsing is forgiving: a missing field reads as empty, an unexpected shape as no HCs.

#include "../Platform/JsonParser.h"

#include <string>
#include <vector>

namespace Filmmaker::HcTracker {

constexpr size_t kMaxListed = 5;       // per list, in the dropdown
constexpr size_t kMaxTitleBytes = 80;  // a runaway title is cut

struct Contest {
	std::string title, host, code;  // code: the share code in hctracker.com/hc/<code>
	int entries = 0;
	std::string closesAt, startsAt; // ISO 8601 UTC, "" when not set
};

// A share code as the site uses them (H9FXZF): 4-16 letters and digits. Anything else is never
// put into a URL.
inline bool IsShareCode(const std::string& code) {
	if (code.size() < 4 || code.size() > 16) return false;
	for (char c : code)
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
	return true;
}

inline std::string ClipTitle(const std::string& s) {
	if (s.size() <= kMaxTitleBytes) return s;
	size_t cut = kMaxTitleBytes - 3;
	while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
	return s.substr(0, cut) + "...";
}

// results.contests.items[] and results.contests.pagination.total of an /api/explore answer.
// `total` may exceed the items returned (the page size). False when the JSON is unreadable.
inline bool ParseContests(const std::string& json, std::vector<Contest>& out, int& total) {
	out.clear();
	total = 0;
	JsonValue root;
	if (!JsonParse(json, root)) return false;
	const JsonValue* results = root.Find("results");
	const JsonValue* contests = results ? results->Find("contests") : nullptr;
	if (!contests) return true; // a valid answer with nothing in it
	if (const JsonValue* pagination = contests->Find("pagination"))
		if (const JsonValue* t = pagination->Find("total")) total = t->AsInt(0);
	const JsonValue* items = contests->Find("items");
	if (!items || items->type != JsonValue::Type::Array) return true;
	for (const JsonValue& item : items->arr) {
		Contest c;
		if (const JsonValue* v = item.Find("shareCode")) c.code = v->AsString();
		if (!IsShareCode(c.code)) continue; // never link something we cannot vouch for
		if (const JsonValue* v = item.Find("title")) c.title = ClipTitle(v->AsString());
		if (const JsonValue* host = item.Find("host"))
			if (const JsonValue* v = host->Find("username")) c.host = ClipTitle(v->AsString());
		if (const JsonValue* v = item.Find("entryCount")) c.entries = v->AsInt(0);
		if (const JsonValue* v = item.Find("submissionsCloseAt")) c.closesAt = v->AsString();
		if (const JsonValue* v = item.Find("scheduledStartAt")) c.startsAt = v->AsString();
		out.push_back(c);
		if (out.size() >= kMaxListed) break;
	}
	if (total < int(out.size())) total = int(out.size());
	return true;
}

} // namespace Filmmaker::HcTracker
