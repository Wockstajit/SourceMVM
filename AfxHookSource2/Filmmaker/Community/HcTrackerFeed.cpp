#include "HcTrackerFeed.h"
#include "HcTrackerFeedMath.h"

#include "../Platform/HttpRequest.h"
#include "../Platform/JsonBuilder.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace Filmmaker {

namespace {

// The same queries hctracker.com's homepage makes.
const char* const kOpenUrl = "https://www.hctracker.com/api/explore?type=contests&status=open&pageSize=5";
const char* const kScheduledUrl = "https://www.hctracker.com/api/explore?type=contests&status=scheduled&pageSize=5";
constexpr auto kEvery = std::chrono::minutes(5);       // a polite poll: HCs run for days
constexpr auto kRetry = std::chrono::minutes(2);       // after a failed fetch

std::mutex g_mutex;
bool g_haveData = false;
std::vector<HcTracker::Contest> g_open, g_scheduled;
int g_liveTotal = 0, g_upcomingTotal = 0;
std::atomic<unsigned> g_version{0};
std::atomic<bool> g_inFlight{false};
std::chrono::steady_clock::time_point g_nextFetch{};

void Fetch() {
	std::vector<HcTracker::Contest> open, scheduled;
	int liveTotal = 0, upcomingTotal = 0;
	const HttpResponse a = HttpRequest("GET", kOpenUrl);
	const HttpResponse b = HttpRequest("GET", kScheduledUrl);
	const bool ok = a.status == 200 && b.status == 200 &&
		HcTracker::ParseContests(a.body, open, liveTotal) && HcTracker::ParseContests(b.body, scheduled, upcomingTotal);
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_nextFetch = std::chrono::steady_clock::now() + (ok ? kEvery : kRetry);
		if (ok) {
			g_open = std::move(open);
			g_scheduled = std::move(scheduled);
			g_liveTotal = liveTotal;
			g_upcomingTotal = upcomingTotal;
			g_haveData = true;
		}
	}
	if (ok) ++g_version;
	g_inFlight = false;
}

void List(JsonBuilder& j, const char* name, const std::vector<HcTracker::Contest>& list) {
	j.Key(name); j.BeginArray();
	for (const auto& c : list) {
		j.BeginObject();
		j.StringField("title", c.title);
		j.StringField("host", c.host);
		j.StringField("code", c.code);
		j.IntField("entries", c.entries);
		j.StringField("closesAt", c.closesAt);
		j.StringField("startsAt", c.startsAt);
		j.EndObject();
	}
	j.EndArray();
}

} // namespace

void HcTrackerFeed_Tick() {
	if (g_inFlight) return;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (std::chrono::steady_clock::now() < g_nextFetch) return;
		g_nextFetch = std::chrono::steady_clock::now() + kRetry; // until this one answers
	}
	g_inFlight = true;
	std::thread(Fetch).detach();
}

unsigned HcTrackerFeed_Version() { return g_version.load(); }

void HcTrackerFeed_AppendJson(JsonBuilder& j) {
	std::lock_guard<std::mutex> lock(g_mutex);
	j.Key("hc"); j.BeginObject();
	j.BoolField("ok", g_haveData);
	j.IntField("live", g_liveTotal);
	j.IntField("upcoming", g_upcomingTotal);
	List(j, "open", g_open);
	List(j, "scheduled", g_scheduled);
	j.EndObject();
}

} // namespace Filmmaker
