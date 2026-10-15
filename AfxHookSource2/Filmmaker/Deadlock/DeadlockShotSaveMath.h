#pragma once

#include "../Demo/DemoProjectStoreMath.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Filmmaker {

// Main-thread pending snapshots. A failed save owns both its destination and its
// payload; subsequent edits to another replay cannot alter either. Retain only
// the newest snapshot per project, and release it only after a successful write.
class DeadlockShotSaves {
public:
    struct Attempt {
        std::wstring demo;
        size_t bytes = 0;
        bool succeeded = false;
        bool attempted() const { return !demo.empty(); }
    };

    void Stage(const std::wstring& demo, std::string payload) {
        if (demo.empty()) return;
        for (auto& entry : m_entries) {
            if (!WideEqualsI(entry.demo, demo)) continue;
            entry.payload = std::move(payload);
            return; // edits do not defeat the retry delay after a disk failure
        }
        m_entries.push_back({demo, std::move(payload), 0});
    }

    const std::string* Pending(const std::wstring& demo) const {
        for (const auto& entry : m_entries)
            if (WideEqualsI(entry.demo, demo)) return &entry.payload;
        return nullptr;
    }

    size_t Count() const { return m_entries.size(); }
    size_t Bytes() const {
        size_t total = 0;
        for (const auto& entry : m_entries) total += entry.payload.size();
        return total;
    }

    // At most one disk attempt per frame. A failing project enters its own delay,
    // so it cannot starve other eligible projects or retry on every slider event.
    template<typename Writer>
    Attempt FlushOne(std::uint64_t now, Writer write) {
        for (size_t i = 0; i < m_entries.size(); ++i)
            if (now >= m_entries[i].retryAt) return Write(i, now, write);
        return {};
    }

    // Explicit save bypasses only this project's delay.
    template<typename Writer>
    Attempt SaveNow(const std::wstring& demo, std::uint64_t now, Writer write) {
        for (size_t i = 0; i < m_entries.size(); ++i)
            if (WideEqualsI(m_entries[i].demo, demo)) return Write(i, now, write);
        return {};
    }

private:
    struct Entry {
        std::wstring demo;
        std::string payload;
        std::uint64_t retryAt;
    };
    std::vector<Entry> m_entries;

    template<typename Writer>
    Attempt Write(size_t i, std::uint64_t now, Writer write) {
        auto& entry = m_entries[i];
        Attempt result{entry.demo, entry.payload.size(), write(entry.demo, entry.payload)};
        if (result.succeeded) m_entries.erase(m_entries.begin() + i);
        else entry.retryAt = now + 5000;
        return result;
    }
};

} // namespace Filmmaker
