#pragma once

#include <Windows.h>
#include <atomic>
#include <string>

namespace Filmmaker {

// Publish only a complete, flushed sibling file. An unsuccessful operation leaves the target
// intact and removes its own temporary file. Unique CREATE_NEW names avoid sharing another
// writer's temporary file or following an existing temporary-file link.
// Returns a Win32 error (ERROR_SUCCESS on completion). The parent directory must already exist.
inline DWORD WriteFileAtomic(const std::wstring& path, const std::string& bytes) {
	if (bytes.size() > MAXDWORD) return ERROR_FILE_TOO_LARGE;
	static std::atomic<unsigned long long> sequence{ 0 };
	std::wstring temporary;
	HANDLE file = INVALID_HANDLE_VALUE;
	for (int attempt = 0; attempt < 16; ++attempt) {
		temporary = path + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"."
			+ std::to_wstring(sequence.fetch_add(1));
		file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file != INVALID_HANDLE_VALUE) break;
		const DWORD error = GetLastError();
		if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) return error;
	}
	if (file == INVALID_HANDLE_VALUE) return ERROR_FILE_EXISTS;
	DWORD written = 0;
	DWORD error = ERROR_SUCCESS;
	if (!WriteFile(file, bytes.data(), (DWORD)bytes.size(), &written, nullptr)) error = GetLastError();
	else if (written != bytes.size()) error = ERROR_WRITE_FAULT;
	else if (!FlushFileBuffers(file)) error = GetLastError();
	if (!CloseHandle(file) && error == ERROR_SUCCESS) error = GetLastError();
	if (error == ERROR_SUCCESS) {
		// A just-closed reader or another replacing writer can briefly deny a rename. Retry only
		// access/sharing failures, with a small bounded delay; a persistent lock still fails closed.
		for (int attempt = 0; attempt < 8; ++attempt) {
			if (MoveFileExW(temporary.c_str(), path.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { error = ERROR_SUCCESS; break; }
			error = GetLastError();
			if ((error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) || attempt == 7) break;
			Sleep(1);
		}
	}
	if (error != ERROR_SUCCESS) DeleteFileW(temporary.c_str());
	return error;
}

} // namespace Filmmaker
