#include "DeadlockUi.h"

#include "DeadlockAddresses.h"

#include "../DeathMsg.h" // AfxHookSource2_GetPanorama* accessors
#include "../../Shared/AfxConsole.h"

#include <windows.h>
#include <cstdint>
#include <string>

namespace Deadlock {

namespace {

bool g_wants[(int)UiPanel::Count] = {};
const char* const g_names[(int)UiPanel::Count] = {
	"cameraeditor", "cameratimeline", "grapheditor", "markermenu", "config", "render",
};

bool g_inputDenied = false;

// Whether `UiToolkitAPI` exists in the context this hook runs scripts in. PROBED, once, from
// inside that context -- it is a fact about Deadlock, not something to assume in either direction.
enum class ToolkitState { Unknown, Present, Absent };
ToolkitState g_toolkit = ToolkitState::Unknown;

bool RunScriptGuarded(const char* js) {
	typedef void(__fastcall* RunScript_t)(void* engine, void* ctx, const char* src,
	                                      const char* originFile, uint64_t line);
	unsigned char** engine = AfxHookSource2_GetPanoramaUiEngine();
	unsigned char* panel = AfxHookSource2_GetPanoramaHudPanel();
	void* run = AfxHookSource2_GetPanoramaRunScript();
	if (!engine || !*engine || !panel || !run) return false;
	static uint64_t line = 0;
	__try {
		((RunScript_t)run)(*engine, panel, js, "deadlock-ui", ++line);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Ask the Panorama context whether the toolkit is there, and have it say so out loud.
//
// RunScript has no return value, so a question put to that context can only be answered the way
// every panel answers one: a marked line on the console, picked up by the tier0 listener in
// DeadlockUiCommandBridge.cpp and dispatched back here as `ui toolkit 0|1`. The marker opens with
// STX because no console font draws it, so the answer does not also become visible noise.
void ProbeToolkit() {
	if (g_toolkit != ToolkitState::Unknown) return;
	// Marked as in flight before the reply arrives, so a probe cannot be issued once per frame
	// while the answer is on its way back.
	g_toolkit = ToolkitState::Absent;
	// NOTE THE DOUBLED BACKSLASH. This is an ordinary C++ string, not one of the raw literals the
	// *Js.h fragments use, so C++ eats the escapes first: a plain "\n" here becomes a real newline
	// INSIDE the JavaScript string literal, which is a syntax error and takes the whole script
	// with it ("Uncaught SyntaxError: Invalid or unexpected token"). The marker's \x02 is meant to
	// be a real byte and stays single.
	RunScriptGuarded(
		"(function(){try{"
		"var ok = (typeof UiToolkitAPI !== 'undefined' && UiToolkitAPI"
		" && UiToolkitAPI.AddDenyInputFlagsToGame) ? 1 : 0;"
		"$.Msg('\x02MVMCMD mirv_filmmaker ui toolkit ' + ok + '\\n');"
		"}catch(e){}})();");
}

} // namespace

void Ui_SetPanelWantsCursor(UiPanel panel, bool wants) {
	int i = (int)panel;
	if (i < 0 || i >= (int)UiPanel::Count) return;
	if (g_wants[i] == wants) return;
	g_wants[i] = wants;

	// The engine-side half of the same decision. Doing it here, once per transition, is what keeps
	// "sliders work but buttons do not" from coming back: the two halves cannot drift apart if one
	// call site owns both.
	Ui_SetGameInputDenied(Ui_AnyPanelWantsCursor());
}

bool Ui_PanelWantsCursor(UiPanel panel) {
	int i = (int)panel;
	if (i < 0 || i >= (int)UiPanel::Count) return false;
	return g_wants[i];
}

bool Ui_AnyPanelWantsCursor() {
	for (int i = 0; i < (int)UiPanel::Count; ++i)
		if (g_wants[i]) return true;
	return false;
}

bool Ui_ConsoleVisible() {
	// No Deadlock console-visibility accessor has been derived. Calling CS2's vtable slot here
	// raised an engine access violation even behind SEH, so remain neutral until there is a
	// verified Deadlock signal.
	return false;
}

void Ui_SetToolkitAvailable(bool available) {
	const ToolkitState was = g_toolkit;
	g_toolkit = available ? ToolkitState::Present : ToolkitState::Absent;
	if (!available && was == ToolkitState::Unknown) {
		advancedfx::Message(
			"AfxHookDeadlock: no UiToolkitAPI in this Panorama context, so there is no ShareMouse\n"
			"flag to set. Panels take clicks through Deadlock's own replay cursor instead.\n");
	}
}

void Ui_SetGameInputDenied(bool denied) {
	if (g_inputDenied == denied) return;

	// THIS USED TO LIE. It ran an AddDenyInputFlagsToGame call wrapped in a try, took RunScript
	// returning true as success, and recorded the flag as set. But `UiToolkitAPI` does not exist in
	// Deadlock's HUD context at all -- measured on build 6687 by enumerating the global object --
	// so the call threw inside the try, nothing changed, and `health` reported the mouse as shared
	// when it was not. A status line that is wrong is worse than a missing one: it sends the next
	// investigation to the wrong file.
	//
	// So the API is PROBED, and the flag only flips when something actually happened. Where the
	// toolkit is absent, clicks arrive through Deadlock's own replay cursor, which a replay shows
	// by default -- the game is a MOBA and expects a pointer on screen.
	ProbeToolkit();
	if (g_toolkit != ToolkitState::Present) return;

	const char* js = denied
		? "(function(){try{UiToolkitAPI.AddDenyInputFlagsToGame('MVM','ShareMouse');}catch(e){}})();"
		: "(function(){try{UiToolkitAPI.RemoveDenyInputFlagsFromGame('MVM');}catch(e){}})();";

	if (RunScriptGuarded(js))
		g_inputDenied = denied;
}

bool Ui_GameInputDenied() { return g_inputDenied; }

void Ui_Report() {
	std::string claiming;
	for (int i = 0; i < (int)UiPanel::Count; ++i) {
		if (!g_wants[i]) continue;
		if (!claiming.empty()) claiming += ", ";
		claiming += g_names[i];
	}
	const char* toolkit =
		(g_toolkit == ToolkitState::Present) ? "present"
		: (g_toolkit == ToolkitState::Absent) ? "absent (no ShareMouse flag to set)"
		: "not probed yet";
	advancedfx::Message(
		"  ui:        console=%s  uiToolkitAPI=%s\n"
		"             gameInputDenied=%s  cursorClaimedBy=%s\n",
		Ui_ConsoleVisible() ? "visible" : "hidden", toolkit,
		g_inputDenied ? "yes" : "no",
		claiming.empty() ? "nobody" : claiming.c_str());
}

} // namespace Deadlock
