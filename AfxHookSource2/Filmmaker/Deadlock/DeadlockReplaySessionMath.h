#pragma once

#include "../Demo/DemoProjectStoreMath.h"
#include <utility>

namespace Filmmaker {

// The caller already debounces replay unload. An unavailable filename during
// that interval is a seek/load gap, not permission to discard the current shot.
class DeadlockReplayIdentity {
public:
    bool Observe(bool loaded, const std::wstring& filename) {
        if (loaded && filename.empty()) return false;
        const std::wstring next = loaded ? filename : std::wstring();
        if (WideEqualsI(m_path, next)) return false;
        m_path = next;
        return true;
    }
    const std::wstring& Path() const { return m_path; }
private:
    std::wstring m_path;
};

// Session-local authored state without a disk format (currently graph edits and
// undo history). Keep it with its replay, never with camera ownership or gestures.
// Moving the active state out and back avoids copies of its curve/undo buffers.
template<typename State>
class DeadlockReplaySessions {
public:
    State& Current() { return m_current; }
    size_t CachedCount() const { return m_saved.size(); }
    void Switch(const std::wstring& path) {
        if (WideEqualsI(m_path, path)) return;
        if (!m_path.empty()) m_saved.push_back({m_path, std::move(m_current)});
        m_current = State();
        for (auto it = m_saved.begin(); it != m_saved.end(); ++it) {
            if (!WideEqualsI(it->first, path)) continue;
            m_current = std::move(it->second);
            m_saved.erase(it);
            break;
        }
        m_path = path;
    }
private:
    std::wstring m_path;
    State m_current;
    std::vector<std::pair<std::wstring, State>> m_saved;
};

} // namespace Filmmaker
