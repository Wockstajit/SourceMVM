#include <Windows.h>
#include <vector>

namespace {
thread_local std::vector<DWORD> renameErrors;
thread_local size_t renameCalls = 0;
BOOL RenameWithFaults(LPCWSTR source, LPCWSTR target, DWORD flags) {
	const size_t call = renameCalls++;
	if (call < renameErrors.size()) {
		SetLastError(renameErrors[call]);
		return FALSE;
	}
	return MoveFileExW(source, target, flags);
}
}
// Exercise the real helper and real files, injecting only the OS rename error boundary.
#define MoveFileExW RenameWithFaults
#include "../../AfxHookSource2/Filmmaker/Platform/AtomicFile.h"
#undef MoveFileExW

#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;
namespace {
int failed = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAILED line " << __LINE__ << ": " #x "\n"; ++failed; } } while (0)
std::string Read(const fs::path& path) {
	std::ifstream in(path, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}
}
int main() {
	const auto folder = fs::current_path() / ("AtomicFileTests-" + std::to_string(GetCurrentProcessId())
		+ "-" + std::to_string(GetTickCount64()));
	if (!fs::create_directory(folder)) return 1;
	const auto target = folder / L"preferences.json";
	CHECK(Filmmaker::WriteFileAtomic(target.wstring(), "original") == ERROR_SUCCESS);
	CHECK(Read(target) == "original");
	// A reader that denies rename must prevent replacement without truncating the old data.
	HANDLE locked = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	CHECK(locked != INVALID_HANDLE_VALUE);
	const DWORD lockedError = Filmmaker::WriteFileAtomic(target.wstring(), "replacement");
	std::cout << "locked replacement error=" << lockedError << "\n";
	CHECK(lockedError == ERROR_SHARING_VIOLATION || lockedError == ERROR_ACCESS_DENIED);
	CHECK(Read(target) == "original");
	CHECK(std::distance(fs::directory_iterator(folder), fs::directory_iterator()) == 1);
	if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
	CHECK(Filmmaker::WriteFileAtomic(target.wstring(), "replacement") == ERROR_SUCCESS);
	CHECK(Read(target) == "replacement");
	const DWORD emptyError = Filmmaker::WriteFileAtomic(target.wstring(), "");
	if (emptyError != ERROR_SUCCESS) std::cerr << "empty replacement error=" << emptyError << "\n";
	CHECK(emptyError == ERROR_SUCCESS);
	CHECK(fs::file_size(target) == 0);
	// Transient failures retry the same complete temporary file; a hard failure or persistent
	// lock must leave the last successful payload intact and remove the unpublished temporary.
	renameErrors = { ERROR_ACCESS_DENIED, ERROR_SHARING_VIOLATION }; renameCalls = 0;
	CHECK(Filmmaker::WriteFileAtomic(target.wstring(), "after contention") == ERROR_SUCCESS);
	CHECK(renameCalls >= 3);
	CHECK(Read(target) == "after contention");
	renameErrors.assign(32, ERROR_ACCESS_DENIED); renameCalls = 0;
	CHECK(Filmmaker::WriteFileAtomic(target.wstring(), "must not publish") == ERROR_ACCESS_DENIED);
	CHECK(renameCalls > 0 && renameCalls <= 8);
	CHECK(Read(target) == "after contention");
	CHECK(std::distance(fs::directory_iterator(folder), fs::directory_iterator()) == 1);
	renameErrors = { ERROR_DISK_FULL }; renameCalls = 0;
	CHECK(Filmmaker::WriteFileAtomic(target.wstring(), "must not publish") == ERROR_DISK_FULL);
	CHECK(renameCalls == 1);
	CHECK(Read(target) == "after contention");
	CHECK(std::distance(fs::directory_iterator(folder), fs::directory_iterator()) == 1);
	renameErrors.clear(); renameCalls = 0;
	CHECK(Filmmaker::WriteFileAtomic((folder / L"missing" / L"file").wstring(), "x") == ERROR_PATH_NOT_FOUND);
	CHECK(Filmmaker::WriteFileAtomic(folder.wstring(), "x") != ERROR_SUCCESS);
	CHECK(fs::is_directory(folder));
	// A stale conventional temp file belongs to somebody else and must remain untouched.
	const auto unrelated = fs::path(target.wstring() + L".tmp");
	{ std::ofstream out(unrelated); out << "do not overwrite"; }
	std::vector<std::thread> writers;
	std::vector<DWORD> errors(8);
	for (int i = 0; i < 8; ++i) writers.emplace_back([&, i] {
		errors[i] = Filmmaker::WriteFileAtomic(target.wstring(), std::string(32768, (char)('a' + i)));
	});
	for (auto& writer : writers) writer.join();
	bool succeeded = false;
	for (DWORD error : errors) {
		CHECK(error == ERROR_SUCCESS || error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED);
		succeeded = succeeded || error == ERROR_SUCCESS;
	}
	CHECK(succeeded);
	const auto result = Read(target);
	CHECK(result.size() == 32768);
	CHECK(!result.empty() && result[0] >= 'a' && result[0] <= 'h');
	CHECK(!result.empty() && result == std::string(result.size(), result[0]));
	CHECK(Read(unrelated) == "do not overwrite");
	CHECK(std::distance(fs::directory_iterator(folder), fs::directory_iterator()) == 2);
	fs::remove_all(folder); // only the unique directory this test created
	std::cout << "atomic file: " << failed << " failure(s)\n";
	return failed ? 1 : 0;
}
