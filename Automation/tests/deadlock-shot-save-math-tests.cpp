#include "../../AfxHookSource2/Filmmaker/Deadlock/DeadlockShotSaveMath.h"
#include <cstdio>
#include <map>

int main() {
    int failures = 0, calls = 0;
    auto check = [&](bool ok, const char* reason) {
        if (!ok) { ++failures; std::printf("FAIL %s\n", reason); }
    };
    Filmmaker::DeadlockShotSaves saves;
    std::map<std::wstring, std::string> disk;
    bool failA = true;
    auto writer = [&](const std::wstring& demo, const std::string& payload) {
        ++calls;
        if (demo == L"A.dem" && failA) return false;
        disk[demo] = payload;
        return true;
    };
    saves.Stage(L"", "no destination");
    check(saves.Count() == 0, "menu edits never latch an empty save destination");
    std::string a = "A fov=55";
    saves.Stage(L"A.dem", a);
    check(!saves.FlushOne(100, writer).succeeded && calls == 1, "A write fails");
    a = "B fov=110";
    saves.Stage(L"B.dem", a);
    check(saves.FlushOne(101, writer).succeeded, "B saves while A is delayed");
    check(disk[L"B.dem"] == "B fov=110", "B has its own payload");
    check(*saves.Pending(L"A.dem") == "A fov=55", "A retains its immutable snapshot");
    for (int i = 0; i < 1000; ++i) saves.Stage(L"a.DEM", "A fov=65");
    check(saves.Count() == 1 && saves.Bytes() == 8, "case aliases and repeated edits coalesce");
    check(!saves.FlushOne(5099, writer).attempted() && calls == 2, "edits preserve failure backoff");
    check(!saves.SaveNow(L"A.dem", 200, writer).succeeded && calls == 3, "explicit retry may fail again");
    saves.Stage(L"C.dem", "C newest");
    check(saves.SaveNow(L"C.dem", 201, writer).succeeded, "explicit C save succeeds independently");
    check(saves.Count() == 1 && *saves.Pending(L"A.dem") == "A fov=65", "C save cannot discard A");
    failA = false;
    check(!saves.FlushOne(5199, writer).attempted(), "explicit failure re-arms A delay");
    check(saves.FlushOne(5200, writer).succeeded, "A recovers at its deadline");
    check(disk[L"A.dem"] == "A fov=65" && disk[L"B.dem"] == "B fov=110", "recovery keeps destinations isolated");
    check(saves.Count() == 0 && saves.Bytes() == 0 && !saves.Pending(L"A.dem"), "successful saves release snapshots");
    check(!saves.FlushOne(6000, writer).attempted(), "empty queue performs no disk IO");
    std::printf("DeadlockShotSaves: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
