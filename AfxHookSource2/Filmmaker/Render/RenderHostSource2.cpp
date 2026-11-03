// CS2's answers to the render workspace's shared code (RenderHost.h).

#include "RenderHost.h"

#include "RenderEngineSize.h"
#include "RenderHdrCapture.h"
#include "RenderPassPlan.h"
#include "RenderPlayersOnlyGreen.h"
#include "RenderResolution.h"
#include "RenderStateGuard.h"
#include "RenderSupersample.h"
#include "RenderTargetProbe.h"
#include "RenderWorldOnly.h"
#include "RenderWorldPlayersNoGun.h"
#include "../../RenderSystemDX11Hooks.h" // the live pass view, pass key tolerance
#include "../Filmmaker.h" // CameraEditor_Active, PlayingDemoPath
#include "../Debug/MvmDebugLog.h"
#include "../Movie/CameraBridge.h" // CameraBridge_GetFreeCamEnabled (camera-source probe)
#include "../Movie/CameraPath.h"
#include "../Movie/FollowCamera.h"
#include "../Movie/MovieMode.h"
#include "../Panorama/CameraEditorHud.h"
#include "../Panorama/GraphEditorExperimentHud.h"
#include "../Demo/DemoHeaderReader.h"
#include "../Demo/PlayingDemoPath.h"

#include "../../ClientEntitySystem.h" // AfxGetSpectatedPawnIndex (POV source availability)
#include "../../MirvTime.h"
#include "../../WrpConsole.h"

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <atomic>
#include <cstring>
#include <string>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

// RenderSystemDX11Hooks.cpp: whether a recording forces r_wait_on_present (diagnostic).
extern std::atomic<bool> g_RecordWaitOnPresent;

namespace Filmmaker {

namespace {

// Demo tick bounds for FullDemo resolution + the timeline range highlight. Cached per demo
// path (the header read is a small bounded file read -- same pattern as DemoEndHold).
std::wstring s_boundsPath;
int s_demoStartTick = -1;
int s_demoEndTick = -1;

} // namespace

bool RenderHost_DemoBounds(int& outStartTick, int& outEndTick) {
	if (!g_pEngineToClient) return false;
	auto* demo = g_pEngineToClient->GetDemoFile();
	if (!demo || !(demo->IsPlayingDemo() || demo->IsDemoPaused())) { s_boundsPath.clear(); return false; }
	std::wstring path = ResolvePlayingDemoPath();
	if (path.empty()) return false;
	if (path != s_boundsPath) {
		s_boundsPath = path;
		DemoHeaderInfo hi = ReadDemoHeader(path);
		int start = demo->GetDemoStartTick();
		if (start < 0) start = 0;
		if (hi.ok && hi.playbackTicks > 0) {
			s_demoStartTick = start;
			s_demoEndTick = start + hi.playbackTicks;
		} else {
			s_demoStartTick = s_demoEndTick = -1;
		}
		// Only on a cache miss (once per demo), so it is free per frame. Carries the INPUTS
		// (engine start tick, header ok + playback_ticks) next to the cached result: a Full
		// Demo range that comes out empty is either a bad header or a bad start tick, and
		// this line says which.
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("ui.render.bounds",
				"resolve headerOk=%d playbackTicks=%d engineStart=%d -> start=%d end=%d",
				hi.ok ? 1 : 0, hi.playbackTicks, start, s_demoStartTick, s_demoEndTick);
	}
	if (s_demoStartTick < 0 || s_demoEndTick <= s_demoStartTick) return false;
	outStartTick = s_demoStartTick;
	outEndTick = s_demoEndTick;
	return true;
}

int RenderHost_CurrentTick() {
	int tick = 0;
	g_MirvTime.GetCurrentDemoTick(tick);
	return tick;
}

double RenderHost_TickRate() { return 64.0; }

std::wstring RenderHost_PlayingDemoPath() { return PlayingDemoPath(); }

int RenderHost_PathKeyCount() { return CameraPathRef().Count(); }

int RenderHost_PathKeyTick(int index) {
	CameraPath& cp = CameraPathRef();
	return index >= 0 && index < cp.Count() ? cp.Markers()[index].tick : 0;
}

int RenderHost_PathSelected() { return CameraPathRef().Selected(); }

void RenderHost_PathCoverage(int& firstTick, int& lastTick) {
	CameraPath& cp = CameraPathRef();
	firstTick = cp.FirstPathTick();
	lastTick = cp.LastPathTick();
}

bool RenderHost_FollowReady() { return FollowCameraRef().State().hasCamera; }

bool RenderHost_Spectating() { return AfxGetSpectatedPawnIndex() >= 0; }

// Graph drive and any follow preview are disengaged first (they would otherwise keep owning the
// camera); the mode switch keeps the UI cursor on so the workspace stays clickable.
void RenderHost_ApplyCameraSource(RenderCameraSource src, const char* cmd) {
	FollowCamera& fc = FollowCameraRef();
	GraphEditorExperimentHud& ge = GraphEditorExperimentHudRef();
	MovieMode& mm = MovieModeRef();
	CameraPath& cp = CameraPathRef();

	// Camera-path playback is a THIRD camera owner beside follow-preview and graph drive, and it
	// was the only one no source released. Picking any non-path source while a path was playing
	// (or parked after a play) left the dolly still driving the view -- so a POV/third-person
	// render captured the path's last keyframe pose instead of the source the user chose. Release
	// it exactly like the other two owners.
	auto releaseCameraPath = [&](const char* why) {
		if (cp.IsPlaying() || cp.PlaybackPending() || cp.OwnsView())
			cp.StopPreview(); // Esc: stop playback and hand the view back to editing
		(void)why;
	};

	switch (src) {
	case RenderCameraSource::CurrentViewer:
		// Freeze whatever is on screen right now: deliberately no live switch, and deliberately
		// no path release -- "current viewer" means exactly what is on screen, path included.
		break;
	case RenderCameraSource::Pov:
		fc.StopPreview("render camera pov");
		ge.SetDrive(false);
		releaseCameraPath("render camera pov");
		mm.SetMode(MovieMode::Mode::Default); // CS2 first person
		break;
	case RenderCameraSource::ThirdPerson:
		fc.StopPreview("render camera thirdperson");
		ge.SetDrive(false);
		releaseCameraPath("render camera thirdperson");
		mm.SetMode(MovieMode::Mode::ThirdPerson); // CS2 chase cam
		break;
	case RenderCameraSource::FreeCamera:
		fc.StopPreview("render camera freecam");
		ge.SetDrive(false);
		releaseCameraPath("render camera freecam");
		mm.SetMode(MovieMode::Mode::FreeCam); // CS2 free cam
		break;
	case RenderCameraSource::CameraPath:
		fc.StopPreview("render camera path");
		ge.SetDrive(false);
		// Selecting the source must NOT start playback. Auto-playing here seeked the demo and
		// parked the camera on the path's last keyframe as a side effect of a dropdown change,
		// and there is already a Play button that owns this: the camera timeline's. Selecting
		// the source only says "this shot is the dolly"; pressing Play runs it.
		if (cp.Count() < 2)
			advancedfx::Warning("%s: Camera Path source needs 2+ camera keys (place them with K).\n", cmd);
		else
			advancedfx::Message("%s: camera source = Camera Path (%d keys). Press Play on the camera "
				"timeline to run the dolly; selecting the source no longer starts it.\n", cmd, cp.Count());
		break;
	case RenderCameraSource::FollowCamera:
		ge.SetDrive(false);
		releaseCameraPath("render camera follow");
		fc.Preview(); // warns itself when no target/camera is set up
		break;
	}
	// The bare "applied live" line could not distinguish "we switched" from "we asked and
	// nothing happened" (POV with nobody spectated, Path with <2 keys, Follow with no target).
	// Log the preconditions and the resulting mode alongside the request.
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("ui.render.camera",
			"source=%d applied mode=%d freeCam=%d pathKeys=%d pathPlaying=%d followPreview=%d graphDrive=%d",
			(int)src, (int)mm.GetMode(), CameraBridge_GetFreeCamEnabled() ? 1 : 0,
			cp.Count(), (cp.IsPlaying() || cp.PlaybackPending()) ? 1 : 0,
			fc.State().hasCamera ? 1 : 0, ge.Drive() ? 1 : 0);
}

bool RenderHost_EditorActive() { return CameraEditor_Active(); }

// Render is an all-UI workspace. A completed job can restore a camera mode that normally
// returns the mouse to CS2; opening (or re-opening) Render must always hand it back to
// Panorama so dropdowns and output controls are physically clickable.
void RenderHost_WorkspaceOpened() { CameraEditor_SetCursorMode(true); }

// The pass's visibility group through the scene draw policies, and for Depth / Normal the pass
// compositor over the finished scene. Gun only shows its viewmodel depth band live (a job also
// adds the muzzle flash, which needs a second render of the frame).
void RenderHost_ApplyLivePreview(const RenderPasses& passes, bool open) {
	const PassConfig live = open ? passes.preview : PassConfig{};
	const VisibleElements e = live.elements;
	AfxStreams_SetLivePassPreview((int)live.type, (int)live.elements, passes.depthNear, passes.depthFar, passes.keyColor);
	RenderWorldOnly_SetArmed(e == VisibleElements::OnlyWorld, "render workspace preview");
	// Players Only is the no-gun view masked by depth against a World Only reference render the
	// render loop adds while this is on (AfxStreams_SetLivePassPreview) -- no chroma repaint.
	RenderWorldPlayersNoGun_SetArmed(e == VisibleElements::WorldAndPlayers || e == VisibleElements::OnlyPlayers,
		"render workspace preview");
	// Only Beauty + Everything can carry the game HUD (the HUD setting is that plate's alone), so the
	// live view of any other plate hides it as well. The HUD's own bottom shading laid a dark gradient
	// over the white gun of a live Depth + Gun only view, which the render never had (tester report).
	if (live.type != PassType::Beauty || e != VisibleElements::Everything)
		CameraEditorHudRef().SetHudViewOverride(CameraEditorHud::HudView::HideAll);
	else
		CameraEditorHudRef().ClearHudViewOverride();
}

const char* RenderHost_FormatUnavailable(RenderOutputFormat) { return nullptr; }
const char* RenderHost_PassUnavailable(const PassConfig&) { return nullptr; }

int RenderHost_PhysicalRendersPerFrame(const std::vector<PassConfig>& passes) {
    return BuildPassPlan(passes).PhysicalRendersPerFrame();
}

bool RenderHost_RunCommand(const char* sub, int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (0 == _stricmp(sub, "hdrgrab")) {
		RenderHdrCapture_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "waitpresent")) {
		if (argc >= 4) {
			const char* v = args->ArgV(3);
			if (0 == strcmp(v, "0") || 0 == strcmp(v, "1")) ::g_RecordWaitOnPresent.store(v[0] == '1', std::memory_order_relaxed);
			else { advancedfx::Warning("usage: %s render waitpresent 0|1\n", cmd); return true; }
		}
		advancedfx::Message("%s: waitpresent=%d (transient diagnostic; 1 forces r_wait_on_present while recording, the old behaviour)\n",
			cmd, ::g_RecordWaitOnPresent.load(std::memory_order_relaxed) ? 1 : 0);
	} else if (0 == _stricmp(sub, "worldonly")) {
		RenderWorldOnly_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "playersonlygreen")
		|| 0 == _stricmp(sub, "playersgreen")
		|| 0 == _stricmp(sub, "greenscreen")) {
		RenderPlayersOnlyGreen_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "res")) {
		RenderResolution_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "rtprobe")) {
		RenderTargetProbe_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "supersample") || 0 == _stricmp(sub, "ss")) {
		RenderSupersample_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "enginesize")) {
		RenderEngineSize_RunCommand(argc, args, cmd);
	} else {
		return false;
	}
	return true;
}

void RenderHost_PrintHelp(const char* cmd) {
	advancedfx::Message("%s render waitpresent 0|1 - diagnostic: 1 forces r_wait_on_present while recording (the old behaviour).\n", cmd);
}

bool RenderHost_PassKeyTolerance(float& tolerance) {
	tolerance = AfxStreams_GetPassKeyTolerance();
	return true;
}

bool RenderHost_SetPassKeyTolerance(float tolerance) {
	AfxStreams_SetPassKeyTolerance(tolerance);
	return true;
}

bool RenderHost_SetPassDepthDebug(bool on) {
	AfxStreams_SetPassDepthDebug(on);
	return true;
}

void RenderHost_RestoreCoverage(std::size_t& covered, std::size_t& uncovered) {
	covered = RenderStateRestoreGuard::Covered().size();
	uncovered = RenderStateRestoreGuard::Uncovered().size();
}

std::string RenderHost_RestoreCoverageReport() { return RenderStateRestoreGuard::CoverageReport(); }

} // namespace Filmmaker
