#include "UiLiveReload.h"

#include "PanoramaFindPanel.h"

#include "../../DeathMsg.h" // Panorama main-menu / HUD context accessors
#include "../../WrpConsole.h"

#include "../../../Shared/AfxConsole.h"

#include <string>
#include <vector>

#include <windows.h>

namespace Filmmaker {

namespace {

PanoramaBridge s_bridge;
void* s_contextPanel = nullptr;

// Which Panorama root a script runs against. AUTO keeps the historical policy (menu, then HUD),
// which is right for editing native Settings pages but leaves a HUD-only probe unreachable while
// a demo is loaded: the main-menu tree still exists behind the game, so AUTO always picks it and
// FindChildTraverse can never see CamEditorRoot and friends. HUD forces the in-game root.
enum ContextPref { kCtxAuto = 0, kCtxMenu = 1, kCtxHud = 2 };
ContextPref s_contextPref = kCtxAuto;

std::wstring s_watchPath;
bool s_watching = false;
unsigned long long s_lastWriteTime = 0;
unsigned long long s_lastSize = 0;
int s_runCount = 0;
std::string s_lastError;
std::string s_lastStatus = "never run";

// Poll at most ~8x/second. A stat per frame would be harmless in isolation but this runs inside
// the main-thread pump next to everything else, and nobody edits a file 200 times a second.
double s_nextPollMs = 0.0;

// Set by the console (any thread), consumed by the main-thread pump. Plain bool: it only ever
// goes false->true off-thread and true->false on the main thread, and a missed frame just defers
// the run by one tick.
volatile bool s_runRequested = false;
double NowMs() {
	static LARGE_INTEGER freq = {};
	if (freq.QuadPart == 0) ::QueryPerformanceFrequency(&freq);
	LARGE_INTEGER now = {};
	::QueryPerformanceCounter(&now);
	return freq.QuadPart ? (1000.0 * (double)now.QuadPart / (double)freq.QuadPart) : 0.0;
}

std::wstring Utf8ToWide(const char* s) {
	if (!s || !*s) return std::wstring();
	const int need = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
	if (need <= 0) return std::wstring();
	std::wstring out((size_t)need - 1, L'\0');
	::MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], need);
	return out;
}

std::string WideToUtf8Local(const std::wstring& w) {
	if (w.empty()) return std::string();
	const int need = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
	if (need <= 0) return std::string();
	std::string out((size_t)need - 1, '\0');
	::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &out[0], need, nullptr, nullptr);
	return out;
}

bool StatFile(const std::wstring& path, unsigned long long& outWrite, unsigned long long& outSize) {
	WIN32_FILE_ATTRIBUTE_DATA fad = {};
	if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
		return false;
	outWrite = ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32)
		| (unsigned long long)fad.ftLastWriteTime.dwLowDateTime;
	outSize = ((unsigned long long)fad.nFileSizeHigh << 32) | (unsigned long long)fad.nFileSizeLow;
	return true;
}

bool ReadWholeFile(const std::wstring& path, std::string& out, std::string& err) {
	HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
		// Share everything: the editor writing the file must never be blocked by our read, and a
		// half-written file simply fails to parse and is reported, then re-read on the next change.
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		err = "cannot open file";
		return false;
	}
	LARGE_INTEGER size = {};
	if (!::GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > (4 << 20)) {
		::CloseHandle(h);
		err = "bad size (max 4 MiB)";
		return false;
	}
	out.assign((size_t)size.QuadPart, '\0');
	DWORD got = 0;
	const bool ok = out.empty()
		|| (::ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr) && got == out.size());
	::CloseHandle(h);
	if (!ok) {
		err = "read failed";
		return false;
	}
	// Strip a UTF-8 BOM: V8 rejects it at the start of a script.
	if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB
		&& (unsigned char)out[2] == 0xBF)
		out.erase(0, 3);
	return true;
}

// Match PanoramaBridge's normal context policy: settings and other native menu pages live in the
// main-menu tree even while a map is loaded, while HUD-only tools need the in-game fallback.
// Both roots can be recreated, so resolve the current pointer on every run.
bool EnsureBridge() {
	s_bridge.Init();
	unsigned char* context = nullptr;
	if (s_contextPref == kCtxHud) {
		context = AfxHookSource2_GetPanoramaHudPanel();
	} else if (s_contextPref == kCtxMenu) {
		context = AfxHookSource2_GetPanoramaMainMenuPanel();
	} else {
		context = AfxHookSource2_GetPanoramaMainMenuPanel();
		if (!context) {
			if (unsigned char* hud = AfxHookSource2_GetPanoramaHudPanel())
				context = hud;
		}
	}
	if (!context)
		return false;
	if ((void*)context != s_contextPanel) {
		s_contextPanel = (void*)context;
		s_bridge.SetContextPanel(context);
	}
	return s_bridge.CanRunScript() && s_bridge.ContextPanel() != nullptr;
}

bool RunFileNow(const std::wstring& path, bool announce) {
	std::string js, err;
	if (!ReadWholeFile(path, js, err)) {
		s_lastError = err;
		s_lastStatus = "read failed";
		if (announce) advancedfx::Warning("mirv_filmmaker livejs: %s (%s)\n", err.c_str(), WideToUtf8Local(path).c_str());
		return false;
	}
	if (!EnsureBridge()) {
		s_lastError = "no Panorama menu or HUD context yet";
		s_lastStatus = "no UI context";
		if (announce) advancedfx::Warning("mirv_filmmaker livejs: %s\n", s_lastError.c_str());
		return false;
	}
	// RunScript returns false when V8 refused the script (syntax error). A THROWN error inside the
	// script is caught by the script's own try/catch, exactly like the compiled-in fragments -- so
	// author live scripts with one, or the failure is silent here too.
	const bool ok = s_bridge.RunScript(js.c_str());
	++s_runCount;
	if (ok) {
		s_lastError.clear();
		s_lastStatus = "ok";
		if (announce) advancedfx::Message("mirv_filmmaker livejs: ran %s (%d bytes, run #%d).\n",
			WideToUtf8Local(path).c_str(), (int)js.size(), s_runCount);
	} else {
		s_lastError = "RunScript refused the script (syntax error?)";
		s_lastStatus = "script error";
		advancedfx::Warning("mirv_filmmaker livejs: RunScript FAILED for %s -- syntax error?\n",
			WideToUtf8Local(path).c_str());
	}
	return ok;
}

} // namespace

void UiLive_PumpMainThread() {
	// An explicit load/watch request from the console. This is the ONLY place a live script is
	// ever executed, because this function runs on the main/UI thread and the console does not.
	if (s_runRequested) {
		s_runRequested = false;
		if (!s_watchPath.empty()) {
			RunFileNow(s_watchPath, true);
			StatFile(s_watchPath, s_lastWriteTime, s_lastSize);
		}
		return;
	}
	if (!s_watching || s_watchPath.empty())
		return;
	const double now = NowMs();
	if (now < s_nextPollMs)
		return;
	s_nextPollMs = now + 125.0;

	unsigned long long writeTime = 0, size = 0;
	if (!StatFile(s_watchPath, writeTime, size))
		return; // file temporarily gone (editors write via rename); just wait for it to come back
	if (writeTime == s_lastWriteTime && size == s_lastSize)
		return;
	s_lastWriteTime = writeTime;
	s_lastSize = size;
	RunFileNow(s_watchPath, /*announce*/ true);
}

void UiLive_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* sub = (argc >= 3) ? args->ArgV(2) : "status";

	// Everything after the sub-command is the path, rejoined with spaces, so an unquoted path with
	// spaces still works -- which matters because this repo lives under "Github Projects".
	std::string joined;
	for (int i = 3; i < argc; ++i) {
		if (i > 3) joined += ' ';
		joined += args->ArgV(i);
	}
	// ...but the console tokenises ':' as its own token, so "C:\dir" arrives as "C", ":", "\dir"
	// and the naive rejoin produced "C : \dir", which cannot be opened. Squeeze the spaces back out
	// around every colon: ':' is illegal inside a Windows filename, so it can only be the drive
	// separator (or a stream suffix) and never legitimately sits next to a space in a real path.
	for (size_t pos = joined.find(':'); pos != std::string::npos; pos = joined.find(':', pos + 1)) {
		size_t begin = pos, end = pos + 1;
		while (begin > 0 && joined[begin - 1] == ' ') --begin;
		while (end < joined.size() && joined[end] == ' ') ++end;
		joined.replace(begin, end - begin, ":");
		pos = begin;
	}

	if (0 == _stricmp(sub, "load")) {
		if (joined.empty()) { advancedfx::Warning("usage: %s livejs load <path>\n", cmd); return; }
		s_watchPath = Utf8ToWide(joined.c_str());
		s_runRequested = true;   // the main-thread pump does the RunScript -- see the note above
		advancedfx::Message("%s: livejs queued %s (runs on the next frame).\n", cmd, joined.c_str());
		return;
	}
	if (0 == _stricmp(sub, "watch")) {
		if (joined.empty()) { advancedfx::Warning("usage: %s livejs watch <path>\n", cmd); return; }
		s_watchPath = Utf8ToWide(joined.c_str());
		s_watching = true;
		s_lastWriteTime = 0; s_lastSize = 0; // force the first run
		s_nextPollMs = 0.0;
		s_runRequested = true;   // first run happens on the main thread, like every later one
		advancedfx::Message("%s: livejs watching %s -- it will re-run on every save.\n",
			cmd, joined.c_str());
		return;
	}
	if (0 == _stricmp(sub, "context")) {
		if (0 == _stricmp(joined.c_str(), "hud")) s_contextPref = kCtxHud;
		else if (0 == _stricmp(joined.c_str(), "menu")) s_contextPref = kCtxMenu;
		else if (0 == _stricmp(joined.c_str(), "auto")) s_contextPref = kCtxAuto;
		else if (!joined.empty()) {
			advancedfx::Warning("usage: %s livejs context [auto|menu|hud]\n", cmd);
			return;
		}
		s_contextPanel = nullptr; // force the next run to re-resolve against the new preference
		advancedfx::Message("%s: livejs context = %s\n", cmd,
			s_contextPref == kCtxHud ? "hud" : (s_contextPref == kCtxMenu ? "menu" : "auto"));
		return;
	}
	if (0 == _stricmp(sub, "unwatch")) {
		s_watching = false;
		advancedfx::Message("%s: livejs stopped watching.\n", cmd);
		return;
	}
	if (0 == _stricmp(sub, "status")) {
		advancedfx::Message(
			"%s livejs:\n"
			"  watching : %s\n"
			"  path     : %s\n"
			"  runs     : %d\n"
			"  last     : %s\n"
			"  error    : %s\n",
			cmd,
			s_watching ? "yes" : "no",
			s_watchPath.empty() ? "(none)" : WideToUtf8Local(s_watchPath).c_str(),
			s_runCount,
			s_lastStatus.c_str(),
			s_lastError.empty() ? "(none)" : s_lastError.c_str());
		return;
	}

	advancedfx::Message(
		"%s livejs load <path>    - run a Panorama JS file once, in the current menu/HUD context.\n"
		"%s livejs watch <path>   - run it now and again on every save (125ms poll).\n"
		"%s livejs context [auto|menu|hud] - which Panorama root to run in; AUTO prefers\n"
		"   the main menu, so a HUD-only probe needs 'hud' while a demo is loaded.\n"
		"%s livejs unwatch        - stop watching.\n"
		"%s livejs status         - what is watched, run count, last error.\n"
		"Development tool: the console cannot carry real JS (it splits on ';', mangles quotes and\n"
		"truncates ~250 bytes), so the script is read from disk instead. Anything built this way is\n"
		"ordinary Panorama JS and can be pasted into a *Js.h fragment unchanged.\n",
		cmd, cmd, cmd, cmd, cmd);
}

} // namespace Filmmaker
