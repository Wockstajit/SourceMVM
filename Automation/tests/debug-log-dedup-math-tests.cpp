#include "../../AfxHookSource2/Filmmaker/Debug/DebugLogDedupMath.h"
#include <cstdlib>
#include <limits>
#include <map>
#include <string>

static void Check(bool ok) { if (!ok) std::abort(); }
struct Event { unsigned count = 0; };

int main() {
    // Interleaved repeats remain attached to their first event until a bounded window rolls.
    Filmmaker::DebugLogDedupCache<Event> cache(8, 128);
    std::map<std::string, unsigned> written, expected;
    unsigned windows = 0;
    auto flush = [&](const auto& events) {
        ++windows;
        for (const auto& entry : events) written[entry.first] += entry.second.count;
    };
    for (unsigned i = 0; i < 20000; ++i) {
        const std::string key = std::to_string(i / 3);
        auto* event = cache.Retain(key, key.size(), flush);
        Check(event != nullptr);
        ++event->count;
        ++expected[key];
        if (i % 3 == 2) {
            const auto other = std::to_string(i / 3 + 1);
            auto* interleaved = cache.Retain(other, other.size(), flush);
            Check(interleaved != nullptr);
            ++interleaved->count;
            ++expected[other];
        }
        Check(cache.Size() <= 8 && cache.PayloadBytes() <= 128);
    }
    flush(cache.All());
    Check(windows > 100 && written == expected);

    // The byte budget can roll before the entry limit; a huge message bypasses retention
    // without disturbing pending summaries, including when its byte count would overflow.
    Filmmaker::DebugLogDedupCache<Event> bytes(100, 10);
    unsigned rolls = 0;
    auto countRoll = [&](const auto&) { ++rolls; };
    Check(bytes.Retain("abc", 3, countRoll) != nullptr);
    Check(bytes.PayloadBytes() == 6);
    Check(bytes.Retain("abc", 3, countRoll) == bytes.Find("abc"));
    Check(bytes.PayloadBytes() == 6 && rolls == 0);
    Check(bytes.Retain("12345678901", 0, countRoll) == nullptr);
    Check(bytes.Retain("x", std::numeric_limits<size_t>::max(), countRoll) == nullptr);
    Check(bytes.Size() == 1 && rolls == 0);
    Check(bytes.Retain("def", 3, countRoll) != nullptr);
    Check(rolls == 1 && bytes.Size() == 1 && bytes.Find("abc") == nullptr);
    bytes.Clear();
    Check(bytes.Size() == 0 && bytes.PayloadBytes() == 0);
    Filmmaker::DebugLogDedupCache<Event> disabled(0, 100);
    Check(disabled.Retain("x", 1, countRoll) == nullptr);
}
