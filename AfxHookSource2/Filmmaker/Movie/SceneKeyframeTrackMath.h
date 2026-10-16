#pragma once
#include <cctype>

#include "SceneKeyframeMath.h"
#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"
#include <map>
#include <set>
#include <limits>
#include <stdexcept>

namespace Filmmaker::SceneKeys {
struct Value {
    std::vector<double> numbers;
    std::string asset;
    bool operator==(const Value& b) const { return numbers == b.numbers && asset == b.asset; }
    bool operator!=(const Value& b) const { return !(*this == b); }
};
struct ChannelSpec {
    std::string id, label, family, group, groupId, source;
    ChannelType type = ChannelType::Float;
    double min = 0, max = 1;
    int components = 1, offset = 0;
    Value initial;
    bool Stepped() const { return type == ChannelType::Bool || type == ChannelType::Enum || type == ChannelType::SkyId; }
};
inline const char* TypeName(ChannelType t) {
    switch (t) {
    case ChannelType::Angle: return "angle";
    case ChannelType::Colour: return "colour";
    case ChannelType::Bool: return "bool";
    case ChannelType::Enum: return "enum";
    case ChannelType::SkyId: return "asset";
    default: return "float";
    }
}
inline bool ValidValue(const ChannelSpec& s, const Value& v) {
    if (s.type == ChannelType::SkyId) {
        if (!v.numbers.empty() || v.asset.size() > 128) return false;
        for (unsigned char c : v.asset) if (!(std::isalnum(c) || c == '_' || c == '-')) return false;
        return true;
    }
    if (!v.asset.empty() || v.numbers.size() != size_t(s.components)) return false;
    for (double n : v.numbers) if (!std::isfinite(n) || n < s.min || n > s.max ||
        (s.Stepped() && std::floor(n) != n)) return false;
    return true;
}
struct Key {
    int id = 0;
    double tick = 0;
    Value value;
    ControlPoint left, right;
    AngleDir direction = AngleDir::Shortest;
};
struct TrackData {
    std::map<std::string, std::vector<Key>> fields;
    std::set<std::string> armed;
    // Disabled properties: their keys stay, but nothing is applied (the graph's right-click
    // "Disable"). Saved with the keys; absent from a file = nothing disabled.
    std::set<std::string> muted;
    int nextId = 1;
};
struct Event {
    double tick = 0;
    std::vector<std::string> fields;
};
inline std::vector<Event> Events(const TrackData& data) {
    std::map<double, std::vector<std::string>> ticks;
    for (const auto& f : data.fields) for (const auto& k : f.second) ticks[k.tick].push_back(f.first);
    std::vector<Event> out;
    for (const auto& t : ticks) out.push_back({t.first, t.second});
    return out;
}
inline Value Sample(const ChannelSpec& s, const std::vector<Key>& keys, double tick, const Value& baseline, bool* active = nullptr) {
    if (active) *active = false;
    if (keys.size() < 2 || !std::isfinite(tick) || tick < keys.front().tick || tick > keys.back().tick) return baseline;
    if (active) *active = true;
    auto right = std::upper_bound(keys.begin(), keys.end(), tick, [](double t, const Key& k) { return t < k.tick; });
    if (right == keys.end()) return keys.back().value;
    const Key& b = *right; const Key& a = *(right - 1);
    if (s.Stepped() || tick == a.tick) return a.value;
    const double t = Clamp01((tick - a.tick) / (b.tick - a.tick));
    Value out = a.value;
    if (s.type == ChannelType::Colour) {
        const double ease = Clamp01(BezierValue(0, 1, t, a.right, b.left));
        LerpOklabRgb(a.value.numbers[0], a.value.numbers[1], a.value.numbers[2],
            b.value.numbers[0], b.value.numbers[1], b.value.numbers[2], ease,
            out.numbers[0], out.numbers[1], out.numbers[2]);
        if (s.components == 4) out.numbers[3] = Lerp(a.value.numbers[3], b.value.numbers[3], ease);
    } else if (s.type == ChannelType::Angle) {
        const double delta = AngleDelta(a.value.numbers[0], b.value.numbers[0], a.direction);
        out.numbers[0] = a.value.numbers[0] + BezierValue(0, delta, t, a.right, b.left);
        while (out.numbers[0] > s.max) out.numbers[0] -= 360;
        while (out.numbers[0] < s.min) out.numbers[0] += 360;
    } else out.numbers[0] = (std::max)(s.min, (std::min)(s.max, BezierValue(a.value.numbers[0], b.value.numbers[0], t, a.right, b.left)));
    return out;
}
inline void WriteValue(JsonBuilder& j, const Value& v, bool asset = false) {
    if (asset) j.ValueString(v.asset);
    else if (v.numbers.size() == 1) j.ValueDouble(v.numbers[0]);
    else { j.BeginArray(); for (double n : v.numbers) j.ValueDouble(n); j.EndArray(); }
}
inline void WriteHandle(JsonBuilder& j, const char* side, const ControlPoint& h) {
    j.Key(side); j.BeginObject(); j.BoolField("a", h.active); j.DoubleField("tx", h.tx); j.DoubleField("dv", h.dv); j.EndObject();
}
inline std::string Encode(const TrackData& data) {
    JsonBuilder j; j.BeginObject(); j.IntField("version", 1); j.IntField("nextId", data.nextId);
    j.Key("armed"); j.BeginArray(); for (const auto& id : data.armed) j.ValueString(id); j.EndArray();
    j.Key("muted"); j.BeginArray(); for (const auto& id : data.muted) j.ValueString(id); j.EndArray();
    j.Key("fields"); j.BeginObject();
    for (const auto& f : data.fields) {
        j.Key(f.first.c_str()); j.BeginArray();
        for (const auto& k : f.second) {
            j.BeginObject(); j.IntField("id", k.id); j.DoubleField("tick", k.tick);
            j.Key("value"); WriteValue(j, k.value, k.value.numbers.empty());
            WriteHandle(j, "left", k.left); WriteHandle(j, "right", k.right);
            j.IntField("direction", int(k.direction)); j.EndObject();
        }
        j.EndArray();
    }
    j.EndObject(); j.EndObject(); return j.Str();
}
inline const ChannelSpec* FindSpec(const std::vector<ChannelSpec>& catalog, const std::string& id) {
    for (const auto& s : catalog) if (s.id == id) return &s;
    return nullptr;
}
inline bool Decode(const std::string& text, const std::vector<ChannelSpec>& catalog, TrackData& out, std::string& error) {
    try {
        if (text.size() > 16 * 1024 * 1024) throw std::runtime_error("Scene key file exceeds 16 MiB");
        // Bound nesting before the generic recursive JSON parser touches untrusted sidecars.
        int depth = 0; bool quoted = false, escaped = false;
        for (char c : text) {
            if (quoted) { if (escaped) escaped = false; else if (c == '\\') escaped = true; else if (c == '"') quoted = false; }
            else if (c == '"') quoted = true;
            else if (c == '{' || c == '[') { if (++depth > 16) throw std::runtime_error("Scene key nesting exceeds limit"); }
            else if (c == '}' || c == ']') --depth;
        }
        JsonValue root;
        if (!JsonParse(text, root) || root.type != JsonValue::Type::Object) throw std::runtime_error("Malformed scene key JSON");
        auto field = [](const JsonValue& v, const char* name, JsonValue::Type type) -> const JsonValue& {
            auto p = v.Find(name); if (!p || p->type != type) throw std::runtime_error(std::string("Invalid scene key field: ") + name); return *p;
        };
        if (field(root, "version", JsonValue::Type::Number).number != 1) throw std::runtime_error("Unsupported scene key version");
        TrackData pending;
        for (const auto& a : field(root, "armed", JsonValue::Type::Array).arr) {
            if (a.type != JsonValue::Type::String || !FindSpec(catalog, a.str) || !pending.armed.insert(a.str).second) throw std::runtime_error("Invalid armed property");
        }
        if (const auto* muted = root.Find("muted")) {
            if (muted->type != JsonValue::Type::Array) throw std::runtime_error("Invalid scene key field: muted");
            for (const auto& m : muted->arr)
                if (m.type != JsonValue::Type::String || !FindSpec(catalog, m.str) || !pending.muted.insert(m.str).second) throw std::runtime_error("Invalid disabled property");
        }
        std::set<int> ids;
        for (const auto& f : field(root, "fields", JsonValue::Type::Object).obj) {
            const auto* spec = FindSpec(catalog, f.first);
            if (!spec || pending.fields.count(f.first) || f.second.type != JsonValue::Type::Array) throw std::runtime_error("Unknown or duplicate scene property");
            auto& keys = pending.fields[f.first];
            for (const auto& item : f.second.arr) {
                Key k;
                const double id = field(item, "id", JsonValue::Type::Number).number;
                if (!std::isfinite(id) || id < 1 || id >= INT32_MAX || id != std::floor(id) || !ids.insert(int(id)).second) throw std::runtime_error("Invalid or duplicate key id");
                k.id = int(id); pending.nextId = (std::max)(pending.nextId, k.id + 1);
                k.tick = field(item, "tick", JsonValue::Type::Number).number;
                if (!std::isfinite(k.tick) || k.tick < 0 || k.tick > INT32_MAX || k.tick != std::floor(k.tick)) throw std::runtime_error("Invalid key tick");
                const auto* value = item.Find("value"); if (!value) throw std::runtime_error("Missing key value");
                if (spec->type == ChannelType::SkyId && value->type != JsonValue::Type::String) throw std::runtime_error("Invalid sky identifier type");
                if (value->type == JsonValue::Type::String) k.value.asset = value->str;
                else if (value->type == JsonValue::Type::Number) k.value.numbers = {value->number};
                else if (value->type == JsonValue::Type::Array) for (const auto& n : value->arr) {
                    if (n.type != JsonValue::Type::Number) throw std::runtime_error("Invalid color component");
                    k.value.numbers.push_back(n.number);
                }
                if (!ValidValue(*spec, k.value)) throw std::runtime_error("Scene key value out of range: " + f.first);
                auto handle = [&](const char* side, ControlPoint& h) {
                    const auto& p = field(item, side, JsonValue::Type::Object);
                    h.active = field(p, "a", JsonValue::Type::Bool).boolean;
                    h.tx = field(p, "tx", JsonValue::Type::Number).number;
                    h.dv = field(p, "dv", JsonValue::Type::Number).number;
                    if (!std::isfinite(h.tx) || h.tx < 0 || h.tx > 1 || !std::isfinite(h.dv) || std::fabs(h.dv) > 1e9 || (spec->Stepped() && h.active)) throw std::runtime_error("Invalid Bezier handle");
                };
                handle("left", k.left); handle("right", k.right);
                double dir = field(item, "direction", JsonValue::Type::Number).number;
                if (dir < 0 || dir > 2 || dir != std::floor(dir)) throw std::runtime_error("Invalid angle direction");
                k.direction = AngleDir(int(dir)); keys.push_back(k);
            }
            std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.tick < b.tick; });
            for (size_t i = 1; i < keys.size(); ++i) if (keys[i-1].tick == keys[i].tick) throw std::runtime_error("Duplicate property tick");
        }
        double next = field(root, "nextId", JsonValue::Type::Number).number;
        if (!std::isfinite(next) || next < pending.nextId || next > INT32_MAX || next != std::floor(next)) throw std::runtime_error("Invalid next key id");
        pending.nextId = int(next); out = std::move(pending); error.clear(); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
class SceneKeyframeTrack {
public:
    TrackData data;
    std::set<int> selected;
    std::map<std::string, bool> hidden, solo;
    int revision = 1;
    void BeginEdit() { if (!editing) { before = data; editing = true; } }
    bool EndEdit() {
        if (!editing) return false;
        editing = false;
        if (Encode(before) == Encode(data)) return false;
        undo.push_back(std::move(before)); redo.clear(); ++revision; return true;
    }
    bool CanUndo() const { return !undo.empty(); }
    bool CanRedo() const { return !redo.empty(); }
    bool Undo() { EndEdit(); if (undo.empty()) return false; redo.push_back(data); data = std::move(undo.back()); undo.pop_back(); selected.clear(); ++revision; return true; }
    bool Redo() { EndEdit(); if (redo.empty()) return false; undo.push_back(data); data = std::move(redo.back()); redo.pop_back(); selected.clear(); ++revision; return true; }
    Key* Find(const std::string& id, int kid) {
        auto f = data.fields.find(id); if (f == data.fields.end()) return nullptr;
        for (auto& k : f->second) if (k.id == kid) return &k;
        return nullptr;
    }
    int Put(const ChannelSpec& s, double tick, const Value& value) {
        if (!ValidValue(s, value) || !std::isfinite(tick) || tick < 0 || tick > INT32_MAX || data.nextId == INT32_MAX) return 0;
        tick = std::round(tick);
        auto& keys = data.fields[s.id];
        for (auto& k : keys) if (k.tick == tick) { k.value = value; ++revision; return k.id; }
        Key k; k.id = data.nextId++; k.tick = tick; k.value = value;
        keys.push_back(k); std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.tick < b.tick; }); ++revision; return k.id;
    }
    void DeleteSelected() {
        for (auto f = data.fields.begin(); f != data.fields.end();) {
            auto& keys = f->second;
            keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const Key& k) { return selected.count(k.id) != 0; }), keys.end());
            if (keys.empty()) f = data.fields.erase(f); else ++f;
        }
        selected.clear(); ++revision;
    }
    // dt/dv are the TOTAL offset since the open edit began (BeginEdit), not a per-frame step. A
    // drag sends where the pointer is now relative to where it went down, so a step that is
    // refused (a collision) or clamped (a value bound) is not lost: the next frame is measured
    // from the same origin and the key lands back under the pointer. Per-frame deltas used to
    // drop every refused step for good, which left a dragged key drifting away from the cursor.
    // Outside an open edit the origin is the current data.
    void MoveSelected(const std::vector<ChannelSpec>& catalog, double dt, double dv) {
        if (!std::isfinite(dt) || !std::isfinite(dv)) return;
        // Preserve spacing and reject collisions atomically; no silent loss of sparse fields.
        TrackData pending = editing ? before : data;
        for (auto& f : pending.fields) {
            const auto* spec = FindSpec(catalog, f.first); if (!spec) continue;
            std::set<double> ticks;
            for (auto& k : f.second) {
                if (selected.count(k.id)) {
                    k.tick = std::round(k.tick + dt);
                    if (k.tick < 0 || k.tick > INT32_MAX) return;
                    if (!spec->Stepped() && spec->type != ChannelType::Colour)
                        k.value.numbers[0] = (std::max)(spec->min, (std::min)(spec->max, k.value.numbers[0] + dv));
                }
                if (!ticks.insert(k.tick).second) return;
            }
            std::sort(f.second.begin(), f.second.end(), [](const Key& a, const Key& b) { return a.tick < b.tick; });
        }
        data = std::move(pending); ++revision;
    }
private:
    bool editing = false;
    TrackData before;
    std::vector<TrackData> undo, redo;
};
} // namespace Filmmaker::SceneKeys
