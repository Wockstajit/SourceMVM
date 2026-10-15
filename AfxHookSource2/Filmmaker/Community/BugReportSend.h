#pragma once

// Sends a bug or crash report the user reviewed and approved to the developer's Discord channel
// through a webhook: a message card (what happened, build, map, health) with the report .txt
// attached, which Discord shows as a download. Nothing calls this without the user pressing Send.
// Runs on its own thread; the UI polls BugReportSend_State.
//
// The webhook URL is NOT in the repository. It comes from BugReportWebhook.local.h next to this
// file (git-ignored), which defines SOURCEMVM_BUGREPORT_WEBHOOK. Without it, sending is off and
// reports are only saved locally. See Docs/features/filmmaker_community.md.

#include "BugReport.h"
#include "BugReportMath.h"

#include <string>
#include <vector>

namespace Filmmaker {

enum class BugReportSendState { Off, Idle, Sending, Sent, Failed };

// True when this build carries a webhook to send to.
bool BugReportSend_Enabled();

// Starts sending `report` (its file must exist). False, with `why`, when it cannot start: no
// webhook, one already in flight, or the one-per-minute limit.
bool BugReportSend_Start(const BugReportResult& report, std::string& why);

// Sends `report` on the calling thread and waits for Discord's answer. For crash reports the user
// approved (CrashReport.cpp): no rate limit and no effect on the dialog's state.
bool BugReportSend_Blocking(const BugReportResult& report, std::string& why);

// Posts one message to the same channel on the calling thread: `payloadJson` is the Discord message
// (its embed card), `files` the attachments, sent straight from memory -- nothing is written to
// disk. No rate limit and no effect on the dialog's state. Bug and crash reports post through it.
bool BugReportSend_PostAttachments(const std::string& payloadJson, const std::vector<BugReport::UploadFile>& files,
	std::string& detail);

// Where the latest send is. `detail` receives a short reason on failure.
BugReportSendState BugReportSend_State(std::string* detail = nullptr);

} // namespace Filmmaker
