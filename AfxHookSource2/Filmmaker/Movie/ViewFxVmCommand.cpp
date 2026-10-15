#include "ViewFxVm.h"

#include "ViewFxVmInternal.h"
#include "../../../Shared/AfxConsole.h"

#include <cstdlib>
#include <cstring>

namespace Filmmaker {

void ViewFxVm_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	using namespace ViewFxVmInternal;
	const char* sub = argc >= 4 ? args->ArgV(3) : "";
	const char* val = argc >= 5 ? args->ArgV(4) : "";

	if (0 == _stricmp(sub, "info")) {
		SceneTransform_DumpInfo();
		return;
	}
	if (0 == _stricmp(sub, "site")) {
		const int site = atoi(val);
		if (site == 4 && !CalcHook_EnsureInstalled()) {
			advancedfx::Warning("[viewfxvm] site 4 unavailable; staying at %d.\n", g_site.load());
			return;
		}
		g_site.store(site);
		g_siteExplicit.store(true, std::memory_order_relaxed);
		advancedfx::Message("[viewfxvm] site=%d.\n", site);
		return;
	}
	if (0 == _stricmp(sub, "field")) {
		int field = 1;
		if (0 == _stricmp(val, "quat")) field = 2;
		else if (0 == _stricmp(val, "both")) field = 3;
		g_field.store(field);
		advancedfx::Message("[viewfxvm] field=%d (1=local ang, 2=nodeToWorld quat, 3=both).\n",
			field);
		return;
	}
	if (0 == _stricmp(sub, "spin")) {
		g_mode.store(0);
		g_spinDegPerApply = atof(val);
		advancedfx::Message("[viewfxvm] test spin=%.2f deg/apply (mode=test).\n",
			g_spinDegPerApply);
		return;
	}
	if (0 == _stricmp(sub, "mode")) {
		g_mode.store(0 == _stricmp(val, "deadzone") ? 1 : 0);
		advancedfx::Message("[viewfxvm] mode=%s.\n",
			g_mode.load() == 1 ? "deadzone" : "test");
		return;
	}
	if (0 == _stricmp(sub, "state")) {
		advancedfx::Message(
			"[viewfxvm][state] site=%d field=%d mode=%d spin=%.2f applies=%llu "
			"lastVmEnt=%d quatOff=%d callerHook=%d freeAim=(%.2f %.2f)\n",
			g_site.load(), g_field.load(), g_mode.load(), g_spinDegPerApply,
			SceneTransform_ApplyCount(), SceneTransform_LastViewmodelIndex(),
			SceneTransform_QuatOffset(), CalcHook_Installed() ? 1 : 0,
			g_freeAimPitch, g_freeAimYaw);
		return;
	}
	advancedfx::Message(
		"%s viewfx vmtest info - list viewmodel entities + scene-node dump.\n"
		"%s viewfx vmtest site <0-4> - write site.\n"
		"%s viewfx vmtest field <local|quat|both> - scene-node field.\n"
		"%s viewfx vmtest spin <deg> - additive diagnostic yaw.\n"
		"%s viewfx vmtest mode <test|deadzone> - input mode.\n"
		"%s viewfx vmtest state - current configuration.\n",
		cmd, cmd, cmd, cmd, cmd, cmd);
}

} // namespace Filmmaker
