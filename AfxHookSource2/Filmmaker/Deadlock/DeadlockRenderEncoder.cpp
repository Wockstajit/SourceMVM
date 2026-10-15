#include "DeadlockRenderEncoder.h"
#include "DeadlockRender.h"
#include "../Render/RenderOutput.h"
#include "../Debug/MvmDebugLog.h"
#include "../Platform/TextEncoding.h"
#include <windows.h>
#include <filesystem>
#include <algorithm>
#include <mutex>
#include <sstream>
#include <vector>

namespace Filmmaker {
namespace {
std::mutex g_mutex;
HANDLE g_process = nullptr, g_input = nullptr;
unsigned g_width = 0, g_height = 0;
std::string g_error;
std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }
void Replace(std::string& text, const std::string& from, const std::string& to) {
    size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) { text.replace(at, from.size(), to); at += to.size(); }
}
bool WriteBytes(const void* data, DWORD length) {
    auto p = static_cast<const BYTE*>(data);
    while (length) {
        DWORD n = 0;
        if (!WriteFile(g_input, p, length, &n, nullptr) || !n) return false;
        p += n; length -= n;
    }
    return true;
}
}
std::wstring DeadlockEncoder_ResolveExe() {
    wchar_t value[32768]{};
    if (GetEnvironmentVariableW(L"SOURCEMVM_FFMPEG", value, _countof(value)) > 0
        && GetFileAttributesW(value) != INVALID_FILE_ATTRIBUTES) return value;
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&DeadlockEncoder_ResolveExe), &module);
    GetModuleFileNameW(module, value, _countof(value));
    auto base = std::filesystem::path(value).parent_path();
    for (int level = 0; level < 7 && !base.empty(); ++level, base = base.parent_path()) {
        for (const auto* relative : { L"ffmpeg", L"bin/ffmpeg", L"build/source2-release/bin/ffmpeg", L"Resources/ffmpeg" }) {
            const auto root = base / relative;
            const auto ini = (root / L"ffmpeg.ini").wstring();
            if (GetPrivateProfileStringW(L"Ffmpeg", L"Path", L"", value, _countof(value), ini.c_str()) > 0
                && GetFileAttributesW(value) != INVALID_FILE_ATTRIBUTES) return value;
            const auto exe = root / L"bin/ffmpeg.exe";
            if (std::filesystem::is_regular_file(exe)) return exe.wstring();
        }
    }
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, _countof(value), value, nullptr)) return value;
    return {};
}
bool DeadlockEncoder_Start(const RenderSettings& settings, const std::wstring& take, int width, int height) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_process) { g_error = "The previous encoder is still finishing."; return false; }
    g_error.clear();
    auto exe = DeadlockEncoder_ResolveExe();
    if (exe.empty()) { g_error = "FFmpeg was not found. Configure ffmpeg.ini or SOURCEMVM_FFMPEG."; return false; }
    if (settings.format == "gif" && settings.fps > 100) { g_error = "GIF supports at most 100 fps. Choose a lower frame rate or MP4."; return false; }
    if (settings.format == "exr") {
        g_error = "HDR EXR capture requires a pre-tonemap source; Deadlock currently exposes the display image.";
        return false;
    }
    g_width = width; g_height = height;
    int w = settings.width > 0 ? (std::min)(settings.width, width) : width;
    int h = settings.height > 0 ? (std::min)(settings.height, height) : height;
    if (settings.format == "mp4") { w -= w % 2; h -= h % 2; }
    if (w < 2 || h < 2) { g_error = "The output size is too small."; return false; }
    std::string filter = "scale=" + std::to_string(w) + ":" + std::to_string(h)
        + ":force_original_aspect_ratio=decrease:flags=lanczos,pad=" + std::to_string(w)
        + ":" + std::to_string(h) + ":(ow-iw)/2:(oh-ih)/2";
    std::string options;
    std::string file;
    if (settings.format == "mp4") {
        RenderOutputSettings output;
        output.crf = settings.quality; output.bitrateKbps = settings.bitrateKbps;
        output.speed = settings.speed == "fast" ? RenderEncodeSpeed::Fast
            : settings.speed == "quality" ? RenderEncodeSpeed::Quality : RenderEncodeSpeed::Balanced;
        // Explicit GPU requests are honoured; Auto uses the reliable CPU path until a probe proves GPU.
        options = BuildFfmpegVideoOptions(output, settings.encoder == "gpu", "video", filter);
        Replace(options, "{QUOTE}", "\""); Replace(options, "{AFX_STREAM_PATH}", WideToUtf8(take));
    } else {
        if (settings.format == "avi") { options = "-c:v huffyuv"; file = "video.avi"; }
        else if (settings.format == "gif") { options = "-loop 0"; file = "video.gif"; }
        else {
            const auto codec = settings.format == "tga" ? "targa" : settings.format == "jpg" ? "mjpeg" : settings.format.c_str();
            options = "-c:v " + std::string(codec) + " -f image2 -start_number 0";
            file = "frame_%06d." + settings.format;
        }
        options += " -vf " + filter + " \"" + WideToUtf8(take) + "\\" + file + "\"";
    }
    const std::wstring command = Quote(exe) + L" -hide_banner -loglevel warning -nostdin -y -f rawvideo -pixel_format bgra -video_size "
        + std::to_wstring(width) + L"x" + std::to_wstring(height) + L" -framerate " + std::to_wstring(settings.fps)
        + L" -i pipe:0 -an " + Utf8ToWide(options);
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE read = nullptr;
    if (!CreatePipe(&read, &g_input, &security, 0)) { g_error = "Could not create the encoder pipe."; return false; }
    SetHandleInformation(g_input, HANDLE_FLAG_INHERIT, 0);
    HANDLE log = CreateFileW((take + L"\\encoder.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        CloseHandle(read); CloseHandle(g_input); g_input = nullptr;
        g_error = "Could not create encoder.log in the output folder."; return false;
    }
    // Inherit only stdin and the log, never unrelated Steam/game handles.
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<BYTE> attributes(attributeBytes);
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    bool initialized = InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeBytes) != FALSE;
    HANDLE inherited[] = {read, log};
    bool configured = initialized && UpdateProcThreadAttribute(startup.lpAttributeList, 0,
        PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = read; startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = log;
    PROCESS_INFORMATION process{}; auto mutableCommand = command;
    const bool ok = configured && CreateProcessW(exe.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process) != FALSE;
    if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
    CloseHandle(read); if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) { CloseHandle(g_input); g_input = nullptr; g_error = "FFmpeg could not be started."; return false; }
    CloseHandle(process.hThread); g_process = process.hProcess;
    if (MvmDebugLog_Active()) MvmDebugLog_LinefAlways("deadlock.encoder", "%s", WideToUtf8(command).c_str());
    return true;
}
bool DeadlockEncoder_Write(const void* pixels, unsigned pitch, unsigned width, unsigned height, bool bgra) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_input || width != g_width || height != g_height || pitch < width * 4) return false;
    std::vector<BYTE> row;
    if (!bgra) row.resize(width * 4);
    for (unsigned y = 0; y < height; ++y) {
        const BYTE* source = static_cast<const BYTE*>(pixels) + y * pitch;
        if (!bgra) {
            for (unsigned x = 0; x < width; ++x) {
                row[x*4] = source[x*4+2]; row[x*4+1] = source[x*4+1];
                row[x*4+2] = source[x*4]; row[x*4+3] = 255;
            }
            source = row.data();
        }
        if (!WriteBytes(source, width * 4)) { g_error = "Encoder write failed; see encoder.log in the take folder."; return false; }
    }
    return true;
}
void DeadlockEncoder_Finish() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_input) { CloseHandle(g_input); g_input = nullptr; }
}
int DeadlockEncoder_Poll() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_process) return g_error.empty() ? 1 : -1;
    DWORD code = STILL_ACTIVE;
    if (!GetExitCodeProcess(g_process, &code)) code = 1;
    if (code == STILL_ACTIVE) return 0;
    CloseHandle(g_process); g_process = nullptr;
    if (g_input) { CloseHandle(g_input); g_input = nullptr; }
    if (code != 0) g_error = "FFmpeg failed; see encoder.log in the take folder.";
    return code == 0 && g_error.empty() ? 1 : -1;
}
const char* DeadlockEncoder_Error() { thread_local std::string copy; std::lock_guard<std::mutex> lock(g_mutex); copy = g_error; return copy.c_str(); }
}
