#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "OsuPlatform.h"

#include "../../Platform/TextEncoding.h"
#include "../../../hlaeFolder.h"

#include <Windows.h>
#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")

#include <algorithm>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace Filmmaker {
namespace OsuPlatform {

double QpcMs() {
	static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
	LARGE_INTEGER c; QueryPerformanceCounter(&c);
	return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

std::string ReadFile(const fs::path& p) {
	std::ifstream f(p, std::ios::binary);
	if (!f) return {};
	std::stringstream ss; ss << f.rdbuf();
	return ss.str();
}

bool RunHidden(const std::wstring& cmdline, unsigned long timeoutMs) {
	STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi{};
	std::wstring cmd = cmdline;
	if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
	const DWORD w = WaitForSingleObject(pi.hProcess, timeoutMs);
	DWORD code = 1;
	if (w == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
	else TerminateProcess(pi.hProcess, 1);
	CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
	return code == 0;
}

bool RunCapture(const std::wstring& cmdline, unsigned long timeoutMs, std::string& out) {
	out.clear();
	SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
	HANDLE rd = nullptr, wr = nullptr;
	if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) return false;
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
	STARTUPINFOW si{}; si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES; si.wShowWindow = SW_HIDE;
	si.hStdOutput = wr; si.hStdError = nullptr; si.hStdInput = nullptr;
	PROCESS_INFORMATION pi{};
	std::wstring cmd = cmdline;
	const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
	CloseHandle(wr);
	if (!ok) { CloseHandle(rd); return false; }
	const ULONGLONG t0 = GetTickCount64();
	char buf[1 << 16];
	// Poll instead of waiting for EOF: EOF needs EVERY copy of the write end closed, and any other
	// process the game starts meanwhile with inherited handles keeps a copy -- the read then never
	// ends. The child exiting with nothing left in the pipe is the real end.
	for (;;) {
		DWORD avail = 0;
		if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break; // broken pipe: all writers gone
		if (avail > 0) {
			DWORD got = 0;
			if (!::ReadFile(rd, buf, (DWORD)std::min<size_t>(sizeof(buf), avail), &got, nullptr) || got == 0) break;
			out.append(buf, got);
			continue;
		}
		if (WaitForSingleObject(pi.hProcess, 1) == WAIT_OBJECT_0) {
			if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
			continue; // drain what it wrote before exiting
		}
		if (GetTickCount64() - t0 > timeoutMs) { TerminateProcess(pi.hProcess, 1); break; }
	}
	CloseHandle(rd);
	WaitForSingleObject(pi.hProcess, 2000);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
	return code == 0 && !out.empty();
}

static fs::path HlaeBin() {
	const char* h = GetHlaeFolder();
	return h ? fs::path(Utf8ToWide(h)) : fs::path();
}

fs::path Ffmpeg() {
	// GetHlaeFolder() is the install's bin\ folder (the hook lives in bin\x64\).
	const fs::path p = HlaeBin() / L"ffmpeg" / L"ffmpeg.exe";
	std::error_code ec;
	return fs::exists(p, ec) ? p : fs::path(L"ffmpeg.exe");
}

std::string Md5Hex(const std::string& bytes) {
	HCRYPTPROV prov = 0; HCRYPTHASH hash = 0;
	std::string out;
	if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return out;
	if (CryptCreateHash(prov, CALG_MD5, 0, 0, &hash)) {
		if (CryptHashData(hash, (const BYTE*)bytes.data(), (DWORD)bytes.size(), 0)) {
			BYTE digest[16]; DWORD len = 16;
			if (CryptGetHashParam(hash, HP_HASHVAL, digest, &len, 0)) {
				static const char* hex = "0123456789abcdef";
				for (DWORD i = 0; i < len; ++i) { out += hex[digest[i] >> 4]; out += hex[digest[i] & 15]; }
			}
		}
		CryptDestroyHash(hash);
	}
	CryptReleaseContext(prov, 0);
	return out;
}

uint64_t Fnv64(const std::wstring& s) {
	uint64_t h = 1469598103934665603ull;
	for (wchar_t c : s) { h ^= (uint64_t)towlower(c); h *= 1099511628211ull; }
	return h;
}

std::string Hex64(uint64_t v) {
	char buf[17];
	snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)v);
	return buf;
}

fs::path RoamingRoot() {
	std::wstring p = GetHlaeRoamingAppDataFolderW();
	if (!p.empty() && p.back() != L'\\' && p.back() != L'/') p += L'\\';
	return fs::path(p) / L"osu";
}
fs::path BundledRoot() { return HlaeBin() / L"osu"; }
fs::path CacheDir() { return RoamingRoot() / L"cache"; }

} // namespace OsuPlatform
} // namespace Filmmaker
