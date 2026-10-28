#include "RenderCommand.h"

// The render workspace's settings, console surface and state, shared by CS2 and CS:S. What it asks
// the game goes through RenderHost.h; the job is the game's own (RenderSessionDriver.h).

#include <algorithm>
#include "RenderTypes.h"
#include "RenderRange.h"
#include "RenderHost.h"
#include "RenderSession.h"
#include "RenderSessionDriver.h"
#include "RenderValidation.h"
#include "RenderOutput.h"
#include "RenderEncoderDetect.h"
#include "RenderPassPlan.h"
#include "RenderDoneSound.h"
#include "RenderTakeHistory.h"
#include "../Platform/TextEncoding.h" // WideToUtf8 (output folder -> console / state JSON)
#include "../Platform/JsonBuilder.h"   // JsonBuilder::Escape (paths/messages carry backslashes + quotes)
#include "../Debug/MvmDebugLog.h"

#include <cmath>
#include <sstream>
#include <string>

namespace Filmmaker {

// Paths and validation messages contain backslashes and quotes; unescaped they would break the
// whole state attribute (the JS does one JSON.parse and bails on failure, taking the entire
// editor UI with it).
static std::string JsonQuote(const std::string& s) {
	return "\"" + JsonBuilder::Escape(s) + "\"";
}

static RenderRange s_range;
// The last CUSTOM In/Out the user set, remembered independently of s_range: switching to Path or
// Round overwrites s_range's ticks (those modes resolve eagerly), which used to destroy the user's
// custom span. Clicking Custom now restores this pair instead of whatever the last mode left.
static int s_customInTick = -1;
static int s_customOutTickExclusive = -1;
static void RememberCustomRange() {
	s_customInTick = s_range.startTick;
	s_customOutTickExclusive = s_range.endTickExclusive;
}
static RenderCameraSource s_camera = RenderCameraSource::CurrentViewer;
static CameraPathExtension s_extension = CameraPathExtension::Fail;
static bool s_jobActive = false;
static int s_modalTab = 0;
static bool s_modalOpen = false;

// -- accessors --

const RenderRange& Render_GetRange() { return s_range; }
void Render_SetRange(const RenderRange& range) { s_range = range; }

RenderCameraSource Render_GetCameraSource() { return s_camera; }
void Render_SetCameraSource(RenderCameraSource source) { s_camera = source; }

CameraPathExtension Render_GetExtension() { return s_extension; }
void Render_SetExtension(CameraPathExtension ext) { s_extension = ext; }

bool Render_IsJobActive() { return s_jobActive; }
void Render_SetJobActive(bool active) { s_jobActive = active; }

// Delegate to RenderSession: it owns the legal-transition table, so a caller cannot move the
// machine into an impossible state (Idle -> Capturing, or back out of a terminal state).
RenderSessionState Render_GetSessionState() { return RenderSessionRef().State(); }
void Render_SetSessionState(RenderSessionState state) {
	const char* why = nullptr;
	if (!RenderSessionRef().TryTransition(state, &why)) {
		advancedfx::Warning("mirv_filmmaker render: refused %s -> %s (%s).\n",
			RenderSession::StateName(RenderSessionRef().State()),
			RenderSession::StateName(state), why ? why : "illegal");
	}
}

int Render_GetModalTab() { return s_modalTab; }
void Render_SetModalTab(int tab) { s_modalTab = tab; }

bool Render_IsModalOpen() { return s_modalOpen; }
void Render_SetModalOpen(bool open) {
	// Render is an all-UI workspace. A completed job can restore a camera mode that normally
	// returns the mouse to CS2; opening (or re-opening) Render must always hand it back to
	// Panorama so dropdowns and output controls are physically clickable.
	if (open && RenderHost_EditorActive())
		RenderHost_WorkspaceOpened();
	if (s_modalOpen == open)
		return;
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("ui.render.action", "open %d -> %d", s_modalOpen ? 1 : 0, open ? 1 : 0);
	s_modalOpen = open;
	// Start the NVENC probe as soon as the workspace is opened. The default encoder is Auto, so
	// without this the Output tab could only say "GPU encoding not probed yet" until the user
	// happened to touch the encoder control -- and Auto would fall back to CPU at job start
	// purely because nothing had asked yet.
	if (open) {
		RenderEncoderDetect_EnsureProbe();
		// The PREVIOUS RENDERS list: what this demo's earlier jobs left in the output folder.
		RenderTakeHistory_Refresh();
	}
	Render_ApplyLivePreview();
}

// The workspace shows ONE pass live on the game view, so a shot can be framed against the plate it
// will deliver (RenderHost_ApplyLivePreview). Scoped to the open workspace on purpose, and never
// during a job: the job owns what is drawn.
void Render_ApplyLivePreview() {
	if (RenderSessionDriver_IsActive())
		return;
	RenderHost_ApplyLivePreview(RenderPassesRef(), s_modalOpen);
}

bool Render_IsCancelRequested() { return RenderSessionRef().IsCancelRequested(); }
void Render_RequestCancel() { RenderSessionRef().RequestCancel(); }

bool Render_GetDemoBounds(int& outStartTick, int& outEndTick) {
	outStartTick = -1; outEndTick = -1;
	return RenderHost_DemoBounds(outStartTick, outEndTick);
}

static bool ResolveRangeForDisplayImpl(RenderRange& out) {
	out = s_range;
	switch (s_range.mode) {
	case RenderRangeMode::FullDemo: {
		int a = 0, b = 0;
		if (!Render_GetDemoBounds(a, b)) return false;
		out.startTick = a;
		out.endTickExclusive = b + 1;
		return true;
	}
	case RenderRangeMode::CameraPathBounds: {
		const int keys = RenderHost_PathKeyCount();
		if (keys < 2) return false;
		out.startTick = RenderHost_PathKeyTick(0);
		out.endTickExclusive = RenderHost_PathKeyTick(keys - 1) + 1;
		return true;
	}
	case RenderRangeMode::SelectedCameraSegment: {
		const int sel = RenderHost_PathSelected();
		if (sel < 0 || sel + 1 >= RenderHost_PathKeyCount()) return false;
		out.startTick = RenderHost_PathKeyTick(sel);
		out.endTickExclusive = RenderHost_PathKeyTick(sel + 1) + 1;
		return true;
	}
	case RenderRangeMode::CurrentRound:
		// Round boundaries live in the native demo bar's round markers; the timeline JS
		// resolves the span and pushes it back through "render setrange". Until then the
		// stored ticks are whatever was last pushed (invalid while still 0/0).
		return s_range.Valid();
	case RenderRangeMode::CustomTicks:
		return s_range.Valid();
	}
	return false;
}

// The timeline calls this EVERY frame, so the probe is change-gated rather than throttled:
// what matters is each transition (mode switch, round pushback landing, a span going
// unresolvable), and a sampled probe would miss the exact frame one happened.
bool Render_ResolveRangeForDisplay(RenderRange& out) {
	const bool ok = ResolveRangeForDisplayImpl(out);
	if (MvmDebugLog_Active()) {
		static int s_mode = -999, s_start = -999, s_end = -999, s_ok = -1;
		const int mode = (int)s_range.mode;
		if (mode != s_mode || out.startTick != s_start || out.endTickExclusive != s_end || (ok ? 1 : 0) != s_ok) {
			s_mode = mode; s_start = out.startTick; s_end = out.endTickExclusive; s_ok = ok ? 1 : 0;
			MvmDebugLog_Linef("ui.render.range",
				"resolve mode=%d stored=%d..%d -> %d..%d ok=%d",
				mode, s_range.startTick, s_range.endTickExclusive,
				out.startTick, out.endTickExclusive, ok ? 1 : 0);
		}
	}
	return ok;
}

// One place that builds the FULL validation result (range/camera + output/passes), so the
// console command, the UI token and the start guard can never disagree about whether a job may
// run. RenderValidation stays dependency-free; the RenderOutput facts are passed in here.
static RenderValidationResult ValidateCurrentJob() {
	const RenderOutputSettings& out = RenderOutputSettingsRef();
	// Camera-path coverage is looked up HERE, not in RenderValidation -- that file is kept free of
	// engine headers so it can be unit-tested standalone.
	RenderCameraPathCoverage coverage;
	coverage.resolved = true;
	coverage.keyCount = RenderHost_PathKeyCount();
	if (coverage.keyCount >= 2)
		RenderHost_PathCoverage(coverage.firstTick, coverage.lastTick);
	// Validate against the capture rate (= output fps). Frame count is ticks * fps / tickRate.
	RenderValidationResult result = RenderValidate(s_range, s_camera, s_extension,
		RenderCaptureFps(out.fps), RenderHost_TickRate(), coverage);
	RenderValidateOutput(result,
		!out.folder.empty(),
		RenderFormatSupported(out.format),
		RenderFormatName(out.format),
		RenderFormatNote(out.format));
	RenderValidateRepeatability(result,
		RenderCaptureFps(out.fps), RenderHost_TickRate());
	const PassPlan plan = BuildPassPlan(RenderPassesRef().passes);
	for (const PassConfig& pass : plan.passes) {
		if (const char* why = RenderHost_PassUnavailable(pass))
			result.AddFatal(std::string(PassTypeLabel(pass.type)) + " / " + VisibleElementsLabel(pass.elements) + ": " + why);
	}
	if (plan.StreamCount() == 0)
		result.AddFatal("no render pass selected (add at least one pass)");
	// The pre-tonemap EXR sequence is written at the BeforeUi boundary of the ONE recorded render;
	// extra renders would each fire it. It is a Beauty + Everything feature.
	if (out.format == RenderOutputFormat::Exr
		&& !(plan.StreamCount() == 1 && PassIsDirectCapture(plan.passes[0]) && plan.passes[0].elements == VisibleElements::Everything))
		result.AddFatal("EXR output records only the Beauty + Everything plate; remove the other passes or pick another format");
	return result;
}

RenderValidationResult Render_ValidateCurrentJobFull() {
	return ValidateCurrentJob();
}

const char* Render_GetValidationString() {
	static RenderValidationResult result;
	result = ValidateCurrentJob();
	// RenderValidation.cpp is deliberately dependency-free (it is unit-tested standalone), so
	// the probe lives here, at the only live caller, and is change-gated: this runs per frame.
	if (MvmDebugLog_Active()) {
		static std::string s_last;
		std::string now = std::string(result.Token()) + "|f=" + std::to_string(result.FatalCount())
			+ "|w=" + std::to_string(result.WarningCount()) + "|" + result.FirstFatal();
		if (now != s_last) {
			s_last = now;
			MvmDebugLog_Linef("ui.render.validate",
				"token=%s fatal=%d warn=%d mode=%d range=%d..%d cam=%d ext=%d first='%s'",
				result.Token(), result.FatalCount(), result.WarningCount(),
				(int)s_range.mode, s_range.startTick, s_range.endTickExclusive,
				(int)s_camera, (int)s_extension, result.FirstFatal().c_str());
		}
	}
	return result.Token();
}

// The whole render-workspace state as one JSON object, for the Camera Editor state bridge.
// Lives here rather than in CameraEditorHud so the HUD file does not have to know the render
// model's field set (it already carries camera path, follow, cosmetics and debug blocks).
std::string Render_BuildStateJson() {
	RenderRange resolved;
	const bool resolvedOk = Render_ResolveRangeForDisplay(resolved);
	int demoStart = -1, demoEnd = -1;
	Render_GetDemoBounds(demoStart, demoEnd);
	const RenderOutputSettings& out = RenderOutputSettingsRef();
	const RenderPasses& passes = RenderPassesRef();
	const RenderValidationResult v = ValidateCurrentJob();

	std::ostringstream o;
	o << "{"
		<< "\"open\":" << (Render_IsModalOpen() ? "true" : "false")
		<< ",\"jobActive\":" << (Render_IsJobActive() ? "true" : "false")
		<< ",\"tab\":" << Render_GetModalTab()
		<< ",\"rangeMode\":" << (int)s_range.mode
		<< ",\"startTick\":" << s_range.startTick
		<< ",\"endTickExclusive\":" << s_range.endTickExclusive
		<< ",\"resStart\":" << resolved.startTick
		<< ",\"resEnd\":" << resolved.endTickExclusive
		<< ",\"resOk\":" << (resolvedOk && resolved.Valid() ? "true" : "false")
		<< ",\"demoStart\":" << demoStart
		<< ",\"demoEnd\":" << demoEnd
		<< ",\"cameraSource\":" << (int)s_camera
		<< ",\"extension\":" << (int)s_extension
		<< ",\"sessionState\":" << (int)Render_GetSessionState()
		<< ",\"validation\":\"" << v.Token() << "\""
		<< ",\"fatalCount\":" << v.FatalCount()
		<< ",\"warnCount\":" << v.WarningCount()
		<< ",\"firstFatal\":" << JsonQuote(v.FirstFatal())
		<< ",\"out\":{"
		<< "\"folder\":" << JsonQuote(WideToUtf8(out.folder))
		<< ",\"folderPending\":" << (RenderOutput_PickFolderPending() ? "true" : "false")
		<< ",\"project\":" << JsonQuote(out.project)
		<< ",\"format\":" << JsonQuote(RenderFormatToken(out.format))
		<< ",\"formatName\":" << JsonQuote(RenderFormatName(out.format))
		<< ",\"formatNote\":" << JsonQuote(RenderFormatNote(out.format))
		<< ",\"formatOk\":" << (RenderFormatSupported(out.format) ? "true" : "false")
		<< ",\"fps\":" << out.fps
		// Capture samples at the chosen FPS; dial is preview-only and forced to 1 during the job.
		<< ",\"timescale\":1"
		<< ",\"captureFps\":" << RenderCaptureFps(out.fps)
		<< ",\"encoder\":" << (int)out.encoder
		<< ",\"speed\":" << (int)out.speed
		<< ",\"crf\":" << out.crf
		<< ",\"bitrateKbps\":" << out.bitrateKbps
		<< ",\"nvenc\":" << (int)RenderEncoderDetect_State()
		<< ",\"nvencNote\":" << JsonQuote(RenderEncoderDetect_Describe())
		<< "}"
		<< ",\"pass\":{"
		<< "\"hud\":" << (int)passes.hud
		<< ",\"cameraData\":" << (passes.cameraData ? "true" : "false")
		<< ",\"blood\":" << (passes.blood ? "true" : "false")
		<< ",\"key\":[" << (int)(passes.keyColor[0] * 255.0f + 0.5f) << "," << (int)(passes.keyColor[1] * 255.0f + 0.5f)
		<< "," << (int)(passes.keyColor[2] * 255.0f + 0.5f) << "]"
		<< ",\"depthNear\":" << passes.depthNear
		<< ",\"depthFar\":" << passes.depthFar
		<< ",\"preview\":{\"type\":" << JsonQuote(PassTypeToken(passes.preview.type))
		<< ",\"elements\":" << JsonQuote(VisibleElementsToken(passes.preview.elements)) << "}"
		<< ",\"list\":[";
	{
		bool first = true;
		for (const PassConfig& pc : passes.passes) {
			o << (first ? "" : ",") << "{\"type\":" << JsonQuote(PassTypeToken(pc.type))
				<< ",\"elements\":" << JsonQuote(VisibleElementsToken(pc.elements))
				<< ",\"stream\":" << JsonQuote(PassStreamName(pc)) << "}";
			first = false;
		}
		const PassPlan plan = BuildPassPlan(passes.passes);
		o << "],\"rendersPerFrame\":" << RenderHost_PhysicalRendersPerFrame(plan.passes) << "}";
	}

	// Live job progress, so the JOB column can show a real progress readout, and the chrome-hide
	// gate the JS uses to keep Source MVM panels out of before-present captures.
	{
		const RenderProgress& p = RenderSessionDriver_Progress();
		o << ",\"prog\":{"
			<< "\"curTick\":" << p.curTick
			<< ",\"framesDone\":" << p.framesDone
			<< ",\"framesTotal\":" << p.framesTotal
			<< ",\"percent\":" << (int)(p.percent + 0.5)
			<< ",\"passes\":" << p.passes
			<< ",\"rendersPerFrame\":" << p.rendersPerFrame
			<< ",\"physicalDone\":" << p.physicalDone
			<< ",\"physicalTotal\":" << p.physicalTotal
			<< ",\"phase\":" << JsonQuote(p.phase)
			<< ",\"lastError\":" << JsonQuote(p.lastError)
			<< ",\"takeDir\":" << JsonQuote(p.takeDirUtf8)
			<< "}"
			<< ",\"captureHide\":" << (RenderSessionDriver_WantsChromeHidden() ? "true" : "false");
	}

	o
		// What each camera source would actually be able to do RIGHT NOW. The UI needs this to
		// refuse a source up front (and say why) instead of issuing a command that warns to a
		// console nobody has open -- which is what made Camera Path and Follow Camera look like
		// dead buttons.
		<< ",\"avail\":{"
		<< "\"pathKeys\":" << RenderHost_PathKeyCount()
		<< ",\"pathSel\":" << RenderHost_PathSelected()
		<< ",\"followReady\":" << (RenderHost_FollowReady() ? "true" : "false")
		<< ",\"spectating\":" << (RenderHost_Spectating() ? "true" : "false")
		<< "}"
		// The header's SOUND chip and the OUTPUT tab's volume (RenderDoneSound.h).
		<< ",\"sound\":{\"on\":" << (RenderDoneSound_Enabled() ? "true" : "false")
		<< ",\"volume\":" << RenderDoneSound_Volume() << "}";
	RenderTakeHistory_AppendJson(o);
	o << "}";
	return o.str();
}

// One-line dump for `state.render` (emitted by MvmCommandTrace after any
// `mirv_filmmaker render ...`). Everything the render workspace decides from, in one place.
std::string Render_DebugStateLine() {
	RenderRange resolved;
	const bool resOk = ResolveRangeForDisplayImpl(resolved);
	int demoStart = -1, demoEnd = -1;
	const bool boundsOk = Render_GetDemoBounds(demoStart, demoEnd);
	const RenderValidationResult v = ValidateCurrentJob();
	std::ostringstream o;
	o << "modalOpen=" << (s_modalOpen ? 1 : 0)
		<< " tab=" << s_modalTab
		<< " jobActive=" << (s_jobActive ? 1 : 0)
		<< " sessionState=" << RenderSession::StateName(RenderSessionRef().State())
		<< " cancelReq=" << (RenderSessionRef().IsCancelRequested() ? 1 : 0)
		<< " rangeMode=" << (int)s_range.mode
		<< " stored=" << s_range.startTick << ".." << s_range.endTickExclusive
		<< " storedValid=" << (s_range.Valid() ? 1 : 0)
		<< " resolved=" << resolved.startTick << ".." << resolved.endTickExclusive
		<< " resolvedOk=" << (resOk ? 1 : 0)
		<< " demoBounds=" << demoStart << ".." << demoEnd << " boundsOk=" << (boundsOk ? 1 : 0)
		<< " camera=" << (int)s_camera
		<< " extension=" << (int)s_extension
		<< " validation=" << v.Token() << " fatal=" << v.FatalCount() << " warn=" << v.WarningCount()
		<< " firstFatal='" << v.FirstFatal() << "'"
		<< " pathCount=" << RenderHost_PathKeyCount()
		<< " pathSel=" << RenderHost_PathSelected();
	return o.str();
}

// -- help --

static void PrintHelp(const char* cmd) {
	advancedfx::Message(
		"%s render open      - open render modal.\n"
		"%s render close     - close render modal.\n"
		"%s render status   - show current render state.\n"
		"%s render range full|round|path|segment|custom <start> <endExcl>\n"
		"%s render in <tick> - set render in point.\n"
		"%s render out <tick> - set render out point.\n"
		"%s render setrange <start> <endExcl> - store resolved ticks without changing the mode.\n"
		"%s render clearrange - clear render range.\n"
		"%s render validate  - validate current render job.\n"
		"%s render camera current|pov|thirdperson|free|path|follow\n"
		"%s render extension fail|holdfirst|holdlast|clamp\n"
		"%s render output [...] - output folder, project name, format, fps, size.\n"
		"%s render pass [...]   - game HUD policy + camera-data sidecar.\n"
		"%s render tab <0-3> - select modal tab.\n"
		"%s render restorecoverage - what the state-restore guard does and does NOT put back.\n"
		"%s render start     - begin the recorder-backed render session.\n"
		"%s render cancel    - request cancellation of active render.\n"
		"%s render sound on|off|toggle|volume <0..100>|test - the chime when a job finishes or fails.\n"
		"%s render takes [refresh] - this demo's earlier takes in the output folder, newest first.\n"
		"%s render import <n> - start from take n's settings: range, camera, passes, fps, format.\n"
		, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
	RenderHost_PrintHelp(cmd);
}

// The chime a job plays when it ends (RenderDoneSound.h): the workspace header's bell and volume.
static void RunSoundCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* what = argc >= 4 ? args->ArgV(3) : "";
	if (0 == _stricmp(what, "on") || 0 == strcmp(what, "1")) {
		RenderDoneSound_SetEnabled(true);
	} else if (0 == _stricmp(what, "off") || 0 == strcmp(what, "0")) {
		RenderDoneSound_SetEnabled(false);
	} else if (0 == _stricmp(what, "toggle")) {
		RenderDoneSound_SetEnabled(!RenderDoneSound_Enabled());
	} else if (0 == _stricmp(what, "volume") && argc >= 5) {
		RenderDoneSound_SetVolume(atoi(args->ArgV(4)));
	} else if (0 == _stricmp(what, "test")) {
		RenderDoneSound_Preview();
		return;
	} else if (*what) {
		advancedfx::Warning("usage: %s render sound on|off|toggle|volume <0..100>|test\n", cmd);
		return;
	}
	advancedfx::Message("%s: render sound %s, volume %d%%.\n", cmd,
		RenderDoneSound_Enabled() ? "on" : "off", RenderDoneSound_Volume());
}

// -- handlers --

// Range = the camera path's first key to its last. False (range untouched) without 2+ keys.
static bool SetCameraPathRange(const char* cmd) {
	const int keys = RenderHost_PathKeyCount();
	if (keys < 2) {
		advancedfx::Warning("%s: Camera Path Bounds needs 2+ camera keys (place them with K).\n", cmd);
		return false;
	}
	s_range.mode = RenderRangeMode::CameraPathBounds;
	s_range.startTick = RenderHost_PathKeyTick(0);
	s_range.endTickExclusive = RenderHost_PathKeyTick(keys - 1) + 1;
	advancedfx::Message("%s: range CameraPathBounds (%d .. %d).\n",
		cmd, s_range.startTick, s_range.endTickExclusive - 1);
	return true;
}

static void RunRangeCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 4) {
		advancedfx::Message(
			"%s: range mode=%d start=%d end=%d\n",
			cmd, (int)s_range.mode, s_range.startTick, s_range.endTickExclusive);
		return;
	}
	const char* mode = args->ArgV(3);
	if (0 == _stricmp(mode, "full")) {
		s_range.mode = RenderRangeMode::FullDemo;
		// Resolve immediately so the UI (and validation) shows real ticks, not a dead mode name.
		int a = 0, b = 0;
		if (Render_GetDemoBounds(a, b)) {
			s_range.startTick = a;
			s_range.endTickExclusive = b + 1;
			advancedfx::Message("%s: range FullDemo (%d .. %d).\n", cmd, a, b);
		} else {
			advancedfx::Warning("%s: FullDemo range: demo bounds unavailable (no demo/header?).\n", cmd);
		}
	} else if (0 == _stricmp(mode, "round")) {
		s_range.mode = RenderRangeMode::CurrentRound;
		// Zero the ticks: the timeline JS resolves the current round from the native round
		// markers and pushes it back via "render setrange" (see CameraTimelineJs). Starting
		// invalid keeps stale ticks from another mode posing as the round span.
		s_range.startTick = 0;
		s_range.endTickExclusive = 0;
		advancedfx::Message("%s: range CurrentRound (resolves from the timeline round markers).\n", cmd);
	} else if (0 == _stricmp(mode, "path")) {
		if (SetCameraPathRange(cmd) && s_camera != RenderCameraSource::CameraPath) {
			// Rendering the path's span means rendering the path.
			s_camera = RenderCameraSource::CameraPath;
			Render_ApplyCameraSourceLive(s_camera, cmd);
			advancedfx::Message("%s: camera source set to Camera Path with the range.\n", cmd);
		}
	} else if (0 == _stricmp(mode, "segment")) {
		s_range.mode = RenderRangeMode::SelectedCameraSegment;
		const int sel = RenderHost_PathSelected();
		if (sel >= 0 && sel + 1 < RenderHost_PathKeyCount()) {
			s_range.startTick = RenderHost_PathKeyTick(sel);
			s_range.endTickExclusive = RenderHost_PathKeyTick(sel + 1) + 1;
			advancedfx::Message("%s: range SelectedCameraSegment (%d .. %d).\n",
				cmd, s_range.startTick, s_range.endTickExclusive - 1);
		} else {
			advancedfx::Warning("%s: Selected Segment needs a camera key selected that has a NEXT key "
				"(select one in the Camera tab; the last key has no outgoing segment).\n", cmd);
		}
	} else if (0 == _stricmp(mode, "custom")) {
		s_range.mode = RenderRangeMode::CustomTicks;
		if (argc >= 6) {
			s_range.startTick = atoi(args->ArgV(4));
			s_range.endTickExclusive = atoi(args->ArgV(5));
			RememberCustomRange();
		} else if (s_customInTick >= 0 && s_customOutTickExclusive > s_customInTick) {
			// Coming back from Path/Round/Full: restore the user's own span, not whatever ticks
			// the previous mode happened to leave behind in s_range.
			s_range.startTick = s_customInTick;
			s_range.endTickExclusive = s_customOutTickExclusive;
		} else if (!s_range.Valid()) {
			// Reactive default: clicking "Custom Ticks" with no usable In/Out yet seeds a
			// 10-second window starting at the playhead, so the button visibly does something.
			const int cur = RenderHost_CurrentTick();
			s_range.startTick = cur;
			s_range.endTickExclusive = cur + (int)std::lround(10.0 * RenderHost_TickRate());
		}
		advancedfx::Message("%s: range CustomTicks start=%d end=%d\n",
			cmd, s_range.startTick, s_range.endTickExclusive);
	} else {
		advancedfx::Warning("%s: unknown range mode '%s'.\n", cmd, mode);
	}
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("ui.render.action", "range mode=%d start=%d end=%d",
			(int)s_range.mode, s_range.startTick, s_range.endTickExclusive);
}

static void RunInCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 4) {
		advancedfx::Warning("usage: %s render in <tick>\n", cmd);
		return;
	}
	s_range.mode = RenderRangeMode::CustomTicks;
	s_range.startTick = atoi(args->ArgV(3));
	// Dragging In past Out (or setting it from the console) must not leave an inverted range that
	// every downstream reader then has to special-case. Push Out along instead, keeping the
	// minimum one-tick span the exclusive end implies.
	if (s_range.endTickExclusive <= s_range.startTick)
		s_range.endTickExclusive = s_range.startTick + 1;
	RememberCustomRange();
	advancedfx::Message("%s: in point set to %d (out %d).\n",
		cmd, s_range.startTick, s_range.endTickExclusive);
}

static void RunOutCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 4) {
		advancedfx::Warning("usage: %s render out <tick>\n", cmd);
		return;
	}
	s_range.mode = RenderRangeMode::CustomTicks;
	s_range.endTickExclusive = atoi(args->ArgV(3));
	// Symmetric to "in": dragging Out before In pushes In along rather than inverting the range.
	if (s_range.startTick >= s_range.endTickExclusive)
		s_range.startTick = (s_range.endTickExclusive > 0) ? s_range.endTickExclusive - 1 : 0;
	RememberCustomRange();
	advancedfx::Message("%s: out point set to %d (in %d).\n",
		cmd, s_range.endTickExclusive, s_range.startTick);
}

static void RunValidateCommand(const char* cmd) {
	const RenderValidationResult result = ValidateCurrentJob();
	if (result.Ok())
		advancedfx::Message("%s: %s\n", cmd, result.Summary().c_str());
	else
		advancedfx::Warning("%s: validation failed:\n%s", cmd, result.Summary().c_str());
}

static void RunStatusCommand(const char* cmd) {
	const char* rangeStatus;
	const char* validation;
	if (s_range.mode == RenderRangeMode::CustomTicks) {
		rangeStatus = s_range.Valid() ? "ok" : "invalid (start >= end)";
		validation = s_range.Valid() ? "available" : "not available";
	} else {
		rangeStatus = "unresolved";
		validation = "not available";
	}

	const char* extName = "Fail";
	switch (s_extension) {
		case CameraPathExtension::HoldFirst: extName = "HoldFirst"; break;
		case CameraPathExtension::HoldLast: extName = "HoldLast"; break;
		case CameraPathExtension::Clamp: extName = "Clamp"; break;
	}

	advancedfx::Message(
		"%s render status:\n"
		"  job active     : %s\n"
		"  cancel pending : %s\n"
		"  range mode     : %d\n"
		"  start tick     : %d\n"
		"  end exclusive  : %d\n"
		"  range status   : %s\n"
		"  validation     : %s\n"
		"  camera source  : %d\n"
		"  extension      : %s\n",
		cmd,
		s_jobActive ? "yes" : "no",
		RenderSessionRef().IsCancelRequested() ? "yes" : "no",
		(int)s_range.mode,
		s_range.startTick,
		s_range.endTickExclusive,
		rangeStatus,
		validation,
		(int)s_camera,
		extName);

	// Surfaced in plain status, not behind a debug flag: the guard covers only part of the state a
	// job will eventually touch, and a caller assuming full coverage is exactly the failure this
	// is meant to prevent.
	std::size_t covered = 0, uncovered = 0;
	RenderHost_RestoreCoverage(covered, uncovered);
	advancedfx::Message("  state restore  : %zu categories covered, %zu NOT covered"
		" (see \"%s render restorecoverage\")\n",
		covered, uncovered, cmd);
}

// Make the LIVE view match the chosen render source, so the workspace preview is WYSIWYG ("click
// Third-Person -> go to third person"). The game decides how (RenderHost_ApplyCameraSource).
void Render_ApplyCameraSourceLive(RenderCameraSource src, const char* cmd) {
	RenderHost_ApplyCameraSource(src, cmd);
}

static void RunCameraCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 4) {
		advancedfx::Message("%s: camera source=%d\n", cmd, (int)s_camera);
		return;
	}
	const char* mode = args->ArgV(3);
	if (0 == _stricmp(mode, "current")) {
		s_camera = RenderCameraSource::CurrentViewer;
	} else if (0 == _stricmp(mode, "pov")) {
		s_camera = RenderCameraSource::Pov;
	} else if (0 == _stricmp(mode, "thirdperson")) {
		s_camera = RenderCameraSource::ThirdPerson;
	} else if (0 == _stricmp(mode, "free")) {
		s_camera = RenderCameraSource::FreeCamera;
	} else if (0 == _stricmp(mode, "path")) {
		s_camera = RenderCameraSource::CameraPath;
	} else if (0 == _stricmp(mode, "follow")) {
		s_camera = RenderCameraSource::FollowCamera;
	} else {
		advancedfx::Warning("%s: unknown camera source '%s'.\n", cmd, mode);
		return;
	}
	Render_ApplyCameraSourceLive(s_camera, cmd);
	advancedfx::Message("%s: camera source=%d\n", cmd, (int)s_camera);
	// Playing the path renders the path's span, unless a piece of it is already chosen.
	if (s_camera == RenderCameraSource::CameraPath && PathCameraTakesPathRange(s_range.mode)
		&& RenderHost_PathKeyCount() >= 2)
		SetCameraPathRange(cmd);
}

// -- output + passes --

static void RunOutputCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	RenderOutputSettings& out = RenderOutputSettingsRef();
	if (argc < 4) {
		advancedfx::Message(
			"%s: output folder='%s' project='%s' format=%s fps=%d size=game resolution (capture host_framerate=%d)\n",
			cmd,
			out.folder.empty() ? "(not chosen)" : WideToUtf8(out.folder).c_str(),
			out.project.c_str(), RenderFormatToken(out.format), out.fps,
			RenderCaptureFps(out.fps));
		advancedfx::Message(
			"%s render output folder            - open the native folder picker.\n"
			"%s render output path <path>       - set the output root directly.\n"
			"%s render output project <name>    - set the take name (sanitized).\n"
			"%s render output format <mp4|avi|gif|png|tiff|tga|bmp|exr>\n"
			"%s render output fps <value>\n"
			"%s render output encoder auto|cpu|gpu - auto (default) uses NVENC when proven; the GPU\n"
			"                                      encoder never holds a frame up, x264 can (MP4).\n"
			"%s render output quality <0-51>    - constant-quality CRF/CQ; lower = better.\n"
			"%s render output bitrate <kbps>    - target bitrate; 0 = use quality mode.\n"
			"%s render output speed quality|balanced|fast - encode speed preset.\n",
			cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
		advancedfx::Message("%s: %s\n", cmd, RenderEncoderDetect_Describe());
		return;
	}

	const char* sub = args->ArgV(3);
	if (0 == _stricmp(sub, "folder")) {
		if (RenderOutput_PickFolderPending()) {
			advancedfx::Warning("%s: the folder picker is already open.\n", cmd);
			return;
		}
		RenderOutput_PickFolderAsync();
		advancedfx::Message("%s: folder picker opened (it runs on its own thread).\n", cmd);
	} else if (0 == _stricmp(sub, "path")) {
		if (argc < 5) { advancedfx::Warning("usage: %s render output path <path>\n", cmd); return; }
		// Everything after the token, so an unquoted path with spaces still lands intact.
		//
		// DRIVE-COLON TRAP: the console tokenizer splits ':' into its own argument, so
		// "C:\takes" arrives as {"C", ":", "\takes"} and a naive space-join produced
		// "C : \takes" -- which failed directory creation for EVERY absolute Windows path.
		// A ':' never has spaces around it in a path, so suppress them there.
		std::string joined;
		for (int i = 4; i < argc; ++i) {
			const char* tok = args->ArgV(i);
			if (!tok) continue;
			const bool isColon = (tok[0] == ':' && tok[1] == 0);
			const bool prevColon = !joined.empty() && joined[joined.size() - 1] == ':';
			if (i > 4 && !isColon && !prevColon) joined += ' ';
			joined += tok;
		}
		out.folder.assign(joined.begin(), joined.end());
		advancedfx::Message("%s: output folder set to '%s'.\n", cmd, joined.c_str());
	} else if (0 == _stricmp(sub, "project")) {
		if (argc < 5) { advancedfx::Warning("usage: %s render output project <name>\n", cmd); return; }
		std::string joined;
		for (int i = 4; i < argc; ++i) { if (i > 4) joined += ' '; joined += args->ArgV(i); }
		out.project = SanitizeProjectName(joined);
		advancedfx::Message("%s: project name = %s\n", cmd, out.project.c_str());
	} else if (0 == _stricmp(sub, "format")) {
		if (argc < 5) { advancedfx::Warning("usage: %s render output format <token>\n", cmd); return; }
		RenderOutputFormat fmt;
		if (!RenderFormatFromToken(args->ArgV(4), fmt)) {
			advancedfx::Warning("%s: unknown output format '%s'.\n", cmd, args->ArgV(4));
			return;
		}
		out.format = fmt;
		// Selecting an unsupported format is allowed (the UI shows why it is unavailable) but it
		// must say so here too, rather than failing silently at start time.
		if (!RenderFormatSupported(fmt))
			advancedfx::Warning("%s: %s -- %s\n", cmd, RenderFormatName(fmt), RenderFormatNote(fmt));
		else
			advancedfx::Message("%s: output format = %s\n", cmd, RenderFormatName(fmt));
	} else if (0 == _stricmp(sub, "fps")) {
		if (argc < 5) { advancedfx::Warning("usage: %s render output fps <value>\n", cmd); return; }
		const int fps = atoi(args->ArgV(4));
		if (fps < 1 || fps > 1000) {
			advancedfx::Warning("%s: fps must be 1..1000.\n", cmd);
			return;
		}
		out.fps = fps;
		advancedfx::Message("%s: output fps = %d (host_framerate set automatically on render)\n",
			cmd, out.fps);
	} else if (0 == _stricmp(sub, "encoder")) {
		if (argc < 5) { advancedfx::Warning("usage: %s render output encoder auto|cpu|gpu\n", cmd); return; }
		const char* v = args->ArgV(4);
		if (0 == _stricmp(v, "auto")) out.encoder = RenderEncoder::Auto;
		else if (0 == _stricmp(v, "cpu") || 0 == _stricmp(v, "x264")) out.encoder = RenderEncoder::X264;
		else if (0 == _stricmp(v, "gpu") || 0 == _stricmp(v, "nvenc")) out.encoder = RenderEncoder::Nvenc;
		else { advancedfx::Warning("%s: encoder must be auto|cpu|gpu.\n", cmd); return; }
		RenderEncoderDetect_EnsureProbe();
		advancedfx::Message("%s: encoder = %s. %s\n", cmd,
			out.encoder == RenderEncoder::Nvenc ? "GPU (NVENC)"
				: out.encoder == RenderEncoder::X264 ? "CPU (x264)"
				: "Auto (GPU when available -- much faster; encode is the dominant per-frame cost)",
			RenderEncoderDetect_Describe());
	} else if (0 == _stricmp(sub, "quality")) {
		if (argc < 5) { advancedfx::Warning("usage: %s render output quality <0-51>\n", cmd); return; }
		const int q = atoi(args->ArgV(4));
		if (q < 0 || q > 51) { advancedfx::Warning("%s: quality must be 0..51.\n", cmd); return; }
		out.crf = q;
		out.bitrateKbps = 0; // choosing a CRF is choosing quality mode
		advancedfx::Message("%s: quality = CRF %d (bitrate mode off).\n", cmd, q);
	} else if (0 == _stricmp(sub, "bitrate")) {
		if (argc < 5) { advancedfx::Warning("usage: %s render output bitrate <kbps> (0 = quality mode)\n", cmd); return; }
		const int b = atoi(args->ArgV(4));
		if (b < 0 || b > 400000) { advancedfx::Warning("%s: bitrate out of range.\n", cmd); return; }
		out.bitrateKbps = b;
		advancedfx::Message("%s: bitrate = %d kbps%s\n", cmd, b, b == 0 ? " (quality/CRF mode)" : "");
	} else if (0 == _stricmp(sub, "speed")) {
		if (argc < 5) { advancedfx::Warning("usage: %s render output speed quality|balanced|fast\n", cmd); return; }
		const char* v = args->ArgV(4);
		if (0 == _stricmp(v, "quality")) out.speed = RenderEncodeSpeed::Quality;
		else if (0 == _stricmp(v, "balanced")) out.speed = RenderEncodeSpeed::Balanced;
		else if (0 == _stricmp(v, "fast")) out.speed = RenderEncodeSpeed::Fast;
		else { advancedfx::Warning("%s: speed must be quality|balanced|fast.\n", cmd); return; }
		advancedfx::Message("%s: encode speed = %s.\n", cmd, v);
	} else {
		advancedfx::Warning("%s: unknown output setting '%s'.\n", cmd, sub);
		return;
	}

	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("ui.render.output", "set %s -> format=%s fps=%d folderSet=%d project=%s",
			sub, RenderFormatToken(out.format), out.fps,
			out.folder.empty() ? 0 : 1, out.project.c_str());
}

static void PrintPasses(const char* cmd) {
	const RenderPasses& p = RenderPassesRef();
	const PassPlan plan = BuildPassPlan(p.passes);
	advancedfx::Message("%s: %d pass(es), %d render(s) per output frame, hud=%d cameraData=%d blood=%d preview=%s\n", cmd,
		plan.StreamCount(), RenderHost_PhysicalRendersPerFrame(plan.passes), (int)p.hud, p.cameraData ? 1 : 0, p.blood ? 1 : 0,
		p.preview == PassConfig{} ? "off" : PassStreamName(p.preview).c_str());
	for (const PassConfig& pc : plan.passes)
		advancedfx::Message("   %-22s %s + %s\n", PassStreamName(pc).c_str(), PassTypeLabel(pc.type),
			VisibleElementsLabel(pc.elements));
	float tolerance = 0.0f;
	if (RenderHost_PassKeyTolerance(tolerance))
		advancedfx::Message("   key=%.3f %.3f %.3f  depth near=%g far=%g  keytolerance=%.3f\n",
			p.keyColor[0], p.keyColor[1], p.keyColor[2], p.depthNear, p.depthFar, tolerance);
	else
		advancedfx::Message("   key=%.3f %.3f %.3f  depth near=%g far=%g\n",
			p.keyColor[0], p.keyColor[1], p.keyColor[2], p.depthNear, p.depthFar);
}

static void RunPassCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	RenderPasses& p = RenderPassesRef();
	if (argc < 4) {
		PrintPasses(cmd);
		advancedfx::Message(
			"%s render pass add|remove|toggle <beauty|depth|normal|key> <all|worldplayers|world|players|gun>\n"
			"%s render pass clear                   - no passes (a job needs at least one).\n"
			"%s render pass key <r> <g> <b>         - isolation background colour, 0-255.\n"
			"%s render pass depth <near> <far>      - depth: white at near, black at far (units).\n"
			"%s render pass keytolerance <0..1>     - players-only background match tolerance.\n"
			"%s render pass preview <type> <elements>|off - show one pass live in the workspace view.\n"
			"%s render pass hud none|game|full      - game UI in Beauty + Everything.\n"
			"%s render pass cameradata 0|1          - per-frame camera sidecar.\n"
			"%s render pass blood 0|1               - 0: no blood spawns during the job, in any pass.\n",
			cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
		return;
	}
	const char* sub = args->ArgV(3);
	const char* val = (argc >= 5) ? args->ArgV(4) : nullptr;
	if (0 == _stricmp(sub, "add") || 0 == _stricmp(sub, "remove") || 0 == _stricmp(sub, "toggle")) {
		PassConfig pc;
		if (argc < 6 || !PassTypeFromToken(args->ArgV(4), pc.type) || !VisibleElementsFromToken(args->ArgV(5), pc.elements)) {
			advancedfx::Warning("usage: %s render pass %s <beauty|depth|normal|key> <all|worldplayers|world|players|gun>\n", cmd, sub);
			return;
		}
		if (!PassAllowed(pc)) {
			advancedfx::Warning("%s: Players only and Gun only take Key (their colour plate); the other groups take Beauty.\n", cmd);
			return;
		}
		auto it = p.passes.begin();
		for (; it != p.passes.end(); ++it) if (*it == pc) break;
		const bool present = it != p.passes.end();
		const bool want = 0 == _stricmp(sub, "add") || (0 == _stricmp(sub, "toggle") && !present);
		if (want && !present) p.passes.push_back(pc);
		if (!want && present) {
			p.passes.erase(it);
			// The live view only ever shows a pass the job renders.
			if (p.preview == pc) p.preview = PassConfig{};
		}
	} else if (0 == _stricmp(sub, "clear")) {
		p.passes.clear();
		p.preview = PassConfig{};
	} else if (0 == _stricmp(sub, "key")) {
		if (argc < 7) { advancedfx::Warning("usage: %s render pass key <r> <g> <b> (0-255)\n", cmd); return; }
		for (int k = 0; k < 3; ++k) {
			int v = atoi(args->ArgV(4 + k));
			p.keyColor[k] = (float)(v < 0 ? 0 : (v > 255 ? 255 : v)) / 255.0f;
		}
	} else if (0 == _stricmp(sub, "depth")) {
		if (argc < 6) { advancedfx::Warning("usage: %s render pass depth <near> <far>\n", cmd); return; }
		const float n = (float)atof(args->ArgV(4)), f = (float)atof(args->ArgV(5));
		if (!(n >= 0.0f) || !(f > n)) { advancedfx::Warning("%s: depth needs 0 <= near < far.\n", cmd); return; }
		p.depthNear = n; p.depthFar = f;
	} else if (0 == _stricmp(sub, "depthdebug")) {
		if (!val) { advancedfx::Warning("usage: %s render pass depthdebug 0|1\n", cmd); return; }
		if (!RenderHost_SetPassDepthDebug(0 != atoi(val))) { advancedfx::Warning("%s: this game has no depth debug.\n", cmd); return; }
	} else if (0 == _stricmp(sub, "keytolerance")) {
		if (!val) { advancedfx::Warning("usage: %s render pass keytolerance <0..1>\n", cmd); return; }
		if (!RenderHost_SetPassKeyTolerance((float)atof(val))) { advancedfx::Warning("%s: this game has no key tolerance.\n", cmd); return; }
	} else if (0 == _stricmp(sub, "preview")) {
		PassConfig pc;
		if (val && 0 == _stricmp(val, "off")) {
			p.preview = PassConfig{};
		} else if (argc < 6 || !PassTypeFromToken(val, pc.type) || !VisibleElementsFromToken(args->ArgV(5), pc.elements)) {
			advancedfx::Warning("usage: %s render pass preview <beauty|depth|normal|key> <all|worldplayers|world|players> | off\n", cmd);
			return;
		} else if (pc != PassConfig{} && std::find(p.passes.begin(), p.passes.end(), pc) == p.passes.end()) {
			// The live view only ever shows a pass the job renders.
			advancedfx::Warning("%s: %s is not on -- turn it on in the pass grid first.\n", cmd, PassStreamName(pc).c_str());
			return;
		} else {
			p.preview = pc;
		}
	} else if (0 == _stricmp(sub, "hud")) {
		if (!val) { advancedfx::Warning("usage: %s render pass hud none|game|full\n", cmd); return; }
		if (0 == _stricmp(val, "none")) p.hud = RenderHudMode::None;
		else if (0 == _stricmp(val, "game")) p.hud = RenderHudMode::Game;
		else if (0 == _stricmp(val, "full")) p.hud = RenderHudMode::Full;
		else { advancedfx::Warning("%s: hud must be none|game|full.\n", cmd); return; }
	} else if (0 == _stricmp(sub, "cameradata")) {
		if (!val) { advancedfx::Warning("usage: %s render pass cameradata 0|1\n", cmd); return; }
		p.cameraData = (0 != atoi(val));
	} else if (0 == _stricmp(sub, "blood")) {
		if (!val) { advancedfx::Warning("usage: %s render pass blood 0|1\n", cmd); return; }
		p.blood = (0 != atoi(val));
	} else {
		advancedfx::Warning("%s: unknown pass setting '%s'.\n", cmd, sub);
		return;
	}
	// The live view follows every pass setting it reads: the previewed pass, the depth range, the key.
	Render_ApplyLivePreview();
	PrintPasses(cmd);
	if (MvmDebugLog_Active()) {
		std::string list;
		for (const PassConfig& pc : p.passes) { if (!list.empty()) list += ","; list += PassStreamName(pc); }
		MvmDebugLog_Linef("ui.render.pass", "set %s passes=[%s] preview=%s hud=%d", sub, list.c_str(),
			p.preview == PassConfig{} ? "off" : PassStreamName(p.preview).c_str(), (int)p.hud);
	}
}

static void RunTabCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 4) {
		advancedfx::Message("%s: active tab=%d\n", cmd, s_modalTab);
		return;
	}
	int tab = atoi(args->ArgV(3));
	if (tab < 0 || tab > 3) {
		advancedfx::Warning("%s: tab must be 0-3 (Range, Camera, HUD, Output).\n", cmd);
		return;
	}
	s_modalTab = tab;
	advancedfx::Message("%s: active tab=%d\n", cmd, s_modalTab);
}

static void RunExtensionCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 4) {
		const char* name = "Fail";
		switch (s_extension) {
			case CameraPathExtension::HoldFirst: name = "HoldFirst"; break;
			case CameraPathExtension::HoldLast: name = "HoldLast"; break;
			case CameraPathExtension::Clamp: name = "Clamp"; break;
		}
		advancedfx::Message("%s: extension=%s\n", cmd, name);
		return;
	}
	const char* mode = args->ArgV(3);
	if (0 == _stricmp(mode, "fail")) {
		s_extension = CameraPathExtension::Fail;
	} else if (0 == _stricmp(mode, "holdfirst")) {
		s_extension = CameraPathExtension::HoldFirst;
	} else if (0 == _stricmp(mode, "holdlast")) {
		s_extension = CameraPathExtension::HoldLast;
	} else if (0 == _stricmp(mode, "clamp")) {
		s_extension = CameraPathExtension::Clamp;
	} else {
		advancedfx::Warning("%s: unknown extension mode '%s'.\n", cmd, mode);
		return;
	}
	advancedfx::Message("%s: extension=%d\n", cmd, (int)s_extension);
}

// -- main dispatch --

void Render_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 3) {
		PrintHelp(cmd);
		return;
	}

	const char* sub = args->ArgV(2);

	if (0 == _stricmp(sub, "open")) {
		// The workspace lives inside the Camera Editor; "open" outside it would set state for a
		// UI that does not exist and make the console path diverge from what a user can see.
		if (!RenderHost_EditorActive()) {
			advancedfx::Warning("%s: open the Camera Editor first (\"%s editor on\"), then Render.\n", cmd, cmd);
			return;
		}
		// Through the setter, not the flag: Render_SetModalOpen also arms the NVENC probe, and
		// writing s_modalOpen directly here is why the Output tab still read "GPU encoding not
		// probed yet" after opening the workspace -- with the default encoder on Auto, that also
		// meant Auto silently resolved to CPU because nothing had ever asked.
		Render_SetModalOpen(true);
		advancedfx::Message("%s: render workspace opened.\n", cmd);
	} else if (0 == _stricmp(sub, "close")) {
		Render_SetModalOpen(false);
		advancedfx::Message("%s: render modal closed.\n", cmd);
	} else if (0 == _stricmp(sub, "status")) {
		RunStatusCommand(cmd);
	} else if (0 == _stricmp(sub, "restorecoverage")) {
		advancedfx::Message("%s", RenderHost_RestoreCoverageReport().c_str());
	} else if (0 == _stricmp(sub, "range")) {
		RunRangeCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "in")) {
		RunInCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "out")) {
		RunOutCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "clearrange")) {
		s_range = RenderRange{};
		// Clearing is the one explicit "forget my span" action, so the memory goes with it.
		s_customInTick = -1;
		s_customOutTickExclusive = -1;
		advancedfx::Message("%s: render range cleared.\n", cmd);
	} else if (0 == _stricmp(sub, "setrange")) {
		// Store concrete ticks WITHOUT changing the mode. The timeline JS uses this to push
		// the CurrentRound span it resolved from the native round markers back into C++ state.
		if (argc < 5) {
			advancedfx::Warning("usage: %s render setrange <startTick> <endTickExclusive>\n", cmd);
			return;
		}
		s_range.startTick = atoi(args->ArgV(3));
		s_range.endTickExclusive = atoi(args->ArgV(4));
		// CameraTimelineJs re-pushes this while the round highlight is live, so log only real
		// changes. A round span that never lands, or lands then gets overwritten by a stale
		// push, is exactly what this line is for.
		if (MvmDebugLog_Active()) {
			static int s_lastStart = -999, s_lastEnd = -999;
			if (s_range.startTick != s_lastStart || s_range.endTickExclusive != s_lastEnd) {
				s_lastStart = s_range.startTick; s_lastEnd = s_range.endTickExclusive;
				MvmDebugLog_Linef("ui.render.setrange", "pushback mode=%d ticks=%d..%d valid=%d",
					(int)s_range.mode, s_range.startTick, s_range.endTickExclusive,
					s_range.Valid() ? 1 : 0);
			}
		}
	} else if (0 == _stricmp(sub, "validate")) {
		RunValidateCommand(cmd);
	} else if (0 == _stricmp(sub, "camera")) {
		RunCameraCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "output")) {
		RunOutputCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "pass")) {
		RunPassCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "tab")) {
		RunTabCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "extension")) {
		RunExtensionCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "start")) {
		if (s_jobActive || RenderSessionDriver_IsActive()) {
			advancedfx::Warning("%s: a render job is already active.\n", cmd);
		} else if (!s_modalOpen) {
			// Jobs start from the Render workspace, where the range, camera, passes and output
			// are visible. A headless start would run whatever settings happen to be stored,
			// with nobody looking at the validation panel.
			advancedfx::Warning("%s: open the Render workspace first (\"%s render open\" inside the Camera Editor).\n",
				cmd, cmd);
		} else {
			std::string err;
			if (!RenderSessionDriver_Start(err))
				advancedfx::Warning("%s: cannot start: %s.\n", cmd, err.c_str());
		}
	} else if (0 == _stricmp(sub, "sound")) {
		RunSoundCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "takes")) {
		if (argc >= 4 && 0 == _stricmp(args->ArgV(3), "refresh")) RenderTakeHistory_Refresh();
		RenderTakeHistory_PrintList(cmd);
	} else if (0 == _stricmp(sub, "import")) {
		if (s_jobActive || RenderSessionDriver_IsActive()) {
			advancedfx::Warning("%s: a render job is running; import its settings after it ends.\n", cmd);
			return;
		}
		if (argc < 4) { advancedfx::Warning("usage: %s render import <n> (from %s render takes)\n", cmd, cmd); return; }
		std::string summary, error;
		if (RenderTakeHistory_Import(atoi(args->ArgV(3)), summary, error))
			advancedfx::Message("%s: settings imported from %s.\n", cmd, summary.c_str());
		else
			advancedfx::Warning("%s: cannot import: %s.\n", cmd, error.c_str());
	} else if (0 == _stricmp(sub, "cancel")) {
		if (!s_jobActive && !RenderSessionDriver_IsActive()) {
			advancedfx::Message("%s: no active job to cancel.\n", cmd);
		} else {
			// Cooperative: raise the flag and let the driver acknowledge at its next
			// checkpoint. Nothing is torn down here.
			RenderSessionRef().RequestCancel();
			advancedfx::Message("%s: cancellation requested. The job will finalize safely.\n", cmd);
		}
	} else if (!RenderHost_RunCommand(sub, argc, args, cmd)) {
		PrintHelp(cmd);
	}
}

} // namespace Filmmaker
