#pragma once
// Shared camera-look control. The graphics hosts read the snapshot at their pre-UI boundary.
namespace advancedfx { class ICommandArgs; }
namespace Filmmaker {
bool Fisheye_Enabled();
void Fisheye_SetEnabled(bool on);
void Fisheye_Toggle();
void Fisheye_RunCommand(int argc,advancedfx::ICommandArgs* args,const char* cmd);
}
