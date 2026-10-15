// ParticleFx disk prefetch: warms the OS file cache for the mounted FX asset pack so
// the main-thread blocking resolves during the warm-up burst (see ParticleFx.cpp) hit
// cached pages instead of cold disk. USRLOCALCSGO points at the staged pack game dir
// (~1220 loose files / ~240 MB, mounted by Automation/launch/launch-cs2-netcon.ps1 and
// consumed in main.cpp's AddSearchPath hook), so reading everything under it is bounded.
// This thread touches Win32 file APIs and the debug log only -- no engine calls -- which
// is what makes it safe off the main thread.

#include "ParticleFxInternal.h"

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_* (thread-safe flight recorder)

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <thread>
#include <vector>

namespace Filmmaker {
namespace fx {

void PrefetchFxPackOnce() {
	static std::atomic<bool> s_started{ false };
	if (s_started.exchange(true))
		return; // once per process: the OS file cache outlives the per-level handle cache

	// Detached like the folder-picker thread (Filmmaker.cpp): nothing joins it, and dying
	// mid-read at process exit is harmless.
	std::thread([] {
		const wchar_t* root = _wgetenv(L"USRLOCALCSGO");
		if (!root || !root[0]) {
			if (MvmDebugLog_Active())
				MvmDebugLog_Linef("fx.prefetch", "USRLOCALCSGO not set; no pack to warm");
			return;
		}
		const unsigned long long t0 = GetTickCount64();
		unsigned files = 0;
		unsigned long long bytes = 0;
		std::vector<char> buf(1 << 20);
		std::error_code ec;
		std::filesystem::recursive_directory_iterator it(
			root, std::filesystem::directory_options::skip_permission_denied, ec);
		const std::filesystem::recursive_directory_iterator end;
		for (; !ec && it != end; it.increment(ec)) {
			std::error_code fec;
			if (!it->is_regular_file(fec) || fec)
				continue;
			HANDLE h = CreateFileW(it->path().c_str(), GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
				OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
			if (h == INVALID_HANDLE_VALUE)
				continue;
			DWORD got = 0;
			while (ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr) && got)
				bytes += got;
			CloseHandle(h);
			++files;
		}
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("fx.prefetch", "warmed %u file(s), %llu MB in %llums",
				files, bytes >> 20, GetTickCount64() - t0);
	}).detach();
}

} // namespace fx
} // namespace Filmmaker
