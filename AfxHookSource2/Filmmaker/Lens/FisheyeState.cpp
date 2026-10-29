#include "FisheyeState.h"
#include "../../../Shared/AfxConsole.h"
#include <atomic>
#include <cstring>
namespace Filmmaker {
namespace { std::atomic<bool> g_enabled{false}; }
// --- Fisheye lens (barrel-distortion D3D pass, decoupled from deleted ActionCam) ---
bool Fisheye_Enabled() { return g_enabled.load(); }
void Fisheye_SetEnabled(bool on) { g_enabled.store(on); }
void Fisheye_Toggle() { Fisheye_SetEnabled(!g_enabled.load()); }
void Fisheye_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* arg = (argc >= 3) ? args->ArgV(2) : "toggle";
	if (0 == _stricmp(arg, "state")) {
		advancedfx::Message("[fisheye][state] %s\n", g_enabled.load() ? "on" : "off");
		return;
	}
	if (0 == _stricmp(arg, "on") || 0 == _stricmp(arg, "1")) Fisheye_SetEnabled(true);
	else if (0 == _stricmp(arg, "off") || 0 == _stricmp(arg, "0")) Fisheye_SetEnabled(false);
	else Fisheye_Toggle();
	advancedfx::Message("mirv_filmmaker: fisheye lens %s.\n", g_enabled.load() ? "ON" : "off");
}

} // namespace Filmmaker
