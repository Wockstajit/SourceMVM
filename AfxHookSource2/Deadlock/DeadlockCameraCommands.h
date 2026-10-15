#pragma once

namespace Deadlock {
// Main-thread camera commands use the same accepted view and demo clock as the editor.
bool CameraCommands_Active();
void CameraCommands_RunFrame(bool demoLoaded);
bool CameraCommands_Apply(int width, int height, float& x, float& y, float& z,
    float& pitch, float& yaw, float& roll, float& fov);
void CameraCommands_Record(int width, int height, double x, double y, double z,
    double pitch, double yaw, double roll, double fov);
void MirvCommands_RunFrame();
// ONE PANORAMA EVAL for `mirv_panorama eval` and `mirv_filmmaker ui_eval`: arguments from
// `firstArg` on are rejoined with single spaces (the tokenizer splits JS on quotes and
// commas) and run in the workspace context. Returns false, after warning under `who`, when
// there is no ready Panorama context.
}
namespace advancedfx { class ICommandArgs; }
namespace Deadlock {
bool CameraCommands_EvalJs(advancedfx::ICommandArgs* args, int firstArg, const char* who);
}
