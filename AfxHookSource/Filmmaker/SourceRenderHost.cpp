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
#include "SourceTimelineDock.h"
#include <AfxHookSource2/Filmmaker/Movie/FollowCamera.h>
#include "SourceRenderPassTable.h"
#include "SourceRenderPasses.h"

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
	return FollowCameraRef().State().hasCamera || FollowCameraRef().State().mode == FollowMode::Attach;
}

bool RenderHost_Spectating() { return SourceFilmmaker::Players_ObservedIndex() > 0; }

// The director's cameras, as the demo bar's camera button sets them. The camera path and the follow
// camera are only chosen here: the job flies them (SourceRenderDriver.cpp), and selecting a source
// starts nothing, as in CS2.
void RenderHost_ApplyCameraSource(RenderCameraSource source, const char* cmd) {
	using namespace SourceFilmmaker::Ui;
    // As in CS2, another owner must release the view before the selected source takes it.
    if (source != RenderCameraSource::CurrentViewer) {
        if (source != RenderCameraSource::FollowCamera && FollowCameraRef().State().enabled)
            FollowCameraRef().StopPreview("render camera source");
        SourceFilmmaker::TimelineDock_SetDrive(false);
        if (source != RenderCameraSource::CameraPath && SourceFilmmaker::CameraPath_OwnsView())
            SourceFilmmaker::CameraPath_Stop();
    }
	switch (source) {
	case RenderCameraSource::Pov: SourceFilmmaker::Director_SetCamera(DirectorMath::kInEye); break;
	case RenderCameraSource::ThirdPerson: SourceFilmmaker::Director_SetCamera(DirectorMath::kChase); break;
	case RenderCameraSource::FreeCamera: SourceFilmmaker::Director_SetCamera(DirectorMath::kRoaming); break;
	case RenderCameraSource::CameraPath:
		if (RenderHost_PathKeyCount() < 2)
			advancedfx::Warning("%s: Camera Path source needs 2+ camera keys (place them with K).\n", cmd);
		break;
	case RenderCameraSource::FollowCamera:
        FollowCameraRef().Preview();
		break;
	case RenderCameraSource::CurrentViewer:
		break;
	}
}

bool RenderHost_EditorActive() { return SourceFilmmaker::Ui_EditorOpen(); }

void RenderHost_WorkspaceOpened() {} // the workspaces always keep the mouse until the game is clicked

void RenderHost_ApplyLivePreview(const RenderPasses& passes, bool open) {
    using namespace SourceFilmmaker;
    const auto route = RoutePass((int)passes.preview.type,(int)passes.preview.elements);
    ExtraPassLook look; look.depthNear = passes.depthNear; look.depthFar = passes.depthFar;
    for (int c=0;c<3;++c) look.key[c] = (unsigned char)std::lround(passes.keyColor[c]*255);
    RenderPasses_Preview(route.extra,look,open && route.way == PassWay::Extra);
}

const char* RenderHost_FormatUnavailable(RenderOutputFormat format) {
	if (format == RenderOutputFormat::Exr)
		return "EXR keeps the scene from before tone mapping, which the CS:S frame no longer has. Use PNG or TGA.";
	return nullptr;
}

const char* RenderHost_PassUnavailable(const PassConfig& pass) {
    const SourceFilmmaker::PassRoute route = SourceFilmmaker::RoutePass((int)pass.type, (int)pass.elements);
    if (route.way == SourceFilmmaker::PassWay::Unavailable) return route.why;
    if (route.way == SourceFilmmaker::PassWay::NotAPass) return "This pass combination does not exist.";
    return nullptr;
}

int RenderHost_PhysicalRendersPerFrame(const std::vector<PassConfig>& passes) {
    int extra = 0; bool reference = false;
    for (const PassConfig& pass : passes) {
        const auto route = SourceFilmmaker::RoutePass((int)pass.type,(int)pass.elements);
        if (route.way == SourceFilmmaker::PassWay::Extra) {
            ++extra; reference |= SourceFilmmaker::PassDepthMask(route.extra) == 2;
        }
    }
    return 1 + extra + (extra ? 1 : 0) + (reference ? 1 : 0);
}

bool RenderHost_RunCommand(const char*, int, advancedfx::ICommandArgs*, const char*) { return false; }

void RenderHost_PrintHelp(const char*) {}

bool RenderHost_PassKeyTolerance(float&) { return false; }
bool RenderHost_SetPassKeyTolerance(float) { return false; }
bool RenderHost_SetPassDepthDebug(bool) { return false; }

// SourceRenderDriver.cpp's job puts these back on every outcome.
void RenderHost_RestoreCoverage(std::size_t& covered, std::size_t& uncovered) {
	covered = 10;
	uncovered = 0;
}

std::string RenderHost_RestoreCoverageReport() {
	return "render state restore (CS:S): host_framerate, demo_timescale, mat_queue_mode, cl_drawhud and the blood cvars "
		"as they were; camera mode, watched player, free-camera pose and FOV; the demo's tick and paused/playing state.\n";
}

} // namespace Filmmaker
