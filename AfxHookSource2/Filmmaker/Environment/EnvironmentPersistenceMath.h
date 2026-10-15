#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <map>

namespace Filmmaker::Environment {
// Wait for a rendered world, then a short run of advancing physical presents.
// A main-thread heartbeat alone must not release expensive scene restoration.
struct SceneRestoreGate {
    uint64_t firstPresent = 0, lastPresent = 0, startedAt = 0, progressedAt = 0;
    bool started = false;
    void Reset() { *this = {}; }
    bool Ready(bool worldReady, uint64_t presents, uint64_t now) {
        if (!worldReady || (started && (presents < lastPresent || now < progressedAt || now - progressedAt > 500)))
            Reset();
        if (!worldReady) return false;
        if (!started) {
            started = true; firstPresent = lastPresent = presents; startedAt = progressedAt = now;
            return false;
        }
        if (presents != lastPresent) { lastPresent = presents; progressedAt = now; }
        return now >= startedAt && now - startedAt >= 750 && presents - firstPresent >= 30;
    }
};
// The scene owner supplies only validated, committed snapshots. A cancelled drag
// must never enter this queue. Time is monotonic milliseconds, not demo time.
struct SceneSaveQueue {
    std::string map, saved, pending;
    uint64_t changedAt = 0, retryAt = 0;
    bool dirty = false;

    void Bind(std::string identity, std::string initial = {}) {
        map = std::move(identity);
        saved = std::move(initial);
        pending = saved;
        changedAt = retryAt = 0;
        dirty = false;
    }
    void Observe(const std::string& bytes, uint64_t now) {
        if (bytes == pending) return;
        pending = bytes;
        changedAt = now;
        dirty = pending != saved;
    }
    bool Due(uint64_t now, bool flush = false) const {
        return dirty && !map.empty() &&
            (flush || (now >= retryAt && now >= changedAt && now - changedAt >= 750));
    }
    void Complete(bool success, uint64_t now) {
        if (success) { saved = pending; dirty = false; retryAt = 0; }
        else retryAt = now + 5000;
    }
};
inline void BindSceneSaveQueue(SceneSaveQueue &active,std::map<std::string,SceneSaveQueue> &unsaved,const std::string& map) {
    if(active.dirty&&!active.map.empty())unsaved[active.map]=active;
    auto pending=unsaved.find(map);
    if(pending!=unsaved.end()){active=pending->second;unsaved.erase(pending);}
    else active.Bind(map);
}
}
