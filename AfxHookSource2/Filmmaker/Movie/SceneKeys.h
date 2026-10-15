#pragma once

#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

void SceneKeys_RunFrame();
void SceneKeys_Flush();
void SceneKeys_Reassert();
void SceneKeys_ApplyCaptureTick(double tick);
void SceneKeys_AppendGraphState(std::ostringstream& out);
void SceneKeys_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);
std::string SceneKeys_StateJson();
std::string SceneKeys_ChannelsJson();
int SceneKeys_Revision();

} // namespace Filmmaker
