// CS:S's answers to the render workspace's shared code (AfxHookSource2/Filmmaker/Render/RenderHost.h):
// the demo from the engine's demo player and the demo index, the camera path and the follow camera
// from their CS:S facades, the camera sources through the director's cameras.

#include "stdafx.h"

#include "SourceCameraPath.h"
#include "SourceDemoIndex.h"
#include "SourceDirector.h"
#include "SourceEngine.h"
#include "SourceFollowCamera.h"
#include "SourceUi.h"

#include "Ui/DirectorMath.h"
#include "Ui/FollowInspector.h"

#include <AfxHookSource2/Filmmaker/Platform/TextEncoding.h>
#include <AfxHookSource2/Filmmaker/Render/RenderHost.h>
#include <Shared/AfxConsole.h>

#include <cmath>

namespace Filmmaker {

bool RenderHost_DemoBounds(int& startTick, int& endTick) {
	const SourceFilmmaker::DemoState demo = SourceFilmmaker::Demo_State();
	if (!demo.playing || demo.totalTicks <= 0) return false;
	startTick = 0;
	endTick = demo.totalTicks;
	return true;
}

int RenderHost_CurrentTick() { return SourceFilmmaker::Demo_State().tick; }

double RenderHost_TickRate() {
	const double interval = SourceFilmmaker::Demo_State().intervalPerTick;
	return interval > 0.0 ? 1.0 / interval : 66.0 + 2.0 / 3.0; // CS:S's 66.7 tick until a demo says otherwise
}

std::wstring RenderHost_PlayingDemoPath() {
	if (!SourceFilmmaker::Demo_State().playing) return std::wstring();
	return Utf8ToWide(SourceFilmmaker::DemoIndex_Get().path);
}

int RenderHost_PathKeyCount() { return SourceFilmmaker::CameraPath_Markers().Count(); }

int RenderHost_PathKeyTick(int index) {
	const std::vector<CamMarker>& keys = SourceFilmmaker::CameraPath_Markers().All();
	return index >= 0 && index < (int)keys.size() ? keys[index].tick : 0;
}

int RenderHost_PathSelected() { return SourceFilmmaker::CameraPath_State().selected; }

void RenderHost_PathCoverage(int& firstTick, int& lastTick) {
	const std::vector<CamMarker>& keys = SourceFilmmaker::CameraPath_Markers().All();
	firstTick = keys.empty() ? 0 : keys.front().tick;
	lastTick = keys.empty() ? 0 : keys.back().tick;
}

bool RenderHost_FollowReady() {
	SourceFilmmaker::Ui::FollowInspectorModel follow;
	SourceFilmmaker::FollowCamera_InspectorModel(follow);
	return follow.enabled && follow.hasCamera;
}

bool RenderHost_Spectating() { return SourceFilmmaker::Players_ObservedIndex() > 0; }

// The director's cameras, as the demo bar's camera button sets them. The camera path and the follow
// camera are only chosen here: the job flies them (SourceRenderDriver.cpp), and selecting a source
// starts nothing, as in CS2.
void RenderHost_ApplyCameraSource(RenderCameraSource source, const char* cmd) {
	using namespace SourceFilmmaker::Ui;
	switch (source) {
	case RenderCameraSource::Pov: SourceFilmmaker::Director_SetCamera(DirectorMath::kInEye); break;
	case RenderCameraSource::ThirdPerson: SourceFilmmaker::Director_SetCamera(DirectorMath::kChase); break;
	case RenderCameraSource::FreeCamera: SourceFilmmaker::Director_SetCamera(DirectorMath::kRoaming); break;
	case RenderCameraSource::CameraPath:
		if (RenderHost_PathKeyCount() < 2)
			advancedfx::Warning("%s: Camera Path source needs 2+ camera keys (place them with K).\n", cmd);
		break;
	case RenderCameraSource::FollowCamera:
		if (!RenderHost_FollowReady())
			advancedfx::Warning("%s: Follow Camera source needs a follow camera placed (the FOLLOW CAMERA tab).\n", cmd);
		break;
	case RenderCameraSource::CurrentViewer:
		break;
	}
}

bool RenderHost_EditorActive() { return SourceFilmmaker::Ui_EditorOpen(); }

void RenderHost_WorkspaceOpened() {} // the workspaces always keep the mouse until the game is clicked

// CS:S renders Beauty only so far: there is no other pass to show live.
void RenderHost_ApplyLivePreview(const RenderPasses&, bool) {}

const char* RenderHost_FormatUnavailable(RenderOutputFormat format) {
	if (format == RenderOutputFormat::Exr)
		return "EXR keeps the scene from before tone mapping, which the CS:S frame no longer has. Use PNG or TGA.";
	return nullptr;
}

bool RenderHost_RunCommand(const char*, int, advancedfx::ICommandArgs*, const char*) { return false; }

void RenderHost_PrintHelp(const char*) {}

bool RenderHost_PassKeyTolerance(float&) { return false; }
bool RenderHost_SetPassKeyTolerance(float) { return false; }
bool RenderHost_SetPassDepthDebug(bool) { return false; }

// SourceRenderDriver.cpp's job puts these back on every outcome.
void RenderHost_RestoreCoverage(std::size_t& covered, std::size_t& uncovered) {
	covered = 6;
	uncovered = 0;
}

std::string RenderHost_RestoreCoverageReport() {
	return "render state restore (CS:S): host_framerate, demo_timescale, mat_queue_mode, cl_drawhud and the blood cvars "
		"as they were; the camera's mode; the demo paused where the job ended.\n";
}

} // namespace Filmmaker
