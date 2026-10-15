#include "BugReportSend.h"
#include "BugReport.h"
#include "BugReportMath.h"

#include "../Platform/HttpRequest.h"
#include "../Platform/JsonBuilder.h"
#include "../Platform/TextEncoding.h"

// The webhook lives outside git: BugReportWebhook.local.h defines SOURCEMVM_BUGREPORT_WEBHOOK.
#if __has_include("BugReportWebhook.local.h")
#include "BugReportWebhook.local.h"
#endif
#ifndef SOURCEMVM_BUGREPORT_WEBHOOK
#define SOURCEMVM_BUGREPORT_WEBHOOK ""
#endif

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace Filmmaker {

namespace {

constexpr int kMinSecondsBetween = 60; // one report a minute: a stuck button cannot flood the channel

std::mutex g_mutex;
BugReportSendState g_state = BugReportSendState::Idle;
std::string g_detail;
std::chrono::steady_clock::time_point g_lastStart{};
bool g_everStarted = false;

const std::string& WebhookUrl() {
	static const std::string url = SOURCEMVM_BUGREPORT_WEBHOOK;
	return url;
}

void SetState(BugReportSendState s, const std::string& detail = {}) {
	std::lock_guard<std::mutex> lock(g_mutex);
	g_state = s;
	g_detail = detail;
}

std::string Clip(const std::string& s, size_t max) {
	if (s.size() <= max) return s;
	size_t cut = max - 3;
	while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
	return s.substr(0, cut) + "...";
}

void Field(JsonBuilder& j, const char* name, const std::string& value, bool inl) {
	j.BeginObject();
	j.StringField("name", name);
	j.StringField("value", Clip(value.empty() ? "-" : BugReport_ScrubPrivate(value), 1000));
	j.BoolField("inline", inl);
	j.EndObject();
}

// The Discord message: an embed card the developer can read at a glance; the full report is the
// attached file. Mentions are disabled so a description cannot ping anyone.
std::string PayloadJson(const BugReportResult& r) {
	JsonBuilder j;
	j.BeginObject();
	j.StringField("username", "SOURCE:MVM Reports");
	j.Key("allowed_mentions"); j.BeginObject(); j.Key("parse"); j.BeginArray(); j.EndArray(); j.EndObject();
	j.Key("embeds"); j.BeginArray();
	j.BeginObject();
	if (!r.crash.empty()) {
		// Nobody typed anything, so the card leads with what crashed.
		j.StringField("title", "Crash report");
		j.StringField("description", Clip(BugReport_ScrubPrivate(r.crash), 3500));
		j.IntField("color", 0xE0473C);
	} else {
		j.StringField("title", "Bug report");
		j.StringField("description", Clip(r.description.empty() ? "(nothing written)" : BugReport_ScrubPrivate(r.description), 3500));
		j.IntField("color", 0xF0B323);
	}
	j.Key("fields"); j.BeginArray();
	if (r.crash.empty()) Field(j, "Discord", r.sender.discord.empty() ? "(not given)" : r.sender.discord, true);
	Field(j, "Map", r.map, true);
	Field(j, "Demo", r.demo, true);
	Field(j, "Health", r.health, false);
	Field(j, "CS2", r.cs2Build, false);
	Field(j, "SOURCE:MVM build", r.hookBuild, true);
	Field(j, "Windows", r.windows, true);
	j.EndArray();
	j.Key("footer"); j.BeginObject();
	j.StringField("text", r.fileName + (r.dumpPath.empty() ? " - full logs attached" : " - full logs and minidump attached"));
	j.EndObject();
	j.EndObject();
	j.EndArray();
	j.EndObject();
	return j.Str();
}

// Posts the card with the report attached and waits for Discord's answer. `detail` gets a short
// reason on failure.
bool Post(const BugReportResult& report, std::string& detail) {
	std::ifstream in(report.path, std::ios::binary);
	std::ostringstream bytes;
	bytes << in.rdbuf();
	// Scrubbed again on the way out: the Steam name may not have been known when it was written.
	std::vector<BugReport::UploadFile> files = {{report.fileName, BugReport_ScrubPrivate(bytes.str())}};
	// A crash's minidump: every thread's stack and registers, so a frame resolves to its function and
	// line against the archived PDB. Only when the player sent it, and only under Discord's limit.
	if (!report.dumpPath.empty()) {
		std::ifstream dump(report.dumpPath, std::ios::binary);
		std::ostringstream dumpBytes;
		dumpBytes << dump.rdbuf();
		const std::string data = dumpBytes.str();
		if (!data.empty() && data.size() <= BugReport::kMaxDumpUploadBytes)
			files.push_back({WideToUtf8(fs::path(report.dumpPath).filename().wstring()), data, "application/octet-stream"});
	}
	return BugReportSend_PostAttachments(PayloadJson(report), files, detail);
}

} // namespace

bool BugReportSend_Enabled() {
	return BugReport::IsDiscordWebhookUrl(WebhookUrl());
}

bool BugReportSend_PostAttachments(const std::string& payloadJson, const std::vector<BugReport::UploadFile>& files,
	std::string& detail) {
	if (!BugReportSend_Enabled()) { detail = "sending is not set up in this build"; return false; }
	const std::string boundary = "----SourceMvmReport" + std::to_string(GetTickCount64());
	const std::string body = BugReport::MultipartBody(boundary, payloadJson, files);
	const HttpResponse res = HttpRequest("POST", WebhookUrl() + "?wait=true", "multipart/form-data; boundary=" + boundary, body, 64 * 1024);
	const unsigned long status = res.status;
	if (status >= 200 && status < 300) return true;
	if (status == 429) detail = "Discord is rate limiting, try again soon";
	else if (status == 404 || status == 401) detail = "the report channel's webhook is gone";
	else if (status) detail = "Discord answered " + std::to_string(status);
	else detail = res.error.empty() ? "no connection" : res.error;
	return false;
}

bool BugReportSend_Start(const BugReportResult& report, std::string& why) {
	if (!BugReportSend_Enabled()) { why = "sending is not set up in this build"; return false; }
	if (!report.ok) { why = "the report was not written"; return false; }
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_state == BugReportSendState::Sending) { why = "a report is already sending"; return false; }
		const auto now = std::chrono::steady_clock::now();
		if (g_everStarted && now - g_lastStart < std::chrono::seconds(kMinSecondsBetween)) {
			why = "please wait a minute between reports";
			return false;
		}
		g_everStarted = true;
		g_lastStart = now;
		g_state = BugReportSendState::Sending;
		g_detail.clear();
	}
	std::thread([report] {
		std::string detail;
		if (Post(report, detail)) {
			// Sent: the developer has it, so it does not pile up on this PC.
			DeleteFileW(report.path.c_str());
			SetState(BugReportSendState::Sent);
		} else {
			SetState(BugReportSendState::Failed, detail);
		}
	}).detach();
	return true;
}

bool BugReportSend_Blocking(const BugReportResult& report, std::string& why) {
	if (!BugReportSend_Enabled()) { why = "sending is not set up in this build"; return false; }
	if (!report.ok) { why = "the report was not written"; return false; }
	return Post(report, why);
}

BugReportSendState BugReportSend_State(std::string* detail) {
	if (!BugReportSend_Enabled()) return BugReportSendState::Off;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (detail) *detail = g_detail;
	return g_state;
}

} // namespace Filmmaker
