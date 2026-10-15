// Ported from ValveResourceFormat 20.0 (MIT) ResourceTypes/EntityLump.cs, the
// ValveKeyValue KV3 text serializer it uses for property values, and the output
// shape of Automation/tools/LightingReferenceInspector.
#include "MapEntities.h"
#include "MapEntityKnownKeys.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace Filmmaker::MapData {
namespace {

std::string Lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = char(c | 0x20);
    return s;
}

// ----------------------------------------------------------------------------
// StringToken inverse lookup: the known entity keys plus every key the process
// has stored, matching VRF StringToken.InvertedTable (first writer wins).

std::unordered_map<uint32_t, std::string> &TokenTable() {
    static std::unordered_map<uint32_t, std::string> table = [] {
        std::unordered_map<uint32_t, std::string> t;
        for (const char *key : kEntityLumpKnownKeys) t.emplace(StringToken(key), key);
        return t;
    }();
    return table;
}

uint32_t TokenStore(const std::string &key) {
    const uint32_t token = StringToken(key);
    auto &table = TokenTable();
    if (table.find(token) == table.end()) table.emplace(token, key);
    return token;
}

std::string KnownString(uint32_t hash) {
    const auto &table = TokenTable();
    const auto it = table.find(hash);
    return it != table.end() ? it->second : "vrf_unknown_key_" + std::to_string(hash);
}

// ----------------------------------------------------------------------------
// KV3 text serializer port (ValveKeyValue KV3TextSerializer, SkipHeader output,
// newline "\n", indent "\t"), trimmed at the end as EntityLump.StringifyValue does.

const char *FlagName(uint8_t flag) {
    switch (flag) {
    case 1: return "resource";
    case 2: return "resource_name";
    case 3: return "panorama";
    case 4: return "soundevent";
    case 5: return "subclass";
    case 6: return "entity_name";
    default: return nullptr;
    }
}

// F6 (fixed six decimals) with trailing zeros trimmed, at least one decimal kept.
std::string KvDouble(double v) {
    if (!std::isfinite(v)) return v != v ? "nan" : (v > 0 ? "inf" : "-inf");
    char text[64];
    snprintf(text, sizeof text, "%.6f", v);
    std::string s = text;
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s += '0';
    return s;
}

bool NeedsQuoting(const std::string &key) {
    for (char c : key)
        if (c != '.' && c != '_' && !isalnum(static_cast<unsigned char>(c))) return true;
    return false;
}

void WriteKey(std::string &out, const std::string *key) {
    if (!key) return;
    if (!key->empty() && !isdigit(static_cast<unsigned char>((*key)[0])) && !NeedsQuoting(*key)) {
        out += *key;
    } else {
        out += '"';
        for (char c : *key) {
            switch (c) {
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\'': out += "\\'"; break;
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            default: out += c;
            }
        }
        out += '"';
    }
    out += " = ";
}

void WriteText(std::string &out, const std::string &text) {
    if (text.find('\n') != std::string::npos) {
        std::string multiline = text;
        for (size_t at = multiline.find("\r\n"); at != std::string::npos; at = multiline.find("\r\n", at))
            multiline.replace(at, 2, "\n");
        for (size_t at = multiline.find("\"\"\""); at != std::string::npos; at = multiline.find("\"\"\"", at + 4))
            multiline.replace(at, 3, "\\\"\"\"");
        out += "\"\"\"\n";
        out += multiline;
        out += "\n\"\"\"";
        return;
    }
    if (text.find_first_of("\n\t\\\"") == std::string::npos) {
        out += '"';
        out += text;
        out += '"';
        return;
    }
    out += '"';
    for (char c : text) {
        switch (c) {
        case '\t': out += "\\t"; break;
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        default: out += c;
        }
    }
    out += '"';
}

struct Kv3Writer {
    std::string out;
    int indent = 0;

    void Tabs() { out.append(size_t(indent), '\t'); }
    void Flag(uint8_t flag) {
        if (const char *name = FlagName(flag)) { out += name; out += ':'; }
    }

    static bool Simple(const Kv3 &v) {
        switch (v.type) {
        case Kv3::Type::Null:
        case Kv3::Type::Bool:
        case Kv3::Type::Int:
        case Kv3::Type::UInt:
        case Kv3::Type::Double:
            return true;
        default:
            return false;
        }
    }

    void Blob(const Kv3 &v, bool inArray) {
        const size_t n = v.blob.size();
        if (n <= 32) {
            out += "#[ ";
            for (size_t i = 0; i < n; ++i) {
                char hex[4];
                snprintf(hex, sizeof hex, "%02X", v.blob[i]);
                out += hex;
                if (i + 1 < n) out += ' ';
            }
            out += " ]";
            return;
        }
        if (inArray) {
            out += "#[ \n";
        } else {
            out += '\n';
            Tabs();
            out += "#[\n";
        }
        ++indent;
        Tabs();
        char hex[4];
        for (size_t i = 0; i + 1 < n; ++i) {
            snprintf(hex, sizeof hex, "%02X", v.blob[i]);
            out += hex;
            if ((i + 1) % 32 == 0) {
                out += '\n';
                Tabs();
            } else {
                out += ' ';
            }
        }
        snprintf(hex, sizeof hex, "%02X", v.blob[n - 1]);
        out += hex;
        --indent;
        out += '\n';
        Tabs();
        out += ']';
    }

    // A scalar value: flag prefix plus its text form.
    void Scalar(const Kv3 &v, bool inArray) {
        Flag(v.flag);
        switch (v.type) {
        case Kv3::Type::Null: out += "null"; break;
        case Kv3::Type::Bool: out += v.b ? "true" : "false"; break;
        case Kv3::Type::Int: out += std::to_string(v.i); break;
        case Kv3::Type::UInt: out += std::to_string(v.u); break;
        case Kv3::Type::Double: out += KvDouble(v.d); break;
        case Kv3::Type::String: WriteText(out, v.s); break;
        case Kv3::Type::Blob: Blob(v, inArray); break;
        default: break;
        }
    }

    void Emit(const Kv3 &v, const std::string *name, bool inArray) {
        if (v.type == Kv3::Type::Object) {
            Tabs();
            if (indent > 0) WriteKey(out, name);
            Flag(v.flag);
            if ((name || v.flag) && indent > 0) {
                out += '\n';
                Tabs();
            }
            out += '{';
            ++indent;
            out += '\n';
            for (const auto &[key, child] : v.obj) Emit(child, &key, false);
            --indent;
            Tabs();
            out += '}';
            if (inArray) out += ',';
            out += '\n';
            return;
        }
        if (v.type == Kv3::Type::Array) {
            const size_t count = v.arr.size();
            bool allSimple = true;
            for (const Kv3 &e : v.arr)
                if (!Simple(e)) { allSimple = false; break; }
            const bool isShort = count <= 4 && allSimple;
            Tabs();
            WriteKey(out, name);
            Flag(v.flag);
            if (isShort) {
                out += "[ ";
                for (size_t i = 0; i < count; ++i) {
                    Scalar(v.arr[i], false);
                    if (i + 1 < count) out += ", ";
                }
                out += " ]";
            } else {
                if (name || v.flag) {
                    out += '\n';
                    Tabs();
                }
                out += '[';
                ++indent;
                out += '\n';
                for (size_t i = 0; i < count; ++i) {
                    const Kv3 &e = v.arr[i];
                    const bool last = i + 1 == count;
                    if (Simple(e)) {
                        if (i % 4 == 0) Tabs();
                        Scalar(e, true);
                        out += ',';
                        if (!last && (i + 1) % 4 != 0) out += ' ';
                        else out += '\n';
                    } else if (e.type == Kv3::Type::Object || e.type == Kv3::Type::Array) {
                        Emit(e, nullptr, true);
                    } else {
                        Tabs();
                        Scalar(e, true);
                        out += ',';
                        out += '\n';
                    }
                }
                --indent;
                Tabs();
                out += ']';
            }
            if (inArray) out += ',';
            out += '\n';
            return;
        }
        // Scalar child of an object: "key = value\n"; root scalar: "value\n".
        if (name) {
            Tabs();
            WriteKey(out, name);
            Scalar(v, false);
            out += '\n';
        } else {
            Scalar(v, inArray);
            out += '\n';
        }
    }
};

// EntityLump.StringifyValue: KV3 text serialization of the value, trimmed.
std::string StringifyValue(const Kv3 &v) {
    Kv3Writer w;
    w.Emit(v, nullptr, false);
    std::string s = std::move(w.out);
    const auto whitespace = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; };
    size_t a = 0, b = s.size();
    while (a < b && whitespace(s[a])) ++a;
    while (b > a && whitespace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string KvTypeName(const Kv3 &v) {
    switch (v.type) {
    case Kv3::Type::Null: return "Null";
    case Kv3::Type::Bool: return "Boolean";
    case Kv3::Type::Int:
        switch (v.width) {
        case Kv3::Width::Int16: return "Int16";
        case Kv3::Width::Int32: return "Int32";
        default: return "Int64";
        }
    case Kv3::Type::UInt:
        switch (v.width) {
        case Kv3::Width::UInt16: return "UInt16";
        case Kv3::Width::UInt32: return "UInt32";
        default: return "UInt64";
        }
    case Kv3::Type::Double: return v.width == Kv3::Width::Float ? "FloatingPoint" : "FloatingPoint64";
    case Kv3::Type::String: return "String";
    case Kv3::Type::Blob: return "BinaryBlob";
    case Kv3::Type::Array: return "Array";
    case Kv3::Type::Object: return "Collection";
    }
    return "Null";
}

// Shortest round-trip formatting of a float, matching .NET's default float.ToString
// (invariant): minimal %g digits that read back to the same value, "E+NN" exponents.
std::string FloatToString(float v) {
    if (!std::isfinite(v)) return v != v ? "NaN" : (v > 0 ? "Infinity" : "-Infinity");
    char text[32];
    for (int precision = 1; precision <= 9; ++precision) {
        snprintf(text, sizeof text, "%.*g", precision, double(v));
        if (strtof(text, nullptr) == v) break;
    }
    std::string s = text;
    const size_t e = s.find('e');
    if (e != std::string::npos) s[e] = 'E';
    return s;
}

// EntityLump.ParseEntityProperties(byte[]) for lumps that still carry m_keyValuesData.
void ReadHashedValues(const std::vector<uint8_t> &bytes, std::map<std::string, Kv3> &out) {
    size_t at = 0;
    const auto need = [&](size_t n) {
        if (n > bytes.size() || at > bytes.size() - n) throw MapDataError("truncated entity data");
    };
    const auto u32 = [&] {
        need(4);
        uint32_t v;
        std::memcpy(&v, bytes.data() + at, 4);
        at += 4;
        return v;
    };
    const auto f32 = [&] {
        need(4);
        float v;
        std::memcpy(&v, bytes.data() + at, 4);
        at += 4;
        return v;
    };
    const auto f64 = [&] {
        need(8);
        double v;
        std::memcpy(&v, bytes.data() + at, 8);
        at += 8;
        return v;
    };
    const auto i64 = [&] {
        need(8);
        int64_t v;
        std::memcpy(&v, bytes.data() + at, 8);
        at += 8;
        return v;
    };
    const auto cstr = [&] {
        const size_t start = at;
        while (true) {
            need(1);
            if (!bytes[at++]) break;
        }
        return std::string(reinterpret_cast<const char *>(bytes.data() + start), at - start - 1);
    };
    if (u32() != 1) throw MapDataError("unsupported entity data version");
    const uint32_t hashed = u32(), named = u32();
    const auto typed = [&](uint32_t hash, const std::string *name) {
        Kv3 v;
        const uint32_t type = u32();
        switch (type) {
        case 0x6: v.type = Kv3::Type::Bool; need(1); v.b = bytes[at++] != 0; break;
        case 0x1: v.type = Kv3::Type::Double; v.d = f32(); break; // Float widens to double
        case 0x22: v.type = Kv3::Type::Double; v.d = f64(); break;
        case 0x9: { // Color32 -> array of four byte integers
            v.type = Kv3::Type::Array;
            need(4);
            for (int k = 0; k < 4; ++k) {
                Kv3 c;
                c.type = Kv3::Type::Int;
                c.i = bytes[at++];
                v.arr.push_back(c);
            }
            break;
        }
        case 0x5: v.type = Kv3::Type::Int; v.i = int32_t(u32()); break; // Integer -> long
        case 0x25: v.type = Kv3::Type::UInt; v.u = u32(); break; // UInt -> ulong
        case 0x1a: v.type = Kv3::Type::Int; v.i = i64(); break;
        case 0x21: v.type = Kv3::Type::UInt; v.u = i64(); break;
        case 0x3:
        case 0x27: { // Vector / QAngle -> "x y z" of round-trip floats
            const float x = f32(), y = f32(), z = f32();
            v.type = Kv3::Type::String;
            v.s = FloatToString(x) + " " + FloatToString(y) + " " + FloatToString(z);
            break;
        }
        case 0x1e: v.type = Kv3::Type::String; v.s = cstr(); break;
        default: throw MapDataError("unknown entity field type " + std::to_string(type));
        }
        if (!name) {
            const std::string known = KnownString(hash);
            out[known] = std::move(v);
        } else {
            if (TokenStore(*name) != hash)
                throw MapDataError("entity key hash mismatch for " + *name);
            out[*name] = std::move(v);
        }
    };
    for (uint32_t k = 0; k < hashed; ++k) typed(u32(), nullptr);
    for (uint32_t k = 0; k < named; ++k) {
        const uint32_t hash = u32();
        const std::string name = cstr();
        typed(hash, &name);
    }
}

// System.Text.Json default escaping, needed only for content-hash identities.
std::string StjEscape(const std::string &utf8) {
    std::string out;
    for (size_t k = 0; k < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8[k]);
        uint32_t cp = c;
        size_t len = 1;
        if (c >= 0xF0 && k + 3 < utf8.size()) {
            cp = (c & 7u) << 18 | (utf8[k + 1] & 63u) << 12 | (utf8[k + 2] & 63u) << 6 | (utf8[k + 3] & 63u);
            len = 4;
        } else if (c >= 0xE0 && k + 2 < utf8.size()) {
            cp = (c & 15u) << 12 | (utf8[k + 1] & 63u) << 6 | (utf8[k + 2] & 63u);
            len = 3;
        } else if (c >= 0xC0 && k + 1 < utf8.size()) {
            cp = (c & 31u) << 6 | (utf8[k + 1] & 63u);
            len = 2;
        }
        k += len;
        char hex[16];
        const auto unit = [&](uint32_t u) {
            snprintf(hex, sizeof hex, "\\u%04X", u);
            out += hex;
        };
        if (cp == '\\') out += "\\\\";
        else if (cp == '\n') out += "\\n";
        else if (cp == '\r') out += "\\r";
        else if (cp == '\t') out += "\\t";
        else if (cp == '\b') out += "\\b";
        else if (cp == '\f') out += "\\f";
        else if (cp < 0x20 || cp == '"' || cp == '<' || cp == '>' || cp == '&' || cp == '\'' || cp == '+' || cp == '`' ||
                 cp > 0x7E) {
            if (cp > 0xFFFF) {
                const uint32_t v = cp - 0x10000;
                unit(0xD800 + (v >> 10));
                unit(0xDC00 + (v & 0x3FF));
            } else {
                unit(cp);
            }
        } else {
            out += char(cp);
        }
    }
    return out;
}

std::string PropertiesJson(const std::vector<EntityProperty> &properties) {
    std::string out = "{";
    for (size_t k = 0; k < properties.size(); ++k) {
        const auto &p = properties[k];
        if (k) out += ",";
        out += "\"" + StjEscape(p.key) + "\":{\"type\":\"" + StjEscape(p.type) + "\",\"value\":\"" +
               StjEscape(p.value) + "\"}";
    }
    return out + "}";
}

std::string UpperHex(std::string hex) {
    for (char &c : hex)
        if (c >= 'a' && c <= 'f') c = char(c - 32);
    return hex;
}

bool Related(const std::string &kind) {
    if (kind.rfind("env_", 0) == 0) return true;
    for (const char *token : {"light", "probe", "beam", "cubemap", "sky", "fog", "glow", "exposure", "tonemap",
                              "color_correction"})
        if (Lower(kind).find(token) != std::string::npos) return true;
    return false;
}

std::string VecJson(const std::array<float, 3> &v) {
    return "[" + JsonNumber(v[0]) + ", " + JsonNumber(v[1]) + ", " + JsonNumber(v[2]) + "]";
}

} // namespace

std::string KnownEntityString(uint32_t hash) { return KnownString(hash); }

const EntityProperty *MapEntity::Find(const char *key) const {
    for (const auto &p : properties)
        if (p.key == key) return &p;
    return nullptr;
}

std::string MapEntity::String(const char *key) const {
    const EntityProperty *p = Find(key);
    return p && p->raw.type == Kv3::Type::String ? p->raw.s : std::string();
}

float MapEntity::Float(const char *key, float fallback) const {
    const EntityProperty *p = Find(key);
    if (!p) return fallback;
    const Kv3 &v = p->raw;
    switch (v.type) {
    case Kv3::Type::Bool: return v.b ? 1.f : 0.f;
    case Kv3::Type::Int: return float(v.i);
    case Kv3::Type::UInt: return float(v.u);
    case Kv3::Type::Double: return float(v.d);
    case Kv3::Type::String: return strtof(v.s.c_str(), nullptr);
    default: throw MapDataError("cannot convert entity property to float: " + p->key);
    }
}

int32_t MapEntity::Integer(const char *key, int32_t fallback) const {
    const EntityProperty *p = Find(key);
    if (!p) return fallback;
    const Kv3 &v = p->raw;
    switch (v.type) {
    case Kv3::Type::Bool: return v.b ? 1 : 0;
    case Kv3::Type::Int: return int32_t(v.i);
    case Kv3::Type::UInt: return int32_t(uint32_t(v.u));
    case Kv3::Type::Double: return int32_t(v.d);
    case Kv3::Type::String: return int32_t(strtol(v.s.c_str(), nullptr, 10));
    default: return fallback;
    }
}

std::array<float, 3> MapEntity::Vector(const char *key, std::array<float, 3> fallback) const {
    const EntityProperty *p = Find(key);
    if (!p || p->raw.IsNull()) return fallback;
    const Kv3 &v = p->raw;
    const auto element = [](const Kv3 &e) {
        switch (e.type) {
        case Kv3::Type::Bool: return e.b ? 1.f : 0.f;
        case Kv3::Type::Int: return float(e.i);
        case Kv3::Type::UInt: return float(e.u);
        case Kv3::Type::Double: return float(e.d);
        case Kv3::Type::String: return strtof(e.s.c_str(), nullptr);
        default: return 0.f;
        }
    };
    if (v.type == Kv3::Type::Array) {
        std::array<float, 3> out = {0, 0, 0};
        for (size_t k = 0; k < 3 && k < v.arr.size(); ++k) out[k] = element(v.arr[k]);
        return out;
    }
    if (v.type == Kv3::Type::Object) {
        std::array<float, 3> out = {0, 0, 0};
        for (size_t k = 0; k < 3 && k < v.obj.size(); ++k) out[k] = element(v.obj[k].second);
        return out;
    }
    if (v.type == Kv3::Type::String) {
        float f[3] = {};
        if (sscanf_s(v.s.c_str(), "%f %f %f", &f[0], &f[1], &f[2]) == 3) return {f[0], f[1], f[2]};
    }
    return fallback;
}

std::vector<MapEntity> ReadMapEntities(const Vpk &package) {
    std::vector<MapEntity> out;
    for (const std::string &path : package.List("vents_c")) {
        std::vector<uint8_t> bytes;
        if (!package.Read(path, bytes)) continue;
        Resource resource(std::move(bytes));
        const Kv3 data = resource.Kv3Block("DATA");
        for (const Kv3 &entry : data["m_entityKeyValues"].arr) {
            std::map<std::string, Kv3> values;
            if (const Kv3 *kv3 = entry.Find("keyValues3Data")) {
                if ((*kv3)["version"].Integer(-1) != 1) throw MapDataError("unsupported entity data version");
                for (const char *section : {"values", "attributes"}) {
                    const Kv3 &group = (*kv3)[section];
                    if (group.IsNull()) continue;
                    if (group.type != Kv3::Type::Object) throw MapDataError("unsupported entity data values type");
                    for (const auto &[key, value] : group.obj) {
                        const std::string lower = Lower(key);
                        TokenStore(lower);
                        values[lower] = value;
                    }
                }
            } else {
                const Kv3 &blob = entry["m_keyValuesData"];
                if (blob.type == Kv3::Type::Blob) ReadHashedValues(blob.blob, values);
            }
            if (!values.count("classname")) continue;
            MapEntity entity;
            entity.resource = path;
            for (auto &[key, value] : values) {
                EntityProperty p;
                p.key = key;
                p.type = KvTypeName(value);
                p.value = StringifyValue(value);
                p.raw = value;
                entity.properties.push_back(std::move(p));
            }
            out.push_back(std::move(entity));
        }
    }
    return out;
}

std::string JsonEscape(const std::string &text) {
    std::string out;
    out.reserve(text.size() + 2);
    for (unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char hex[8];
                snprintf(hex, sizeof hex, "\\u%04x", c);
                out += hex;
            } else {
                out += char(c);
            }
        }
    }
    return out;
}

std::string JsonNumber(double value) {
    if (!std::isfinite(value)) return "null";
    if (value == std::floor(value) && std::fabs(value) < 1e15) {
        char text[32];
        snprintf(text, sizeof text, "%.0f", value);
        return text;
    }
    // Shortest representation that round-trips, mirroring System.Text.Json floats.
    char text[32];
    for (int precision = 1; precision <= 17; ++precision) {
        snprintf(text, sizeof text, "%.*g", precision, value);
        if (strtod(text, nullptr) == value) break;
    }
    std::string s = text;
    const size_t e = s.find('e');
    if (e != std::string::npos) s[e] = 'E';
    return s;
}

std::string BuildLightsJson(const std::string &map, const std::vector<MapEntity> &entities) {
    std::ostringstream lights, environment;
    bool firstLight = true, firstEnvironment = true;
    std::vector<std::string> identities;
    for (const MapEntity &e : entities) {
        const std::string kind = e.String("classname");
        if (!Related(kind)) continue;
        const EntityProperty *authoredProp = e.Find("hammeruniqueid");
        const bool hasAuthored = authoredProp && authoredProp->raw.type == Kv3::Type::String;
        const std::string authored = hasAuthored ? authoredProp->raw.s : "";
        const std::string identity = e.resource + ":" +
            (authored.empty()
                 ? "content:" + UpperHex(Sha256Bytes(PropertiesJson(e.properties).data(), PropertiesJson(e.properties).size()))
                 : "hammer:" + authored);
        if (std::find(identities.begin(), identities.end(), identity) != identities.end())
            throw MapDataError("Ambiguous authored light identity: " + identity);
        identities.push_back(identity);
        const std::string properties = [&] {
            std::string out = "{";
            for (size_t k = 0; k < e.properties.size(); ++k) {
                const auto &p = e.properties[k];
                if (k) out += ",";
                out += "\"" + JsonEscape(p.key) + "\":{\"type\":\"" + JsonEscape(p.type) + "\",\"value\":\"" +
                       JsonEscape(p.value) + "\"}";
            }
            return out + "}";
        }();
        const std::string head = "{\"identity\":\"" + JsonEscape(identity) + "\",\"resource\":\"" +
                                 JsonEscape(e.resource) + "\",\"kind\":\"" + JsonEscape(kind) + "\"";
        if (kind.rfind("light_", 0) != 0) {
            environment << (firstEnvironment ? "" : ",") << head << ",\"origin\":" << VecJson(e.Vector("origin"))
                        << ",\"angles\":" << VecJson(e.Vector("angles")) << ",\"properties\":" << properties << "}";
            firstEnvironment = false;
            continue;
        }
        lights << (firstLight ? "" : ",") << head;
        if (hasAuthored) lights << ",\"authored\":\"" << JsonEscape(authored) << "\"";
        else lights << ",\"authored\":null";
        lights << ",\"origin\":" << VecJson(e.Vector("origin")) << ",\"angles\":" << VecJson(e.Vector("angles"))
               << ",\"color\":" << VecJson(e.Vector("color"));
        if (e.Has("brightness")) lights << ",\"brightness\":" << JsonNumber(e.Float("brightness", 0));
        else lights << ",\"brightness\":null";
        if (e.Has("brightness_lumens")) lights << ",\"brightnessLumens\":" << JsonNumber(e.Float("brightness_lumens", 0));
        else lights << ",\"brightnessLumens\":null";
        lights << ",\"brightnessScale\":" << JsonNumber(e.Float("brightnessscale", 1))
               << ",\"range\":" << JsonNumber(e.Float("range", 512))
               << ",\"bakedLightIndex\":" << e.Integer("bakelightindex", -1)
               << ",\"bakedShadowIndex\":" << e.Integer("bakedshadowindex", -1)
               << ",\"directLight\":" << e.Integer("directlight", -1) << ",\"properties\":" << properties << "}";
        firstLight = false;
    }
    return "{\"format\":1,\"map\":\"" + JsonEscape(map) + "\",\"lights\":[" + lights.str() + "],\"environment\":[" +
           environment.str() + "]}";
}

} // namespace Filmmaker::MapData
