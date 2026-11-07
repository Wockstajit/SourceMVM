#include "UiLayout.h"

#include "../Config/FilmmakerSettingsStore.h"

#include "../../../Shared/AfxConsole.h"

#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>

namespace Filmmaker {

namespace {

struct PanelRect { int x = 0, y = 0, w = 0, h = 0; };

std::mutex g_mutex;
PanelRect g_rects[4];
UiPanel g_front = UiPanel::Editor;
// The settings store is main-thread only, and MovieMode asks from the input thread: the choice is
// mirrored here once a frame (UiLayout_MainThreadFrame) and on every change made through this file.
std::atomic<bool> g_modular{ false };

bool Contains(const PanelRect& r, int x, int y) {
	return r.w > 0 && r.h > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

}  // namespace

bool UiLayout_Modular() { return g_modular.load(std::memory_order_relaxed); }

void UiLayout_SetModular(bool modular) {
	FilmmakerSettings_SetUi("layout", modular ? "modular" : "workspace");
	g_modular.store(modular, std::memory_order_relaxed);
}

void UiLayout_MainThreadFrame() {
	g_modular.store(FilmmakerSettings_UiString("layout", "workspace") == "modular", std::memory_order_relaxed);
}

void UiLayout_SetPanelRect(UiPanel panel, int x, int y, int w, int h) {
	const int i = (int)panel;
	if (i <= 0 || i >= 4) return;
	std::lock_guard<std::mutex> lk(g_mutex);
	g_rects[i] = PanelRect{ x, y, w, h };
}

void UiLayout_SetPanelRectString(UiPanel panel, const char* xywh) {
	int x = 0, y = 0, w = 0, h = 0;
	if (!xywh || 4 != std::sscanf(xywh, "%d %d %d %d", &x, &y, &w, &h)) w = h = 0;
	UiLayout_SetPanelRect(panel, x, y, w, h);
}

void UiLayout_SetFrontWindow(UiPanel panel) {
	if (panel != UiPanel::Config && panel != UiPanel::Editor) return;
	std::lock_guard<std::mutex> lk(g_mutex);
	g_front = panel;
}

UiPanel UiLayout_PanelAt(int x, int y) {
	if (x < 0 || y < 0) return UiPanel::None;
	std::lock_guard<std::mutex> lk(g_mutex);
	if (Contains(g_rects[(int)UiPanel::Header], x, y)) return UiPanel::Header;
	const UiPanel back = (g_front == UiPanel::Config) ? UiPanel::Editor : UiPanel::Config;
	if (Contains(g_rects[(int)g_front], x, y)) return g_front;
	if (Contains(g_rects[(int)back], x, y)) return back;
	return UiPanel::None;
}

void UiLayout_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* arg = (argc >= 3) ? args->ArgV(2) : nullptr;
	if (!arg) {
		advancedfx::Message("%s layout is \"%s\".\nusage: %s layout [workspace|modular|toggle]\n",
			cmd, UiLayout_Modular() ? "modular" : "workspace", cmd);
		return;
	}
	bool modular = UiLayout_Modular();
	if (0 == _stricmp(arg, "modular") || 0 == _stricmp(arg, "windows")) modular = true;
	else if (0 == _stricmp(arg, "workspace") || 0 == _stricmp(arg, "docked")) modular = false;
	else if (0 == _stricmp(arg, "toggle")) modular = !modular;
	else {
		advancedfx::Warning("%s layout: unknown \"%s\" (workspace|modular|toggle).\n", cmd, arg);
		return;
	}
	UiLayout_SetModular(modular);
	advancedfx::Message("mirv_filmmaker: layout = %s.\n", modular ? "modular" : "workspace");
}

}  // namespace Filmmaker
