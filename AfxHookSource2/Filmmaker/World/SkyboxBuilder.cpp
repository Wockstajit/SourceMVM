#include "SkyboxBuilder.h"

#include "SkyboxLibrary.h"

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_Linef
#include "../../../Shared/AfxConsole.h"

#include <Windows.h>
#include <commdlg.h>
#include <shlwapi.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "comdlg32.lib")

namespace Filmmaker {

namespace {

std::mutex g_mx;
SkyBuildState g_state = SkyBuildState::Idle;
std::string g_status;
std::atomic<bool> g_busy{ false };
std::thread g_worker;

// Set by the worker when a build succeeds; consumed on the main thread, because applying a
// skybox runs a console command and that is main-thread work.
std::mutex g_pendingMx;
std::string g_pendingApply;

void SetState(SkyBuildState s, const std::string& text) {
	std::lock_guard<std::mutex> lock(g_mx);
	g_state = s;
	g_status = text;
}

std::wstring Widen(const std::string& s) {
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring out(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
	return out;
}

std::string Narrow(const std::wstring& s) {
	if (s.empty()) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
	std::string out(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
	return out;
}

bool FileExistsW(const std::wstring& p) {
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Resolves THIS DLL's directory from the address of a local function -- same approach as
// DemoRepair.cpp. There is no global module handle to import, and taking one from an address
// keeps the lookup correct however the DLL was loaded.
std::wstring DllDir() {
	HMODULE hm = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCWSTR)&FileExistsW, &hm) || !hm) {
		return std::wstring();
	}
	wchar_t buf[MAX_PATH] = { 0 };
	if (!GetModuleFileNameW(hm, buf, MAX_PATH)) return std::wstring();
	std::wstring p(buf);
	size_t slash = p.find_last_of(L"\\/");
	return (slash == std::wstring::npos) ? std::wstring() : p.substr(0, slash);
}

// The script lives in the REPO, while the DLL runs from build/staging-release/bin/x64. Walk up
// looking for it rather than hard-coding a depth, so a differently-staged build still finds it.
std::wstring FindScript() {
	std::wstring dir = DllDir();
	for (int up = 0; up < 8 && !dir.empty(); ++up) {
		std::wstring cand = dir + L"\\FX\\tools\\build-skybox.ps1";
		if (FileExistsW(cand)) return cand;
		size_t slash = dir.find_last_of(L"\\/");
		if (slash == std::wstring::npos) break;
		dir = dir.substr(0, slash);
	}
	return std::wstring();
}

// pwsh (PowerShell 7) first, then the in-box Windows PowerShell. Both run the script fine.
std::wstring FindPowerShell() {
	const wchar_t* candidates[] = {
		L"pwsh.exe",
		L"powershell.exe",
	};
	for (const wchar_t* c : candidates) {
		wchar_t found[MAX_PATH] = { 0 };
		// SearchPathW resolves against PATH, which is how both of these are normally reachable.
		if (SearchPathW(nullptr, c, nullptr, MAX_PATH, found, nullptr)) return std::wstring(found);
	}
	return std::wstring();
}

int RunHidden(const std::wstring& exe, const std::wstring& cmdLine) {
	std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
	buf.push_back(L'\0');

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi{};
	if (!CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
		return -1;
	}
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return (int)code;
}

// Strips anything that could break out of the quoted -Name argument. The name also has to survive
// becoming a filename, and the script sanitises it again on its side.
std::string SanitiseName(const std::string& in) {
	std::string out;
	for (char c : in) {
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
			out += (char)tolower((unsigned char)c);
		else if (c == ' ' || c == '-')
			out += '_';
	}
	while (!out.empty() && out.front() == '_') out.erase(out.begin());
	while (!out.empty() && out.back() == '_') out.pop_back();
	return out;
}

std::string BaseName(const std::string& path) {
	size_t slash = path.find_last_of("\\/");
	std::string leaf = (slash == std::string::npos) ? path : path.substr(slash + 1);
	size_t dot = leaf.find_last_of('.');
	return (dot == std::string::npos) ? leaf : leaf.substr(0, dot);
}

void JoinWorker() {
	if (g_worker.joinable()) g_worker.join();
}

// The whole build, on a worker thread. `imagePath` empty means "ask first".
void BuildThread(std::string imagePath, std::string name, double rotation, bool flip, bool pick) {
	if (pick) {
		SetState(SkyBuildState::Picking, "Choose an image or HDRI...");

		wchar_t file[MAX_PATH * 4] = { 0 };
		OPENFILENAMEW ofn{};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = nullptr;
		ofn.lpstrFilter =
			L"Sky images (*.hdr;*.exr;*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.pfm)\0"
			L"*.hdr;*.exr;*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.pfm\0"
			L"All files (*.*)\0*.*\0\0";
		ofn.lpstrFile = file;
		ofn.nMaxFile = MAX_PATH * 4;
		ofn.lpstrTitle = L"Pick a panorama for your skybox (2:1 equirectangular works best)";
		ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;

		if (!GetOpenFileNameW(&ofn)) {
			// Cancel is not a failure; say nothing alarming.
			SetState(SkyBuildState::Idle, "");
			g_busy.store(false);
			return;
		}
		imagePath = Narrow(std::wstring(file));
		if (name.empty()) name = BaseName(imagePath);
	}

	std::string safe = SanitiseName(name.empty() ? BaseName(imagePath) : name);
	if (safe.empty()) safe = "custom_sky";

	const std::wstring script = FindScript();
	const std::wstring shell = FindPowerShell();
	if (script.empty() || shell.empty()) {
		SetState(SkyBuildState::Failed, "Skybox build tooling not found (FX/tools/build-skybox.ps1 or PowerShell).");
		g_busy.store(false);
		return;
	}

	SetState(SkyBuildState::Building, "Building skybox '" + safe + "' - this takes a few seconds...");
	MvmDebugLog_Linef("world.skybuild", "start name=%s rot=%.1f flip=%d img=%s",
		safe.c_str(), rotation, flip ? 1 : 0, imagePath.c_str());

	wchar_t rot[64];
	_snwprintf_s(rot, _countof(rot), _TRUNCATE, L"%.2f", rotation);

	std::wstring cmd = L"\"" + shell + L"\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \""
		+ script + L"\" -Image \"" + Widen(imagePath) + L"\" -Name \"" + Widen(safe) + L"\""
		+ L" -Rotation " + rot;
	if (flip) cmd += L" -Flip";

	const int code = RunHidden(shell, cmd);
	MvmDebugLog_Linef("world.skybuild", "finished name=%s exit=%d", safe.c_str(), code);

	if (0 != code) {
		SetState(SkyBuildState::Failed,
			"Skybox build failed (exit " + std::to_string(code) + "). See mvm_debug world.skybuild.");
		g_busy.store(false);
		return;
	}

	SetState(SkyBuildState::Done, "Skybox '" + safe + "' ready.");
	{
		// Applying runs a console command, so hand it to the main thread rather than doing it here.
		std::lock_guard<std::mutex> lock(g_pendingMx);
		g_pendingApply = safe;
	}
	g_busy.store(false);
}

void Start(const std::string& imagePath, const std::string& name,
           double rotation, bool flip, bool pick) {
	bool expected = false;
	if (!g_busy.compare_exchange_strong(expected, true)) {
		advancedfx::Message("A skybox build is already running.\n");
		return;
	}
	JoinWorker();
	g_worker = std::thread(BuildThread, imagePath, name, rotation, flip, pick);
}

} // namespace

void SkyboxBuilder_PickAndImport() { Start("", "", 0.0, false, true); }

void SkyboxBuilder_Import(const std::string& imagePath, const std::string& name,
                          double rotation, bool flip) {
	Start(imagePath, name, rotation, flip, false);
}

bool SkyboxBuilder_Rebuild(const std::string& name, double rotation, bool flip,
                           std::string* outReason) {
	const std::vector<SkyboxEntry>& entries = SkyboxLibrary_Entries();
	for (const SkyboxEntry& e : entries) {
		if (0 != _stricmp(e.name.c_str(), name.c_str())) continue;
		if (e.builtin) {
			if (outReason) *outReason = "CS2's own skies cannot be rotated - there is no source image to re-project. Import your own to rotate it.";
			return false;
		}
		if (e.sourcePath.empty()) {
			if (outReason) *outReason = "This skybox has no recorded source image (built by an older version of the tool). Re-import it once and rotation will work.";
			return false;
		}
		if (!FileExistsW(Widen(e.sourcePath))) {
			if (outReason) *outReason = "Source image is gone: " + e.sourcePath;
			return false;
		}
		SkyboxBuilder_Import(e.sourcePath, e.name, rotation, flip);
		return true;
	}
	if (outReason) *outReason = "No skybox named \"" + name + "\".";
	return false;
}

SkyBuildState SkyboxBuilder_State() { std::lock_guard<std::mutex> lock(g_mx); return g_state; }
std::string   SkyboxBuilder_Status() { std::lock_guard<std::mutex> lock(g_mx); return g_status; }
bool          SkyboxBuilder_Busy()  { return g_busy.load(); }

bool SkyboxBuilder_Available(std::string* outReason) {
	if (FindScript().empty()) {
		if (outReason) *outReason = "FX/tools/build-skybox.ps1 not found next to the build.";
		return false;
	}
	if (FindPowerShell().empty()) {
		if (outReason) *outReason = "Neither pwsh.exe nor powershell.exe is on PATH.";
		return false;
	}
	return true;
}

// Main thread. Applies a just-finished build.
void SkyboxBuilder_RunFrame() {
	std::string toApply;
	{
		std::lock_guard<std::mutex> lock(g_pendingMx);
		if (g_pendingApply.empty()) return;
		toApply.swap(g_pendingApply);
	}
	// The index on disk has a new entry (or a changed one), so re-read before applying.
	SkyboxLibrary_Refresh();
	SkyboxLibrary_Apply(toApply.c_str());
}

void SkyboxBuilder_AppendStateJson(std::ostringstream& o) {
	std::string status;
	SkyBuildState state;
	{
		std::lock_guard<std::mutex> lock(g_mx);
		status = g_status;
		state = g_state;
	}
	const char* stateName = "idle";
	switch (state) {
	case SkyBuildState::Picking:  stateName = "picking"; break;
	case SkyBuildState::Building: stateName = "building"; break;
	case SkyBuildState::Done:     stateName = "done"; break;
	case SkyBuildState::Failed:   stateName = "failed"; break;
	default: break;
	}
	o << ",\"skyBuildState\":\"" << stateName << "\"";
	o << ",\"skyBuildBusy\":" << (g_busy.load() ? "true" : "false");
	o << ",\"skyBuildCan\":" << (SkyboxBuilder_Available(nullptr) ? "true" : "false");

	o << ",\"skyBuildMsg\":\"";
	for (char c : status) {
		if (c == '\"' || c == '\\') o << '\\';
		if (c == '\n' || c == '\r') { o << ' '; continue; }
		o << c;
	}
	o << "\"";
}

void SkyboxBuilder_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	// argv: mirv_filmmaker world skybuild <op> ...
	const char* op = (argc >= 4) ? args->ArgV(3) : "";

	if (0 == _stricmp(op, "pick")) {
		std::string reason;
		if (!SkyboxBuilder_Available(&reason)) { advancedfx::Message("%s\n", reason.c_str()); return; }
		SkyboxBuilder_PickAndImport();
		advancedfx::Message("Opening the file picker...\n");
		return;
	}

	if (0 == _stricmp(op, "import")) {
		if (argc < 5) { advancedfx::Message("Usage: %s world skybuild import <imagePath> [name] [rotation] [flip]\n", cmd); return; }
		const std::string img = args->ArgV(4);
		const std::string name = (argc >= 6) ? args->ArgV(5) : "";
		const double rot = (argc >= 7) ? atof(args->ArgV(6)) : 0.0;
		const bool flip = (argc >= 8) && (0 != atoi(args->ArgV(7)));
		SkyboxBuilder_Import(img, name, rot, flip);
		advancedfx::Message("Building...\n");
		return;
	}

	if (0 == _stricmp(op, "rebuild")) {
		if (argc < 5) { advancedfx::Message("Usage: %s world skybuild rebuild <name> [rotation] [flip]\n", cmd); return; }
		const std::string name = args->ArgV(4);
		const double rot = (argc >= 6) ? atof(args->ArgV(5)) : 0.0;
		const bool flip = (argc >= 7) && (0 != atoi(args->ArgV(6)));
		std::string reason;
		if (!SkyboxBuilder_Rebuild(name, rot, flip, &reason)) {
			advancedfx::Message("%s\n", reason.c_str());
			return;
		}
		advancedfx::Message("Rebuilding \"%s\" at %.1f deg%s...\n", name.c_str(), rot, flip ? ", mirrored" : "");
		return;
	}

	if (0 == _stricmp(op, "status")) {
		advancedfx::Message("Skybox build: %s\n", SkyboxBuilder_Status().empty()
			? "idle" : SkyboxBuilder_Status().c_str());
		return;
	}

	advancedfx::Message(
		"mirv_filmmaker world skybuild - make a skybox out of an image, from inside the game.\n"
		"Usage:\n"
		"  %s world skybuild pick                        - file picker, then import + apply\n"
		"  %s world skybuild import <img> [name] [rot] [flip]\n"
		"  %s world skybuild rebuild <name> [rot] [flip] - re-bake an existing one\n"
		"  %s world skybuild status\n"
		"Rotation is baked, not live: sky.vfx has no rotation parameter, so the panorama is\n"
		"re-projected. That takes a few seconds and then hot-swaps.\n",
		cmd, cmd, cmd, cmd);
}


} // namespace Filmmaker
