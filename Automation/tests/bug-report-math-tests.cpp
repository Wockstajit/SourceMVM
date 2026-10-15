// Behavior tests for Community/BugReportMath.h: the bug report's text rules.
#include "../../AfxHookSource2/Filmmaker/Community/BugReportMath.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

using namespace Filmmaker::BugReport;

void UserFoldersAreScrubbed() {
	const std::string profile = "C:\\Users\\ExampleUser";
	CHECK(ScrubUserPaths("log C:\\Users\\ExampleUser\\AppData\\Roaming\\HLAE\\x.log", profile)
		== "log %USERPROFILE%\\AppData\\Roaming\\HLAE\\x.log");
	// Case and slash style do not matter.
	CHECK(ScrubUserPaths("c:/users/EXAMPLEUSER/Desktop/a.dem", profile) == "%USERPROFILE%/Desktop/a.dem");
	// A different account (or a name with spaces) is caught by the generic rule.
	CHECK(ScrubUserPaths("D:\\Users\\John Smith\\Videos\\take", profile) == "%USERPROFILE%\\Videos\\take");
	// A longer name that merely starts with the profile is still its own name, and still scrubbed.
	CHECK(ScrubUserPaths("C:\\Users\\ExampleUser2\\x", profile) == "%USERPROFILE%\\x");
	// Shared folders are not a person.
	CHECK(ScrubUserPaths("C:\\Users\\Public\\Documents", profile) == "C:\\Users\\Public\\Documents");
	// Text without a user folder is untouched, and the scrub is idempotent.
	CHECK(ScrubUserPaths("F:\\SteamLibrary\\cs2.exe", profile) == "F:\\SteamLibrary\\cs2.exe");
	const std::string once = ScrubUserPaths("\"C:\\Users\\ExampleUser\"", profile);
	CHECK(once == "\"%USERPROFILE%\"" && ScrubUserPaths(once, profile) == once);
	// No profile known: the generic rule still applies.
	CHECK(ScrubUserPaths("C:\\Users\\ExampleUser\\x", "") == "%USERPROFILE%\\x");
}

void TailKeepsWholeLines() {
	const std::string log = "first line\nsecond line\nthird line\n";
	size_t omitted = 99;
	CHECK(TailText(log, 1000, &omitted) == log && omitted == 0);
	// 15 bytes from the end lands inside "second line": the tail starts at the next whole line.
	const std::string tail = TailText(log, 15, &omitted);
	CHECK(tail == "third line\n");
	CHECK(omitted == log.size() - tail.size());
	CHECK(tail.size() <= 15);
}

void TailNeverSplitsACharacter() {
	// One long line of two-byte characters: no line break to start on, so it must start on a
	// character boundary instead of in the middle of one.
	std::string text;
	for (int i = 0; i < 50; ++i) text += "\xC3\xA9"; // é
	const std::string tail = TailText(text, 7);
	CHECK(!tail.empty() && (static_cast<unsigned char>(tail[0]) & 0xC0) != 0x80);
	CHECK(tail.size() <= 7);
}

void DescriptionIsCleaned() {
	CHECK(CleanDescription("  \n  ") == "");
	CHECK(CleanDescription("  It crashed\r\nwhen I hit play\t \n") == "It crashed\nwhen I hit play");
	CHECK(CleanDescription(std::string("bell\x07 and nul") + '\0' + "x") == "bell and nulx");
	// Over the limit: cut to kMaxDescription bytes, on a whole character.
	std::string longText(kMaxDescription - 1, 'a');
	longText += "\xC3\xA9\xC3\xA9";
	const std::string cleaned = CleanDescription(longText);
	CHECK(cleaned.size() <= kMaxDescription);
	CHECK((static_cast<unsigned char>(cleaned.back()) & 0xC0) != 0xC0); // no dangling lead byte
}

void DescriptionSurvivesTransport() {
	// encodeURIComponent("Map: mirage\nFX broke 100%") on the JS side.
	CHECK(DecodeUriComponent("Map%3A%20mirage%0AFX%20broke%20100%25") == "Map: mirage\nFX broke 100%");
	CHECK(DecodeUriComponent("caf%C3%A9") == "caf\xC3\xA9");
	CHECK(DecodeUriComponent("50%") == "50%");     // malformed: kept as typed
	CHECK(DecodeUriComponent("%zz%4") == "%zz%4");
}

void FileNameSortsByTime() {
	CHECK(ReportFileName(2026, 10, 10, 1, 4, 9) == "SourceMVM-bug-2026-10-10_01-04-09.txt");
	CHECK(ReportFileName(2026, 1, 2, 3, 4, 5) < ReportFileName(2026, 1, 2, 3, 4, 6));
}

void RecorderKeepsTheNewest() {
	LineRing ring(40);
	ring.Add("one");       // 4 bytes with its line break
	ring.Add("two");
	CHECK(ring.Text() == "one\ntwo\n" && ring.Dropped() == 0);
	ring.Add(std::string(35, 'x')); // 4+4+36 > 40: over capacity: the oldest lines fall off the front
	CHECK(ring.Bytes() <= 40);
	CHECK(ring.Text().rfind("one", 0) != 0);
	CHECK(ring.Text().find(std::string(35, 'x')) != std::string::npos);
	CHECK(ring.Dropped() >= 1 && ring.Lines() + ring.Dropped() == 3);
	// A runaway line is cut, on a whole character, and marked.
	LineRing big(100000);
	std::string huge(kMaxLineBytes - 1, 'a');
	huge += "\xC3\xA9tail";
	big.Add(huge);
	const std::string kept = big.Text();
	CHECK(kept.size() < kMaxLineBytes + 16);
	CHECK(kept.find("[...]") != std::string::npos);
	CHECK(kept.find('\xC3') == std::string::npos); // the split character was dropped whole
}

void UploadBodyIsWellFormed() {
	const std::string body = MultipartBody("BOUNDARY", "{\"content\":\"hi\"}", {{"r.txt", "log line\n"}});
	CHECK(body.rfind("--BOUNDARY\r\n", 0) == 0);
	CHECK(body.find("name=\"payload_json\"\r\nContent-Type: application/json\r\n\r\n{\"content\":\"hi\"}\r\n") != std::string::npos);
	CHECK(body.find("name=\"files[0]\"; filename=\"r.txt\"") != std::string::npos);
	CHECK(body.find("\r\n\r\nlog line\n\r\n--BOUNDARY--\r\n") != std::string::npos);
	CHECK(body.size() >= 9 && body.substr(body.size() - 14) == "--BOUNDARY--\r\n");
	// A crash report and its dump: two numbered parts, each with its own type, binary kept intact.
	const std::string dump("MDMP\0\x01\xff", 7);
	const std::string two = MultipartBody("B", "{}", {{"c.txt", "text"}, {"c.dmp", dump, "application/octet-stream"}});
	CHECK(two.find("name=\"files[0]\"; filename=\"c.txt\"") != std::string::npos);
	CHECK(two.find("name=\"files[1]\"; filename=\"c.dmp\"\r\nContent-Type: application/octet-stream\r\n\r\n" + dump + "\r\n--B--\r\n") != std::string::npos);
	CHECK(two.find("files[2]") == std::string::npos);
}

void OnlyWebhookUrlsAreAccepted() {
	CHECK(IsDiscordWebhookUrl("https://discord.com/api/webhooks/123456789/abcDEF_-123"));
	CHECK(IsDiscordWebhookUrl("https://discordapp.com/api/webhooks/1/x"));
	CHECK(!IsDiscordWebhookUrl(""));
	CHECK(!IsDiscordWebhookUrl("http://discord.com/api/webhooks/1/x"));          // not https
	CHECK(!IsDiscordWebhookUrl("https://discord.com/channels/1/2"));             // a channel link
	CHECK(!IsDiscordWebhookUrl("https://discord.com/api/webhooks/abc/token"));   // id not numeric
	CHECK(!IsDiscordWebhookUrl("https://discord.com/api/webhooks/1/"));          // no token
	CHECK(!IsDiscordWebhookUrl("https://discord.com/api/webhooks/1/tok?x=1"));   // stray query
	CHECK(!IsDiscordWebhookUrl("https://evil.example/api/webhooks/1/tok"));
}

void SteamIdentityIsScrubbed() {
	// CS2's own console lines, as the recorder keeps them.
	CHECK(ScrubSteamIdentity("Player1 connected", "Player1") == "<player> connected");
	CHECK(ScrubSteamIdentity("Added prediction for player slot 0 (Player1)", "Player1")
		== "Added prediction for player slot 0 (<player>)");
	CHECK(ScrubSteamIdentity("AuthStatus (steamid:76561198000000001):  OK", "")
		== "AuthStatus (steamid:<steamid>):  OK");
	CHECK(ScrubSteamIdentity("id [U:1:39734273] joined", "") == "id [U:1:<steamid>] joined");
	CHECK(ScrubSteamIdentity("STEAM_1:1:19867136 left", "") == "STEAM_<steamid> left");
	// Not an ID: a longer number, a shorter one, or a different prefix stays as it is.
	CHECK(ScrubSteamIdentity("tick 765611980000000012345", "") == "tick 765611980000000012345");
	CHECK(ScrubSteamIdentity("frame 7656119800", "") == "frame 7656119800");
	CHECK(ScrubSteamIdentity("[U:1:] [U:2:5]", "") == "[U:1:] [U:2:5]");
	// A name under 3 bytes would eat ordinary words: left alone. An empty name changes nothing.
	CHECK(ScrubSteamIdentity("at a map", "a") == "at a map");
	CHECK(ScrubSteamIdentity("Player1", "") == "Player1");
	// Every occurrence, with IDs and the name together.
	CHECK(ScrubSteamIdentity("Ab1 76561198000000001 Ab1", "Ab1") == "<player> <steamid> <player>");
}

} // namespace

int main() {
	UserFoldersAreScrubbed();
	SteamIdentityIsScrubbed();
	RecorderKeepsTheNewest();
	UploadBodyIsWellFormed();
	OnlyWebhookUrlsAreAccepted();
	TailKeepsWholeLines();
	TailNeverSplitsACharacter();
	DescriptionIsCleaned();
	DescriptionSurvivesTransport();
	FileNameSortsByTime();
	if (g_failures) { std::printf("%d failure(s)\n", g_failures); return EXIT_FAILURE; }
	std::printf("bug report math tests passed\n");
	return EXIT_SUCCESS;
}
