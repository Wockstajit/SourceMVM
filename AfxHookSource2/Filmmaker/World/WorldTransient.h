#pragma once
// Immutable transient values read by render backends. Authored module globals,
// CONFIG getters and persistence never see this layer.
#include <map>
#include <vector>
#include <string>
#include <memory>
#include <atomic>
namespace Filmmaker::WorldTransient {
struct State {
    std::map<std::string, std::vector<double>> fields;
    bool hasSky = false;
    std::string skyMaterial;
    bool operator==(const State& b) const { return fields == b.fields && hasSky == b.hasSky && skyMaterial == b.skyMaterial; }
};
inline std::shared_ptr<const State> published = std::make_shared<const State>();
inline std::shared_ptr<const State> Read() { return std::atomic_load(&published); }
inline bool Publish(State state) {
    if (*Read() == state) return false;
    std::atomic_store(&published, std::make_shared<const State>(std::move(state))); return true;
}
inline double Number(const char* id, double baseline, size_t component = 0) {
    const auto state = Read(); const auto f = state->fields.find(id);
    return f != state->fields.end() && component < f->second.size() ? f->second[component] : baseline;
}
inline bool Has(const char* id) { return Read()->fields.count(id) != 0; }
}
