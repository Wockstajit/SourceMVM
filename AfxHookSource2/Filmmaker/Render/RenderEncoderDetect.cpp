#include "RenderEncoderDetect.h"

#include "RenderOutput.h" // RenderNvencQualityFlags: the probe tests what jobs run
#include "../Debug/MvmDebugLog.h"

#include "../../hlaeFolder.h"

#include <atomic>
#include <string>
#include <thread>

#include <windows.h>

namespace Filmmaker {

namespace {

std::atomic<int> s_state{ (int)NvencProbe::Unknown };

// Same resolution rules as Shared/AfxOutStreams.cpp: ffmpeg.ini [Ffmpeg]Path override first,
// the bundled <hlae>\ffmpeg\bin\ffmpeg.exe otherwise.
std::wstring ResolveFfmpegExe() {
	std::wstring ini(GetHlaeFolderW());
	ini.append(L"ffmpeg\\ffmpeg.ini");

	wchar_t pathBuf[MAX_PATH + 3] = {0};
	int n = GetPrivateProfileStringW(L"Ffmpeg", L"Path", NULL, pathBuf, MAX_PATH + 3, ini.c_str());
	if (0 < n && n <= MAX_PATH)
		return std::wstring(pathBuf);

	std::wstring exe(GetHlaeFolderW());
	exe.append(L"ffmpeg\\bin\\ffmpeg.exe");
	return exe;
}

void ProbeWorker() {
	const std::wstring exe = ResolveFfmpegExe();

	if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("render.encoder", "probe: ffmpeg.exe missing");
		s_state.store((int)NvencProbe::Unavailable);
		return;
	}

	// A real (tiny) encode: synthetic 256x256 input, 3 frames, discarded output. Exit 0 proves
	// the encoder session opened on actual hardware.
	// Same quality flags the jobs use (RenderNvencQualityFlags): "available" has to mean the
	// encode a job will actually run starts, not just that some NVENC session opens.
	const std::string flags = RenderNvencQualityFlags();
	std::wstring cmd = L"\"" + exe + L"\" -hide_banner -loglevel error"
		L" -f lavfi -i color=black:s=256x256:r=30 -frames:v 3 -c:v h264_nvenc -preset p5 -tune hq "
		+ std::wstring(flags.begin(), flags.end()) + L" -rc vbr -cq 16 -b:v 0 -f null -";

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};

	// CreateProcessW needs a mutable command-line buffer.
	std::wstring mutableCmd = cmd;
	if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
		CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("render.encoder", "probe: CreateProcess failed err=%u", GetLastError());
		s_state.store((int)NvencProbe::Unavailable);
		return;
	}

	DWORD exitCode = 1;
	const DWORD wait = WaitForSingleObject(pi.hProcess, 10000);
	if (wait == WAIT_OBJECT_0) {
		GetExitCodeProcess(pi.hProcess, &exitCode);
	} else {
		// Hung probe (driver in a bad state): kill it and treat as unavailable.
		TerminateProcess(pi.hProcess, 1);
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);

	const bool ok = (wait == WAIT_OBJECT_0 && exitCode == 0);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.encoder", "probe: nvenc %s (wait=%u exit=%u)",
			ok ? "AVAILABLE" : "unavailable", wait, exitCode);
	s_state.store((int)(ok ? NvencProbe::Available : NvencProbe::Unavailable));
}

} // namespace

NvencProbe RenderEncoderDetect_State() {
	return (NvencProbe)s_state.load();
}

void RenderEncoderDetect_EnsureProbe() {
	int expected = (int)NvencProbe::Unknown;
	if (!s_state.compare_exchange_strong(expected, (int)NvencProbe::Probing))
		return; // already probing / probed
	std::thread(ProbeWorker).detach();
}

const char* RenderEncoderDetect_Describe() {
	switch (RenderEncoderDetect_State()) {
	case NvencProbe::Unknown: return "GPU encoding not probed yet";
	case NvencProbe::Probing: return "probing GPU encoder…";
	case NvencProbe::Available: return "GPU encoding available (NVENC)";
	case NvencProbe::Unavailable: return "GPU encoding unavailable (no NVENC in this FFmpeg/GPU); CPU x264 is used";
	}
	return "?";
}

} // namespace Filmmaker
