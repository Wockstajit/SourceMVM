#include "../Movie/SceneKeys.h"
#include "RenderSessionDriver.h"

#include "RenderCommand.h"
#include "RenderTypes.h"
#include "RenderRange.h"
#include "RenderJob.h"
#include "RenderSession.h"
#include "RenderStateGuard.h"
#include "RenderOutput.h"
#include "RenderValidation.h"
#include "RenderManifest.h"
#include "RenderResolution.h"
#include "RenderEncoderDetect.h"
#include "RenderHdrCapture.h"
#include "RenderWorldOnly.h"
#include "RenderWorldPlayersNoGun.h"
#include "RenderPlayersOnlyGreen.h"
#include "RenderPassPlan.h"
#include "../Movie/CameraPath.h"
#include "../Movie/CameraBridge.h"
#include "../Movie/DemoTimescale.h"
#include "../Panorama/CameraEditorHud.h"
#include "../Debug/MvmDebugLog.h"
#include "../Platform/TextEncoding.h"
#include "../Demo/PlayingDemoPath.h"
#include "../Cosmetics/CosmeticOverrideSystem.h" // SeekRefreshPending: the pre-roll landing waits for skins

#include "../../MirvTime.h"
#include "../../RenderSystemDX11Hooks.h"
#include "../../WrpConsole.h"

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/icvar.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include "../EasterEggs/EasterEggs.h"

#include <string>
#include <vector>

#include <windows.h>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

// The camera-path tick the active render job is evaluating, for the view-side layers that ride on
// the path (CamPathShake). The editor's CurrentPathTick() is false during a render -- the job drives
// the path camera itself (DrivePathCamera), the editor is not playing -- so without this the path
// shake was silently missing from every rendered take. Written on the main thread, read on the
// view thread: atomics.
static std::atomic<bool> g_renderPathTickValid{false};
static std::atomic<double> g_renderPathTick{0.0};

bool Render_PathCameraTick(double& tick) {
	if (!g_renderPathTickValid.load(std::memory_order_acquire))
		return false;
	tick = g_renderPathTick.load(std::memory_order_relaxed);
	return true;
}

namespace {

using S = RenderSessionState;

// The job's pass plan (RenderPassPlan.h), built once at start: one recorder stream per pass, and
// the physical render count behind the logical progress.
PassPlan s_plan;

// Shared-arms material side for the Players Only render, as it was before the job.
bool s_prevGreenViewmodelPlayerSide = false;
bool s_greenViewmodelTouched = false;

RenderJob s_job;
RenderProgress s_progress;
RenderStateRestoreGuard* s_guard = nullptr;

// Recorder knobs as they were before the job, restored verbatim afterwards.
struct PrevRecorder {
	std::string recordName;
	bool screenEnabled = false;
	std::string screenSettings;
	bool fpsOverride = false;
	float fpsValue = 0.0f;
	float outputFps = 0.0f;
	bool camExport = false;
	bool formatBmp = false;
	bool takeSubfolder = true;
	bool valid = false;
} s_prevRec;

int s_returnTick = -1;          // demo position to put back
bool s_returnPaused = true;
int s_seekTick = 0;             // pre-roll seek target
bool s_seekIssued = false;
int s_seekRetries = 0;         // pre-roll re-seeks after an overrun (see SeekingPreRoll)
bool s_preRollPaused = false;  // this landing was paused for its settle window (demo_pause toggles)
int s_stateFrames = 0;          // frames spent in the current session state
int s_settleCount = 0;
// Tick-exact start (see WarmingUp): frames spent approaching the start tick; bounds the
// per-frame render.align trace only.
int s_alignSettle = 0;
// Start-tick hold: the approach pauses ON startTick and the recorder is armed while paused, so
// the arm->first-frame latency cannot advance demo time. See WarmingUp.
bool s_holdIssued = false;
int s_holdStable = 0;
int s_holdLastTick = -1;
int s_heldStartTick = -1;       // tick frame 0 was actually captured on
int s_approachFramesLeft = 0;   // deterministic 1-tick-per-frame countdown to the start tick
bool s_awaitFirstFrame = false;
int s_frameIndex = 0;           // captured output frames (frames.csv rows)
bool s_restored = false;        // RestoreEverything ran for this job
std::vector<std::string> s_addedStreamNames;
std::string s_demoPathUtf8;
std::string s_manifestNotes;

void Probe(const char* fmt, ...) {
	if (!MvmDebugLog_Active()) return;
	char buf[512];
	va_list va;
	va_start(va, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va);
	va_end(va);
	MvmDebugLog_LinefAlways("render.session", "%s", buf);
}

void DemoCmd(const char* cmd) {
	if (g_pEngineToClient)
		g_pEngineToClient->ExecuteClientCmd(0, cmd, true);
}

bool DemoPlayingOrPaused() {
	if (!g_pEngineToClient) return false;
	auto* demo = g_pEngineToClient->GetDemoFile();
	return demo && (demo->IsPlayingDemo() || demo->IsDemoPaused());
}

bool DemoPaused() {
	if (!g_pEngineToClient) return false;
	auto* demo = g_pEngineToClient->GetDemoFile();
	return demo && demo->IsDemoPaused();
}

int CurTick() {
	int tick = 0;
	g_MirvTime.GetCurrentDemoTick(tick);
	return tick;
}

void AppendFrameMetadata(int frameIndex, int tick) {
	double origin[3] = {0}, angles[3] = {0}, fov = 90.0;
	CameraBridge_GetCurrentCamera(origin, angles, fov);
	double demoTime = 0.0;
	g_MirvTime.GetCurrentDemoTime(demoTime);
	RenderFrameMap_Append(frameIndex, tick, demoTime, origin, angles, fov);
}

std::string ReadFloatCvar(const char* name, const char* fallback) {
	using namespace SOURCESDK::CS2;
	if (!g_pCVar) return fallback;
	Cvar_s* handle = g_pCVar->GetCvar(g_pCVar->FindConVar(name, false).Get());
	if (!handle) return fallback;
	char buf[64];
	snprintf(buf, sizeof(buf), "%g", handle->m_Value.m_flValue);
	return buf;
}

std::string ReadBoolCvar(const char* name, const char* fallback) {
	using namespace SOURCESDK::CS2;
	if (!g_pCVar) return fallback;
	Cvar_s* handle = g_pCVar->GetCvar(g_pCVar->FindConVar(name, false).Get());
	if (!handle) return fallback;
	return handle->m_Value.m_bValue ? "1" : "0";
}

// Recursive directory creation without pulling in shell32: walks the path and CreateDirectoryW's
// each missing component. Returns true when the leaf exists afterwards.
bool CreateDirectoryRecursive(const std::wstring& path) {
	if (path.empty()) return false;
	DWORD attr = ::GetFileAttributesW(path.c_str());
	if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return true;
	// Find the parent (skip trailing separators).
	size_t end = path.find_last_not_of(L"\\/");
	if (end == std::wstring::npos) return false;
	size_t sep = path.find_last_of(L"\\/", end);
	if (sep != std::wstring::npos && sep > 2) { // keep "C:\" intact
		if (!CreateDirectoryRecursive(path.substr(0, sep)))
			return false;
	}
	if (::CreateDirectoryW(path.c_str(), nullptr)) return true;
	return ::GetLastError() == ERROR_ALREADY_EXISTS;
}

bool Transition(S to) {
	const char* why = nullptr;
	const S from = RenderSessionRef().State();
	const bool ok = RenderSessionRef().TryTransition(to, &why);
	Probe("transition %s -> %s %s", RenderSession::StateName(from), RenderSession::StateName(to),
		ok ? "ok" : (why ? why : "REFUSED"));
	if (!ok)
		advancedfx::Warning("mirv_filmmaker render: session refused %s -> %s.\n",
			RenderSession::StateName(from), RenderSession::StateName(to));
	return ok;
}

void SetPhase(const char* phase) {
	s_progress.phase = phase ? phase : "";
}

// Drives the camera for CameraPath jobs: path evaluation at the current capture position, with the
// job's extension behavior at the edges. Runs every frame from warm-up through capture.
void DrivePathCamera() {

	// FRACTIONAL tick, not CurTick(). The demo clock only moves once per tick (64 Hz), so sampling
	// the curve at an integer tick made the camera a staircase: measured at 1000 fps, 250 rendered
	// frames carried only 17 distinct camera poses, each held ~15 frames. Everything else in the
	// frame (animation, particles) does advance per frame, so the camera was the one stepping thing.
	//
	// A camera path is a continuous curve, and a capture is a deterministic frame->time mapping:
	// output frame i sits exactly i/fps seconds after the range start. Evaluating at that position
	// gives smooth sub-tick motion at any output rate. Falls back to the demo clock outside capture
	// (warm-up, seeking), where no frame index is meaningful yet.
	double tick = (double)CurTick();
	if (RenderSessionRef().State() == S::Capturing
		&& s_job.captureFps > 0 && s_job.tickRate > 0.0) {
		const double ticksPerFrame = s_job.tickRate / (double)s_job.captureFps;
		// s_frameIndex is the index of the NEXT frame this pump renders.
		tick = (double)s_job.range.startTick + (double)s_frameIndex * ticksPerFrame;
	}
	SceneKeys_ApplyCaptureTick(tick);
	if (s_job.camera != RenderCameraSource::CameraPath) {
		g_renderPathTickValid.store(false, std::memory_order_release);
		return;
	}
	CameraPath& cp = CameraPathRef();
	if (cp.Count() < 2) {
		g_renderPathTickValid.store(false, std::memory_order_release);
		return;
	}
	const double first = (double)cp.FirstPathTick();
	const double last = (double)cp.LastPathTick();
	// Validation already rejected out-of-coverage ranges unless an extension permits them, so
	// clamping here implements HoldFirst/HoldLast (and is a no-op for covered ticks).
	if (tick < first) tick = first;
	if (tick > last) tick = last;
	double pose[7] = {0};
	if (cp.EvalPoseAtTick(tick, pose)) {
		CameraBridge_SetFreeCamEnabled(true);
		CameraBridge_SetCameraPose(pose[0], pose[1], pose[2], pose[3], pose[4], pose[5], pose[6]);
		g_renderPathTick.store(tick, std::memory_order_relaxed);
		g_renderPathTickValid.store(true, std::memory_order_release);
	}
}

// Puts back everything the job changed. Idempotent; runs on success, cancel and failure.
void RestoreEverything() {
	if (s_restored)
		return;
	s_restored = true;
	g_renderPathTickValid.store(false, std::memory_order_release);
	SetPhase("restoring");
	Probe("restore begin (framesDone=%d)", s_frameIndex);

	// Recorder first: never leave it running.
	if (AfxStreams_IsRcording())
		AfxStreams_RecordEnd();
	RenderFrameMap_Close();

	// Disarm a still-armed HDR EXR sequence (cancel/failure paths). Success already ended it
	// in PostProcessing; this second call is an idempotent no-op there.
	RenderHdrCapture_EndSequence(nullptr, nullptr);

	// Give the in-world camera-path gizmos back. This only lifts the capture suppression; the
	// user's own path-draw toggle was never touched, so whatever they had before is what returns.
	CameraBridge_SetPathDrawSuppressed(false);

	// The job's World Only render is gone; the live workspace preview comes back once the session
	// is terminal (FinishJob / the cancel path call Render_ApplyLivePreview).
	RenderWorldOnly_SetJobUsesPass(false);
	if (s_greenViewmodelTouched) {
		RenderPlayersOnlyGreen_SetViewmodelPlayerSide(s_prevGreenViewmodelPlayerSide);
		s_greenViewmodelTouched = false;
	}

	for (auto it = s_addedStreamNames.rbegin(); it != s_addedStreamNames.rend(); ++it)
		AfxStreams_RemoveStream(it->c_str());
	s_addedStreamNames.clear();

	if (s_prevRec.valid) {
		AfxStreams_SetRecordNameUtf8(s_prevRec.recordName.c_str());
		AfxStreams_SetRecordScreenEnabled(s_prevRec.screenEnabled);
		if (!s_prevRec.screenSettings.empty())
			AfxStreams_SetRecordScreenSettingsByName(s_prevRec.screenSettings.c_str());
		AfxStreams_SetOverrideFps(s_prevRec.fpsOverride, s_prevRec.fpsValue);
		AfxStreams_SetOutputFpsOverride(s_prevRec.outputFps);
		AfxStreams_SetCamExport(s_prevRec.camExport);
		AfxStreams_SetFormatBmpNotTga(s_prevRec.formatBmp);
		AfxStreams_SetRecordTakeSubfolder(s_prevRec.takeSubfolder);
		s_prevRec.valid = false;
	}

	RenderResolution_DisengageOverride();

	CameraEditorHudRef().ClearHudViewOverride();

	if (s_guard) {
		s_guard->Restore();
		delete s_guard;
		s_guard = nullptr;
	}

	// Demo position last: gototick auto-pauses; resume only if the user was playing.
	if (s_returnTick >= 0 && DemoPlayingOrPaused()) {
		char cmd[64];
		snprintf(cmd, sizeof(cmd), "demo_gototick %d", s_returnTick);
		DemoCmd(cmd);
		if (!s_returnPaused)
			DemoCmd("demo_resume");
	}
	s_returnTick = -1;

	Probe("restore done");
}

void FinishJob(S terminal, const char* status, const std::string& note) {
	if (!note.empty()) {
		if (!s_manifestNotes.empty()) s_manifestNotes += " ";
		s_manifestNotes += note;
	}
	if (!s_job.takeDir.empty())
		RenderManifest_WriteFinal(s_job, s_demoPathUtf8, status, s_frameIndex, s_manifestNotes);
	RestoreEverything();
	Transition(terminal);
	Render_SetJobActive(false);
	Render_ApplyLivePreview();
	SetPhase(status);
	Probe("job %s frames=%d/%d note='%s'", status, s_frameIndex, s_job.framesTotal, note.c_str());
	advancedfx::Message("mirv_filmmaker render: job %s (%d frame(s), %s).\n",
		status, s_frameIndex, s_progress.takeDirUtf8.c_str());
}

void FailJob(const std::string& reason) {
	s_progress.lastError = reason;
	advancedfx::Warning("mirv_filmmaker render: FAILED: %s\n", reason.c_str());
	if (AfxStreams_IsRcording())
		AfxStreams_RecordEnd();
	FinishJob(S::Failed, "failed", reason);
}

} // namespace

bool RenderSessionDriver_Start(std::string& outError) {
	outError.clear();
	RenderSession& session = RenderSessionRef();

	if (session.IsBusy()) {
		outError = "a render job is already running";
		return false;
	}
	if (session.State() != S::Idle && !session.Reset()) {
		outError = "the previous session could not be reset";
		return false;
	}

	if (!DemoPlayingOrPaused()) {
		outError = "no demo is playing";
		return false;
	}

	// Jobs are started from the Render workspace (the command layer enforces this too); this
	// second check keeps the invariant even if a future caller bypasses the console grammar.
	if (!Render_IsModalOpen()) {
		outError = "the Render workspace is not open";
		return false;
	}

	const RenderValidationResult v = Render_ValidateCurrentJobFull();
	if (!v.Ok()) {
		outError = v.FirstFatal();
		return false;
	}

	RenderRange resolved;
	if (!Render_ResolveRangeForDisplay(resolved) || !resolved.Valid()) {
		outError = "the render range does not resolve to a usable tick span";
		return false;
	}

	RenderJob job;
	job.range = resolved;
	job.camera = Render_GetCameraSource();
	job.extension = Render_GetExtension();
	job.out = RenderOutputSettingsRef();
	job.passes = RenderPassesRef();
	job.passes.passes = NormalizePasses(job.passes.passes);
	if (job.passes.passes.empty()) {
		outError = "no render pass selected";
		return false;
	}

	// Contract rules 3+4: a CameraPath job must be covered by the keyframe interval unless the
	// extension explicitly permits overrun; Clamp adjusts the resolved ticks here, once.
	if (job.camera == RenderCameraSource::CameraPath) {
		CameraPath& cp = CameraPathRef();
		if (cp.Count() < 2) {
			outError = "Camera Path needs 2+ camera keys";
			return false;
		}
		const int first = cp.FirstPathTick();
		const int last = cp.LastPathTick();
		const bool outside = job.range.startTick < first || job.range.endTickExclusive > last + 1;
		if (outside) {
			switch (job.extension) {
			case CameraPathExtension::Fail:
				outError = "the range extends beyond the camera path (extension is Fail)";
				return false;
			case CameraPathExtension::Clamp:
				if (job.range.startTick < first) job.range.startTick = first;
				if (job.range.endTickExclusive > last + 1) job.range.endTickExclusive = last + 1;
				if (!job.range.Valid()) {
					outError = "clamping the range to the camera path left nothing to render";
					return false;
				}
				break;
			case CameraPathExtension::HoldFirst:
			case CameraPathExtension::HoldLast:
				break; // pose evaluator holds the boundary key
			}
		}
	}

	// The warm-up approaches the start tick from below (SeekingPreRoll / WarmingUp). The demo's
	// first tick has nothing below it: a seek there lands a tick or two past it, so every Full
	// Demo job failed with "the pre-roll seek kept overrunning the start tick". Such a range starts
	// one warm-up later instead -- the demo's opening second is its load-in.
	{
		int demoStart = 0, demoEnd = 0;
		if (Render_GetDemoBounds(demoStart, demoEnd) && demoStart >= 0 &&
			job.range.startTick < demoStart + job.warmupTicks) {
			job.range.startTick = demoStart + job.warmupTicks;
			if (!job.range.Valid()) {
				outError = "the range ends before the demo's first second, which the pre-roll needs";
				return false;
			}
		}
	}

	// Recording FPS is both mux rate and sampling rate. The timeline dial is preview-only —
	// capture does not divide by it. demo_timescale is forced to 1.0 in SavingState so a leftover
	// dial cannot stretch the clock underneath host_framerate.
	job.timescale = 1.0;
	job.captureFps = RenderCaptureFps(job.out.fps);
	job.framesTotal = job.range.FrameCount(job.captureFps, job.tickRate);
	if (job.framesTotal <= 0) {
		outError = "the range and fps produce zero output frames";
		return false;
	}

	job.takeDir = BuildTakeDirectory(job.out);
	if (job.takeDir.empty()) {
		outError = "no output folder chosen";
		return false;
	}

	// Snapshot the return position before anything moves.
	s_returnTick = CurTick();
	s_returnPaused = DemoPaused();
	s_demoPathUtf8 = WideToUtf8(ResolvePlayingDemoPath());
	s_job = job;
	s_plan = BuildPassPlan(job.passes.passes);
	s_frameIndex = 0;
	s_seekIssued = false;
	s_seekRetries = 0;
	s_preRollPaused = false;
	s_settleCount = 0;
	s_alignSettle = 0;
	s_holdIssued = false;
	s_holdStable = 0;
	s_holdLastTick = -1;
	s_heldStartTick = -1;
	s_approachFramesLeft = 0;
	s_awaitFirstFrame = false;
	s_stateFrames = 0;
	s_restored = false;
	s_manifestNotes.clear();
	s_progress = RenderProgress{};
	s_progress.framesTotal = job.framesTotal;
	s_progress.passes = s_plan.StreamCount();
	s_progress.rendersPerFrame = s_plan.PhysicalRendersPerFrame();
	s_progress.physicalTotal = ComputePassProgress(s_plan, 0, job.framesTotal).physicalTotal;
	s_progress.takeDirUtf8 = WideToUtf8(job.takeDir);
	SetPhase("starting");

	RenderEncoderDetect_EnsureProbe();

	if (!Transition(S::Validating)) {
		outError = "session refused to leave Idle";
		return false;
	}
	Render_SetJobActive(true);
	// Kill the in-world camera-path / follow-camera gizmos for the WHOLE job, not just the
	// capturing states. They are drawn into the scene target before UI, so they would otherwise
	// be baked into every stream -- POV, free cam, path camera, beauty, depth and isolation
	// alike. Cleared unconditionally by RestoreEverything() on success, cancel and failure.
	CameraBridge_SetPathDrawSuppressed(true);
	Probe("start range=%d..%d cam=%d ext=%d fps=%d timescale=%g captureFps=%d fmt=%s frames=%d passes=%d renders/frame=%d take='%s'",
		job.range.startTick, job.range.endTickExclusive, (int)job.camera, (int)job.extension,
		job.out.fps, job.timescale, job.captureFps,
		RenderFormatToken(job.out.format), job.framesTotal, s_plan.StreamCount(),
		s_plan.PhysicalRendersPerFrame(), s_progress.takeDirUtf8.c_str());
	advancedfx::Message("mirv_filmmaker render: starting job, %d frame(s) at %gx speed to %s\n",
		job.framesTotal, job.timescale, s_progress.takeDirUtf8.c_str());
	EasterEggs_OnRenderStarted();
	return true;
}

bool RenderSessionDriver_IsActive() {
	return RenderSessionRef().IsBusy();
}

const RenderProgress& RenderSessionDriver_Progress() {
	return s_progress;
}

bool RenderSessionDriver_WantsChromeHidden() {
	// Contract rule 10: a render must not change what the user sees. The capture STAGE decides
	// whether our chrome could ever reach a frame, so this is gated on the job's HUD mode, not on
	// "is a job running".
	//
	// HUD None captures at BeforeUi -- before Panorama and before the editor's scaled-preview blit
	// -- so the workspace, its small preview window and the whole UI can stay exactly as they are
	// while the take records clean. Hiding them anyway is what made the game jump to full screen
	// and blank its UI at the start of every render (the pass-era blanket gate; the override
	// streams that needed it no longer exist).
	//
	// HUD Game/Full capture at BeforePresent, which is AFTER our chrome draws, so there the
	// chrome genuinely must be hidden for the capture window.
	if (s_job.passes.hud == RenderHudMode::None)
		return false;
	const S s = RenderSessionRef().State();
	return s == S::WarmingUp || s == S::Capturing || s == S::FlushingGpu;
}

void RenderSessionDriver_RunFrame() {
	RenderSession& session = RenderSessionRef();
	const S state = session.State();
	if (state == S::Idle || RenderSession::IsTerminal(state))
		return;

	s_stateFrames++;
	s_progress.curTick = CurTick();

	// A demo that stops existing mid-job (disconnect to menu) fails the job from any live state.
	if (state != S::RestoringState && !DemoPlayingOrPaused()) {
		auto* demo = g_pEngineToClient ? g_pEngineToClient->GetDemoFile() : nullptr;
		Probe("demo unavailable state=%s stateFrames=%d demo=%p playing=%d paused=%d tick=%d",
			RenderSession::StateName(state), s_stateFrames, demo,
			demo && demo->IsPlayingDemo() ? 1 : 0, demo && demo->IsDemoPaused() ? 1 : 0,
			s_progress.curTick);
		FailJob("demo playback ended while the job was running");
		return;
	}

	switch (state) {
	case S::Validating: {
		s_stateFrames = 0;
		Transition(S::PreparingOutput);
		break;
	}

	case S::PreparingOutput: {
		SetPhase("preparing output");
		// Frame 1: create the take dir. Rendering larger than the game resolution is not
		// available: CS2 renders its scene at the game resolution, every decoupling lever was
		// measured broken (window resize, client-rect spoof, enlarged scene targets), and an
		// FFmpeg upscale would only fabricate pixels. Oversized requests are capped here so a
		// stale UI/job value can never smuggle one through.
		if (s_stateFrames == 1) {
			if (!CreateDirectoryRecursive(s_job.takeDir)) {
				FailJob("could not create the output directory");
				return;
			}
			// EXR: the HDR frames are written by RenderHdrCapture at the BeforeUi capture
			// boundary. That boundary is only the single, unambiguous "one recorded source
			// frame" event when the beauty stream itself rides BeforeUi -- which is the HUD
			// None configuration. Refuse anything else rather than guess frame alignment.
			if (s_job.out.format == RenderOutputFormat::Exr) {
				if (s_job.passes.hud != RenderHudMode::None) {
					FailJob("EXR output requires HUD mode None (the pre-tonemap HDR capture is "
						"aligned to the clean-plate BeforeUi boundary)");
					return;
				}
				const std::wstring hdrDir = s_job.takeDir + L"\\hdr";
				if (!CreateDirectoryRecursive(hdrDir)) {
					FailJob("could not create the hdr/ output directory");
					return;
				}
				RenderHdrCapture_BeginSequence(hdrDir.c_str());
			}
			return;
		}

		// One recorder preset per pass: the preset carries the pass's file name (beauty\world.mp4).
		// All of them are registered before any recorder state is touched, so a format without a
		// runnable path fails here with nothing to undo.
		std::string note;
		const bool nvencOk = RenderEncoderDetect_State() == NvencProbe::Available;
		std::vector<std::string> passPresets;
		for (const PassConfig& pass : s_plan.passes) {
			std::string passNote;
			const std::string preset = RenderOutput_PrepareRecorderPreset(s_job.out, nvencOk,
				"mvmRender_" + PassStreamName(pass), PassFileStem(pass.elements), passNote);
			if (preset.empty()) {
				FailJob(passNote.empty() ? "the selected format has no encoder path" : passNote);
				return;
			}
			if (note.empty()) note = passNote; // every pass gets the same encoder sentence
			passPresets.push_back(preset);
		}
		if (passPresets.empty()) {
			FailJob("the job has no passes to record");
			return;
		}
		s_job.presetName = passPresets.front();
		if (!note.empty()) {
			advancedfx::Message("mirv_filmmaker render: %s\n", note.c_str());
			s_manifestNotes += note;
		}

		// Save recorder knobs, then configure ours.
		s_prevRec.recordName = AfxStreams_GetRecordNameUtf8();
		s_prevRec.screenEnabled = AfxStreams_GetRecordScreenEnabled();
		s_prevRec.screenSettings = AfxStreams_GetRecordScreenSettingsName();
		AfxStreams_GetOverrideFps(s_prevRec.fpsOverride, s_prevRec.fpsValue);
		s_prevRec.outputFps = AfxStreams_GetOutputFpsOverride();
		s_prevRec.camExport = AfxStreams_GetCamExport();
		s_prevRec.formatBmp = AfxStreams_GetFormatBmpNotTga();
		s_prevRec.takeSubfolder = AfxStreams_GetRecordTakeSubfolder();
		s_prevRec.valid = true;

		// The job's directory IS the take: no recorder "take0000" folder inside it.
		AfxStreams_SetRecordNameUtf8(s_progress.takeDirUtf8.c_str());
		AfxStreams_SetRecordTakeSubfolder(false);
		AfxStreams_SetRecordScreenEnabled(false); // the job rides one explicit beauty stream

		AfxStreams_SetOverrideFps(true, (float)s_job.captureFps);
		// captureFps == out.fps: sample and mux at the chosen recording FPS.
		AfxStreams_SetOutputFpsOverride((float)s_job.out.fps);
		AfxStreams_SetCamExport(s_job.passes.cameraData);
		AfxStreams_SetFormatBmpNotTga(s_job.out.format == RenderOutputFormat::Bmp);

		// A leftover stream from a crashed job must not double-capture: remove every pass name, then
		// register one stream per pass before the single RecordStart. All streams record from the
		// same engine frames, so frame N of every output shares one simulation state.
		for (int t = 0; t < (int)PassType::Count; ++t)
			for (int e = 0; e < (int)VisibleElements::Count; ++e) {
				PassConfig any; any.type = (PassType)t; any.elements = (VisibleElements)e;
				AfxStreams_RemoveStream(PassStreamName(any).c_str());
			}
		s_addedStreamNames.clear();
		const bool imageSequence = RenderFormatIsSequence(s_job.out.format);
		for (size_t passIndex = 0; passIndex < s_plan.passes.size(); ++passIndex) {
			const PassConfig& pass = s_plan.passes[passIndex];
			const std::string streamName = PassStreamName(pass);
			const std::string& streamPreset = passPresets[passIndex];
			// The game HUD can only appear in the plate the user sees: Beauty + Everything taken
			// before present. Every other pass is a clean BeforeUi capture of its own render.
			const bool hudPlate = pass.type == PassType::Beauty && pass.elements == VisibleElements::Everything
				&& s_job.passes.hud != RenderHudMode::None;
			const bool added = hudPlate
				? AfxStreams_AddNormalStream(streamName.c_str(), false, streamPreset.c_str())
				: AfxStreams_AddPassStream(streamName.c_str(), (int)pass.type, (int)pass.elements,
					streamPreset.c_str(), s_job.passes.depthNear, s_job.passes.depthFar, s_job.passes.keyColor);
			if (!added) {
				FailJob("could not register the capture stream '" + streamName + "'");
				return;
			}
			s_addedStreamNames.emplace_back(streamName);
			// <take>\<type>\<variant>.mp4: every variant of a type shares the type's folder
			// (sequences keep a folder per variant, see PassOutputFolder).
			if (!AfxStreams_SetStreamFolder(streamName.c_str(), PassOutputFolder(pass, imageSequence).c_str())) {
				FailJob("could not set the output folder of '" + streamName + "'");
				return;
			}
			Probe("stream %s (%s + %s) registered", streamName.c_str(), PassTypeLabel(pass.type),
				VisibleElementsLabel(pass.elements));
		}

		if (!RenderManifest_WriteStart(s_job, s_demoPathUtf8, s_manifestNotes)) {
			FailJob("could not write manifest.json into the output directory");
			return;
		}

		s_stateFrames = 0;
		Transition(S::SavingState);
		break;
	}

	case S::SavingState: {
		SetPhase("saving state");
		s_guard = new RenderStateRestoreGuard();
		s_guard->Snapshot();
		s_guard->TrackCvar("demo_timescale", ReadFloatCvar("demo_timescale", "1").c_str());
		// The warm-up drives host_framerate itself (tickRate for the deterministic approach, then
		// captureFps), so the guard must own the user's original value: the recorder's own
		// save/restore would otherwise put back OUR warm-up value on every outcome.
		s_guard->TrackCvar("host_framerate", ReadFloatCvar("host_framerate", "0").c_str());
		DemoCmd("demo_timescale 1.0");

		// Spectator X-ray paints team-colored silhouettes and glows through walls -- an overlay,
		// not scene content, so it never belongs in a delivered plate (user requirement
		// 2026-08-07: render jobs contain no UI of any kind). Tracked through the guard like the
		// debug-view cvars, so the viewer gets their X-ray back after every job outcome.
		s_guard->TrackCvar("spec_show_xray", ReadBoolCvar("spec_show_xray", "1").c_str());
		DemoCmd("spec_show_xray 0");
		// HUD None hides the Panorama HUD (HudView::HideAll below), but CS2's crosshair is not a
		// Panorama panel: it stays drawn into the frame unless its cvar is off (same reason as
		// HudBlackout). Game/Full renders keep it, as part of the game HUD they ask for.
		if (s_job.passes.hud == RenderHudMode::None) {
			s_guard->TrackCvar("crosshair", ReadBoolCvar("crosshair", "1").c_str());
			DemoCmd("crosshair 0");
		}
		// World + Players / No Gun is implemented by the engine's own viewmodel visibility cvar.
		// Track it here as well as in the live-preview module so every job outcome restores the exact
		// pre-job value, including a cancelled or failed warm-up.
		s_guard->TrackCvar("r_drawviewmodel", ReadBoolCvar("r_drawviewmodel", "1").c_str());
		// Blood off: none spawns during the job, so no pass has any to hide (the Players Only
		// render could otherwise only tint what it cannot skip). Job-wide by nature -- the Beauty
		// pass loses it too -- which is what the option says.
		if (!s_job.passes.blood) {
			s_guard->TrackCvar("violence_hblood", ReadBoolCvar("violence_hblood", "1").c_str());
			s_guard->TrackCvar("violence_ablood", ReadBoolCvar("violence_ablood", "1").c_str());
			DemoCmd("violence_hblood 0");
			DemoCmd("violence_ablood 0");
		}
		// Isolation renders are extra renders of their own: the view the user sees (and the Main render
		// every pass is taken from) stays the full scene. Drop any live workspace preview for the job;
		// FinishJob / cancel re-apply it. Nothing here touches global scene state.
		RenderWorldOnly_SetArmed(false, "render job");
		RenderPlayersOnlyGreen_SetArmed(false, "render job");
		RenderWorldPlayersNoGun_SetArmed(false, "render job");
		AfxStreams_SetLivePassPreview(0, 0, 0.0f, 1.0f, nullptr);
		RenderWorldOnly_SetJobUsesPass(s_plan.renders[(int)RenderKey::WorldOnly]);
		s_prevGreenViewmodelPlayerSide = RenderPlayersOnlyGreen_ViewmodelIsPlayerSide();
		s_greenViewmodelTouched = true;
		RenderPlayersOnlyGreen_SetViewmodelPlayerSide(s_job.camera != RenderCameraSource::Pov);

		switch (s_job.passes.hud) {
		case RenderHudMode::None: CameraEditorHudRef().SetHudViewOverride(CameraEditorHud::HudView::HideAll); break;
		case RenderHudMode::Game: CameraEditorHudRef().SetHudViewOverride(CameraEditorHud::HudView::InGame); break;
		case RenderHudMode::Full: CameraEditorHudRef().SetHudViewOverride(CameraEditorHud::HudView::ShowAll); break;
		}

		if (s_job.camera == RenderCameraSource::CameraPath) {
			// Deliberately NOT PlayFromEditor(): the driver owns seeking and playback; the path
			// pose is evaluated per frame in DrivePathCamera.
			// A scrub left over from the editor timeline (a playhead drag with no release) re-pushes its
			// held pose from CameraPath::RunFrame every frame and wins over this driver: every frame of
			// the take froze on the scrub tick's pose. The job owns the path camera now.
			CameraPathRef().StopScrub();
			CameraBridge_SetFreeCamEnabled(true);
			DrivePathCamera();
		} else {
			// A camera path left playing (or parked after a play) keeps driving the view and
			// seeking the demo, which would fight this job's own seeking and silently capture
			// the dolly instead of the chosen source. If the job did not select Camera Path, the
			// path does not get the camera -- including under CurrentViewer, where "whatever is
			// on screen" must still not mean "a dolly racing the capture".
			CameraPath& cp = CameraPathRef();
			if (cp.IsPlaying() || cp.PlaybackPending() || cp.OwnsView()) {
				cp.StopPreview();
				Probe("camera path released before capture (job source=%d)", (int)s_job.camera);
			}
			Render_ApplyCameraSourceLive(s_job.camera, "mirv_filmmaker");
		}

		s_stateFrames = 0;
		Transition(S::SeekingPreRoll);
		break;
	}

	case S::SeekingPreRoll: {
		SetPhase("seeking pre-roll");
		if (!s_seekIssued) {
			s_seekIssued = true;
			int demoStart = 0, demoEnd = 0;
			Render_GetDemoBounds(demoStart, demoEnd);
			s_seekTick = s_job.range.startTick - s_job.warmupTicks;
			if (s_seekTick < demoStart) s_seekTick = demoStart;
			if (s_seekTick < 0) s_seekTick = 0;
			char cmd[64];
			snprintf(cmd, sizeof(cmd), "demo_gototick %d", s_seekTick);
			DemoCmd(cmd);
			Probe("seek preroll tick=%d (start=%d warmup=%d)", s_seekTick, s_job.range.startTick, s_job.warmupTicks);
			return;
		}
		const int tick = CurTick();
		// The seek is an asynchronous multi-100ms stall; accept a landing anywhere in the
		// pre-roll window (a demo cannot land before the previous keyframe boundary exactly).
		const bool landed = tick >= s_seekTick - 128 && tick <= s_job.range.startTick;

		// PER-FRAME seek trace. MEASURED 2026-08-10: a job started while the demo was PLAYING
		// landed the pre-roll seek at 51072 and then kept playing straight past the start tick
		// (51136) before this state had settled, so `landed` never matched again and the job sat
		// here -- job active, nothing progressing, "CANCEL RENDER" on screen -- until a 3600-frame
		// timeout that takes over a minute at 45 fps. Throttled; the transition itself is logged
		// by Transition().
		if (MvmDebugLog_Active() && (s_stateFrames % 30) == 0)
			MvmDebugLog_LinefAlways("render.seek",
				"n=%d tick=%d seekTick=%d start=%d landed=%d settle=%d paused=%d",
				s_stateFrames, tick, s_seekTick, s_job.range.startTick, landed ? 1 : 0,
				s_settleCount, DemoPaused() ? 1 : 0);

		if (landed) {
			// Hold the landing still while it settles. A demo that was playing keeps playing after
			// the seek, and the settle window is counted in FRAMES: at a heavy scene's frame rate
			// (Environment Lighting, ~15 fps) it outlasted the ~50-tick pre-roll, so every landing
			// overran the start and the job re-seeked to the path's first marker until it failed
			// (user report 2026-10-13). WarmingUp's demo_resume releases it from below the start.
			if (!s_preRollPaused) {
				s_preRollPaused = true;
				if (!DemoPaused()) DemoCmd("demo_pause");
			}
			if (++s_settleCount >= s_job.settleFrames) {
				// SKINS BEFORE THE FIRST FRAME. The pre-roll seek rebuilt every weapon, and a skin
				// override is redrawn by a composite that runs on paused frames only. This landing is
				// the last pause before the capture, so it lasts until the redraw is done: releasing it
				// after the bare settle window rendered the clip with the demo's own skins until some
				// later pause (user report 2026-10-21). Bounded, so a stuck refresh cannot stall a job.
				constexpr int kSkinRefreshWaitFrames = 600;
				const bool skinsPending = CosmeticsRef().SeekRefreshPending();
				if (skinsPending && s_settleCount < s_job.settleFrames + kSkinRefreshWaitFrames) {
					if (s_settleCount == s_job.settleFrames)
						Probe("holding the pre-roll landing for the skin refresh");
					break;
				}
				if (s_settleCount > s_job.settleFrames)
					Probe("skin refresh %s after %d extra frame(s)", skinsPending ? "STILL PENDING" : "done",
						s_settleCount - s_job.settleFrames);
				s_stateFrames = 0;
				Transition(S::WarmingUp);
				DemoCmd("demo_resume");
			}
		} else if (tick > s_job.range.startTick) {
			// Overran the start while settling (the demo was playing when the job began, or the
			// settle window outlasted the remaining pre-roll). Re-seek rather than wait out the
			// timeout: the warm-up MUST approach the start tick from below to hit it exactly.
			s_settleCount = 0;
			s_preRollPaused = false;
			if (++s_seekRetries > 4) {
				FailJob("the pre-roll seek kept overrunning the start tick");
				return;
			}
			char cmd[64];
			snprintf(cmd, sizeof(cmd), "demo_gototick %d", s_seekTick);
			DemoCmd(cmd);
			s_stateFrames = 0;
			Probe("preroll re-seek %d (tick=%d overran start=%d)", s_seekRetries, tick,
				s_job.range.startTick);
		} else if (s_stateFrames > 1800) {
			FailJob("demo seek did not land (timeout)");
		}
		break;
	}

	case S::WarmingUp: {
		SetPhase("warming up");
		DrivePathCamera();
		const int tick = CurTick();

		// DETERMINISTIC APPROACH -- the thing that makes two takes of the same range identical, and
		// therefore the thing future passes will rely on.
		//
		// MEASURED 2026-08-10, comparing two 1x takes of one range frame-by-frame: they started on
		// the same tick and then drifted apart by one tick partway through (11/30 rows matched).
		// demo_time is exactly tick/64, and the per-frame steps were 0/1/2/3 ticks rather than a
		// constant 1/30 s -- at 1x a 30 fps frame spans 2.13 ticks, so the engine must alternate 2-
		// and 3-tick steps, and the PHASE of that alternation depends on the sub-tick residue when
		// capture starts. Playing in freely leaves that residue uncontrolled, so the phase differed
		// between takes. Slow motion hid it (at 0.01x a frame is 0.02 ticks, so no step can shift).
		//
		// The fix is to arrive on a whole tick with ZERO residue, every time:
		//   1. the pre-roll seek lands and is held paused -- one deterministic landing tick L;
		//   2. warm-up runs at host_framerate = tickRate, so each frame advances EXACTLY one tick;
		//   3. we advance exactly (startTick - L) frames, counted -- never trusting the tick reading,
		//      which was measured to jitter by one while the demo is stopped;
		//   4. we pause there (residue still zero), switch host_framerate to captureFps and arm.
		// From a zero residue the capture's tick sequence is a pure function of (startTick,
		// captureFps), so every take of the range sees the same ticks at the same frame indices.
		if (!s_holdIssued) {
			// Step 1: settle on the landing. The seek auto-pauses; wait for a stable reading before
			// using it as the anchor.
			if (!DemoPaused()) {
				if ((s_alignSettle % 60) == 0)
					DemoCmd("demo_pause");
				++s_alignSettle;
				break;
			}
			if (tick != s_holdLastTick) {
				s_holdLastTick = tick;
				s_holdStable = 0;
				++s_alignSettle;
				break;
			}
			if (++s_holdStable < 8) {
				++s_alignSettle;
				break;
			}
			if (tick > s_job.range.startTick) {
				FailJob("the pre-roll landed past the start tick");
				return;
			}
			// Step 2: one tick per frame, and count the frames to the start tick.
			s_heldStartTick = tick;
			s_approachFramesLeft = s_job.range.startTick - tick;
			{
				char cmd[64];
				snprintf(cmd, sizeof(cmd), "host_framerate %d", (int)(s_job.tickRate + 0.5));
				DemoCmd(cmd);
			}
			DemoCmd("demo_resume");
			s_holdIssued = true;
			Probe("approach: landing=%d start=%d frames=%d at %d fps (1 tick/frame)", tick,
				s_job.range.startTick, s_approachFramesLeft, (int)(s_job.tickRate + 0.5));
			break;
		}

		if (MvmDebugLog_Active() && s_alignSettle < 400)
			MvmDebugLog_LinefAlways("render.align",
				"n=%d tick=%d want=%d paused=%d framesLeft=%d", s_alignSettle, tick,
				s_job.range.startTick, DemoPaused() ? 1 : 0, s_approachFramesLeft);
		++s_alignSettle;

		// Step 3: count down the approach. The COUNT is authoritative, not the tick reading.
		if (s_approachFramesLeft > 0) {
			if (DemoPaused())
				DemoCmd("demo_resume");
			--s_approachFramesLeft;
			if (s_stateFrames > 7200) {
				FailJob("warm-up never reached the range start (timeout)");
				return;
			}
			break;
		}

		// Step 4: on the start tick with zero residue. Hold, switch to the capture rate, arm.
		if (!DemoPaused()) {
			DemoCmd("demo_pause");
			break;
		}
		{
			char cmd[64];
			snprintf(cmd, sizeof(cmd), "host_framerate %d", s_job.captureFps);
			DemoCmd(cmd);
		}
		s_heldStartTick = tick;
		if (tick != s_job.range.startTick) {
			// The reading disagrees with the deterministic count. The count is what makes takes
			// match, so it wins -- but the manifest must state the tick the frames actually carry.
			char note[168];
			snprintf(note, sizeof(note),
				"frames report tick %d for requested start %d (the paused demo-tick reading drifts); ",
				tick, s_job.range.startTick);
			s_manifestNotes += note;
		}
		s_awaitFirstFrame = true;
		Probe("capture arming HELD tick=%d requested=%d recorderFrames=%d", tick,
			s_job.range.startTick, AfxStreams_GetRecordedSourceFrameCount());
		if (!AfxStreams_RecordStart()) {
			FailJob("the recorder failed to start (output folder writable?)");
			return;
		}
		RenderFrameMap_Open(s_job.takeDir);
		// Release playback HERE, not after frame 0 is counted.
		//
		// MEASURED 2026-08-10: resuming once frame 0 had been counted put the resume a frame too
		// late -- the recorder's first TWO source frames were both taken while still held, so
		// every take opened with a duplicated frame (frames.csv rows 0 and 1 on the same tick, a
		// one-frame stutter at the head of every clip). RecordStart itself only takes effect on
		// the next EngineThread_Prepare, so issuing the resume in the same frame cancels exactly
		// that latency: frame 0 is still the held start tick, and frame 1 is the first advanced
		// one. s_awaitFirstFrame stays as a GUARD only -- it keeps the mid-capture pause handler
		// below from toggling demo_pause while the resume is still landing.
		DemoCmd("demo_resume");
		// RecordStart runs at BeforePresent, after the current frame was scheduled. HLAE begins
		// recording on the next EngineThread_Prepare; do not invent a row for this uncaptured tick.
		s_frameIndex = 0;
		s_progress.framesDone = s_frameIndex;
		s_progress.percent = 0.0;
		s_stateFrames = 0;
		Transition(S::Capturing);
		break;
	}

	case S::Capturing: {
		SetPhase("capturing");
		DrivePathCamera();
		const int tick = CurTick();

		const int recorderFrames = AfxStreams_GetRecordedSourceFrameCount();

		// PER-FRAME capture trace. "The render did not stop at the out tick" can only be diagnosed
		// from the relationship between the recorder's own source-frame count, our written frame
		// count and the demo tick over time -- none of which a console reply can show. Dense for
		// the first frames (where the hold/release happens), then throttled.
		if (MvmDebugLog_Active()
			&& (s_frameIndex < 4 || (s_stateFrames % 30) == 0
				|| tick >= s_job.range.endTickExclusive))
			MvmDebugLog_LinefAlways("render.capture",
				"n=%d tick=%d start=%d endExcl=%d frames=%d/%d recorderFrames=%d paused=%d await0=%d",
				s_stateFrames, tick, s_job.range.startTick, s_job.range.endTickExclusive,
				s_frameIndex, s_job.framesTotal, recorderFrames, DemoPaused() ? 1 : 0,
				s_awaitFirstFrame ? 1 : 0);

		if (s_frameIndex < s_job.framesTotal && recorderFrames > s_frameIndex) {
			AppendFrameMetadata(s_frameIndex, tick);
			s_frameIndex++;
			// Progress is LOGICAL output frames (the same in every stream); the physical render count
			// rides along because a four-pass take is honestly four renders per frame.
			const PassProgress pp = ComputePassProgress(s_plan, s_frameIndex, s_job.framesTotal);
			s_progress.framesDone = pp.logicalDone;
			s_progress.physicalDone = pp.physicalDone;
			s_progress.percent = pp.percent;
			// Frame 0 landed; the resume was already issued at arm time (see WarmingUp), so this
			// only lifts the guard.
			if (s_awaitFirstFrame) {
				s_awaitFirstFrame = false;
				Probe("frame 0 captured at tick=%d", tick);
			}
		}

		// TWO end conditions, and the TICK one is authoritative (IWXMVM does the same: its
		// CaptureFrame stops as soon as the timeline tick passes the requested end). The frame
		// count alone could not stop a take whose recorder count stalls or lags, which is exactly
		// the "it blew past the out point and just sat there saying CANCEL RENDER" failure.
		const bool countDone = s_frameIndex >= s_job.framesTotal;
		// The tick end is a SAFETY NET, not the primary stop, so it carries a 2-tick margin.
		// MEASURED 2026-08-10: with no margin the +1 tick reading jitter fired it a tick early and
		// truncated slow-motion takes (2229/2250 at 0.01x, 22402/22500 at 0.001x) -- one tick is
		// ~47 output frames there. With host_framerate locked, the frame count and the tick reach
		// the end together, so the count fires first in a healthy job and this only catches a
		// recorder that stalled or a demo that ran away.
		const bool tickDone = !s_awaitFirstFrame
			&& tick >= s_job.range.endTickExclusive + 2;
		if (countDone || tickDone) {
			AfxStreams_RecordEnd();
			RenderFrameMap_Close();
			// Stop the world ON the out point rather than letting playback run on under the
			// flush; RestoreEverything puts the demo back where the user was afterwards.
			if (!DemoPaused())
				DemoCmd("demo_pause");
			Probe("capture end tick=%d endExcl=%d frames=%d recorderFrames=%d expected=%d reason=%s",
				tick, s_job.range.endTickExclusive, s_frameIndex, recorderFrames,
				s_job.framesTotal, countDone ? "count" : "tick");
			if (!countDone) {
				char note[176];
				snprintf(note, sizeof(note),
					"capture stopped at the out tick with %d of %d frame(s) (the recorder scheduled "
					"fewer source frames than the range implies); ", s_frameIndex, s_job.framesTotal);
				s_manifestNotes += note;
			}
			s_stateFrames = 0;
			Transition(S::FlushingGpu);
		} else if (DemoPaused() && !s_awaitFirstFrame) {
			// A mid-capture pause would schedule duplicate frames forever. The end-hold is only
			// allowed after the exact requested output count has been observed above.
			if (tick >= s_job.range.endTickExclusive - 1) {
				FailJob("demo reached its end hold before HLAE scheduled every requested output frame");
				return;
			}
			DemoCmd("demo_resume");
		}
		break;
	}

	case S::FlushingGpu: {
		SetPhase("flushing");
		// RecordEnd finalized the streams; give the async readback/encoder pipeline a few
		// frames before declaring numbers final. Capturing already paused on the out tick, and
		// demo_pause is a TOGGLE -- sending it again here would RESUME the demo and let it run
		// on past the take's end point.
		if (s_stateFrames == 1 && !DemoPaused())
			DemoCmd("demo_pause");
		if (s_stateFrames >= 10) {
			s_stateFrames = 0;
			Transition(S::FinalizingStreams);
		}
		break;
	}

	case S::FinalizingStreams: {
		s_stateFrames = 0;
		Transition(S::PostProcessing);
		break;
	}

	case S::PostProcessing: {
		SetPhase("writing manifest");
		const int writerFailures = AfxStreams_GetWriterFailureCount();
		if (writerFailures > 0) {
			char reason[160];
			snprintf(reason, sizeof(reason),
				"capture writer reported %d output failure(s); refusing a completed manifest",
				writerFailures);
			FailJob(reason);
			return;
		}
		// The tick-authoritative end can legitimately finish one frame short (the last output
		// frame's source frame falls exactly on the exclusive end tick). Anything beyond that is
		// a real misalignment and still refuses.
		if (s_job.framesTotal - s_frameIndex > 1 || s_frameIndex > s_job.framesTotal) {
			char reason[160];
			snprintf(reason, sizeof(reason),
				"stream/frame-map count mismatch (%d written, %d expected); refusing a misaligned take",
				s_frameIndex, s_job.framesTotal);
			FailJob(reason);
			return;
		}
		if (s_job.out.format == RenderOutputFormat::Exr) {
			unsigned hdrWritten = 0, hdrFailed = 0;
			RenderHdrCapture_EndSequence(&hdrWritten, &hdrFailed);
			if (hdrFailed > 0 || (int)hdrWritten != s_job.framesTotal) {
				char reason[192];
				snprintf(reason, sizeof(reason),
					"HDR EXR sequence mismatch (%u written, %u failed, %d expected); refusing a "
					"misaligned take", hdrWritten, hdrFailed, s_job.framesTotal);
				FailJob(reason);
				return;
			}
			char note[96];
			snprintf(note, sizeof(note), "hdr/ holds %u pre-tonemap linear EXR frame(s); ", hdrWritten);
			s_manifestNotes += note;
		}
		RenderManifest_WriteFinal(s_job, s_demoPathUtf8, "completed", s_frameIndex, s_manifestNotes);
		s_stateFrames = 0;
		Transition(S::RestoringState);
		break;
	}

	case S::RestoringState: {
		RestoreEverything();
		FinishJob(S::Completed, "completed", "");
		break;
	}

	case S::CancelRequested: {
		SetPhase("canceling");
		Probe("cancel acknowledged at tick=%d frames=%d", CurTick(), s_frameIndex);
		if (AfxStreams_IsRcording())
			AfxStreams_RecordEnd();
		RenderFrameMap_Close();
		RenderManifest_WriteFinal(s_job, s_demoPathUtf8, "canceled", s_frameIndex, s_manifestNotes);
		RestoreEverything();
		Transition(S::Canceled);
		Render_SetJobActive(false);
		Render_ApplyLivePreview();
		SetPhase("canceled");
		advancedfx::Message("mirv_filmmaker render: job canceled (%d frame(s) kept in %s).\n",
			s_frameIndex, s_progress.takeDirUtf8.c_str());
		break;
	}

	default:
		break;
	}
}

} // namespace Filmmaker
