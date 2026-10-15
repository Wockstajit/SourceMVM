#include "BugReport.h"
#include "BugReportMath.h"
#include "CrashReportMath.h"   // SymbolStoreKey
#include "CrashReport.h"

#include "../Filmmaker.h"                     // PumpStatus
#include "../Debug/MvmDebugLog.h"    // MvmDebugLog_Active
#include "../Demo/PlayingDemoPath.h"          // ResolvePlayingDemoPath
#include "../Environment/EnvironmentScene.h"  // CurrentMapName
#include "../Movie/ParticleFx.h"
#include "../Platform/TextEncoding.h"
#include "../../SchemaSystem.h"               // g_cosmeticsOffsetsOk
#include "../../hlaeFolder.h"                 // GetHlaeRoamingAppDataFolderW

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/icvar.h" // the `name` cvar, for the scrub

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace Filmmaker {

namespace {

std::wstring HlaeFolder(const wchar_t* sub) {
	std::wstring folder = GetHlaeRoamingAppDataFolderW();
	if (!folder.empty() && folder.back() != L'\\' && folder.back() != L'/') folder += L'\\';
	return folder + sub;
}

std::string ReadFile(const fs::path& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) return {};
	std::ostringstream s;
	s << in.rdbuf();
	return s.str();
}

// The newest regular file in `dir` whose name matches `prefix` and `ext` (empty = any).
fs::path Newest(const fs::path& dir, const std::wstring& prefix, const std::wstring& ext) {
	fs::path best;
	fs::file_time_type bestTime{};
	std::error_code ec;
	for (const auto& entry : fs::directory_iterator(dir, ec)) {
		if (!entry.is_regular_file(ec)) continue;
		const std::wstring name = entry.path().filename().wstring();
		if (!prefix.empty() && name.rfind(prefix, 0) != 0) continue;
		if (!ext.empty() && _wcsicmp(entry.path().extension().c_str(), ext.c_str()) != 0) continue;
		const auto t = entry.last_write_time(ec);
		if (best.empty() || t > bestTime) { best = entry.path(); bestTime = t; }
	}
	return best;
}

std::string Age(const fs::path& path) {
	std::error_code ec;
	const auto t = fs::last_write_time(path, ec);
	if (ec) return "?";
	const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(fs::file_time_type::clock::now() - t).count();
	if (minutes < 60) return std::to_string(minutes) + " min ago";
	if (minutes < 48 * 60) return std::to_string(minutes / 60) + " h ago";
	return std::to_string(minutes / (60 * 24)) + " days ago";
}

// game/bin/win64/cs2.exe -> the install's game folder.
fs::path GameFolder() {
	wchar_t exe[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	return fs::path(exe).parent_path().parent_path().parent_path();
}

// Which hook this is. __DATE__/__TIME__ only moved when THIS file recompiled, so 0.1.9 and its
// hotfix both reported "Oct 12 18:39:16" (2026-10-13). The PE header's link time changes on every
// link; the launcher's installed.json, two folders up from the DLL, names the shipped version.
std::string HookBuildText() {
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&HookBuildText), &self);
	std::string text = "(unknown)";
	if (self) {
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const unsigned char*>(self) + dos->e_lfanew);
		const time_t linked = time_t(nt->FileHeader.TimeDateStamp);
		tm utc{};
		char stamp[64] = "?";
		if (gmtime_s(&utc, &linked) == 0) strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S UTC", &utc);
		text = std::string("linked ") + stamp + ", id " + BugReport_HookImageKey();
		wchar_t dll[MAX_PATH] = {};
		if (GetModuleFileNameW(self, dll, MAX_PATH)) {
			const std::string installed = ReadFile(fs::path(dll).parent_path().parent_path().parent_path() / "installed.json");
			const size_t runtime = installed.find("\"runtime\"");
			const size_t key = runtime == std::string::npos ? runtime : installed.find("\"version\"", runtime);
			const size_t open = key == std::string::npos ? key : installed.find('"', installed.find(':', key) + 1);
			const size_t close = open == std::string::npos ? open : installed.find('"', open + 1);
			if (close != std::string::npos) text += ", runtime " + installed.substr(open + 1, close - open - 1);
		}
	}
	return text;
}

std::string WindowsVersion() {
	using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
	OSVERSIONINFOW v{};
	v.dwOSVersionInfoSize = sizeof(v);
	if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
		if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) fn(&v);
	char text[64];
	snprintf(text, sizeof(text), "%lu.%lu build %lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
	return text;
}

// The lines of CS2's steam.inf that identify the build (ClientVersion, PatchVersion, ...).
std::string Cs2Build() {
	std::istringstream in(ReadFile(GameFolder() / "csgo" / "steam.inf"));
	std::string line, out;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.rfind("ClientVersion", 0) == 0 || line.rfind("PatchVersion", 0) == 0 || line.rfind("SourceRevision", 0) == 0 || line.rfind("VersionDate", 0) == 0)
			out += (out.empty() ? "" : ", ") + line;
	}
	return out.empty() ? "unknown (steam.inf not read)" : out;
}

void Section(std::ostringstream& out, const std::string& title) {
	out << "\n==== " << title << " " << std::string(title.size() < 70 ? 70 - title.size() : 0, '=') << "\n";
}

// The end of a log file, within `budget` bytes.
void AppendTail(std::ostringstream& out, const fs::path& path, size_t budget) {
	const std::string text = ReadFile(path);
	size_t omitted = 0;
	const std::string tail = BugReport::TailText(text, budget, &omitted);
	if (omitted) out << "(first " << omitted / 1024 << " KB left out; the last " << tail.size() / 1024 << " KB follow)\n";
	out << tail;
	if (!tail.empty() && tail.back() != '\n') out << '\n';
}

std::mutex g_recorderMutex;
BugReport::LineRing g_recorder(BugReport::kRecorderBytes);

} // namespace

void BugReport_Record(const char* category, const std::string& raw) {
	// Console lines arrive single-lined with " | " for their breaks; a trailing one is noise.
	std::string line = raw;
	while (line.size() >= 2 && line.compare(line.size() - 2, 2, " |") == 0) line.resize(line.size() - 2);
	if (line.empty()) return;
	SYSTEMTIME st;
	GetLocalTime(&st);
	char stamp[48];
	snprintf(stamp, sizeof(stamp), "%02u:%02u:%02u.%03u [%s] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
		category ? category : "?");
	std::string full = stamp + line;
	std::lock_guard<std::mutex> lock(g_recorderMutex);
	CrashReport_SessionLogLine(full);
	g_recorder.Add(std::move(full));
}

std::wstring BugReport_Folder() {
	return HlaeFolder(L"bugreports\\");
}

BugReportResult BugReport_Write(const std::string& rawDescription, const BugReportSender& sender,
	const BugReportCrash* crash) {
	BugReportResult r;
	const std::string description = BugReport::CleanDescription(rawDescription);
	r.description = description;
	r.sender = sender;
	if (crash) r.crash = crash->summary.empty() ? "crash" : crash->summary;

	SYSTEMTIME st;
	GetLocalTime(&st);
	char when[64];
	snprintf(when, sizeof(when), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

	std::ostringstream out;
	out << (crash ? "SOURCE:MVM crash report (sent automatically)\n" : "SOURCE:MVM bug report\n");
	out << "Created: " << when << "\n";

	if (crash) {
		Section(out, "Crash");
		if (crash->previousSession)
			out << "(Written when SOURCE:MVM started again; the crash happened in the previous session.)\n";
		out << crash->details;
		if (!crash->details.empty() && crash->details.back() != '\n') out << '\n';
	} else {
		Section(out, "What happened (written by the user)");
		out << (description.empty() ? "(nothing written)" : description) << "\n";
	}

	Section(out, "Contact (optional, typed by the user)");
	out << "Discord               : " << (sender.discord.empty() ? "(not given)" : sender.discord) << "\n";

	Section(out, "Build and system");
	r.hookBuild = HookBuildText();
	r.cs2Build = Cs2Build();
	r.windows = WindowsVersion();
	out << "SOURCE:MVM hook built : " << r.hookBuild << "\n";
	out << "CS2                   : " << r.cs2Build << "\n";
	out << "Windows               : " << r.windows << "\n";
	if (crash) {
		// Engine state is not queried for a crash: the caller is a worker of a crashed process, or
		// a later session. The crash handler remembered the map and demo.
		r.map = crash->map.empty() ? "(unknown)" : crash->map;
		r.demo = crash->demo.empty() ? "(none playing)" : crash->demo;
	} else {
		r.map = Environment::CurrentMapName();
		if (r.map.empty() || r.map == "<empty>") r.map = "(main menu)";
		const std::wstring demo = ResolvePlayingDemoPath();
		r.demo = demo.empty() ? "(none playing)" : WideToUtf8(fs::path(demo).filename().wstring());
	}
	out << "Map                   : " << r.map << "\n";
	out << "Demo                  : " << r.demo << "\n";
	uint64_t renderCalls = 0, mainCalls = 0, scans = 0;
	PumpStatus(renderCalls, mainCalls, scans);
	if (crash && crash->previousSession)
		out << "(The lines below describe the NEW session, not the one that crashed.)\n";
	out << "Render / main pump    : " << renderCalls << " / " << mainCalls << (renderCalls == 0 ? "   <-- render pump dead" : "") << "\n";
	out << "Schema offsets ok     : " << (g_cosmeticsOffsetsOk ? "yes" : "NO") << "\n";
	out << "Particle hook         : " << (ParticleFxRef().Installed() ? "installed" : "NOT installed") << "\n";
	out << "Debug log recording   : " << (MvmDebugLog_Active() ? "yes" : "no") << "\n";
	if (!crash) {
		const std::string injected = CrashReport_InjectedModules();
		out << "Injected              : " << (injected.empty() ? "(none found)" : injected) << "\n";
	}
	r.health = std::string(renderCalls == 0 ? "render pump DEAD" : "render pump ok") +
		(g_cosmeticsOffsetsOk ? ", schema ok" : ", schema BROKEN") +
		(ParticleFxRef().Installed() ? ", particle hook ok" : ", particle hook OFF");

	// The always-on recorder: what happened in the minutes before the report, whether or not
	// anyone turned on mvm_debug.
	Section(out, "Recent history (always recorded)");
	if (crash && crash->previousSession) {
		out << "(the crashed session's log, read from disk)\n" << crash->history;
		if (!crash->history.empty() && crash->history.back() != '\n') out << '\n';
	} else {
		// The crashing thread may hold the recorder lock: a crash report never waits on it for long.
		std::unique_lock<std::mutex> lock(g_recorderMutex, std::defer_lock);
		if (crash) {
			for (int i = 0; i < 20 && !lock.try_lock(); ++i) Sleep(50);
		} else {
			lock.lock();
		}
		if (lock.owns_lock()) {
			if (g_recorder.Dropped()) out << "(" << g_recorder.Dropped() << " older lines rolled off; the newest " << g_recorder.Lines() << " follow)\n";
			out << g_recorder.Text();
		} else {
			out << "(the crashing thread held the recorder; its session log is sent on the next launch)\n";
		}
	}

	// The newest mvm_debug log: console output, commands, UI and camera events.
	Section(out, "mvm_debug log");
	const fs::path debugLog = Newest(HlaeFolder(L"debuglogs"), L"mvm_debug_", L".log");
	if (debugLog.empty()) {
		out << "No mvm_debug log file (the recent history above is always included).\n";
	} else {
		r.hadDebugLog = true;
		out << WideToUtf8(debugLog.filename().wstring()) << " (" << Age(debugLog) << ")\n";
		AppendTail(out, debugLog, BugReport::kDebugLogBudget);
	}

	// CS2's own console log exists only when it was launched with -condebug.
	const fs::path consoleLog = GameFolder() / "csgo" / "console.log";
	std::error_code ec;
	if (fs::is_regular_file(consoleLog, ec)) {
		Section(out, "CS2 console.log");
		out << "(" << Age(consoleLog) << ")\n";
		AppendTail(out, consoleLog, BugReport::kConsoleLogBudget);
	}

	// Crash dumps are far too big for Discord: name the recent ones so they can be asked for.
	Section(out, "Recent crash dumps (not included)");
	std::vector<std::pair<fs::file_time_type, fs::path>> dumps;
	for (const auto& entry : fs::directory_iterator(GameFolder() / "bin" / "win64", ec)) {
		if (entry.is_regular_file(ec) && _wcsicmp(entry.path().extension().c_str(), L".mdmp") == 0)
			dumps.emplace_back(entry.last_write_time(ec), entry.path());
	}
	std::sort(dumps.begin(), dumps.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	if (dumps.empty()) out << "none\n";
	for (size_t i = 0; i < dumps.size() && i < 3; ++i)
		out << WideToUtf8(dumps[i].second.filename().wstring()) << "  (" << Age(dumps[i].second) << ", "
		    << fs::file_size(dumps[i].second, ec) / 1024 << " KB)\n";

	// No Windows account name leaves the PC: logs and paths carry C:\Users\<name>\... everywhere.
	std::string text = BugReport_ScrubPrivate(out.str());
	if (text.size() > BugReport::kMaxReportBytes) text.resize(BugReport::kMaxReportBytes);

	const fs::path folder = HlaeFolder(L"bugreports");
	fs::create_directories(folder, ec);
	const std::string name = crash && !crash->baseName.empty() ? crash->baseName + ".txt"
		: BugReport::ReportFileName(st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	const fs::path path = folder / name;
	std::ofstream file(path, std::ios::binary);
	if (!file || !file.write(text.data(), std::streamsize(text.size()))) {
		r.error = "Could not write " + WideToUtf8(path.wstring());
		return r;
	}
	r.ok = true;
	r.path = path.wstring();
	r.fileName = name;
	r.bytes = text.size();
	return r;
}

// The local Steam name, copied on the main thread; read by whichever thread writes a report. A fixed
// buffer and no lock: the crash handler's worker must never wait on the thread that crashed.
char g_localPlayerName[128] = {};

void BugReport_NoteLocalPlayerName() {
	if (!SOURCESDK::CS2::g_pCVar) return;
	SOURCESDK::CS2::ConVarHandle handle = SOURCESDK::CS2::g_pCVar->FindConVar("name", false);
	if (!handle.IsValid()) return;
	SOURCESDK::CS2::Cvar_s* cvar = SOURCESDK::CS2::g_pCVar->GetCvar(handle.Get());
	if (!cvar || cvar->m_eVarType != SOURCESDK::CS2::EConVarType_String) return;
	const char* name = cvar->m_Value.m_szValue.Get();
	if (!name || 0 == strcmp(name, g_localPlayerName)) return;
	strncpy_s(g_localPlayerName, name, _TRUNCATE);
}

std::string BugReport_ScrubPrivate(const std::string& text) {
	wchar_t profile[MAX_PATH] = {};
	const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
	char name[sizeof(g_localPlayerName)];
	memcpy(name, g_localPlayerName, sizeof(name));
	name[sizeof(name) - 1] = '\0';
	return BugReport::ScrubSteamIdentity(
		BugReport::ScrubUserPaths(text, (n > 0 && n < MAX_PATH) ? WideToUtf8(profile) : std::string()), name);
}

std::string BugReport_HookModuleName() {
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&BugReport_HookModuleName), &self);
	wchar_t dll[MAX_PATH] = {};
	if (!self || !GetModuleFileNameW(self, dll, MAX_PATH)) return "AfxHookSource2.dll";
	return fs::path(dll).filename().string();
}

std::string BugReport_HookImageKey() {
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&BugReport_HookImageKey), &self);
	if (!self) return "?";
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const unsigned char*>(self) + dos->e_lfanew);
	char key[32];
	CrashReport::SymbolStoreKey(key, sizeof(key), nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
	return key;
}

} // namespace Filmmaker
