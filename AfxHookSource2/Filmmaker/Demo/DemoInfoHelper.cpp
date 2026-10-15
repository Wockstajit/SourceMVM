#include "DemoInfoHelper.h"

#include "DemoProjectStore.h"
#include "../Platform/JsonParser.h"

#include <Windows.h>
#include <atomic>
#include <fstream>
#include <sstream>
#include <vector>

namespace Filmmaker {

namespace {

// Must match FilmmakerDemoInfo's schemaVersion. A cached scoreboard.fmjson whose
// "v" differs was produced by an older helper and is discarded.
// v5: added top-level "events" (weapon/C4 drop + pickup ticks) for Follow Camera.
// v6: parser switched to demoinfocs-golang; "team" is the real end-of-match side
//     and MVPs / per-round MVP+side are populated for every parsed demo.
// v7: per-round MVP is now attributed to the round that actually ended (the
//     m_iMVPs delta is read AFTER the round commits), fixing stars that were
//     shifted one round too early on the round-performance timeline.
constexpr int kSchemaVersion = 10;

// Directory containing this DLL (HLAE's bin\x64 when injected from staging).
std::wstring SelfModuleDir() {
	HMODULE h = nullptr;
	GetModuleHandleExW(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&SelfModuleDir), &h);
	wchar_t buf[MAX_PATH];
	DWORD n = GetModuleFileNameW(h, buf, MAX_PATH);
	std::wstring path(buf, n);
	size_t slash = path.find_last_of(L"\\/");
	return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

bool FileExists(const std::wstring& p) {
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string WideToUtf8(const std::wstring& s) {
	if (s.empty()) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
	if (n <= 0) return std::string();
	std::string out((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n, nullptr, nullptr);
	return out;
}

// Locates FilmmakerDemoInfo.exe. An explicit override wins, then a few staging
// layouts relative to this DLL are tried.
std::wstring FindHelperExe() {
	wchar_t over[MAX_PATH];
	DWORD n = GetEnvironmentVariableW(L"MIRV_FILMMAKER_DEMOINFO", over, MAX_PATH);
	if (n > 0 && n < MAX_PATH && FileExists(over))
		return std::wstring(over, n);

	std::wstring dir = SelfModuleDir();
	if (dir.empty())
		return std::wstring();

	const wchar_t* rels[] = {
		L"\\FilmmakerDemoInfo\\FilmmakerDemoInfo.exe", // <dir>\FilmmakerDemoInfo\...
		L"\\FilmmakerDemoInfo.exe",                    // <dir>\FilmmakerDemoInfo.exe
		L"\\..\\FilmmakerDemoInfo\\FilmmakerDemoInfo.exe", // <dir>\..\FilmmakerDemoInfo\...
	};
	for (const wchar_t* rel : rels) {
		std::wstring cand = dir + rel;
		if (FileExists(cand))
			return cand;
	}
	return std::wstring();
}

bool GetWriteTime(const std::wstring& path, ULONGLONG& out) {
	WIN32_FILE_ATTRIBUTE_DATA fad;
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
		return false;
	ULARGE_INTEGER u;
	u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
	u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
	out = u.QuadPart;
	return true;
}

std::string ReadFileUtf8(const std::wstring& path) {
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return std::string();
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

void WriteFileUtf8(const std::wstring& path, const std::string& data) {
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	if (f)
		f.write(data.data(), (std::streamsize)data.size());
}

// Runs exe with one or two arguments, returns its stdout. Empty on failure.
// arg2 is appended (quoted) only when non-empty.
//
// The drain is bounded and poll-based: a demo the helper can't handle must never block
// the caller forever (the scan pool and DemoLibrary::Shutdown() join these threads). We
// cap the total run time and also abort promptly when the caller is cancelling. The
// child's stderr is sent to NUL so a stray .NET runtime message can't be interleaved
// into the JSON we capture on stdout.
std::string RunCaptureStdout(const std::wstring& exe, const std::wstring& arg,
	const std::wstring& arg2, const std::atomic<bool>* cancel) {
	SECURITY_ATTRIBUTES sa{};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;

	HANDLE readPipe = nullptr, writePipe = nullptr;
	if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
		return std::string();
	SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

	// Separate inheritable handle to the NUL device for the child's stderr, so runtime
	// warnings/exception text never corrupt the JSON on stdout. If NUL can't be opened
	// (never expected on Windows) we fall back to the stdout pipe as before.
	HANDLE nulHandle = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		&sa, OPEN_EXISTING, 0, nullptr);

	std::wstring cmd = L"\"" + exe + L"\" \"" + arg + L"\"";
	if (!arg2.empty())
		cmd += L" \"" + arg2 + L"\"";
	std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
	cmdBuf.push_back(L'\0');

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	si.hStdOutput = writePipe;
	si.hStdError = (nulHandle != INVALID_HANDLE_VALUE) ? nulHandle : writePipe;
	si.hStdInput = nullptr;

	PROCESS_INFORMATION pi{};
	BOOL ok = CreateProcessW(exe.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE,
		CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
	CloseHandle(writePipe); // parent keeps only the read end
	if (nulHandle != INVALID_HANDLE_VALUE)
		CloseHandle(nulHandle);
	if (!ok) {
		CloseHandle(readPipe);
		return std::string();
	}

	constexpr ULONGLONG kMaxRunMs = 120000; // hard ceiling against a hung/runaway helper
	const ULONGLONG start = GetTickCount64();
	std::string out;
	char buf[4096];
	bool aborted = false;
	for (;;) {
		if (cancel && cancel->load()) { aborted = true; break; }
		if (GetTickCount64() - start > kMaxRunMs) { aborted = true; break; }

		// PeekNamedPipe so a stuck child (no output, not exited) can't block ReadFile.
		DWORD avail = 0, got = 0;
		if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
			if (ReadFile(readPipe, buf, sizeof(buf), &got, nullptr) && got > 0) {
				out.append(buf, got);
				continue; // drain everything buffered before waiting again
			}
		}

		if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
			// Child exited: drain any final buffered output, then stop.
			while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0
				&& ReadFile(readPipe, buf, sizeof(buf), &got, nullptr) && got > 0)
				out.append(buf, got);
			break;
		}
	}

	if (aborted) {
		TerminateProcess(pi.hProcess, 1);
		WaitForSingleObject(pi.hProcess, 2000); // let the kill settle before closing handles
		out.clear(); // a partial/aborted parse is not trustworthy JSON
	}

	CloseHandle(readPipe);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return out;
}

bool ParseJson(const std::string& json, DemoHelperResult& out) {
	JsonValue root;
	if (!JsonParse(json, root) || root.type != JsonValue::Type::Object) {
		out.error = "helper output was not valid JSON";
		return false;
	}
	const JsonValue* okv = root.Find("ok");
	if (!okv || !okv->AsBool(false)) {
		if (const JsonValue* e = root.Find("error")) out.error = e->AsString("helper returned ok=false");
		if (out.error.empty()) out.error = "helper returned ok=false";
		return false;
	}

	out.ok = true;
	if (const JsonValue* v = root.Find("v")) out.schemaVersion = v->AsInt();
	if (const JsonValue* v = root.Find("namesOnly")) out.namesOnly = v->AsBool();
	if (const JsonValue* v = root.Find("map")) out.map = v->AsString();
	if (const JsonValue* v = root.Find("durationSeconds")) out.durationSeconds = v->AsInt();
	if (const JsonValue* v = root.Find("teamScore0")) out.teamScore0 = v->AsInt();
	if (const JsonValue* v = root.Find("teamScore1")) out.teamScore1 = v->AsInt();
	if (const JsonValue* v = root.Find("hasScoreboard")) out.hasScoreboard = v->AsBool();

	if (const JsonValue* players = root.Find("players"); players && players->type == JsonValue::Type::Array) {
		for (const auto& pj : players->arr) {
			if (pj.type != JsonValue::Type::Object) continue;
			ScoreboardPlayer p;
			if (const JsonValue* v = pj.Find("name")) p.name = v->AsString("[unknown]");
			if (const JsonValue* v = pj.Find("accountId")) p.accountId = (uint32_t)v->AsNumber();
			if (const JsonValue* v = pj.Find("k")) p.kills = v->AsInt();
			if (const JsonValue* v = pj.Find("a")) p.assists = v->AsInt();
			if (const JsonValue* v = pj.Find("d")) p.deaths = v->AsInt();
			if (const JsonValue* v = pj.Find("score")) p.score = v->AsInt();
			if (const JsonValue* v = pj.Find("mvps")) p.mvps = v->AsInt();
			if (const JsonValue* v = pj.Find("team")) p.teamIndex = v->AsInt();
			if (const JsonValue* pr = pj.Find("perRound"); pr && pr->type == JsonValue::Type::Array) {
				p.perRound.reserve(pr->arr.size());
				for (const auto& rj : pr->arr) {
					if (rj.type != JsonValue::Type::Object) continue;
					RoundStat rs;
					if (const JsonValue* v = rj.Find("k")) rs.kills = v->AsInt();
					if (const JsonValue* v = rj.Find("hs")) rs.headshots = v->AsInt();
					if (const JsonValue* v = rj.Find("d")) rs.died = v->AsInt();
					if (const JsonValue* v = rj.Find("w")) rs.won = v->AsInt();
					if (const JsonValue* v = rj.Find("side")) rs.side = v->AsInt();
					if (const JsonValue* v = rj.Find("mvp")) rs.mvp = v->AsInt();
					p.perRound.push_back(rs);
				}
			}
			out.players.push_back(std::move(p));
		}
	}
	if (!out.players.empty())
		out.hasScoreboard = true;

	if (const JsonValue* events = root.Find("events"); events && events->type == JsonValue::Type::Array) {
		out.events.reserve(events->arr.size());
		for (const auto& ej : events->arr) {
			if (ej.type != JsonValue::Type::Object) continue;
			DemoEvent e;
			if (const JsonValue* v = ej.Find("tick")) e.tick = v->AsInt();
			if (const JsonValue* v = ej.Find("type")) e.type = v->AsString();
			if (const JsonValue* v = ej.Find("item")) e.item = v->AsString();
			if (const JsonValue* v = ej.Find("accountId")) e.accountId = (uint32_t)v->AsNumber();
			out.events.push_back(std::move(e));
		}
	}
	return true;
}

} // namespace

DemoHelperResult ReadDemoInfoViaHelper(const std::wstring& demoPath,
	const std::vector<uint32_t>& wantedAccountIds,
	const std::atomic<bool>* cancel) {
	DemoHelperResult result;

	const bool wantNamesFast = !wantedAccountIds.empty();
	std::wstring cachePath = DemoProjectStore_GeneratedForLoad(
		ProjectGame::Cs2, demoPath, ProjectGenerated::Scoreboard);
	const std::wstring exe = FindHelperExe();
	result.helperPath = WideToUtf8(exe);

	ULONGLONG demoTime = 0, cacheTime = 0, helperTime = 0;
	const bool haveDemoTime = GetWriteTime(demoPath, demoTime);
	const bool haveHelperTime = !exe.empty() && GetWriteTime(exe, helperTime);

	// Reuse the cache only if it is newer than BOTH the demo and the helper exe
	// (so rebuilding/updating the helper transparently regenerates stale results),
	// matches the current schema, and actually has the data this call needs (a
	// names-only cache cannot satisfy a full-parse request from a demo that has no
	// matchmaking sidecar).
	if (GetWriteTime(cachePath, cacheTime)
		&& (!haveDemoTime || cacheTime >= demoTime)
		&& (!haveHelperTime || cacheTime >= helperTime)) {
		std::string cachedJson = ReadFileUtf8(cachePath);
		JsonValue cachedRoot;
		if (!cachedJson.empty() && JsonParse(cachedJson, cachedRoot)
			&& cachedRoot.type == JsonValue::Type::Object) {
			const JsonValue* okv = cachedRoot.Find("ok");
			if (!okv || !okv->AsBool(false)) {
				// Cached NEGATIVE: the helper ran but couldn't parse this demo. The
				// freshness gate above already discards it once the demo or helper exe
				// changes, so honoring it here stops us re-launching the (slow, external)
				// helper on every scan for a permanently unparseable demo.
				if (const JsonValue* e = cachedRoot.Find("error"))
					result.error = e->AsString("cached helper result is ok=false");
				else
					result.error = "cached helper result is ok=false";
				return result; // ok == false
			}
			DemoHelperResult cached;
			if (ParseJson(cachedJson, cached)
				&& cached.schemaVersion == kSchemaVersion
				&& (wantNamesFast || !cached.namesOnly)) {
				cached.helperPath = WideToUtf8(exe);
				return cached;
			}
		}
	}

	if (exe.empty()) {
		result.error = "FilmmakerDemoInfo helper executable was not found";
		return result; // no helper -> caller falls back to the sidecar
	}

	std::wstring idsArg;
	if (wantNamesFast) {
		std::string ids;
		for (size_t i = 0; i < wantedAccountIds.size(); ++i) {
			if (i) ids.push_back(',');
			ids += std::to_string(wantedAccountIds[i]);
		}
		idsArg.assign(ids.begin(), ids.end()); // decimal digits/commas: ASCII-safe
	}

	std::string json = RunCaptureStdout(exe, demoPath, idsArg, cancel);
	if (json.empty()) {
		result.error = cancel && cancel->load()
			? "demo info helper was cancelled"
			: "demo info helper produced no JSON before timeout";
		return result;
	}

	// Cache whatever the helper produced (including ok:false negative results, so
	// unparseable demos are not re-run every scan).
	const std::wstring savePath = DemoProjectStore_GeneratedForSave(
		ProjectGame::Cs2, demoPath, ProjectGenerated::Scoreboard);
	if (!savePath.empty())
		WriteFileUtf8(savePath, json);
	ParseJson(json, result);
	result.helperPath = WideToUtf8(exe);
	return result;
}

} // namespace Filmmaker
