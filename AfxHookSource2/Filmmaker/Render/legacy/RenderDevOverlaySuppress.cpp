#include "RenderDevOverlaySuppress.h"

#include "../../Debug/MvmDebugLog.h" // MvmDebugLog_* probes
#include "../../../../Shared/binutils.h"
#include "../../../../Shared/AfxConsole.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../../../Deps/release/Detours/src/detours.h"

#include <atomic>

namespace Filmmaker {

namespace {

// client.dll+0x116fe10 (build of 2026-08-04): outlined-overlay-text helper used by the ToolsVis
// watermark. Full 8-arg forward so the x64 stack args survive the trampoline.
typedef void(__fastcall* DrawOverlayText_t)(unsigned long long a, unsigned long long b,
	unsigned long long c, unsigned long long d, unsigned long long e, unsigned long long f,
	unsigned long long g, unsigned long long h);

// Prologue AOB (FindPatternString: two hex chars per byte, "??" wildcards).
const char* const kDrawOverlayTextPattern =
	"48 89 5C 24 08 48 89 6C 24 18 56 57 41 56 48 83 EC 60 44 8B B4 24 A0 00 00 00 41 8B F1 "
	"0F 29 74 24 50 49 8B E8 0F 29 7C 24 40 48 8B FA F3 0F 10";

// NOTE (2026-08-07): an earlier revision also detoured the render-attribute getter at
// client.dll+0x166e00, forcing the five int attributes the ToolsVis builder reads to 0.
// That was REMOVED: it never affected the reference chart (its actual mechanism is the
// baked debug_chart_color lightmap channel, see the header), and forcing the three
// unidentified attributes broke the vis pass for real -- characters and the viewmodel
// rendered pitch black in the lighting AOVs (probe/dynamic-object lighting disabled).
// The text no-op below is the only suppression that is both needed and proven safe.

DrawOverlayText_t g_origDrawOverlayText = nullptr;
bool g_installed = false;
std::atomic<bool> g_armed(false);
std::atomic<unsigned long long> g_textSkips(0);

size_t FindClientPattern(const char* pattern) {
	HMODULE client = GetModuleHandleA("client.dll");
	if (!client)
		return 0;
	Afx::BinUtils::ImageSectionsReader sections(client);
	if (sections.Eof())
		return 0;
	Afx::BinUtils::MemRange result = Afx::BinUtils::FindPatternString(sections.GetMemRange(), pattern);
	return result.IsEmpty() ? 0 : result.Start;
}

void __fastcall Hook_DrawOverlayText(unsigned long long a, unsigned long long b,
	unsigned long long c, unsigned long long d, unsigned long long e, unsigned long long f,
	unsigned long long g, unsigned long long h) {
	if (g_armed.load(std::memory_order_relaxed)) {
		const unsigned long long n = g_textSkips.fetch_add(1, std::memory_order_relaxed) + 1;
		if (MvmDebugLog_Active() && n <= 16)
			MvmDebugLog_LinefAlways("render.devoverlay", "watermark text draw SKIPPED (n=%llu)", n);
		return;
	}
	g_origDrawOverlayText(a, b, c, d, e, f, g, h);
}

} // namespace

bool RenderDevOverlaySuppress_EnsureInstalled() {
	if (g_installed)
		return true;
	const size_t textFn = FindClientPattern(kDrawOverlayTextPattern);
	if (!textFn) {
		advancedfx::Warning("render devoverlay: pattern resolve failed; ToolsVis watermark "
			"suppression INACTIVE -- a CS2 update likely moved client.dll. Debug-view jobs still "
			"render, with the watermark burned in.\n");
		return false;
	}
	g_origDrawOverlayText = (DrawOverlayText_t)textFn;
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)g_origDrawOverlayText, Hook_DrawOverlayText);
	g_installed = (NO_ERROR == DetourTransactionCommit());
	if (g_installed)
		advancedfx::Message("render devoverlay: ToolsVis watermark suppression installed.\n");
	else
		advancedfx::Warning("render devoverlay: DetourTransactionCommit failed; suppression INACTIVE.\n");
	return g_installed;
}

bool RenderDevOverlaySuppress_Installed() { return g_installed; }

void RenderDevOverlaySuppress_Arm(bool armed) {
	if (armed && !RenderDevOverlaySuppress_EnsureInstalled())
		return;
	g_armed.store(armed, std::memory_order_relaxed);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.devoverlay", "armed=%d installed=%d", armed ? 1 : 0,
			g_installed ? 1 : 0);
}

bool RenderDevOverlaySuppress_Armed() { return g_armed.load(std::memory_order_relaxed); }

} // namespace Filmmaker
