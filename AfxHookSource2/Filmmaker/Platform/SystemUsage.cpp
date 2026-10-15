#include "SystemUsage.h"

#include <Windows.h>
#include <Psapi.h>
#include <Pdh.h>
#include <PdhMsg.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

namespace Filmmaker {

namespace {

std::string ReadCpuName() {
	char name[256] = {};
	DWORD size = sizeof(name);
	if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
	                 "ProcessorNameString", RRF_RT_REG_SZ, nullptr, name, &size) != ERROR_SUCCESS)
		return "?";
	std::string s(name);
	s.erase(0, s.find_first_not_of(' '));
	while (!s.empty() && s.back() == ' ') s.pop_back();
	return s;
}

unsigned long long FileTimeTicks(const FILETIME& ft) {
	return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// The "GPU Engine" counters Task Manager uses: one instance per engine of each process, named
// "pid_<pid>_luid_..._engtype_3D". Loaded at runtime so the hook does not link pdh.lib.
struct GpuCounter {
	using OpenQuery_t = PDH_STATUS(WINAPI*)(LPCWSTR, DWORD_PTR, PDH_HQUERY*);
	using AddCounter_t = PDH_STATUS(WINAPI*)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*);
	using Collect_t = PDH_STATUS(WINAPI*)(PDH_HQUERY);
	using GetArray_t = PDH_STATUS(WINAPI*)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PPDH_FMT_COUNTERVALUE_ITEM_W);
	bool tried = false, ready = false;
	PDH_HQUERY query = nullptr;
	PDH_HCOUNTER counter = nullptr;
	Collect_t collect = nullptr;
	GetArray_t getArray = nullptr;

	void Init() {
		tried = true;
		HMODULE pdh = LoadLibraryW(L"pdh.dll");
		if (!pdh) return;
		auto open = reinterpret_cast<OpenQuery_t>(GetProcAddress(pdh, "PdhOpenQueryW"));
		auto add = reinterpret_cast<AddCounter_t>(GetProcAddress(pdh, "PdhAddEnglishCounterW"));
		collect = reinterpret_cast<Collect_t>(GetProcAddress(pdh, "PdhCollectQueryData"));
		getArray = reinterpret_cast<GetArray_t>(GetProcAddress(pdh, "PdhGetFormattedCounterArrayW"));
		if (!open || !add || !collect || !getArray) return;
		if (open(nullptr, 0, &query) != ERROR_SUCCESS) return;
		wchar_t path[128];
		swprintf(path, 128, L"\\GPU Engine(pid_%lu_*)\\Utilization Percentage", GetCurrentProcessId());
		if (add(query, path, 0, &counter) != ERROR_SUCCESS) return;
		collect(query); // the first sample: rates need two
		ready = true;
	}

	// The busiest engine type (summed over its instances), as Task Manager's GPU column shows.
	double Sample() {
		if (!tried) Init();
		if (!ready || collect(query) != ERROR_SUCCESS) return -1;
		DWORD bytes = 0, count = 0;
		if (getArray(counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != PDH_MORE_DATA || !bytes) return -1;
		std::vector<unsigned char> buffer(bytes);
		auto* items = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buffer.data());
		if (getArray(counter, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) return -1;
		std::map<std::wstring, double> byEngine;
		for (DWORD i = 0; i < count; ++i) {
			if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
			const std::wstring name = items[i].szName ? items[i].szName : L"";
			const size_t at = name.find(L"engtype_");
			byEngine[at == std::wstring::npos ? name : name.substr(at)] += items[i].FmtValue.doubleValue;
		}
		double busiest = 0;
		for (const auto& e : byEngine) busiest = (std::max)(busiest, e.second);
		return (std::min)(busiest, 100.0);
	}
};

GpuCounter g_gpu;
std::string g_cpuName;
unsigned long long g_lastWall = 0, g_lastProc = 0, g_lastSysIdle = 0, g_lastSysTotal = 0;

} // namespace

static SystemUsage SampleNow() {
	SystemUsage u;
	LARGE_INTEGER frequency, start, end;
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&start);
	if (g_cpuName.empty()) g_cpuName = ReadCpuName();
	u.cpuName = g_cpuName;
	SYSTEM_INFO si = {};
	GetSystemInfo(&si);
	u.logicalCpus = si.dwNumberOfProcessors;

	MEMORYSTATUSEX mem = {};
	mem.dwLength = sizeof(mem);
	if (GlobalMemoryStatusEx(&mem)) {
		u.ramTotalMb = mem.ullTotalPhys >> 20;
		u.ramFreeMb = mem.ullAvailPhys >> 20;
	}
	PROCESS_MEMORY_COUNTERS_EX pmc = {};
	if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
		u.gameWorkingSetMb = pmc.WorkingSetSize >> 20;
		u.gamePrivateMb = pmc.PrivateUsage >> 20;
	}

	FILETIME create, exit, kernel, user, now, sysIdle, sysKernel, sysUser;
	GetSystemTimeAsFileTime(&now);
	const unsigned long long wall = FileTimeTicks(now);
	if (GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user)) {
		const unsigned long long proc = FileTimeTicks(kernel) + FileTimeTicks(user);
		if (g_lastWall && wall > g_lastWall && u.logicalCpus)
			u.gameCpuPercent = 100.0 * double(proc - g_lastProc) / double(wall - g_lastWall) / double(u.logicalCpus);
		g_lastProc = proc;
	}
	if (GetSystemTimes(&sysIdle, &sysKernel, &sysUser)) {
		const unsigned long long idle = FileTimeTicks(sysIdle);
		const unsigned long long total = FileTimeTicks(sysKernel) + FileTimeTicks(sysUser); // kernel includes idle
		if (g_lastSysTotal && total > g_lastSysTotal)
			u.systemCpuPercent = 100.0 * (1.0 - double(idle - g_lastSysIdle) / double(total - g_lastSysTotal));
		g_lastSysIdle = idle;
		g_lastSysTotal = total;
	}
	g_lastWall = wall;
	u.gameGpuPercent = g_gpu.Sample();
	QueryPerformanceCounter(&end);
	u.sampleMs = double(end.QuadPart - start.QuadPart) * 1000.0 / double(frequency.QuadPart);
	return u;
}

// Sampled on its own thread: a PDH collection over every GPU engine on the machine can take tens of
// milliseconds, which on the main thread would be a hitch every few seconds.
SystemUsage SystemUsage_Latest() {
	static std::mutex s_mutex;
	static SystemUsage s_latest;
	static std::once_flag s_start;
	std::call_once(s_start, [] {
		std::thread([] {
			for (;;) {
				SystemUsage u = SampleNow();
				{ std::lock_guard<std::mutex> lock(s_mutex); s_latest = std::move(u); }
				Sleep(5000);
			}
		}).detach();
	});
	std::lock_guard<std::mutex> lock(s_mutex);
	return s_latest;
}

} // namespace Filmmaker
