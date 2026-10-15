#pragma once

#include <cstddef>
#include <map>
#include <string>

namespace Filmmaker {

// Bound both map nodes and retained string payloads. The caller writes a new event immediately;
// this cache keeps only the metadata needed to summarize its later repeats.
template<class Event>
class DebugLogDedupCache {
public:
    using Events = std::map<std::string, Event>;
    explicit DebugLogDedupCache(size_t maxEntries = 4096, size_t maxPayloadBytes = 4 * 1024 * 1024)
        : maxEntries_(maxEntries), maxPayloadBytes_(maxPayloadBytes) {}

    Event* Find(const std::string& identity) {
        const auto found = events_.find(identity);
        return found == events_.end() ? nullptr : &found->second;
    }

    template<class FlushWindow>
    Event* Retain(const std::string& identity, size_t eventPayloadBytes, FlushWindow flushWindow) {
        if (auto* found = Find(identity)) return found;
        // Oversized messages still reach the file; they simply bypass repeat retention.
        if (!maxEntries_ || identity.size() > maxPayloadBytes_ ||
            eventPayloadBytes > maxPayloadBytes_ - identity.size()) return nullptr;
        const size_t bytes = identity.size() + eventPayloadBytes;
        if (events_.size() >= maxEntries_ || bytes > maxPayloadBytes_ - payloadBytes_) {
            flushWindow(events_);
            Clear();
        }
        auto inserted = events_.try_emplace(identity);
        payloadBytes_ += bytes;
        return &inserted.first->second;
    }

    const Events& All() const { return events_; }
    size_t Size() const { return events_.size(); }
    size_t PayloadBytes() const { return payloadBytes_; }
    void Clear() { events_.clear(); payloadBytes_ = 0; }

private:
    Events events_;
    size_t payloadBytes_ = 0;
    size_t maxEntries_, maxPayloadBytes_;
};

} // namespace Filmmaker
