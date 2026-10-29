#pragma once

// The render workspace in CS:S. Its settings, console surface and earlier takes are CS2's shared
// code (AfxHookSource2/Filmmaker/Render: RenderCommand.cpp through RenderHost.h, which
// SourceRenderHost.cpp answers). The job is CS:S's own (SourceRenderDriver.cpp implements
// RenderSessionDriver.h): it seeks a little before the range, plays it at the output frame rate
// (host_framerate, demo_timescale 1, the engine single-threaded), flies the camera path when that
// is the camera, and records it with HLAE's screen recorder (mirv_streams record screen, with its
// cam_main.cam), ffmpeg or the classic image writer, with the manifest and frame map beside it.
//
// CS:S draws the shared pass grid through SourceRenderPasses.h. Main thread only.

#include <string>
#include <vector>

namespace advancedfx {
class ICommandArgs;
}

namespace SourceFilmmaker {

namespace Ui {
struct DemoBarModel;
struct RenderWorkspaceModel;
} // namespace Ui

// mirv_filmmaker render ...
void Render_Command(int argc, advancedfx::ICommandArgs* args);
// Once a frame, before the frame renders: the folder picker's answer, the earlier takes, the job.
void Render_Frame();
// One line for mirv_filmmaker health.
void Render_PrintHealth();

// Main thread: the Render workspace's model for this frame (SourceRenderModel.cpp). bar is this frame's
// demo bar, whose rounds resolve the Round range.
void Render_BuildModel(Ui::RenderWorkspaceModel& model, const Ui::DemoBarModel& bar);
// Main thread: the workspace's console commands.
void Render_RunCommands(const std::vector<std::string>& commands);

} // namespace SourceFilmmaker
