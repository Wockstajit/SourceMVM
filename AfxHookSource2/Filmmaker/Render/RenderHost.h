#pragma once

// What the render workspace's shared code asks the game it runs in: the settings, the console
// surface and the state (RenderCommand.cpp), the format table (RenderOutput.cpp) and the earlier
// takes (RenderTakeHistory.cpp). CS2 answers in RenderHostSource2.cpp, CS:S in
// AfxHookSource/Filmmaker/SourceRenderHost.cpp. The job itself is each game's own: both implement
// RenderSessionDriver.h.

#include "RenderTypes.h"

#include <cstddef>
#include <string>

namespace advancedfx {
class ICommandArgs;
}

namespace Filmmaker {

// ---- the demo -----------------------------------------------------------------------------------

// The playing demo's first tick and its last. False without a demo, or when its length is unknown.
bool RenderHost_DemoBounds(int& startTick, int& endTick);
// The demo tick the world is on.
int RenderHost_CurrentTick();
// Demo ticks a second.
double RenderHost_TickRate();
// The playing demo's file (empty without one), for this demo's earlier takes.
std::wstring RenderHost_PlayingDemoPath();

// ---- cameras ------------------------------------------------------------------------------------

// The camera path's keys: how many, the tick of one, and the selected one (-1 for none).
int RenderHost_PathKeyCount();
int RenderHost_PathKeyTick(int index);
int RenderHost_PathSelected();
// The ticks the path's camera covers, first to last (validation: a Camera Path job must not run
// past them). Only asked with two keys or more.
void RenderHost_PathCoverage(int& firstTick, int& lastTick);
// The follow camera has a camera placed; a player is spectated.
bool RenderHost_FollowReady();
bool RenderHost_Spectating();
// Makes the live view what a camera source renders, so the workspace's preview is the take.
void RenderHost_ApplyCameraSource(RenderCameraSource source, const char* cmd);

// ---- the workspace ------------------------------------------------------------------------------

// The Camera Editor is open (the workspace lives inside it).
bool RenderHost_EditorActive();
// The workspace opened: the mouse goes to the UI.
void RenderHost_WorkspaceOpened();
// Shows the previewed pass live while the workspace is open, or clears it (open false).
void RenderHost_ApplyLivePreview(const RenderPasses& passes, bool open);
// Why an output format cannot be made in this game, or nullptr when it can.
const char* RenderHost_FormatUnavailable(RenderOutputFormat format);

// ---- the console --------------------------------------------------------------------------------

// Subcommands only this game has (CS2's diagnostics). True when sub was one of them.
bool RenderHost_RunCommand(const char* sub, int argc, advancedfx::ICommandArgs* args, const char* cmd);
// Their help lines.
void RenderHost_PrintHelp(const char* cmd);
// `render pass keytolerance` and `depthdebug`: false when the game has no such setting.
bool RenderHost_PassKeyTolerance(float& tolerance);
bool RenderHost_SetPassKeyTolerance(float tolerance);
bool RenderHost_SetPassDepthDebug(bool on);
// What the job's state guard puts back afterwards, and what it does not.
void RenderHost_RestoreCoverage(std::size_t& covered, std::size_t& uncovered);
std::string RenderHost_RestoreCoverageReport();

} // namespace Filmmaker
