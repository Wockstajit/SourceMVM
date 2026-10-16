#pragma once

#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

void SceneKeys_RunFrame();
void SceneKeys_Flush();
// A practice session begins or ends: drop the scene keys in memory without saving them and read
// the playing demo's project again on the next frame (CameraPath::ReloadProject).
void SceneKeys_ReloadProject();
void SceneKeys_Reassert();
void SceneKeys_ApplyCaptureTick(double tick);
void SceneKeys_AppendGraphState(std::ostringstream& out);
void SceneKeys_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);
std::string SceneKeys_StateJson();
std::string SceneKeys_ChannelsJson();
int SceneKeys_Revision();
// How many properties are switched on, and how many scene keys exist (onboarding done checks).
int SceneKeys_ArmedCount();
int SceneKeys_KeyCount();

} // namespace Filmmaker
