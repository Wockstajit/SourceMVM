#include "SceneKeyframeCatalog.h"
#include "../Environment/EnvironmentWorld.h"
#include "../Environment/EnvironmentClimateMath.h"
#include <algorithm>
#include <iterator>
#include <map>
namespace Filmmaker::SceneKeys {
namespace {
// THE NAME IS WHAT THE KEY CHANGES. Each one is the label of the CONFIG control that edits the
// same value, prefixed where the bare CONFIG label only makes sense inside its page (CONFIG's
// LIGHTING > Sun "Rotate" is "Sun rotate" here, its SKY > Sun & sky "Azimuth" is "Sun azimuth").
// Values with no CONFIG control are named for what they drive. This used to be generated from
// the field id, which produced "Sun Position yaw", "Smoke Tint · T" and "Sky Targets · fog", and
// the inspector then patched a few of those back in JS -- so the graph and the inspector named
// the same strip differently. This table is the one source for both. Every catalog id must have
// an entry: SceneKeyframeTrackTests rejects a label that is still just its id.
const std::map<std::string, std::string>& Names() {
    static const std::map<std::string, std::string> names = {
        // CONFIG > EFFECTS > LIGHTING
        {"exposure", "Exposure"}, {"lightColor", "Sun colour"}, {"lightAmount", "Sun tint amount"},
        {"lightUniform", "Sun disc colour only"},
        {"lightDirection.yaw", "Sun rotate"}, {"lightDirection.pitch", "Sun tilt"},
        {"sunPositionOverride", "Custom sun position"},
        {"sunPosition.yaw", "Custom sun azimuth"}, {"sunPosition.pitch", "Custom sun elevation"},
        {"sunSize", "Sun disc size"}, {"sunBrightness", "Sun disc brightness"}, {"sunGlow", "Sun glow"},
        {"sunGlowSize", "Sun bloom size"}, {"sunVisible", "Sun visible"},
        // CONFIG > EFFECTS > WEATHER & SKY
        {"sky", "Skybox"}, {"skyDirection.yaw", "Skybox rotate"}, {"skyDirection.pitch", "Skybox tilt"},
        {"skyMirror", "Skybox flip L/R"},
        {"skyTargets.sky", "Orient skybox"}, {"skyTargets.fog", "Orient fog / smoke"},
        {"skyTint", "Sky tint on"}, {"skyColor", "Sky tint colour"},
        {"cloudTint", "Cloud colour on"}, {"cloudColor", "Cloud colour"}, {"cloudsVisible", "Clouds visible"},
        {"cloudDensity", "Cloud density"}, {"cloudBrightness", "Cloud brightness"}, {"cloudSize", "Cloud size"},
        {"cloudDrift", "Cloud drift speed"},
        // CONFIG > EFFECTS > ATMOSPHERE (World tint writes one colour per surface category)
        {"worldTint.world", "World tint on"}, {"worldTint.props", "Prop tint on"},
        {"worldTint.sunclouds", "Sun/cloud tint on"}, {"worldTint.effects", "Effects tint on"},
        {"worldColors.world", "World tint colour"}, {"worldColors.props", "Prop tint colour"},
        {"worldColors.sunclouds", "Sun/cloud tint colour"}, {"worldColors.effects", "Effects tint colour"},
        {"worldStrength", "Tint strength"},
        {"smokeTint.T", "T smoke tint on"}, {"smokeTint.CT", "CT smoke tint on"},
        {"smokeColors.T", "T smoke colour"}, {"smokeColors.CT", "CT smoke colour"},
        // CONFIG > ENVIRONMENT > LOOKS
        {"env.exposure", "Exposure"}, {"env.saturation", "Saturation"}, {"env.contrast", "Contrast"},
        {"env.tint", "Scene tint"}, {"env.bloom", "Bloom"}, {"env.bloomThreshold", "Bloom threshold"},
        {"env.vignette", "Vignette"}, {"env.grain", "Film grain"}, {"env.chromatic", "Chromatic shift"},
        // CONFIG > ENVIRONMENT > SKY
        {"env.sky", "Procedural sky"}, {"env.sunAzimuth", "Sun azimuth"}, {"env.sunElevation", "Sun elevation"},
        {"env.sunPower", "Sun brightness"}, {"env.haze", "Horizon haze"}, {"env.stars", "Stars at night"},
        {"env.clouds", "Clouds"}, {"env.coverage", "Cloud coverage"}, {"env.density", "Cloud density"},
        {"env.cloudBase", "Cloud base height"}, {"env.cloudDepth", "Cloud layer depth"},
        {"env.cloudScale", "Cloud detail scale"}, {"env.windSpeed", "Cloud wind speed"},
        // CONFIG > ENVIRONMENT > LIGHTING
        {"env.enabled", "Environment on"}, {"env.ambient", "Ambient light"}, {"env.sunRelight", "Surface sunlight"},
        {"env.sunAngle", "Sun size (shadows)"}, {"env.bakedFill", "Unbaked fill"},
        // CONFIG > ENVIRONMENT > FOG
        {"env.fogDensity", "Fog density"}, {"env.fogColor", "Fog colour"}, {"env.fogStart", "Fog start distance"},
        {"env.fogHeight", "Fog height"}, {"env.fogNoise", "Fog variation"}, {"env.fogBase", "Fog base height"},
        {"env.fogRange", "Fog ray distance"}, {"env.scatter", "Fog forward scatter"}, {"env.lightShafts", "Light shafts"},
        {"env.localFogRadius", "Local fog radius"},
        {"env.fogX", "Local fog centre X"}, {"env.fogY", "Local fog centre Y"}, {"env.fogZ", "Local fog centre Z"},
        // CONFIG > ENVIRONMENT > WEATHER
        {"env.weather.type", "Weather type"}, {"env.weather.speed", "Fall speed"}, {"env.weather.size", "Particle size"},
        {"env.weather.color", "Particle colour"}, {"env.weather.windX", "Particle wind X"},
        {"env.weather.windY", "Particle wind Y"}, {"env.turbulence", "Turbulence"},
        {"env.particleOpacity", "Particle opacity"}, {"env.softness", "Soft intersections"},
        {"env.streak", "Rain streak length"}, {"env.particleLight", "Light response"},
        // CONFIG > ENVIRONMENT > WEATHER > Ground
        {"env.snow", "Snow coverage"}, {"env.wet", "Wetness"}, {"env.puddles", "Puddle coverage"},
        {"env.puddleHeight", "Puddle height"}, {"env.puddleDepth", "Puddle tolerance"},
        {"env.surfaceRadius", "Surface radius"}, {"env.surfaceX", "Surface centre X"}, {"env.surfaceY", "Surface centre Y"},
        {"env.maskScale", "Patch scale"}, {"env.ripple", "Ripples"}, {"env.reflectionRoughness", "Reflection roughness"},
    };
    return names;
}
std::string Name(const std::string& id) { const auto n = Names().find(id); return n == Names().end() ? id : n->second; }
bool Has(const std::string& list, const std::string& id) { return (" " + list + " ").find(" " + id + " ") != std::string::npos; }
// The procedural environment follows CONFIG's ENVIRONMENT cards, in their order, and each card's
// keys are listed in the order CONFIG's pages show them (values with no CONFIG control last).
struct EnvGroup { const char* id; const char* title; const char* keys; };
const EnvGroup kEnvGroups[] = {
    {"looks", "Looks", "exposure saturation contrast tint bloom bloomThreshold vignette grain chromatic"},
    {"sky", "Sky", "sky sunAzimuth sunElevation sunPower haze stars clouds coverage density cloudBase cloudDepth cloudScale windSpeed"},
    {"lighting", "Lighting", "enabled ambient sunRelight sunAngle bakedFill"},
    {"fog", "Fog", "fogDensity fogColor fogStart fogHeight fogNoise fogBase fogRange scatter lightShafts localFogRadius fogX fogY fogZ"},
    {"weather", "Weather", "weather.type weather.speed weather.size weather.color weather.windX weather.windY turbulence particleOpacity softness streak particleLight"},
    {"ground", "Ground", "snow wet puddles puddleHeight puddleDepth surfaceRadius surfaceX surfaceY maskScale ripple reflectionRoughness"},
};
// Position of `word` in a space-separated list, or -1.
int IndexIn(const std::string& list, const std::string& word) {
    size_t at = 0; int i = 0;
    while (at < list.size()) {
        size_t end = list.find(' ', at); if (end == std::string::npos) end = list.size();
        if (list.compare(at, end - at, word) == 0) return i;
        at = end + 1; ++i;
    }
    return -1;
}
const EnvGroup& EnvGroupOf(const std::string& key) {
    for (const auto& g : kEnvGroups) if (IndexIn(g.keys, key) >= 0) return g;
    return kEnvGroups[2];
}
// Sort key: CONFIG tab, then card, then the control's place on the card. Effects keep schema
// order within a card except the Skybox picker, which is WEATHER & SKY's first page.
int Rank(const ChannelSpec& s) {
    static const char* effects[] = {"lighting", "sky", "atmosphere"};
    if (s.family == "effects") {
        for (int i = 0; i < 3; ++i) if (s.groupId == effects[i]) return i * 1000 + (s.id == "sky" ? 0 : 1);
        return 3000;
    }
    const std::string key = s.id.substr(4); // drop "env."
    for (int i = 0; i < int(std::size(kEnvGroups)); ++i)
        if (s.groupId == kEnvGroups[i].id) { const int at = IndexIn(kEnvGroups[i].keys, key); return 10000 + i * 1000 + (at < 0 ? 999 : at); }
    return 99999;
}
}
const std::vector<ChannelSpec>& Catalog() {
    static const auto catalog = [] {
        std::vector<ChannelSpec> out;
        auto add = [&](std::string id, std::string family, std::string group, std::string groupId,
                       std::string source, ChannelType type, double lo, double hi, int count, int offset, Value initial) {
            const std::string label = Name(id);
            out.push_back({id, label, family, group, groupId, source, type, lo, hi, count, offset, initial});
        };
        auto addEnv = [&](const std::string& key, ChannelType type, double lo, double hi, int count, Value initial) {
            const auto& g = EnvGroupOf(key);
            add("env." + key, "environment", g.title, g.id, "env." + key, type, lo, hi, count, 0, initial);
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
                std::string key = id;
                ChannelType type = colors ? ChannelType::Colour : f.integer ? (f.max == 1 ? ChannelType::Bool : ChannelType::Enum) : ChannelType::Float;
                if (Has("lightDirection sunPosition skyDirection", id)) {
                    key += off == 0 ? ".yaw" : ".pitch";
                    type = off == 0 ? ChannelType::Angle : ChannelType::Float;
                } else if (int(f.defaults.size()) > size) {
                    const char* category[] = {"world", "props", "sunclouds", "effects"};
                    const std::string suffix = id == "worldColors" || id == "worldTint" ? category[off / size]
                        : id == "smokeColors" || id == "smokeTint" ? (off / size == 0 ? "T" : "CT")
                        : off == 0 ? "sky" : "fog";
                    key += "." + suffix;
                }
                Value initial; initial.numbers.assign(f.defaults.begin() + off, f.defaults.begin() + off + size);
                double lo = f.min;
                // CONFIG's exposure range starts at -1 = "auto". That is a mode, not a brightness: a key
                // holding it made the curve between keys run through negative exposures. A keyed exposure
                // is always a pinned value; keying while CONFIG is on auto records neutral 1.0 instead.
                if (id == "exposure") { lo = 0.02; initial.numbers.assign(1, 1.0); }
                add(key, "effects", group, page, id, type, lo, f.max, size, off, initial);
            }
        }
        add("sky", "effects", "Weather & Sky", "sky", "sky", ChannelType::SkyId, 0, 0, 0, 0, {});
        const std::string safe = "sky clouds coverage density cloudBase cloudDepth cloudScale windSpeed sunAzimuth sunElevation sunPower haze stars fogDensity fogHeight fogBase fogNoise fogRange scatter lightShafts snow wet puddles puddleHeight puddleDepth surfaceRadius surfaceX surfaceY maskScale ripple ambient sunRelight turbulence particleOpacity softness streak particleLight bloom bloomThreshold saturation contrast vignette grain chromatic fogStart localFogRadius fogX fogY fogZ reflectionRoughness sunAngle bakedFill";
        for (const auto& f : Environment::ClimateSpecs()) {
            const std::string id = f.key;
            if (!Has(safe, id)) continue;
            const auto type = id == "sunAzimuth" ? ChannelType::Angle : Has("sky clouds lightShafts", id) ? ChannelType::Bool : ChannelType::Float;
            addEnv(id, type, f.lo, f.hi, 1, {{f.initial}, {}});
        }
        addEnv("enabled", ChannelType::Bool, 0, 1, 1, {{0}, {}});
        addEnv("exposure", ChannelType::Float, .05, 4, 1, {{1}, {}});
        addEnv("fogColor", ChannelType::Colour, 0, 255, 3, {{115,128,153}, {}});
        addEnv("tint", ChannelType::Colour, 0, 255, 3, {{255,255,255}, {}});
        addEnv("weather.color", ChannelType::Colour, 0, 255, 3, {{166,191,230}, {}});
        // Count/seed/extent/follow/topology stay in CONFIG. These values are sampled
        // by the deterministic precipitation shader at the capture's demo time.
        struct WeatherSpec { const char* id; double value, lo, hi; };
        for (auto w : {WeatherSpec{"type",0,0,6}, {"speed",600,0,1500}, {"size",1,.1,12}, {"windX",40,-1000,1000}, {"windY",0,-1000,1000}})
            addEnv(std::string("weather.") + w.id, std::string(w.id) == "type" ? ChannelType::Enum : ChannelType::Float,
                w.lo, w.hi, 1, {{w.value}, {}});
        // Keep the inspector in CONFIG's tab/card order, regardless of schema order.
        std::stable_sort(out.begin(), out.end(), [](const ChannelSpec& a, const ChannelSpec& b) { return Rank(a) < Rank(b); });
        return out;
    }();
    return catalog;
}
}
