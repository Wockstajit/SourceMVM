// Where a running process spends its busiest thread's time, by stack sampling.
//
// `mirv_filmmaker health` says how many milliseconds the draw detours take; this says which
// functions and which source lines. About once a millisecond it suspends the process's busiest
// thread (CS2's render thread while a lit scene plays), walks its stack with dbghelp and resumes it.
// It needs no elevation for a process of the same user, and the PDB beside the hook DLL.
//
//   SampleThreadStacks <pid> <seconds> <symbol path> [module] [function,function,...]
//
//   <symbol path>  folder of the PDB that matches the DLL the game loaded, for example
//                  build/x64-source2-release/AfxHookSource2/Release (rebuilding replaces the PDB:
//                  sample before you rebuild, or keep a copy)
//   [module]       whose functions to report (default AfxHookSource2)
//   [functions]    name fragments; each gets a per-source-line breakdown
//
// Output, as shares of the samples: the module executing, self time by symbol, inclusive time of
// the module's functions, the module's innermost function with the module it had called into, and
// the per-line tables. A sample taken while the thread waits (in Present, for the GPU) counts too,
// so a thread that mostly waits shows as ntdll under the present detour: the GPU is the limit.
// Symbols of system and driver modules without a PDB resolve to the nearest export; read their
// module, not their name.
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

struct ThreadInfo { DWORD id; HANDLE handle; ULONGLONG cpu0 = 0, cpu1 = 0; };
static ULONGLONG ThreadCpu(HANDLE h) {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
    return (ULONGLONG(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (ULONGLONG(u.dwHighDateTime) << 32 | u.dwLowDateTime);
}
static HANDLE g_process;
static std::unordered_map<DWORD64, std::string> g_names;
static std::string Module(DWORD64 address) {
    IMAGEHLP_MODULE64 m = {}; m.SizeOfStruct = sizeof(m);
    if (SymGetModuleInfo64(g_process, address, &m)) return m.ModuleName;
    return "?";
}
static const std::string& Name(DWORD64 address) {
    auto found = g_names.find(address);
    if (found != g_names.end()) return found->second;
    char buffer[sizeof(SYMBOL_INFO) + 512] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = 511;
    DWORD64 displacement = 0;
    std::string text = Module(address) + "!";
    if (SymFromAddr(g_process, address, &displacement, symbol)) text += symbol->Name;
    else { char raw[32]; snprintf(raw, sizeof(raw), "0x%llx", address); text += raw; }
    return g_names.emplace(address, std::move(text)).first->second;
}

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "SampleThreadStacks <pid> <seconds> <symbol path> [module] [function,function,...]\n"); return 2; }
    const DWORD pid = strtoul(argv[1], nullptr, 10);
    const double seconds = atof(argv[2]);
    const std::string focus = argc > 4 ? argv[4] : "AfxHookSource2";
    g_process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!g_process) { fprintf(stderr, "OpenProcess failed %lu\n", GetLastError()); return 1; }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    if (!SymInitialize(g_process, argv[3], TRUE)) { fprintf(stderr, "SymInitialize failed %lu\n", GetLastError()); return 1; }

    std::vector<ThreadInfo> threads;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry = {}; entry.dwSize = sizeof(entry);
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
        if (entry.th32OwnerProcessID == pid) {
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
            if (h) threads.push_back({entry.th32ThreadID, h});
        }
    CloseHandle(snapshot);
    // Find the busiest thread over a one-second window, then sample only it.
    for (auto& t : threads) t.cpu0 = ThreadCpu(t.handle);
    Sleep(1000);
    for (auto& t : threads) t.cpu1 = ThreadCpu(t.handle);
    std::sort(threads.begin(), threads.end(), [](const ThreadInfo& a, const ThreadInfo& b) { return a.cpu1 - a.cpu0 > b.cpu1 - b.cpu0; });
    const size_t busy = (std::min)(threads.size(), size_t(1));
    printf("busiest thread (share of one core):");
    for (size_t i = 0; i < busy; ++i) printf(" %lu=%.0f%%", threads[i].id, (threads[i].cpu1 - threads[i].cpu0) / 100000.0);
    printf("\n");

    struct Tally { std::map<std::string, unsigned> self, inclusive, focusFirst, focusSelfLeaf, leafModule; unsigned samples = 0;
                   std::map<std::string, std::map<unsigned, unsigned>> lines; std::map<std::string, unsigned> lineTotals; };
    std::vector<std::string> watch;
    if (argc > 5) { std::string all = argv[5]; size_t at = 0; while (at <= all.size()) { size_t comma = all.find(',', at); if (comma == std::string::npos) comma = all.size(); if (comma > at) watch.push_back(all.substr(at, comma - at)); at = comma + 1; } }
    std::vector<Tally> tallies(busy);
    LARGE_INTEGER frequency, start, now; QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
    std::vector<DWORD64> stack;
    for (;;) {
        QueryPerformanceCounter(&now);
        if (double(now.QuadPart - start.QuadPart) / frequency.QuadPart >= seconds) break;
        for (size_t i = 0; i < busy; ++i) {
            auto& t = threads[i];
            if (SuspendThread(t.handle) == DWORD(-1)) continue;
            CONTEXT context = {}; context.ContextFlags = CONTEXT_FULL;
            stack.clear();
            if (GetThreadContext(t.handle, &context)) {
                STACKFRAME64 frame = {};
                frame.AddrPC.Offset = context.Rip; frame.AddrPC.Mode = AddrModeFlat;
                frame.AddrFrame.Offset = context.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
                frame.AddrStack.Offset = context.Rsp; frame.AddrStack.Mode = AddrModeFlat;
                for (int depth = 0; depth < 48; ++depth) {
                    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, g_process, t.handle, &frame, &context, nullptr,
                                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
                    if (!frame.AddrPC.Offset) break;
                    stack.push_back(frame.AddrPC.Offset);
                }
            }
            ResumeThread(t.handle);
            if (stack.empty()) continue;
            auto& tally = tallies[i];
            ++tally.samples;
            std::vector<const std::string*> names;
            for (DWORD64 a : stack) names.push_back(&Name(a));
            ++tally.self[*names[0]];
            ++tally.leafModule[Module(stack[0])];
            std::vector<const std::string*> unique = names;
            std::sort(unique.begin(), unique.end()); unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
            for (auto* n : unique) if (n->compare(0, focus.size(), focus) == 0) ++tally.inclusive[*n];
            // Per source line of the watched functions: the line each sample was executing or had called from.
            for (const auto& w : watch) {
                for (size_t f = 0; f < names.size(); ++f) {
                    if (names[f]->find(w) == std::string::npos) continue;
                    IMAGEHLP_LINE64 line = {}; line.SizeOfStruct = sizeof(line); DWORD displacement = 0;
                    const DWORD64 address = f ? stack[f] - 1 : stack[f];
                    const unsigned number = SymGetLineFromAddr64(g_process, address, &displacement, &line) ? line.LineNumber : 0;
                    ++tally.lines[w][number]; ++tally.lineTotals[w];
                    break;
                }
            }
            // Innermost frame in the focus module: which of our functions the time is under,
            // together with the module actually executing.
            for (size_t f = 0; f < names.size(); ++f)
                if (names[f]->compare(0, focus.size(), focus) == 0) {
                    ++tally.focusFirst[*names[f] + "  [leaf " + Module(stack[0]) + "]"];
                    break;
                }
        }
        Sleep(1);
    }
    auto print = [](const char* title, const std::map<std::string, unsigned>& values, unsigned total, size_t limit) {
        std::vector<std::pair<unsigned, std::string>> rows;
        for (auto& v : values) rows.push_back({v.second, v.first});
        std::sort(rows.rbegin(), rows.rend());
        printf("  %s\n", title);
        for (size_t i = 0; i < rows.size() && i < limit; ++i)
            printf("    %5.1f%%  %s\n", 100.0 * rows[i].first / total, rows[i].second.c_str());
    };
    for (size_t i = 0; i < busy; ++i) {
        auto& tally = tallies[i];
        if (!tally.samples) continue;
        printf("\nthread %lu: %u samples\n", threads[i].id, tally.samples);
        print("leaf module", tally.leafModule, tally.samples, 12);
        print("self", tally.self, tally.samples, 30);
        print("inclusive (focus module)", tally.inclusive, tally.samples, 45);
        print("innermost focus frame [executing module]", tally.focusFirst, tally.samples, 30);
        for (auto& w : tally.lines) {
            std::vector<std::pair<unsigned, unsigned>> rows;
            for (auto& l : w.second) rows.push_back({l.second, l.first});
            std::sort(rows.rbegin(), rows.rend());
            printf("  lines of %s (%.1f%% of thread)\n", w.first.c_str(), 100.0 * tally.lineTotals[w.first] / tally.samples);
            for (size_t r = 0; r < rows.size() && r < 28; ++r) printf("    %5.2f%%  line %u\n", 100.0 * rows[r].first / tally.samples, rows[r].second);
        }
    }
    SymCleanup(g_process);
    return 0;
}
