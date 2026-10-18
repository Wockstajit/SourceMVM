#include "../../AfxHookSource2/Filmmaker/Demo/DemoPathCacheMath.h"

#include <iostream>

int main() {
    Filmmaker::DemoPathCache<std::string> cache;
    int reads = 0, failures = 0;
    auto resolve = [&] { ++reads; return std::wstring(L"canonical-") + std::to_wstring(reads); };
    auto expect = [&](bool ok, const char* message) {
        if (!ok) { std::cerr << message << '\n'; ++failures; }
    };
    cache.Get("first.dem", 1, 0, 100, resolve);
    for (int i = 101; i < 1100; ++i) cache.Get("first.dem", 1, 0, i, resolve);
    expect(reads == 1, "unchanged per-frame reads must not hit the filesystem");
    expect(cache.Get("second.dem", 1, 0, 1099, resolve) == L"canonical-2",
        "same engine object switching demos resolves immediately");
    cache.Get("second.dem", 2, 0, 1099, resolve);
    expect(reads == 3, "new demo instance with the same name resolves immediately");
    cache.Get("second.dem", 2, 1, 1099, resolve);
    expect(reads == 4, "project or marker publication invalidates immediately");
    cache.Get("second.dem", 2, 1, 2098, resolve);
    expect(reads == 4, "cache stays valid before its deadline");
    cache.Get("second.dem", 2, 1, 2099, resolve);
    expect(reads == 5, "external edits are picked up at the one-second deadline");
    cache.Clear();
    cache.Get("second.dem", 2, 1, 2099, resolve);
    expect(reads == 6, "stop and reopen cannot retain a previous demo identity");
    cache.Get("second.dem", 2, 1, 0, resolve);
    expect(reads == 7, "clock rollback expires rather than retaining indefinitely");
    cache.Clear();
    cache.Get("missing.dem", 2, 1, 0, [] { return std::wstring(); });
    expect(cache.Get("missing.dem", 2, 1, 1000, resolve) == L"canonical-8",
        "an initially missing file is retried");
    return failures ? 1 : 0;
}
