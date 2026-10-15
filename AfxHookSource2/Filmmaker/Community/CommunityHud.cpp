#include "CommunityHud.h"

#include "BugReport.h"
#include "BugReportSend.h"
#include "BugReportMath.h"
#include "CommunityJs.h"
#include "CrashReport.h"
#include "HcTrackerFeed.h"
#include "HcTrackerFeedMath.h"
#include "../Debug/MvmDebugLog.h"
#include "../Panorama/PanoramaBridge.h"
#include "../Panorama/PanoramaFindPanel.h"
#include "../Platform/JsonBuilder.h"
#include "../Platform/ShellOpen.h"
#include "../Platform/TextEncoding.h"

#include "../../DeathMsg.h" // AfxHookSource2_GetPanoramaMainMenuPanel
#include "../../../Shared/AfxConsole.h"

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace Filmmaker {

namespace {

const wchar_t* const kHcTrackerUrl = L"https://www.hctracker.com/";
// Where a report goes when it cannot be sent from the game (no webhook in the build, a failed send).
const wchar_t* const kIssuesUrl = L"https://github.com/Wockstajit/SourceMVM/issues";

PanoramaBridge g_bridge;
bool g_built = false;
bool g_buildAttempted = false;
void* g_host = nullptr;
short g_symCmd = -1;
short g_symState = -1;
std::string g_lastState;
std::vector<std::string> g_evalQueue;
BugReportResult g_lastReport;
int g_reportSeq = 0; // bumped per report, so the dialog waits for ITS result, not the last one
std::string g_sendRefused; // why the latest send did not start (rate limit, ...), for the dialog
bool g_recorderStarted = false;

void* FindHost() {
	void* ctx = g_bridge.ContextPanel();
	return ctx ? FindChildById(ctx, "MvmCommunityHost") : nullptr;
}

void BuildUi() {
	g_buildAttempted = true;
	// Four fragments (MSVC's ~16 KB raw-string cap): the badge + button, the report dialog, the
	// crash prompt, then the live-HC pill and dropdown.
	if (!g_bridge.RunScript(kCommunityJs)) return;
	g_bridge.RunScript(kCommunityReportJs);
	g_bridge.RunScript(kCommunityCrashJs);
	g_bridge.RunScript(kCommunityHcJs);
	g_host = FindHost();
	g_symCmd = g_bridge.MakeSymbol("cmd");
	g_symState = g_bridge.MakeSymbol("state");
	g_built = g_host != nullptr;
	g_lastState.clear(); // force a push
}

std::string StateJson() {
	JsonBuilder j;
	j.BeginObject();
	j.BoolField("debugLog", MvmDebugLog_Active());
	j.Key("report"); j.BeginObject();
	j.BoolField("ok", g_lastReport.ok);
	j.StringField("file", g_lastReport.fileName);
	j.StringField("error", g_lastReport.error);
	j.BoolField("hadDebugLog", g_lastReport.hadDebugLog);
	j.IntField("kb", int((g_lastReport.bytes + 1023) / 1024));
	j.IntField("seq", g_reportSeq);
	j.EndObject();
	// Sending to the developer's Discord: off (no webhook in this build), idle, sending, sent, failed.
	std::string detail;
	static const char* const kSend[] = {"off", "idle", "sending", "sent", "failed"};
	const BugReportSendState send = BugReportSend_State(&detail);
	// A send that never started (rate limit, one in flight) must not show the previous one's result.
	j.StringField("send", send != BugReportSendState::Off && !g_sendRefused.empty() ? "refused" : kSend[int(send)]);
	j.StringField("sendDetail", detail.empty() ? g_sendRefused : detail);
	// Earlier sessions' crash reports waiting for the player's decision (the crash prompt).
	const CrashReportPending crash = CrashReport_Pending();
	j.Key("crash"); j.BeginObject();
	j.IntField("count", crash.count);
	j.StringField("file", crash.newestFile);
	j.StringField("summary", BugReport_ScrubPrivate(crash.newestSummary));
	j.BoolField("dump", crash.hasDump);
	j.StringField("send", crash.send);
	j.StringField("detail", crash.sendDetail);
	j.EndObject();
	HcTrackerFeed_AppendJson(j); // live and upcoming HCs for the badge
	j.EndObject();
	return j.Str();
}

void PushState() {
	const std::string json = StateJson();
	if (json == g_lastState || !g_host || g_symState < 0) return;
	g_lastState = json;
	g_bridge.SetAttributeString(g_host, g_symState, json.c_str());
	g_bridge.RunScript("$.MvmCommunity && $.MvmCommunity.onState();");
}

// Writes the report to this PC only. Sending is a separate step the player takes after reviewing it
// (SendReport). Without a webhook in this build, the saved file is shown in Explorer straight away.
void WriteReport(const std::string& description, const BugReportSender& sender) {
	g_lastReport = BugReport_Write(description, sender);
	g_sendRefused.clear();
	if (g_lastReport.ok) {
		advancedfx::Message("Bug report saved: %s (%zu KB).\n", WideToUtf8(g_lastReport.path).c_str(), (g_lastReport.bytes + 1023) / 1024);
		if (!BugReportSend_Enabled()) ShowInExplorerAsync(g_lastReport.path);
	} else {
		advancedfx::Warning("Bug report failed: %s\n", g_lastReport.error.c_str());
	}
	++g_reportSeq;
}

// The player reviewed report `seq` and pressed Send. A stale seq (another report written since)
// sends nothing.
void SendReport(int seq) {
	if (!g_lastReport.ok || seq != g_reportSeq) return;
	g_sendRefused.clear();
	if (!BugReportSend_Start(g_lastReport, g_sendRefused)) ShowInExplorerAsync(g_lastReport.path);
}

// "bugreport <description> <discord>": each part encodeURIComponent'd, "-" when empty.
void HandleReport(const std::string& arg) {
	std::istringstream in(arg);
	std::string desc, discord;
	in >> desc >> discord;
	for (std::string* part : {&desc, &discord})
		if (*part == "-") part->clear();
	BugReportSender sender;
	sender.discord = BugReport::CleanDescription(BugReport::DecodeUriComponent(discord)).substr(0, 64);
	WriteReport(BugReport::DecodeUriComponent(desc), sender);
}

void Handle(const std::string& line) {
	const size_t space = line.find(' ');
	const std::string verb = line.substr(0, space);
	const std::string arg = space == std::string::npos ? std::string() : line.substr(space + 1);
	if (verb == "hctracker") {
		OpenUrlAsync(kHcTrackerUrl); // fixed: the UI never supplies the URL
	} else if (verb == "hcopen") {
		// One HC's page. Only a share code is accepted, never a URL.
		if (HcTracker::IsShareCode(arg)) OpenUrlAsync(std::wstring(kHcTrackerUrl) + L"hc/" + Utf8ToWide(arg));
	} else if (verb == "bugreport") {
		HandleReport(arg);
	} else if (verb == "sendreport") {
		SendReport(std::atoi(arg.c_str()));
	} else if (verb == "crashsend") {
		CrashReport_SendPending();
	} else if (verb == "crashdecline") {
		CrashReport_DeclinePending();
	} else if (verb == "crashshow") {
		const CrashReportPending crash = CrashReport_Pending();
		if (!crash.newestReport.empty()) ShowInExplorerAsync(crash.newestReport);
	} else if (verb == "debuglog") {
		if (arg == "on" && !MvmDebugLog_Active()) { MvmDebugLog_Start(nullptr); MvmConsoleCapture_Start(); }
		else if (arg == "off" && MvmDebugLog_Active()) { MvmConsoleCapture_Stop(); MvmDebugLog_Stop(nullptr, nullptr); }
	} else if (verb == "issues") {
		OpenUrlAsync(kIssuesUrl); // fixed: the UI never supplies the URL
	} else if (verb == "showreport") {
		if (g_lastReport.ok) ShowInExplorerAsync(g_lastReport.path);
	}
}

void PollCommands() {
	if (!g_host || g_symCmd < 0) return;
	const std::string queued = g_bridge.GetAttributeString(g_host, g_symCmd, "");
	if (queued.empty()) return;
	g_bridge.SetAttributeString(g_host, g_symCmd, "");
	std::istringstream iss(queued);
	std::string line;
	while (std::getline(iss, line)) {
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
		if (!line.empty()) Handle(line);
	}
}

} // namespace

void Community_RunMainThreadFrame() {
	// Before the recorder starts: the first call opens this session's on-disk log and writes out
	// earlier sessions' crash reports for the prompt; later calls track map/demo.
	CrashReport_MainThreadFrame();
	// The bug report's always-on recorder needs CS2's console from here on. Started on the first
	// main-thread frame, never during DLL load (tier0 registration under the loader lock).
	if (!g_recorderStarted) { g_recorderStarted = true; MvmConsoleCapture_KeepAlive(); }
	// The Steam name a report must not carry (it can change while CS2 runs).
	static int nameFrame = 0;
	if (++nameFrame % 300 == 1) BugReport_NoteLocalPlayerName();
	g_bridge.Init();
	// The main menu panel lives for the whole session (in a map it is the pause menu), so the
	// buttons stay injected; it is gone only while CS2 tears it down, and then we rebuild.
	unsigned char* mainMenu = AfxHookSource2_GetPanoramaMainMenuPanel();
	if (!mainMenu) { g_built = g_buildAttempted = false; g_host = nullptr; return; }
	g_bridge.SetContextPanel(mainMenu);

	if (!g_evalQueue.empty() && g_bridge.CanRunScript()) {
		for (const std::string& js : g_evalQueue) g_bridge.RunScript(js);
		g_evalQueue.clear();
	}
	if (!g_built) {
		if (!g_buildAttempted && g_bridge.CanRunScript()) BuildUi();
		if (!g_built) return;
	}
	// Re-found every frame: a menu reload drops it, and a stale pointer would be a use-after-free.
	g_host = FindHost();
	if (!g_host) { g_built = g_buildAttempted = false; return; }
	HcTrackerFeed_Tick();
	PollCommands();
	PushState();
}

void Community_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* sub = argc >= 3 ? args->ArgV(2) : "status";
	auto rest = [&](int from) {
		std::string s;
		for (int i = from; i < argc; ++i) { if (i > from) s += ' '; s += args->ArgV(i); }
		return s;
	};
	if (0 == _stricmp(sub, "status")) {
		advancedfx::Message("community: %s, host %s, main menu %s, last report %s\n", g_built ? "built" : "not built",
			g_host ? "found" : "missing", AfxHookSource2_GetPanoramaMainMenuPanel() ? "present" : "absent",
			g_lastReport.ok ? g_lastReport.fileName.c_str() : "none");
		return;
	}
	if (0 == _stricmp(sub, "rebuild")) { g_built = g_buildAttempted = false; g_host = nullptr; return; }
	if (0 == _stricmp(sub, "report")) { WriteReport(rest(3), BugReportSender{}); return; }
	if (0 == _stricmp(sub, "eval") && argc >= 4) { g_evalQueue.push_back(rest(3)); return; }
	if (0 == _stricmp(sub, "crash")) { CrashReport_Command(argc, args, cmd); return; }
	advancedfx::Message(
		"%s community status - HC Tracker badge + Report Bugs state.\n"
		"%s community rebuild - re-inject the main-menu script.\n"
		"%s community report <what happened> - write a bug report on this PC (not sent).\n"
		"%s community crash status|test|now - crash reports (sent only when the player says so).\n"
		"%s community eval <js> - run Panorama JS in the main-menu context.\n", cmd, cmd, cmd, cmd, cmd);
}

} // namespace Filmmaker
