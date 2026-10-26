#include "stdafx.h"

#include "SourceMainThread.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>

#include <windows.h>

#include <atomic>
#include <mutex>
#include <vector>

namespace SourceFilmmaker {

namespace {
std::atomic<DWORD> g_mainThread{ 0 };
std::mutex g_mutex;
std::vector<std::function<void()>> g_posted;
} // namespace

void MainThread_Frame() {
	g_mainThread.store(GetCurrentThreadId());
	std::vector<std::function<void()>> run;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		run.swap(g_posted);
	}
	for (std::function<void()>& fn : run) fn();
}

void MainThread_Run(std::function<void()> fn) {
	const DWORD main = g_mainThread.load();
	if (main == 0 || main == GetCurrentThreadId()) {
		fn();
		return;
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	g_posted.push_back(std::move(fn));
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("mainthread", "input from thread %lu posted", GetCurrentThreadId());
}

} // namespace SourceFilmmaker
