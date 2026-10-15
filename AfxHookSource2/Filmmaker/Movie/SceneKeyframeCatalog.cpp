#include "SceneKeyframeCatalog.h"
#include "../Environment/EnvironmentWorld.h"
#include "../Environment/EnvironmentClimateMath.h"
#include <cctype>
namespace Filmmaker::SceneKeys {
namespace {
std::string Label(const std::string& id) {
    std::string out;
    for (char c : id) { if (std::isupper(static_cast<unsigned char>(c)) && !out.empty()) out += ' '; out += c; }
    if (!out.empty()) out[0] = char(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}
bool Has(const std::string& list, const std::string& id) { return (" " + list + " ").find(" " + id + " ") != std::string::npos; }
}
const std::vector<ChannelSpec>& Catalog() {
    static const auto catalog = [] {
        std::vector<ChannelSpec> out;
        auto add = [&](std::string id, std::string label, std::string family, std::string group, std::string groupId,
                       std::string source, ChannelType type, double lo, double hi, int count, int offset, Value initial) {
            out.push_back({id, label, family, group, groupId, source, type, lo, hi, count, offset, initial});
        };
        // WorldFields remains the owner of ranges/defaults. Exclude topology, quality,
        // shadow rebuild/takeover controls and particle-pack modes explicitly.
        for (const auto& f : Environment::WorldFields()) {
            const std::string id = f.name, page = f.page;
            if (!Has("exposure lightColor lightAmount lightUniform lightDirection sunPositionOverride sunPosition sunSize sunBrightness sunGlow sunGlowSize sunVisible skyDirection skyMirror skyTargets skyTint skyColor cloudTint cloudColor cloudsVisible cloudDensity cloudBrightness cloudSize cloudDrift worldTint worldColors worldStrength smokeTint smokeColors", id)) continue;
            const std::string group = page == "lighting" ? "Lighting" : page == "sky" ? "Weather & Sky" : "Atmosphere";
            const bool colors = Has("lightColor skyColor cloudColor worldColors smokeColors", id);
            const int size = colors ? (id == "skyColor" ? 4 : 3) : 1;
            for (int off = 0; off < int(f.defaults.size()); off += size) {
                std::string key = id, label = Label(id);
                ChannelType type = colors ? ChannelType::Colour : f.integer ? (f.max == 1 ? ChannelType::Bool : ChannelType::Enum) : ChannelType::Float;
                if (Has("lightDirection sunPosition skyDirection", id)) {
                    key += off == 0 ? ".yaw" : ".pitch"; label += off == 0 ? " yaw" : " pitch";
                    type = off == 0 ? ChannelType::Angle : ChannelType::Float;
                } else if (int(f.defaults.size()) > size) {
                    const char* category[] = {"world", "props", "sunclouds", "effects"};
                    const std::string suffix = id == "worldColors" || id == "worldTint" ? category[off / size]
                        : id == "smokeColors" || id == "smokeTint" ? (off / size == 0 ? "T" : "CT")
                        : off == 0 ? "sky" : "fog";
                    key += "." + suffix; label += " · " + suffix;
                }
                Value initial; initial.numbers.assign(f.defaults.begin() + off, f.defaults.begin() + off + size);
                double lo = f.min;
                // CONFIG's exposure range starts at -1 = "auto". That is a mode, not a brightness: a key
                // holding it made the curve between keys run through negative exposures. A keyed exposure
                // is always a pinned value; keying while CONFIG is on auto records neutral 1.0 instead.
                if (id == "exposure") { lo = 0.02; initial.numbers.assign(1, 1.0); }
                add(key, label, "effects", group, page, id, type, lo, f.max, size, off, initial);
            }
        }
        add("sky", "Skybox", "effects", "Weather & Sky", "sky", "sky", ChannelType::SkyId, 0, 0, 0, 0, {});
        const std::string safe = "sky clouds coverage density cloudBase cloudDepth cloudScale windSpeed sunAzimuth sunElevation sunPower haze stars fogDensity fogHeight fogBase fogNoise fogRange scatter lightShafts snow wet puddles puddleHeight puddleDepth surfaceRadius surfaceX surfaceY maskScale ripple ambient sunRelight turbulence particleOpacity softness streak particleLight bloom bloomThreshold saturation contrast vignette grain chromatic fogStart localFogRadius fogX fogY fogZ reflectionRoughness sunAngle bakedFill";
        for (const auto& f : Environment::ClimateSpecs()) {
            const std::string id = f.key;
            if (!Has(safe, id)) continue;
            const bool coating = Has("snow wet puddles puddleHeight puddleDepth surfaceRadius surfaceX surfaceY maskScale ripple reflectionRoughness", id);
            const bool look = Has("bloom bloomThreshold saturation contrast vignette grain chromatic", id);
            const std::string groupId = coating ? "coatings" : look ? "look" : f.page;
            const std::string group = coating ? "Coatings" : look ? "Look" : groupId == "sky" ? "Weather & Sky" : Label(groupId);
            const auto type = id == "sunAzimuth" ? ChannelType::Angle : Has("sky clouds lightShafts", id) ? ChannelType::Bool : ChannelType::Float;
            add("env." + id, Label(id), "environment", group, groupId, "env." + id, type, f.lo, f.hi, 1, 0, {{f.initial}, {}});
        }
        add("env.enabled", "Environment enabled", "environment", "Lighting", "lighting", "env.enabled", ChannelType::Bool, 0, 1, 1, 0, {{0}, {}});
        add("env.exposure", "Exposure", "environment", "Look", "look", "env.exposure", ChannelType::Float, .05, 4, 1, 0, {{1}, {}});
        add("env.fogColor", "Fog colour", "environment", "Atmosphere", "atmosphere", "env.fogColor", ChannelType::Colour, 0, 255, 3, 0, {{115,128,153}, {}});
        add("env.tint", "Look tint", "environment", "Look", "look", "env.tint", ChannelType::Colour, 0, 255, 3, 0, {{255,255,255}, {}});
        add("env.weather.color", "Precipitation colour", "environment", "Weather", "weather", "env.weather.color", ChannelType::Colour, 0, 255, 3, 0, {{166,191,230}, {}});
        // Count/seed/extent/follow/topology stay in CONFIG. These values are sampled
        // by the deterministic precipitation shader at the capture's demo time.
        struct WeatherSpec { const char* id; double value, lo, hi; };
        for (auto w : {WeatherSpec{"type",0,0,6}, {"speed",600,0,1500}, {"size",1,.1,12}, {"windX",40,-1000,1000}, {"windY",0,-1000,1000}})
            add(std::string("env.weather.") + w.id, Label(w.id), "environment", "Weather", "weather", std::string("env.weather.") + w.id,
                std::string(w.id) == "type" ? ChannelType::Enum : ChannelType::Float, w.lo, w.hi, 1, 0, {{w.value}, {}});
        // Keep the inspector in CONFIG's section/card order, regardless of schema order.
        const std::vector<std::string> groups = {"lighting", "sky", "atmosphere", "weather", "coatings", "look"};
        std::stable_sort(out.begin(), out.end(), [&](const ChannelSpec& a, const ChannelSpec& b) {
            if (a.family != b.family) return a.family == "effects";
            return std::find(groups.begin(), groups.end(), a.groupId) < std::find(groups.begin(), groups.end(), b.groupId);
        });
        return out;
    }();
    return catalog;
}
}
