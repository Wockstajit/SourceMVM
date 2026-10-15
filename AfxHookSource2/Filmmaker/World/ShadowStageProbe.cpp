#include "ShadowStageProbe.h"

#include "../Debug/MvmDebugLog.h"
#include "../../../Shared/AfxConsole.h"
#include "../../../Shared/binutils.h"
#include "../../../Deps/release/Detours/src/detours.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

using namespace Afx::BinUtils;

namespace Filmmaker {
namespace {

// Recovered on scenesystem.dll ClientVersion 2000880 (module dated 2026-07-29).
// Durable re-derive: find "CopySSTDepthToCSM_PS", walk to enclosing function whose
// prologue matches this shape. See Docs/effects/shadow-separation.md and
// Automation/tools/re_sst_csm_boundary.py.
constexpr const char* kCopySstBuilderPattern =
	"48 89 5c 24 08 4c 89 4c 24 20 55 56 57 41 54 41 55 41 56 41 57 "
	"48 8d 6c 24 80 48 81 ec 80 01 00 00";

// (__fastcall)(this, a2, a3, a4...) — virtual method; returns a node pointer or null.
typedef void* (__fastcall* CopySstBuilder_t)(void* self, void* a2, void* a3, void* a4);
CopySstBuilder_t g_original = nullptr;

std::atomic<bool> g_logging{ false };
std::atomic<bool> g_suppress{ false };
std::atomic<unsigned long long> g_calls{ 0 };
std::atomic<unsigned long long> g_suppressed{ 0 };
std::string g_unavailableReason;
bool g_installed = false;

void* __fastcall New_CopySstBuilder(void* self, void* a2, void* a3, void* a4) {
	const unsigned long long n = ++g_calls;
	const bool suppress = g_suppress.load(std::memory_order_relaxed);
	if (g_logging.load(std::memory_order_relaxed) && MvmDebugLog_Active()) {
		static unsigned s_throttle = 0;
		if ((s_throttle++ % 30) == 0) {
			unsigned self50 = 0, self54 = 0;
			void* self10 = nullptr;
			__try {
				if (self) {
					self50 = *(unsigned*)((unsigned char*)self + 0x50);
					self54 = *(unsigned*)((unsigned char*)self + 0x54);
					self10 = *(void**)((unsigned char*)self + 0x10);
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				self50 = self54 = 0xFFFFFFFFu;
				self10 = nullptr;
			}
			MvmDebugLog_Linef("world.shadowstage",
				"CopySSTDepthToCSM call=%llu suppress=%d self=%p a2=%p a3=%p a4=%p "
				"self+0x50=%u self+0x54=%u self+0x10=%p orig=%p",
				n, suppress ? 1 : 0, self, a2, a3, a4,
				self50, self54, self10, (void*)g_original);
		}
	}
	if (suppress) {
		++g_suppressed;
		if (MvmDebugLog_Active()) {
			static unsigned s_suppressThrottle = 0;
			if ((s_suppressThrottle++ % 60) == 0) {
				MvmDebugLog_LinefAlways("world.shadowstage",
					"SUPPRESS CopySSTDepthToCSM call=%llu totalSuppressed=%llu",
					n, g_suppressed.load());
			}
		}
		return nullptr;
	}
	return g_original(self, a2, a3, a4);
}

} // namespace

void ShadowStageProbe_Install(void* sceneSystemDll) {
	if (g_installed) return;
	if (!sceneSystemDll) {
		g_unavailableReason = "scenesystem.dll handle was null.";
		return;
	}

	MemRange textRange = MemRange(0, 0);
	{
		ImageSectionsReader reader((HMODULE)sceneSystemDll);
		if (!reader.Eof()) textRange = reader.GetMemRange();
	}
	if (textRange.IsEmpty()) {
		g_unavailableReason = "could not read scenesystem.dll sections.";
		return;
	}

	MemRange found = FindPatternString(textRange, kCopySstBuilderPattern);
	if (found.IsEmpty()) {
		g_unavailableReason =
			"CopySSTDepthToCSM builder pattern not found (CS2 updated?). "
			"Re-derive via Automation/tools/re_sst_csm_boundary.py.";
		MvmDebugLog_LinefAlways("world.shadowstage", "install FAILED: %s",
			g_unavailableReason.c_str());
		return;
	}

	g_original = (CopySstBuilder_t)found.Start;
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)g_original, New_CopySstBuilder);
	if (NO_ERROR != DetourTransactionCommit()) {
		g_original = nullptr;
		g_unavailableReason = "DetourTransactionCommit failed for CopySSTDepthToCSM.";
		MvmDebugLog_LinefAlways("world.shadowstage", "install FAILED: %s",
			g_unavailableReason.c_str());
		return;
	}

	g_installed = true;
	MvmDebugLog_LinefAlways("world.shadowstage",
		"installed CopySSTDepthToCSM builder at 0x%p (log=%d suppress=%d)",
		(void*)found.Start,
		g_logging.load() ? 1 : 0,
		g_suppress.load() ? 1 : 0);
}

bool ShadowStageProbe_Available(std::string* outReason) {
	if (g_installed) return true;
	if (outReason) {
		*outReason = g_unavailableReason.empty()
			? std::string("CopySSTDepthToCSM probe not installed yet.")
			: g_unavailableReason;
	}
	return false;
}

void ShadowStageProbe_SetLogging(bool enabled) {
	g_logging.store(enabled, std::memory_order_relaxed);
	MvmDebugLog_LinefAlways("world.shadowstage", "logging=%d", enabled ? 1 : 0);
}

bool ShadowStageProbe_Logging() {
	return g_logging.load(std::memory_order_relaxed);
}

void ShadowStageProbe_SetSuppressCopy(bool enabled) {
	g_suppress.store(enabled, std::memory_order_relaxed);
	MvmDebugLog_LinefAlways("world.shadowstage",
		"suppressCopy=%d installed=%d calls=%llu suppressed=%llu",
		enabled ? 1 : 0, g_installed ? 1 : 0,
		g_calls.load(), g_suppressed.load());
}

bool ShadowStageProbe_SuppressCopy() {
	return g_suppress.load(std::memory_order_relaxed);
}

void ShadowStageProbe_AppendStateJson(std::ostringstream& o) {
	std::string reason;
	o << ",\"shadowStageOk\":" << (ShadowStageProbe_Available(&reason) ? "true" : "false");
	o << ",\"shadowStageLogging\":" << (ShadowStageProbe_Logging() ? "true" : "false");
	o << ",\"shadowStageSuppress\":" << (ShadowStageProbe_SuppressCopy() ? "true" : "false");
	o << ",\"shadowStageCalls\":" << g_calls.load();
	o << ",\"shadowStageSuppressed\":" << g_suppressed.load();
}

void ShadowStageProbe_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* op = argc >= 4 ? args->ArgV(3) : "";
	if (0 == _stricmp(op, "log") || 0 == _stricmp(op, "logging")) {
		const char* v = argc >= 5 ? args->ArgV(4) : "";
		if (0 == _stricmp(v, "on") || 0 == _stricmp(v, "1")) ShadowStageProbe_SetLogging(true);
		else if (0 == _stricmp(v, "off") || 0 == _stricmp(v, "0")) ShadowStageProbe_SetLogging(false);
		else if (0 == _stricmp(v, "toggle")) ShadowStageProbe_SetLogging(!ShadowStageProbe_Logging());
		else {
			advancedfx::Message("shadowstage logging is %s.\n",
				ShadowStageProbe_Logging() ? "ON" : "off");
			return;
		}
		advancedfx::Message("shadowstage logging %s.\n",
			ShadowStageProbe_Logging() ? "ON" : "off");
		return;
	}
	if (0 == _stricmp(op, "suppress") || 0 == _stricmp(op, "copy")) {
		const char* v = argc >= 5 ? args->ArgV(4) : "";
		// suppress on|off  OR  copy off  (copy off == suppress the SST->CSM copy)
		bool enable = false;
		bool handled = true;
		if (0 == _stricmp(op, "copy")) {
			if (0 == _stricmp(v, "off") || 0 == _stricmp(v, "0") || 0 == _stricmp(v, "suppress"))
				enable = true;
			else if (0 == _stricmp(v, "on") || 0 == _stricmp(v, "1") || 0 == _stricmp(v, "allow"))
				enable = false;
			else handled = false;
		} else {
			if (0 == _stricmp(v, "on") || 0 == _stricmp(v, "1")) enable = true;
			else if (0 == _stricmp(v, "off") || 0 == _stricmp(v, "0")) enable = false;
			else if (0 == _stricmp(v, "toggle")) enable = !ShadowStageProbe_SuppressCopy();
			else handled = false;
		}
		if (!handled) {
			advancedfx::Message(
				"Usage: %s world shadowstage suppress on|off|toggle\n"
				"       %s world shadowstage copy off|on\n",
				cmd, cmd);
			return;
		}
		ShadowStageProbe_SetSuppressCopy(enable);
		advancedfx::Message("shadowstage SST->CSM copy %s%s.\n",
			enable ? "SUPPRESSED" : "allowed",
			g_installed ? "" : " (hook not installed)");
		return;
	}
	if (0 == _stricmp(op, "status") || 0 == _stricmp(op, "probe")) {
		std::string reason;
		advancedfx::Message(
			"shadowstage: ok=%s logging=%s suppress=%s calls=%llu suppressed=%llu\n"
			"  reason=%s\n",
			ShadowStageProbe_Available(&reason) ? "yes" : "no",
			ShadowStageProbe_Logging() ? "on" : "off",
			ShadowStageProbe_SuppressCopy() ? "on" : "off",
			g_calls.load(), g_suppressed.load(),
			reason.empty() ? "-" : reason.c_str());
		return;
	}

	std::string reason;
	advancedfx::Message(
		"mirv_filmmaker world shadowstage - SST->CSM copy boundary probe (research).\n"
		"Usage:\n"
		"  %s world shadowstage status\n"
		"  %s world shadowstage logging on|off|toggle\n"
		"  %s world shadowstage suppress on|off|toggle\n"
		"  %s world shadowstage copy off|on\n"
		"Current: ok=%s logging=%s suppress=%s calls=%llu.\n"
		"Start mvm_debug before A/B; read world.shadowstage lines.\n",
		cmd, cmd, cmd, cmd,
		ShadowStageProbe_Available(&reason) ? "yes" : reason.c_str(),
		ShadowStageProbe_Logging() ? "on" : "off",
		ShadowStageProbe_SuppressCopy() ? "on" : "off",
		g_calls.load());
}

} // namespace Filmmaker
