// The Render workspace's model for a frame (Ui/RenderWorkspace.h), read from the shared render
// settings, validation and job (AfxHookSource2/Filmmaker/Render), and its commands run.

#include "stdafx.h"

#include "SourceRender.h"
#include "SourceEngine.h"

#include "Ui/DemoBar.h"
#include "Ui/RenderWorkspace.h"

#include "WrpVEngineClient.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <AfxHookSource2/Filmmaker/Platform/TextEncoding.h>
#include <AfxHookSource2/Filmmaker/Render/RenderCommand.h>
#include <AfxHookSource2/Filmmaker/Render/RenderDoneSound.h>
#include <AfxHookSource2/Filmmaker/Render/RenderEncoderDetect.h>
#include <AfxHookSource2/Filmmaker/Render/RenderHost.h>
#include <AfxHookSource2/Filmmaker/Render/RenderOutput.h>
#include <AfxHookSource2/Filmmaker/Render/RenderRange.h>
#include <AfxHookSource2/Filmmaker/Render/RenderSessionDriver.h>
#include <AfxHookSource2/Filmmaker/Render/RenderTakeHistory.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

extern WrpVEngineClient* g_VEngineClient;

namespace SourceFilmmaker {

namespace {

// "2026-10-28T00:20:41" as "Oct 28, 00:20" (CS2's PREVIOUS RENDERS rows).
std::string When(const std::string& iso) {
	static const char* const kMonths[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	int y = 0, mo = 0, d = 0, h = 0, mi = 0;
	if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5 || mo < 1 || mo > 12) return iso;
	char out[32];
	std::snprintf(out, sizeof(out), "%s %d, %02d:%02d", kMonths[mo - 1], d, h, mi);
	return out;
}

std::string Upper(std::string s) {
	for (char& ch : s) ch = (char)std::toupper((unsigned char)ch);
	return s;
}

} // namespace

void Render_BuildModel(Ui::RenderWorkspaceModel& m, const Ui::DemoBarModel& bar) {
	using namespace Filmmaker;
	m = Ui::RenderWorkspaceModel();
	const DemoState demo = Demo_State();
	m.demo = demo.playing;
	m.running = RenderSessionDriver_IsActive();
	const int slot = Render_GetModalTab();
	m.tab = slot <= 1 ? 0 : (slot == 2 ? 1 : 2);
	m.playhead = demo.tick;
	m.tickRate = RenderHost_TickRate();

	// Round: the round the playhead is in, from the demo bar's rounds, as CS2's timeline pushes it
	// back from its round markers (`render setrange`).
	RenderRange stored = Render_GetRange();
	const Ui::DemoBarRound* round = nullptr;
	for (const Ui::DemoBarRound& r : bar.rounds)
		if (demo.tick >= r.start && demo.tick < r.end) round = &r;
	if (stored.mode == RenderRangeMode::CurrentRound && round && !m.running
		&& (stored.startTick != round->start || stored.endTickExclusive != round->end)) {
		stored.startTick = round->start;
		stored.endTickExclusive = round->end;
		Render_SetRange(stored);
	}
	m.rangeMode = (int)stored.mode;
	RenderRange resolved;
	m.rangeOk = Render_ResolveRangeForDisplay(resolved) && resolved.Valid();
	m.in = resolved.startTick;
	m.out = resolved.endTickExclusive;
	m.modeLive[0] = RenderHost_DemoBounds(m.demoStart, m.demoEnd);
	m.modeLive[1] = !bar.rounds.empty();
	m.pathKeys = RenderHost_PathKeyCount();
	m.modeLive[2] = m.pathKeys >= 2;
	const int selected = RenderHost_PathSelected();
	m.modeLive[3] = selected >= 0 && selected + 1 < m.pathKeys;
	const RenderOutputSettings& out = RenderOutputSettingsRef();
	m.frames = m.rangeOk ? resolved.FrameCount(RenderCaptureFps(out.fps), m.tickRate) : 0;

	m.camera = (int)Render_GetCameraSource();
	const bool spectating = RenderHost_Spectating();
	m.cameraBlock[1] = spectating ? "" : "Needs a spectated player";
	m.cameraBlock[2] = spectating ? "" : "Needs a spectated player";
	m.cameraBlock[4] = m.pathKeys >= 2 ? "" : "Needs 2+ camera keys";
	m.cameraBlock[5] = RenderHost_FollowReady() ? "" : "Place a follow camera first";
	m.extension = (int)Render_GetExtension();

	const RenderPasses& passes = RenderPassesRef();
	m.hud = (int)passes.hud;
	m.cameraData = passes.cameraData;
	m.blood = passes.blood;

	m.folder = WideToUtf8(out.folder);
	m.folderPending = RenderOutput_PickFolderPending();
	m.project = out.project;
	m.format = (int)out.format;
	for (int i = 0; i < 8; ++i) m.formatOk[i] = RenderFormatSupported((RenderOutputFormat)i);
	m.formatName = RenderFormatName(out.format);
	m.formatNote = RenderFormatNote(out.format);
	m.fps = out.fps;
	m.crf = out.crf;
	m.bitrateKbps = out.bitrateKbps;
	m.encoder = out.encoder == RenderEncoder::X264 ? 0 : (out.encoder == RenderEncoder::Nvenc ? 1 : 2);
	m.speed = (int)out.speed;
	m.nvenc = (int)RenderEncoderDetect_State();
	m.nvencNote = RenderEncoderDetect_Describe();
	if (g_VEngineClient) g_VEngineClient->GetScreenSize(m.width, m.height);
	m.soundOn = RenderDoneSound_Enabled();
	m.volume = RenderDoneSound_Volume();

	const RenderValidationResult validation = Render_ValidateCurrentJobFull();
	m.fatalCount = validation.FatalCount();
	m.warnCount = validation.WarningCount();
	m.firstFatal = validation.FirstFatal();
	m.sessionState = (int)Render_GetSessionState();
	const RenderProgress& p = RenderSessionDriver_Progress();
	m.phase = p.phase;
	m.lastError = p.lastError;
	m.takeDir = p.takeDirUtf8;
	m.framesDone = p.framesDone;
	m.framesTotal = p.framesTotal;
	m.curTick = p.curTick;
	m.percent = (int)(p.percent + 0.5);

	for (const RenderTakeSettings& t : RenderTakeHistory_Listed()) {
		Ui::RenderTake row;
		row.when = When(t.writtenAt);
		row.status = t.status;
		const int ticks = t.endTickExclusive - t.startTick;
		// Rounded: 200 ticks at 66.67 are 2.99999 s, which is a 0:03 take.
		const long seconds = m.tickRate > 0 ? std::lround(ticks / m.tickRate) : 0;
		char detail[160];
		std::snprintf(detail, sizeof(detail), "%d \xE2\x86\x92 %d \xC2\xB7 %ld:%02ld \xC2\xB7 %d pass%s \xC2\xB7 %d fps %s", t.startTick,
			t.endTickExclusive - 1, seconds / 60, seconds % 60, (int)t.passes.size(), t.passes.size() == 1 ? "" : "es", t.fps,
			Upper(t.format).c_str());
		row.detail = detail;
		m.takes.push_back(std::move(row));
	}
}

void Render_RunCommands(const std::vector<std::string>& commands) {
	for (const std::string& command : commands) {
		if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("render.ui", "%s", command.c_str());
		if (g_VEngineClient) g_VEngineClient->ClientCmd_Unrestricted(command.c_str());
	}
}

} // namespace SourceFilmmaker
