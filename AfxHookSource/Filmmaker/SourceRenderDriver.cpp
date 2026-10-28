#include "stdafx.h"

#include "SourceRender.h"
#include "SourceCameraPath.h"
#include "SourceConCommand.h"
#include "SourceEngine.h"

#include "AfxStreams.h"
#include "WrpVEngineClient.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <AfxHookSource2/Filmmaker/Movie/CameraBridge.h>
#include <AfxHookSource2/Filmmaker/Movie/GraphCurveMath.h>
#include <AfxHookSource2/Filmmaker/Platform/TextEncoding.h>
#include <AfxHookSource2/Filmmaker/Render/RenderCommand.h>
#include <AfxHookSource2/Filmmaker/Render/RenderDoneSound.h>
#include <AfxHookSource2/Filmmaker/Render/RenderEncoderDetect.h>
#include <AfxHookSource2/Filmmaker/Render/RenderHost.h>
#include <AfxHookSource2/Filmmaker/Render/RenderJob.h>
#include <AfxHookSource2/Filmmaker/Render/RenderManifest.h>
#include <AfxHookSource2/Filmmaker/Render/RenderOutput.h>
#include <AfxHookSource2/Filmmaker/Render/RenderPassPlan.h>
#include <AfxHookSource2/Filmmaker/Render/RenderSession.h>
#include <AfxHookSource2/Filmmaker/Render/RenderSessionDriver.h>
#include <AfxHookSource2/Filmmaker/Render/RenderTakeHistory.h>
#include <Shared/AfxConsole.h>
#include <Shared/RecordingSettings.h>

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <string>

extern WrpVEngineClient* g_VEngineClient;
extern bool Hook_MaterialSystem(void);

namespace Filmmaker {

namespace {

using S = RenderSessionState;
using Clock = std::chrono::steady_clock;

// The demo starts this many ticks before the range and plays them unseen, so animation, particles
// and the interpolation history have settled by the first frame (CS2's warmup).
constexpr int kWarmupTicks = 64;
// Frames a seek or a cvar change gets to land before the driver gives up on it.
constexpr int kLandFrames = 600;
const char* const kPresetName = "mvmCssRender";

// Frames the recorder gets after it ends to hand its last frames to the writer.
constexpr int kFinishFrames = 10;

// HLAE's recorder (mirv_streams record) as the user had it, put back on every outcome.
struct SavedRecorder {
	bool valid = false;
	std::string name, format, screenSettings, screenSuffix;
	bool screen = false, takeSubfolder = true, cam = false, overrideFps = false;
	float overrideFpsValue = 60;
};

// What the job changes and puts back on every outcome.
struct Saved {
	float hostFramerate = 0, matQueueMode = -1, drawHud = 1, hBlood = 1, aBlood = 1;
	double timescale = 1;
	bool paused = true;
	int tick = 0;
	bool freeCam = false;
};

RenderJob g_job;
RenderProgress g_progress;
Saved g_saved;
SavedRecorder g_savedRecorder;
std::wstring g_passFolder; // where the recorder writes the Beauty pass
std::string g_demoPathUtf8;
std::string g_notes;
int g_frame = 0;        // frames recorded
int g_wait = 0;         // frames spent in the current phase
int g_preRollTick = 0;  // where the seek lands before the range
bool g_rowPending = false;
int g_rowTick = 0;
double g_rowTime = 0;
// The last row's angles. The engine's view can name one direction two ways from frame to frame (-1
// and 359), so each row takes the turn nearest the row before and a camera import never spins.
double g_rowAngles[3] = { 0, 0, 0 };
bool g_stateSaved = false;
bool g_sourceApplied = false;
Clock::time_point g_startedAt;

void Probe(const char* fmt, ...) {
	if (!MvmDebugLog_Active()) return;
	char buf[400];
	va_list va;
	va_start(va, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va);
	va_end(va);
	MvmDebugLog_LinefAlways("render.job", "%s", buf);
}

void Cmd(const char* command) {
	if (g_VEngineClient) g_VEngineClient->ClientCmd_Unrestricted(command);
}

void Cmdf(const char* fmt, ...) {
	char buf[256];
	va_list va;
	va_start(va, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va);
	va_end(va);
	Cmd(buf);
}

float CvarOr(const char* name, float fallback) {
	float value = fallback;
	return SourceFilmmaker::ConVar_GetFloat(name, value) ? value : fallback;
}

bool Move(S to) {
	const char* why = nullptr;
	if (RenderSessionRef().TryTransition(to, &why)) return true;
	Probe("refused %s -> %s", RenderSession::StateName(RenderSessionRef().State()), RenderSession::StateName(to));
	return false;
}

void SetPhase(const char* phase) {
	g_progress.phase = phase;
	g_wait = 0;
}

// The demo's clock for this frame: its tick and its time in ticks (tick plus the interpolation).
void DemoClock(int& tick, double& time) {
	const SourceFilmmaker::DemoState demo = SourceFilmmaker::Demo_State();
	tick = demo.tick;
	double seconds = 0;
	CameraBridge_GetDemoClock(seconds, tick);
	time = demo.intervalPerTick > 0 ? seconds / demo.intervalPerTick : tick;
}

void SaveState() {
	const SourceFilmmaker::DemoState demo = SourceFilmmaker::Demo_State();
	g_saved.hostFramerate = CvarOr("host_framerate", 0);
	g_saved.matQueueMode = CvarOr("mat_queue_mode", -1);
	g_saved.drawHud = CvarOr("cl_drawhud", 1);
	g_saved.hBlood = CvarOr("violence_hblood", 1);
	g_saved.aBlood = CvarOr("violence_ablood", 1);
	g_saved.timescale = demo.timescale > 0 ? demo.timescale : 1.0;
	g_saved.paused = demo.paused;
	g_saved.tick = demo.tick;
	g_saved.freeCam = CameraBridge_GetFreeCamEnabled();
	g_stateSaved = true;
	Probe("saved host_framerate=%g mat_queue_mode=%g cl_drawhud=%g blood=%g/%g timescale=%g paused=%d tick=%d freeCam=%d",
		g_saved.hostFramerate, g_saved.matQueueMode, g_saved.drawHud, g_saved.hBlood, g_saved.aBlood, g_saved.timescale,
		g_saved.paused ? 1 : 0, g_saved.tick, g_saved.freeCam ? 1 : 0);
}

void RestoreState() {
	if (!g_stateSaved) return;
	g_stateSaved = false;
	Cmdf("host_framerate %g; mat_queue_mode %g; cl_drawhud %g; violence_hblood %g; violence_ablood %g; demo_timescale %g",
		g_saved.hostFramerate, g_saved.matQueueMode, g_saved.drawHud, g_saved.hBlood, g_saved.aBlood, g_saved.timescale);
	CameraBridge_SetFreeCamEnabled(g_saved.freeCam);
	// Back where the user was, as they were (a seek back rewinds in place: SourceDemoRewind.h).
	if (SourceFilmmaker::Demo_State().playing) Cmdf("demo_gototick %d 0 %d", g_saved.tick, g_saved.paused ? 1 : 0);
	Probe("restored");
}

// HLAE's recorder set up for the job: the take folder is the record name itself, the screen goes to
// the Beauty pass folder in it with the job's preset, at the job's frame rate, with HLAE's cam_main.cam
// when the job keeps camera data.
bool RecorderSetUp(std::string& error) {
	if (!Hook_MaterialSystem()) {
		error = "HLAE's screen recorder does not support this game's materialsystem.dll";
		return false;
	}
	CAfxStreams& rec = g_AfxStreams;
	g_savedRecorder.name = rec.Console_RecordName_get();
	g_savedRecorder.format = rec.Console_RecordFormat_get();
	g_savedRecorder.screen = rec.RecordScreen_Enabled_get();
	g_savedRecorder.screenSettings = rec.RecordScreen_Settings_get();
	g_savedRecorder.screenSuffix = rec.m_RecordScreenPathSuffix;
	g_savedRecorder.takeSubfolder = rec.m_RecordTakeSubfolder;
	g_savedRecorder.cam = rec.CamExport_get();
	g_savedRecorder.overrideFps = rec.GetOverrideFps();
	g_savedRecorder.overrideFpsValue = rec.GetOverrideFpsValue();
	g_savedRecorder.valid = true;
	if (!rec.RecordScreen_Settings_set(g_job.presetName.c_str())) {
		error = "the recorder has no preset for this format";
		return false;
	}
	const PassConfig beauty{};
	rec.Console_RecordName_set(WideToUtf8(g_job.takeDir).c_str());
	rec.m_RecordTakeSubfolder = false;
	rec.m_RecordScreenPathSuffix = "\\" + PassOutputFolder(beauty, RenderFormatIsSequence(g_job.out.format));
	rec.RecordScreen_Enabled_set(true);
	rec.Console_RecordFormat_set(g_job.out.format == RenderOutputFormat::Bmp ? "bmp" : "tga");
	rec.CamExport_set(g_job.passes.cameraData);
	rec.SetOverrideFps(true);
	rec.SetOverrideFpsValue((float)g_job.captureFps);
	return true;
}

void RecorderEnd() {
	if (g_AfxStreams.IsRecording()) g_AfxStreams.Console_Record_End();
}

void RecorderRestore() {
	if (!g_savedRecorder.valid) return;
	g_savedRecorder.valid = false;
	CAfxStreams& rec = g_AfxStreams;
	rec.Console_RecordName_set(g_savedRecorder.name.c_str());
	rec.Console_RecordFormat_set(g_savedRecorder.format.c_str());
	rec.RecordScreen_Settings_set(g_savedRecorder.screenSettings.c_str());
	rec.RecordScreen_Enabled_set(g_savedRecorder.screen);
	rec.m_RecordScreenPathSuffix = g_savedRecorder.screenSuffix;
	rec.m_RecordTakeSubfolder = g_savedRecorder.takeSubfolder;
	rec.CamExport_set(g_savedRecorder.cam);
	rec.SetOverrideFps(g_savedRecorder.overrideFps);
	rec.SetOverrideFpsValue(g_savedRecorder.overrideFpsValue);
}

// What the recorder left in the pass folder: the frames of a sequence, or 1 for a video file that
// has data.
int PassFilesWritten() {
	int files = 0;
	std::error_code ec;
	for (std::filesystem::directory_iterator it(g_passFolder, ec), end; !ec && it != end; it.increment(ec))
		if (it->is_regular_file(ec) && it->file_size(ec) > 0) ++files;
	return files;
}

// Ends the job: the state put back, the manifest's outcome, the chime, the earlier takes rescanned.
void FinishJob(S terminal, const char* status, const std::string& note, int written = 0) {
	RecorderEnd();
	RecorderRestore();
	RenderFrameMap_Close();
	RestoreState();
	if (!note.empty()) g_notes += note;
	RenderManifest_WriteFinal(g_job, g_demoPathUtf8, status, written, g_notes);
	Move(terminal);
	Render_SetJobActive(false);
	g_progress.phase = status;
	const double seconds = std::chrono::duration<double>(Clock::now() - g_startedAt).count();
	Probe("job %s: %d frames recorded, %d written, in %.1f s. %s", status, g_frame, written, seconds, note.c_str());
	if (terminal == S::Completed) RenderDoneSound_Play(true);
	else if (terminal == S::Failed) RenderDoneSound_Play(false);
	RenderTakeHistory_Refresh();
}

void FailJob(const std::string& why) {
	g_progress.lastError = why;
	advancedfx::Warning("mirv_filmmaker render: the job failed: %s.\n", why.c_str());
	FinishJob(S::Failed, "failed", why);
}

// Makes the take folder and sets HLAE's recorder up for it. False with the reason when it cannot.
bool PrepareOutput(std::string& error) {
	const PassConfig beauty{};
	const bool sequence = RenderFormatIsSequence(g_job.out.format);
	g_passFolder = g_job.takeDir + L"\\" + Utf8ToWide(PassOutputFolder(beauty, sequence));
	std::error_code ec;
	std::filesystem::create_directories(g_passFolder, ec);
	if (ec) {
		error = "could not make the take folder";
		return false;
	}
	std::string note;
	const bool nvenc = RenderEncoderDetect_State() == NvencProbe::Available;
	g_job.presetName = RenderOutput_PrepareRecorderPreset(g_job.out, nvenc, kPresetName, PassFileStem(beauty.elements), note);
	g_notes += note;
	if (g_job.presetName.empty()) {
		error = "the recorder has no preset for this format";
		return false;
	}
	if (!RecorderSetUp(error)) {
		RecorderRestore();
		return false;
	}
	return true;
}

void FlyPath(double tickTime) {
	double pose[7];
	if (!SourceFilmmaker::CameraPath_EvalPoseAtTick(tickTime, pose)) return;
	CameraBridge_SetFreeCamEnabled(true);
	CameraBridge_SetCameraPose(pose[0], pose[1], pose[2], pose[3], pose[4], pose[5], pose[6]);
}

void UpdateProgress() {
	int tick = 0;
	double time = 0;
	DemoClock(tick, time);
	g_progress.curTick = tick;
	g_progress.framesDone = g_frame;
	g_progress.framesTotal = g_job.framesTotal;
	g_progress.percent = g_job.framesTotal > 0 ? 100.0 * g_progress.framesDone / g_job.framesTotal : 0.0;
	g_progress.passes = 1;
	g_progress.rendersPerFrame = 1;
	g_progress.physicalDone = g_progress.framesDone;
	g_progress.physicalTotal = g_job.framesTotal;
}

void RunJobFrame() {
	RenderSession& session = RenderSessionRef();
	const SourceFilmmaker::DemoState demo = SourceFilmmaker::Demo_State();
	++g_wait;
	if (session.IsCancelRequested() && session.State() != S::CancelRequested && !RenderSession::IsTerminal(session.State()))
		Move(S::CancelRequested);
	if (session.State() == S::CancelRequested) {
		Move(S::RestoringState);
		FinishJob(S::Canceled, "canceled", "Canceled by the user. ");
		return;
	}
	if (!demo.playing) {
		FailJob("the demo stopped");
		return;
	}
	UpdateProgress();
	switch (session.State()) {
	case S::SavingState:
		SaveState();
		// The engine renders on the main thread (one SwapBuffers per frame, which HLAE's recorder takes),
		// the demo at its own speed, no blood when asked, the HUD as the job says, and no console: the
		// recorder takes no frame while it shows.
		Cmdf("hideconsole; mat_queue_mode 0; demo_timescale 1; cl_drawhud %d%s", g_job.passes.hud == RenderHudMode::None ? 0 : 1,
			g_job.passes.blood ? "" : "; violence_hblood 0; violence_ablood 0");
		g_sourceApplied = false;
		g_preRollTick = g_job.range.startTick - kWarmupTicks;
		if (g_preRollTick < 0) g_preRollTick = 0;
		Cmdf("demo_gototick %d 0 1", g_preRollTick);
		Move(S::SeekingPreRoll);
		SetPhase("seeking");
		Probe("seek to %d (range %d..%d, %d frames at %d fps)", g_preRollTick, g_job.range.startTick,
			g_job.range.endTickExclusive, g_job.framesTotal, g_job.captureFps);
		break;
	case S::SeekingPreRoll:
		// Landed (a seek stops on the demo's next packet) and paused. A range that starts within the
		// pre-roll of the demo's start seeks to tick 0, and the demo's first packet can come after
		// the range's start: the demo has nothing earlier, so the job starts where it landed.
		if (demo.paused && demo.tick >= g_preRollTick - 1 && g_wait > 2
			&& (demo.tick < g_job.range.startTick + 1 || g_preRollTick == 0)) {
			if (demo.tick > g_job.range.startTick)
				Probe("landed at %d, past the range's start %d: the demo has no earlier tick", demo.tick, g_job.range.startTick);
			Cmdf("host_framerate %d; demo_resume", g_job.captureFps);
			Move(S::WarmingUp);
			SetPhase("warming up");
		} else if (g_wait > kLandFrames) {
			FailJob("the seek to the range did not land");
		}
		break;
	case S::WarmingUp:
		// The camera source goes on a little before the range, after the director has put SourceTV's
		// camera back from the seek (SourceDirector.cpp), so the source is the last word.
		if (!g_sourceApplied && (g_wait > 20 || demo.tick >= g_job.range.startTick - 16)) {
			Render_ApplyCameraSourceLive(g_job.camera, "mirv_filmmaker render");
			g_sourceApplied = true;
		}
		if (g_job.camera == RenderCameraSource::CameraPath) {
			int tick = 0;
			double time = 0;
			DemoClock(tick, time);
			FlyPath(time);
		}
		if (demo.tick >= g_job.range.startTick) {
			Move(S::Capturing);
			SetPhase("rendering");
			g_frame = 0;
			Probe("capture starts at tick %d", demo.tick);
		} else if (g_wait > kLandFrames * 4) {
			FailJob("the demo did not reach the range");
			break;
		} else {
			break;
		}
		[[fallthrough]]; // this frame is the first captured one
	case S::Capturing: {
		int tick = 0;
		double time = 0;
		DemoClock(tick, time);
		// Last frame has rendered and been recorded: its row, with the view it showed.
		if (g_rowPending) {
			const SourceFilmmaker::ViewPose view = SourceFilmmaker::View_Rendered();
			for (int i = 0; i < 3; ++i)
				g_rowAngles[i] = GraphCurve::Unwrap(g_frame > 1 ? g_rowAngles[i] : 0.0, view.angles[i]);
			RenderFrameMap_Append(g_frame - 1, g_rowTick, g_rowTime * demo.intervalPerTick, view.origin, g_rowAngles, view.fov);
			g_rowPending = false;
		}
		if (g_frame >= g_job.framesTotal) {
			// Before this frame's SwapBuffers: the recorder has exactly the job's frames.
			RecorderEnd();
			Move(S::FlushingGpu);
			Move(S::FinalizingStreams);
			SetPhase("finishing");
			break;
		}
		if (g_job.camera == RenderCameraSource::CameraPath) FlyPath(time);
		// HLAE's recorder takes every frame at SwapBuffers once it runs: started here, before the first
		// frame in range renders, it takes that frame first.
		if (g_frame == 0) g_AfxStreams.Console_Record_Start2();
		if (!g_AfxStreams.IsRecording()) {
			FailJob("HLAE's recorder did not start");
			break;
		}
		g_rowPending = true;
		g_rowTick = tick;
		g_rowTime = time;
		++g_frame;
		break;
	}
	case S::FinalizingStreams:
		// The recorder has ended (a queued end once it is done); its writer gets a few frames for the
		// last of them.
		if (g_wait == 1) Cmd("demo_pause");
		if (!g_AfxStreams.IsRecording() && g_wait > kFinishFrames) {
			Move(S::PostProcessing);
			Move(S::RestoringState);
			const int files = PassFilesWritten();
			const int written = RenderFormatIsSequence(g_job.out.format) ? files : (files > 0 ? g_frame : 0);
			std::string note;
			if (written != g_job.framesTotal) {
				char buf[160];
				std::snprintf(buf, sizeof(buf), "%d of %d frames written. ", written, g_job.framesTotal);
				note = buf;
			}
			if (written == 0) FailJob(note);
			else FinishJob(S::Completed, "completed", note, written);
		}
		break;
	default:
		break;
	}
}

} // namespace

bool RenderSessionDriver_Start(std::string& outError) {
	RenderSession& session = RenderSessionRef();
	if (session.IsBusy()) {
		outError = "a job is already running";
		return false;
	}
	if (RenderSession::IsTerminal(session.State())) session.Reset();
	const SourceFilmmaker::DemoState demo = SourceFilmmaker::Demo_State();
	if (!demo.playing) {
		outError = "no demo is playing";
		return false;
	}
	const RenderValidationResult validation = Render_ValidateCurrentJobFull();
	if (!validation.Ok()) {
		outError = validation.FirstFatal();
		return false;
	}
	RenderRange range;
	if (!Render_ResolveRangeForDisplay(range) || !range.Valid()) {
		outError = "the range is not set";
		return false;
	}
	const RenderPasses& passes = RenderPassesRef();
	if (passes.passes.size() != 1 || !(passes.passes[0] == PassConfig{})) {
		outError = "CS:S renders the Beauty + Everything pass only";
		return false;
	}
	RenderJob job;
	job.range = range;
	job.camera = Render_GetCameraSource();
	job.extension = Render_GetExtension();
	job.out = RenderOutputSettingsRef();
	job.passes = passes;
	job.warmupTicks = kWarmupTicks;
	job.tickRate = RenderHost_TickRate();
	job.captureFps = RenderCaptureFps(job.out.fps);
	job.framesTotal = range.FrameCount(job.captureFps, job.tickRate);
	job.takeDir = BuildTakeDirectory(job.out);
	if (g_VEngineClient) g_VEngineClient->GetScreenSize(job.width, job.height);
	if (job.framesTotal <= 0) {
		outError = "the range has no frames at this frame rate";
		return false;
	}
	if (job.takeDir.empty()) {
		outError = "choose an output folder";
		return false;
	}
	g_job = job;
	g_progress = RenderProgress();
	g_progress.takeDirUtf8 = WideToUtf8(job.takeDir);
	g_progress.framesTotal = job.framesTotal;
	g_notes.clear();
	g_rowPending = false;
	g_frame = 0;
	g_startedAt = Clock::now();
	g_demoPathUtf8 = WideToUtf8(RenderHost_PlayingDemoPath());
	if (!Move(S::Validating) || !Move(S::PreparingOutput)) {
		outError = "the render session is busy";
		return false;
	}
	std::string error;
	if (!PrepareOutput(error)) {
		Move(S::Failed);
		outError = error;
		return false;
	}
	if (!RenderManifest_WriteStart(g_job, g_demoPathUtf8, g_notes)) {
		RecorderRestore();
		Move(S::Failed);
		outError = "could not write manifest.json into the take folder";
		return false;
	}
	if (g_job.passes.cameraData) RenderFrameMap_Open(g_job.takeDir);
	Render_SetJobActive(true);
	Move(S::SavingState);
	SetPhase("starting");
	Probe("job: range %d..%d, camera %d, %s at %d fps, %d frames, %dx%d, take %s", range.startTick, range.endTickExclusive,
		(int)job.camera, RenderFormatToken(job.out.format), job.captureFps, job.framesTotal, job.width, job.height,
		g_progress.takeDirUtf8.c_str());
	advancedfx::Message("mirv_filmmaker render: rendering %d frames into %s\n", job.framesTotal, g_progress.takeDirUtf8.c_str());
	return true;
}

void RenderSessionDriver_RunFrame() {
	const S state = RenderSessionRef().State();
	if (state == S::Idle || RenderSession::IsTerminal(state)) return;
	RunJobFrame();
}

bool RenderSessionDriver_IsActive() { return RenderSessionRef().IsBusy(); }

const RenderProgress& RenderSessionDriver_Progress() { return g_progress; }

// HLAE's recorder takes the frame at SwapBuffers, before the filmmaker's UI draws at Present, so the
// UI never needs hiding.
bool RenderSessionDriver_WantsChromeHidden() { return false; }

} // namespace Filmmaker

namespace SourceFilmmaker {

void Render_Command(int argc, advancedfx::ICommandArgs* args) { Filmmaker::Render_RunCommand(argc, args, "mirv_filmmaker"); }

void Render_Frame() {
	Filmmaker::RenderOutput_PumpMainThread();
	Filmmaker::RenderTakeHistory_PumpMainThread();
	Filmmaker::RenderSessionDriver_RunFrame();
}

void Render_PrintHealth() {
	const Filmmaker::RenderSession& session = Filmmaker::RenderSessionRef();
	const Filmmaker::RenderProgress& p = Filmmaker::RenderSessionDriver_Progress();
	advancedfx::Message("  render:     %s%s, %d/%d frames, phase %s, encoder %s; HLAE's screen recorder %s\n",
		Filmmaker::RenderSession::StateName(session.State()), Filmmaker::Render_IsModalOpen() ? ", workspace open" : "",
		p.framesDone, p.framesTotal, p.phase.empty() ? "-" : p.phase.c_str(), Filmmaker::RenderEncoderDetect_Describe(),
		g_AfxStreams.IsRecording() ? "recording" : "idle");
}

} // namespace SourceFilmmaker
