#pragma once

#include <cstdint>
#include <string>

namespace Filmmaker {

// One active identity, bounded in both storage and staleness. Callers serialize access.
// The engine's raw path is still checked on every call: IDemoFile pointers are reused.
// Our writers bump revision immediately; the deadline also catches external file edits.
template<class Key>
class DemoPathCache {
public:
    template<class Resolve>
    const std::wstring& Get(const Key& key, uintptr_t owner, uint64_t revision,
        uint64_t nowMs, Resolve resolve) {
        if (!m_valid || key != m_key || owner != m_owner || revision != m_revision
            || nowMs < m_checkedMs || nowMs - m_checkedMs >= 1000) {
            m_value = resolve();
            m_key = key;
            m_owner = owner;
            m_revision = revision;
            m_checkedMs = nowMs;
            m_valid = true;
        }
        return m_value;
    }

    void Clear() { m_valid = false; m_key = {}; m_value.clear(); }

private:
    Key m_key{};
    std::wstring m_value;
    uintptr_t m_owner = 0;
    uint64_t m_revision = 0, m_checkedMs = 0;
    bool m_valid = false;
};

} // namespace Filmmaker
