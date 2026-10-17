#pragma once

#include <functional>
#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }
namespace Filmmaker::SceneKeys { struct TrackData; class SceneKeyframeTrack; }

namespace Filmmaker {

void SceneKeys_RunFrame();
void SceneKeys_Flush();
// A practice session begins or ends: drop the scene keys in memory without saving them and read
// the playing demo's project again on the next frame (CameraPath::ReloadProject).
void SceneKeys_ReloadProject();
void SceneKeys_Reassert();
void SceneKeys_ApplyCaptureTick(double tick);
void SceneKeys_AppendTimelineState(std::ostringstream& out);
bool SceneKeys_KeySpan(double& lo, double& hi);
void SceneKeys_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);
std::string SceneKeys_StateJson();
std::string SceneKeys_ChannelsJson();
int SceneKeys_Revision();
// How many properties are switched on, and how many scene keys exist (onboarding done checks).
int SceneKeys_ArmedCount();
int SceneKeys_KeyCount();

// Scene object rigs (SceneRig.cpp) keep their pose keys and attachments in this track.
const SceneKeys::TrackData& SceneKeys_Data();
// The tick the keys were applied at this frame (the capture tick while rendering).
double SceneKeys_AppliedTick();
// One edit of the track: an undo step (unless a gesture is open), Smooth handles re-solved,
// saved with the demo's project.
void SceneKeys_Edit(const std::function<void(SceneKeys::SceneKeyframeTrack&)>& edit);
// A drag that makes many edits as one undo step; EndGesture(false) abandons them.
void SceneKeys_BeginGesture();
void SceneKeys_EndGesture(bool keep);

} // namespace Filmmaker
