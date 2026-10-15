#pragma once

#include <filesystem>
#include <system_error>
#include <atomic>
#include <Windows.h>

namespace Filmmaker {

enum class MatchInfoRestore { Existing, Restored, Missing, Failed };

// Recover a Valve sidecar relocated by an earlier release. Never overwrite a current
// sidecar, never remove the recovery copy, and never create new project-side copies.
inline MatchInfoRestore RestoreMatchInfoIfMissing(const std::filesystem::path& archived,
    const std::filesystem::path& native) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(native, ec)) return MatchInfoRestore::Existing;
    if (!std::filesystem::is_regular_file(archived, ec)) return MatchInfoRestore::Missing;
    static std::atomic<unsigned> sequence{0};
    const auto temporary = std::filesystem::path(native.wstring() + L".mvm-recover-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" +
        std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)) + L".tmp");
    // Publish only a complete copy. A failed/interrupted copy cannot leave a truncated native
    // sidecar; MoveFileEx without REPLACE_EXISTING also preserves a file CS2 creates mid-copy.
    if (!CopyFileW(archived.c_str(), temporary.c_str(), TRUE)) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) DeleteFileW(temporary.c_str());
        return MatchInfoRestore::Failed;
    }
    if (MoveFileExW(temporary.c_str(), native.c_str(), MOVEFILE_WRITE_THROUGH))
        return MatchInfoRestore::Restored;
    const DWORD error = GetLastError();
    DeleteFileW(temporary.c_str());
    // CS2 may have created its own sidecar between the check and the exclusive copy.
    if ((error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) &&
        std::filesystem::is_regular_file(native, ec)) return MatchInfoRestore::Existing;
    return MatchInfoRestore::Failed;
}

} // namespace Filmmaker
