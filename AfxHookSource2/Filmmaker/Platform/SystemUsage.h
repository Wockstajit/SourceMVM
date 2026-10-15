#pragma once

// Machine and process load for the always-on heartbeat (FilmmakerHealth): the CPU model and RAM of
// the machine, and cs2.exe's own RAM, CPU and GPU use. A crash or "it froze" report then shows
// whether something spiked before it. Kernel32/PDH only; PDH is loaded at runtime.

#include <string>

namespace Filmmaker {

struct SystemUsage {
	std::string cpuName;                 // "AMD Ryzen 7 5800X3D 8-Core Processor"
	unsigned logicalCpus = 0;
	unsigned long long ramTotalMb = 0, ramFreeMb = 0;
	unsigned long long gameWorkingSetMb = 0, gamePrivateMb = 0;
	double gameCpuPercent = -1;          // of the whole machine; -1 until two samples exist
	double systemCpuPercent = -1;
	double gameGpuPercent = -1;          // busiest GPU engine of this process (3D, compute, copy...); -1 unavailable
	double sampleMs = 0;                 // what taking this sample cost (its own thread, never the game's)
};

// The newest sample (any thread). A background thread refreshes it every 5 s, so rates (CPU, GPU)
// cover the last 5 s; all fields are at their defaults until the first sample lands.
SystemUsage SystemUsage_Latest();

} // namespace Filmmaker
