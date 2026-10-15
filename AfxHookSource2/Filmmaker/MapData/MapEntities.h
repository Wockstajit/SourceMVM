#pragma once

// Authored entities of a map package: the lights, probes, cubemaps, sky/fog/exposure and other
// environment entities the runtime relighting reads. Entity-lump decoding is ported from
// ValveResourceFormat 20.0 (MIT) EntityLump.cs.

#include "MapDataIo.h"

#include <array>
#include <string>
#include <vector>

namespace Filmmaker::MapData {

struct EntityProperty {
    std::string key;   // lower case
    std::string type;  // KV value type name ("String", "FloatingPoint64", "Array", ...)
    std::string value; // KV3 text form of the value
    Kv3 raw;
};

struct MapEntity {
    std::string resource; // the .vents_c path that holds it
    std::vector<EntityProperty> properties; // sorted by key (ordinal)
    const EntityProperty *Find(const char *key) const;
    std::string String(const char *key) const; // raw string value, "" when absent or not a string
    bool Has(const char *key) const { return Find(key) != nullptr; }
    float Float(const char *key, float fallback) const;
    int32_t Integer(const char *key, int32_t fallback) const;
    std::array<float, 3> Vector(const char *key, std::array<float, 3> fallback = {0, 0, 0}) const;
};

// Every entity (with a classname) in every .vents_c of the package, lumps in ordinal path order.
std::vector<MapEntity> ReadMapEntities(const Vpk &package);

// The format-1 lights document: {"format":1,"map":...,"lights":[...],"environment":[...]}.
// Record shape, identity, and filtering match the old inspector output.
std::string BuildLightsJson(const std::string &map, const std::vector<MapEntity> &entities);

// StringToken inverse lookup over the known Source 2 key/surface-property names (plus keys
// seen in entity lumps); "vrf_unknown_key_<hash>" when unknown.
std::string KnownEntityString(uint32_t hash);

// Helpers shared with the cache writers.
std::string JsonEscape(const std::string &text);
std::string JsonNumber(double value);

} // namespace Filmmaker::MapData
