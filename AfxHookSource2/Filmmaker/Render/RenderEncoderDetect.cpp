#include "RenderEncoderDetect.h"

#include "RenderOutput.h" // RenderNvencQualityFlags: the probe tests what jobs run
#include "../Debug/MvmDebugLog.h"

#include "../../hlaeFolder.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include <windows.h>

namespace Filmmaker {

namespace {

std::atomic<int> s_state{ (int)NvencProbe::Unknown };
// Whether ffmpeg.exe exists and runs (s_ffmpegProblem empty), checked by the same probe.
std::atomic<bool> s_ffmpegOk{ false };
std::mutex s_problemMutex;
std::string s_ffmpegProblem;

void SetFfmpegResult(bool ok, const std::string& problem) {
	{
		std::lock_guard<std::mutex> lock(s_problemMutex);
		s_ffmpegProblem = problem;
	}
	s_ffmpegOk.store(ok);
}

std::string ToUtf8(const std::wstring& w) {
	if (w.empty()) return std::string();
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string out(n > 0 ? n : 0, '\0');
	if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), n, nullptr, nullptr);
	return out;
}

// Runs a command line without a window. Returns false when it could not start (outError holds
// GetLastError); outFinished is false when it did not end within timeoutMs (it is then killed).
bool RunHidden(const std::wstring& cmd, DWORD timeoutMs, bool& outFinished, DWORD& outExit, DWORD& outError) {
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};
	outFinished = false;
	outExit = 1;
	outError = 0;
	// CreateProcessW needs a mutable command-line buffer.
	std::wstring mutableCmd = cmd;
	if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
		CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
		outError = GetLastError();
		return false;
	}
	const DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
	if (wait == WAIT_OBJECT_0) {
		outFinished = true;
		GetExitCodeProcess(pi.hProcess, &outExit);
	} else {
		TerminateProcess(pi.hProcess, 1);
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return true;
}

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
	const std::string exeUtf8 = ToUtf8(exe);

	// Every render format but TGA, BMP and EXR encodes through FFmpeg, so first: is it there, and
	// does it run at all? A job reads this before it starts (RenderEncoderDetect_FfmpegProblem).
	if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("render.encoder", "probe: ffmpeg.exe missing at '%s'", exeUtf8.c_str());
		SetFfmpegResult(false, "FFmpeg is missing: " + exeUtf8 + " does not exist. Reinstall SOURCE:MVM "
			"or put ffmpeg.exe back in that folder (antivirus programs sometimes remove it).");
		s_state.store((int)NvencProbe::Unavailable);
		return;
	}
	{
		bool finished = false;
		DWORD exitCode = 1, error = 0;
		const bool started = RunHidden(L"\"" + exe + L"\" -hide_banner -version", 5000, finished, exitCode, error);
		char problem[96] = {};
		if (!started)
			snprintf(problem, sizeof(problem), " could not be started (Windows error %lu).", error);
		else if (!finished)
			snprintf(problem, sizeof(problem), " did not answer within 5 seconds.");
		else if (exitCode != 0)
			snprintf(problem, sizeof(problem), " stopped with exit code %lu when asked for its version.", exitCode);
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("render.encoder", "probe: ffmpeg '%s' started=%d finished=%d exit=%lu err=%lu",
				exeUtf8.c_str(), started ? 1 : 0, finished ? 1 : 0, exitCode, error);
		if (problem[0]) {
			SetFfmpegResult(false, "FFmpeg at " + exeUtf8 + problem);
			s_state.store((int)NvencProbe::Unavailable);
			return;
		}
		SetFfmpegResult(true, std::string());
	}

	// A real (tiny) encode: synthetic 256x256 input, 3 frames, discarded output. Exit 0 proves
	// the encoder session opened on actual hardware.
	// Same quality flags the jobs use (RenderNvencQualityFlags): "available" has to mean the
	// encode a job will actually run starts, not just that some NVENC session opens.
	const std::string flags = RenderNvencQualityFlags();
	std::wstring cmd = L"\"" + exe + L"\" -hide_banner -loglevel error"
		L" -f lavfi -i color=black:s=256x256:r=30 -frames:v 3 -c:v h264_nvenc -preset p5 -tune hq "
		+ std::wstring(flags.begin(), flags.end()) + L" -rc vbr -cq 16 -b:v 0 -f null -";

	// A hung probe (driver in a bad state) is killed and counts as unavailable.
	bool finished = false;
	DWORD exitCode = 1, error = 0;
	if (!RunHidden(cmd, 10000, finished, exitCode, error)) {
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("render.encoder", "probe: CreateProcess failed err=%lu", error);
		s_state.store((int)NvencProbe::Unavailable);
		return;
	}

	const bool ok = finished && exitCode == 0;
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.encoder", "probe: nvenc %s (finished=%d exit=%lu)",
			ok ? "AVAILABLE" : "unavailable", finished ? 1 : 0, exitCode);
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

void RenderEncoderDetect_RetryIfFfmpegFailed() {
	if (s_ffmpegOk.load())
		return;
	int expected = (int)NvencProbe::Unavailable;
	if (s_state.compare_exchange_strong(expected, (int)NvencProbe::Probing))
		std::thread(ProbeWorker).detach();
	else
		RenderEncoderDetect_EnsureProbe();
}

bool RenderEncoderDetect_Probing() {
	const NvencProbe state = RenderEncoderDetect_State();
	return state == NvencProbe::Unknown || state == NvencProbe::Probing;
}

std::string RenderEncoderDetect_FfmpegProblem() {
	if (RenderEncoderDetect_Probing())
		return std::string();
	std::lock_guard<std::mutex> lock(s_problemMutex);
	return s_ffmpegProblem;
}

const char* RenderEncoderDetect_Describe() {
	switch (RenderEncoderDetect_State()) {
	case NvencProbe::Unknown: return "GPU encoding not probed yet";
	case NvencProbe::Probing: return "probing GPU encoder…";
	case NvencProbe::Available: return "GPU encoding available (NVENC)";
	case NvencProbe::Unavailable:
		return s_ffmpegOk.load() ? "GPU encoding unavailable (no NVENC in this FFmpeg/GPU); CPU x264 is used"
			: "FFmpeg is missing or does not start: renders other than TGA, BMP and EXR will fail";
	}
	return "?";
}

} // namespace Filmmaker
