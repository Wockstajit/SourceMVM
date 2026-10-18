// The experimental relighting engine's state and console surface. See Relight.h for why
// this is separate from the Environment feature.
//
// Everything here is transient by design: nothing is serialised, so a map change or a
// reload starts from the map's authored values again. Editing is console-only.
#include "Relight.h"
#include "RelightNativeCluster.h"
#include "RelightConsoleMath.h"
#include "RelightLightmap.h"
#include "RelightLightEditor.h"

#include "../Environment/EnvironmentScene.h"
#include "../Environment/EnvironmentNativeMaterial.h"
#include "../Environment/EnvironmentShadowMaps.h"
#include "../Platform/JsonParser.h"
#include "../Debug/MvmDebugLog.h"
#include "../../WrpConsole.h"
#include "../../../Shared/AfxConsole.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <cstdio>
#include <string>
#include <vector>

namespace Filmmaker::Relight {
namespace {
std::mutex g_mutex;
Scene g_scene;
std::string g_loadedMetadata;
std::string g_metadataMap;
void Load(const std::string &path,const std::string &map);

bool IsFiniteVec(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

float Number(const char *text) {
    float value=0;
    if(!ParseFiniteNumber(text,value))throw std::runtime_error("Expected a finite number");
    return value;
}

// Authored property values arrive as typed records, so the value we want is nested one
// level down and quoted. Pull the first run of number-like characters out of it.
const JsonValue *PropertyValue(const JsonValue &light, const char *key) {
    const auto *properties = light.Find("properties");
    if (!properties) return nullptr;
    const auto *entry = properties->Find(key);
    if (!entry) return nullptr;
    return entry->Find("value");
}

bool PropertyNumbers(const JsonValue &light, const char *key, float *out, unsigned count) {
    const auto *value = PropertyValue(light, key);
    if (!value) return false;
    const auto text = value->AsString();
    std::string number;
    unsigned found = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? text[i] : ',';
        if ((c >= '0' && c <= '9') || c == '.' || c == '-')
            number += c;
        else if (!number.empty()) {
            if (found < count) out[found++] = float(atof(number.c_str()));
            number.clear();
        }
    }
    return found == count;
}
} // namespace

Scene CaptureScene(const std::string &map) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Scene out = g_scene;
    if(g_scene.map!=map)return Scene{};
    out.enabled = g_scene.enabled;
    return out;
}

std::string HeartbeatText() {
    std::lock_guard<std::mutex> lock(g_mutex);
    char text[160];
    snprintf(text, sizeof(text), "relight=%d lights=%zu gi=%d bounce=%g screenGi=%d shadows=%d",
        int(g_scene.enabled), g_scene.lights.size(), int(g_scene.gi), double(g_scene.bounce), int(g_scene.screenGi), int(g_scene.shadows));
    return text;
}

void OnLevelChanged() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_scene.enabled = false;
    g_loadedMetadata.clear();
    g_metadataMap.clear();
}

bool UpdateMapLighting(const std::string &map,const std::string &metadataPath) {
    if(metadataPath.empty())return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if(g_loadedMetadata==metadataPath&&g_metadataMap==map)return false;
    // Remember failures too: an immutable verified payload cannot fix itself per frame.
    g_loadedMetadata=metadataPath;
    g_metadataMap=map;
    try {
        Load(metadataPath,map);
        MvmDebugLog_LinefAlways("relight.map","map=%s lights=%zu sun=%d source=%s",map.c_str(),g_scene.lights.size(),g_scene.hasSun,metadataPath.c_str());
        return true;
    }catch(const std::exception& e){
        MvmDebugLog_LinefAlways("relight.map","map=%s failure=%s",map.c_str(),e.what());
        return false;
    }
}

namespace {
void Load(const std::string &path, const std::string &map) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
    if (!file || file.tellg() < 0 || file.tellg() > 32 * 1024 * 1024)
        throw std::runtime_error("Invalid map light metadata size");
    const auto bytes = static_cast<size_t>(file.tellg());
    file.seekg(0);
    std::string text(bytes, '\0');
    file.read(text.data(), bytes);
    JsonValue root;
    if (!file || !JsonParse(text, root)) throw std::runtime_error("Invalid map light metadata JSON");
    const auto *mapName = root.Find("map");
    const auto *lights = root.Find("lights");
    const auto *format = root.Find("format");
    if (!mapName || mapName->AsString() != map || !lights ||
        lights->type != JsonValue::Type::Array || !format || format->AsInt() != 1)
        throw std::runtime_error("Map light metadata map/format mismatch");

    Scene next;
    next.map = mapName->AsString();
    next.source = path;
    // Carry the dials across a reload; only the authored data is replaced.
    next.intensity = g_scene.intensity;
    next.indirect = g_scene.indirect;
    next.ao = g_scene.ao;
    next.sun = g_scene.sun;
    next.shadows = g_scene.shadows;
    next.sunShadows = g_scene.sunShadows;
    next.localLights = g_scene.localLights;
    next.ceiling = g_scene.ceiling;
    next.sunAngle = g_scene.sunAngle;
    next.lampAngle = g_scene.lampAngle;
    next.shadowSoftness = g_scene.shadowSoftness;
    next.shadowBias = g_scene.shadowBias;
    next.characterSelfShadowUnits = g_scene.characterSelfShadowUnits;
    next.sunCascade = g_scene.sunCascade;
    next.skyOcclusion = g_scene.skyOcclusion;
    next.reflections = g_scene.reflections;
    next.stainedGlass = g_scene.stainedGlass;
    next.staticShadows = g_scene.staticShadows;
    next.view = g_scene.view;
    next.cost = g_scene.cost;
    next.hoist = g_scene.hoist;
    next.reference = g_scene.reference;
    next.clusters = g_scene.clusters;
    next.reach = g_scene.reach;
    next.shadowReach = g_scene.shadowReach;
    next.cubeShadows = g_scene.cubeShadows;
    next.probeRebuild=g_scene.probeRebuild;next.probeCheck=g_scene.probeCheck;next.gi=g_scene.gi;next.screenGi=g_scene.screenGi;next.bounce=g_scene.bounce;next.bounceSteps=g_scene.bounceSteps;next.albedo=g_scene.albedo;
    next.lightmapDiag=g_scene.lightmapDiag;next.lightmapFile=g_scene.lightmapFile;next.lightmapMode=g_scene.lightmapMode;next.programFilter=g_scene.programFilter;

    size_t skipped = 0;
    for (const auto &light : lights->arr) {
        const auto *kind = light.Find("kind");
        if (!kind) {
            ++skipped;
            continue;
        }
        Light entry;
        entry.kind = kind->AsString();
        auto number = [&](const char *key, float fallback) {
            const auto *v = light.Find(key);
            return v && v->type == JsonValue::Type::Number && std::isfinite(v->number)
                       ? float(v->number)
                       : fallback;
        };
        auto vector = [&](const char *key, Vec3 fallback) {
            const auto *v = light.Find(key);
            if (!v || v->type != JsonValue::Type::Array || v->arr.size() != 3) return fallback;
            Vec3 out;
            float *values = &out.x;
            for (int i = 0; i < 3; ++i) {
                if (v->arr[i].type != JsonValue::Type::Number || !std::isfinite(v->arr[i].number))
                    return fallback;
                values[i] = float(v->arr[i].number);
            }
            return out;
        };
        // The sun is a directional source, a different record shape from a clustered local
        // light, so it is held on its own rather than in the light list.
        if (entry.kind == "light_environment") {
            next.hasSun = true;
            next.sunAngles = AuthoredSunAngles(vector("angles", Vec3{}));
            next.sunColor = vector("color", Vec3{255, 255, 255}) * (1.f / 255);
            next.sunBrightness = number("brightness", 0);
            float sky[3]{130, 205, 255}, intensity = 1;
            if (PropertyNumbers(light, "skycolor", sky, 3))
                next.skyColor = {sky[0] / 255, sky[1] / 255, sky[2] / 255};
            if (PropertyNumbers(light, "skyintensity", &intensity, 1) && intensity >= 0 &&
                intensity <= 1000)
                next.skyIntensity = intensity;
            continue;
        }
        const auto *id = light.Find("identity");
        if (id) entry.identity = id->AsString();
        entry.position = vector("origin", Vec3{});
        entry.angles = vector("angles", Vec3{});
        entry.color = vector("color", Vec3{255, 255, 255}) * (1.f / 255);
        entry.lumens = number("brightnessLumens", 0);
        entry.brightnessScale = number("brightnessScale", 1);
        entry.range = number("range", 512);
        const auto authored=[&](const char* key,float fallback){float value=fallback;
            return PropertyNumbers(light,key,&value,1)&&std::isfinite(value)?value:fallback;};
        entry.innerAngle=std::clamp(authored(entry.kind=="light_spot"?"innerconeangle":"inner_angle",0),0.f,180.f);
        entry.outerAngle=std::clamp(authored(entry.kind=="light_spot"?"outerconeangle":"outer_angle",180),0.f,180.f);
        entry.softX=std::clamp(authored("soft_x",entry.kind=="light_rect"?0.f:.25f),0.f,1.f);
        entry.softY=std::clamp(authored("soft_y",entry.kind=="light_rect"?0.f:.25f),0.f,1.f);
        entry.skirtNear=std::clamp(authored("skirt_near",entry.kind=="light_rect"?0.f:.05f),0.f,1.f);
        entry.falloff=std::clamp(authored("falloff",1),0.f,1.f);
        float castShadows = 2;
        if (PropertyNumbers(light, "castshadows", &castShadows, 1))
            entry.castShadows = int(castShadows);
        float sizeParams[3]{};
        float sourceShape=-1;
        if(PropertyNumbers(light,"shape",&sourceShape,1)&&std::isfinite(sourceShape)){
            entry.shape=int(sourceShape);entry.roundness=std::clamp(sourceShape,0.f,1.f);}
        if (PropertyNumbers(light, "size_params", sizeParams, 3)) {
            entry.sizeX = (std::max)(0.f, sizeParams[0]);
            entry.sizeY = (std::max)(0.f, sizeParams[1]);
            entry.sizeZ = (std::max)(0.f, sizeParams[2]);
            entry.sourceRadius = AuthoredSourceRadius(entry.sizeX, entry.sizeY);
            if(entry.kind=="light_omni2"&&entry.shape==0&&entry.sizeX>0)
                entry.sourceRadius=std::clamp(entry.sizeX,1.f,256.f);
        }
        // A light we cannot describe is dropped rather than guessed at: one bad record
        // would light the whole map wrongly.
        if (!IsFiniteVec(entry.position) || !IsFiniteVec(entry.color) || entry.lumens < 0 ||
            entry.lumens > 1e7f || entry.brightnessScale < 0 || entry.brightnessScale > 100 ||
            !(entry.range >= 1) || entry.range > 10000) {
            ++skipped;
            continue;
        }
        next.lights.push_back(entry);
        if(MvmDebugLog_Active()&&(entry.outerAngle<180||HasFixtureProjection(entry)))
            MvmDebugLog_LinefAlways("relight.authored.shape","identity=%s kind=%s inner=%.3f outer=%.3f size=(%.3f %.3f %.5f) soft=(%.3f %.3f) skirt=%.3f falloff=%.3f faces=%u",
                entry.identity.c_str(),entry.kind.c_str(),entry.innerAngle,entry.outerAngle,entry.sizeX,entry.sizeY,entry.sizeZ,entry.softX,entry.softY,entry.skirtNear,entry.falloff,FixtureFaces(entry));
    }
    if (next.lights.empty()) throw std::runtime_error("No usable authored lights for this map");
    next.generation = g_scene.generation + 1;
    next.enabled = g_scene.enabled && g_scene.map == next.map;
    g_scene = next;
    if (skipped) advancedfx::Message("relight: skipped %zu unusable authored lights\n", skipped);
}

// Resolves a light by list index, or by a substring of its authored identity so a fixture
// can be named rather than counted.
int Resolve(const std::string &which) {
    if (!which.empty() && which.find_first_not_of("0123456789") == std::string::npos) {
        const int index = atoi(which.c_str());
        if (index >= 0 && size_t(index) < g_scene.lights.size()) return index;
        return -1;
    }
    int found = -1;
    for (unsigned i = 0; i < g_scene.lights.size(); ++i)
        if (g_scene.lights[i].identity.find(which) != std::string::npos) {
            if (found >= 0) return -2; // ambiguous
            found = int(i);
        }
    return found;
}

void Status() {
    advancedfx::Message(
        "relight: enabled=%d generation=%llu lights=%zu map='%s'\n"
        "  intensity=%g indirect=%g sunscale=%g ceiling=%g sunsize=%g lampsize=%g softness=%g shadows=%d "
        "cached=%u view=%d hoist=%d cost=%u clusters=%u probecheck=%u gi=%d bounce=%g screenGi=%d lightmapdiag=%d\n"
        "  sun: authored=%d elevation=%g azimuth=%g brightness=%g color=(%.2f %.2f %.2f) "
        "sky=(%.2f %.2f %.2f)x%g\n",
        g_scene.enabled, g_scene.generation, g_scene.lights.size(), g_scene.map.c_str(),
        g_scene.intensity, g_scene.indirect, g_scene.sun, g_scene.ceiling, g_scene.sunAngle, g_scene.lampAngle, g_scene.shadowSoftness,
        g_scene.shadows, g_scene.staticShadows, g_scene.view, g_scene.hoist, g_scene.cost, g_scene.clusters, g_scene.probeCheck, g_scene.gi, g_scene.bounce, g_scene.screenGi, g_scene.lightmapDiag, g_scene.hasSun, g_scene.sunAngles.x, g_scene.sunAngles.y,
        g_scene.sunBrightness, g_scene.sunColor.x, g_scene.sunColor.y, g_scene.sunColor.z,
        g_scene.skyColor.x, g_scene.skyColor.y, g_scene.skyColor.z, g_scene.skyIntensity);
    advancedfx::Message("  probe fallback albedo=(%g %g %g), exported material colours when available\n",g_scene.albedo.x,g_scene.albedo.y,g_scene.albedo.z);
    advancedfx::Message("  reference=%d (untouched native-input replay diagnostic)\n",g_scene.reference);
    advancedfx::Message("  lightmap: file=%s mode=%u status=%s\n",g_scene.lightmapFile.empty()?"(none)":g_scene.lightmapFile.c_str(),g_scene.lightmapMode,Filmmaker::Relight::LightmapStatus().c_str());
    advancedfx::Message("  shadowbias=%g (normalized atlas depth) selfshadow=%g units (players ignore sun casters this close)\n",g_scene.shadowBias,g_scene.characterSelfShadowUnits);
    advancedfx::Message("  skyocclusion=%g (runtime sky visibility on ambient) suncascade=%d\n",g_scene.skyOcclusion,g_scene.sunCascade);
    advancedfx::Message("  ao=%g (owned real-time ambient occlusion)\n",g_scene.ao);
    advancedfx::Message("  reflections=%g (live reflection capture)\n",g_scene.reflections);
    advancedfx::Message("  reach=%g (lights stop where they change a dark pixel by less than this many display steps; 0 = authored ranges)\n",g_scene.reach);
    advancedfx::Message("  shadowreach=%g (lamp shadows stop where the lamp changes a dark pixel by less than this many display steps; 0 = everywhere)\n",g_scene.shadowReach);
    advancedfx::Message("  cuberecords=%d (one light record per shadowed lamp)\n",g_scene.cubeShadows?1:0);
}
} // namespace
} // namespace Filmmaker::Relight

CON_COMMAND(mirv_relight,
            "Experimental relighting engine: light a map from its own authored lights "
            "(mirv_relight help).") {
    using namespace Filmmaker::Relight;
    const int argc = args->ArgC();
    const auto arg = [&](int index) { return std::string(index < argc ? args->ArgV(index) : ""); };
    const auto op = arg(1);
    if (op == "lampsize" || op == "softness") {
        if (argc != 3) { advancedfx::Warning("mirv_relight %s <value>\n", op.c_str()); return; }
        const float n = std::clamp(Number(args->ArgV(2)), 0.f, op == "lampsize" ? 5.f : 4.f);
        { std::lock_guard<std::mutex> lock(g_mutex);
          (op == "lampsize" ? g_scene.lampAngle : g_scene.shadowSoftness) = n; ++g_scene.generation; }
        struct Forward: advancedfx::ICommandArgs {
            std::vector<std::string> v; int ArgC() override { return int(v.size()); }
            const char* ArgV(int i) override { return v[size_t(i)].c_str(); }
        } forward;
        forward.v = {"mirv_filmmaker", "environment", "climate", op == "lampsize" ? "lampAngle" : "shadowSoftness", std::to_string(n)};
        Filmmaker::Environment::RunCommand(&forward);
        return;
    }
    if (op == "sunsize") {
        // The Lighting page's "Sun size". Forwarded to the Environment climate (which owns it while a
        // scene is active) before this feature's own lock is taken, as that path locks in the other order.
        if (argc != 3) { advancedfx::Warning("mirv_relight sunsize <degrees 0-5>\n"); return; }
        const float n = Number(args->ArgV(2));
        if (!(n >= 0) || n > Filmmaker::Environment::SunDiameterMaxDegrees) { advancedfx::Warning("Value out of range (0-5)\n"); return; }
        { std::lock_guard<std::mutex> lock(g_mutex); g_scene.sunAngle = n; ++g_scene.generation; }
        struct Args : advancedfx::ICommandArgs {
            std::vector<std::string> v;
            int ArgC() override { return int(v.size()); }
            const char* ArgV(int i) override { return v[size_t(i)].c_str(); }
        } forward;
        char value[32];std::snprintf(value, sizeof(value), "%g", n);
        forward.v = {"mirv_filmmaker", "environment", "climate", "sunAngle", value};
        Filmmaker::Environment::RunCommand(&forward);
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    try {
        if (op.empty() || op == "help") {
            advancedfx::Message(
                "mirv_relight load <lights.json>      load this map's authored lights\n"
                "mirv_relight on|off|status|list [filter]  enable, disable, report, enumerate\n"
                "mirv_relight intensity|indirect <n>  global light multiplier, skylight contribution\n"
                "mirv_relight sunscale|ceiling <n>    sun multiplier, replacement colour bound\n"
                "mirv_relight sunsize <degrees>       sun angular diameter 0-5 (real sun 0.53); shadows soften with caster distance\n"
                "mirv_relight lampsize <degrees>     lamp source angle 0-5, affects shadow edges only\n"
                "mirv_relight softness <0-4>         multiplier on sun and lamp shadow softness\n"
                "mirv_relight dumpsun <file>         write four raw sun cascade matrices and depth tiles\n"
                "mirv_relight ao <0-2>               owned ambient occlusion strength\n"
                "mirv_relight shadowbias <0-0.01>    normalized shadow receiver bias\n"
                "mirv_relight suncascade <-1|0-3>    diagnostic: shade through one sun cascade only\n"
                "mirv_relight reach <0-8>            light reach cutoff in display steps (4 default, 0 = authored ranges)\n"
                "mirv_relight shadowreach <0-64>     lamp shadows stop below this many display steps (4 default, 0 = everywhere, -1 = none)\n"
                "mirv_relight cuberecords 0|1        one light record per shadowed lamp (1) or six face records (0); same picture\n"
                "mirv_relight skyocclusion <0-1>     runtime sky visibility on the ambient\n"
                "mirv_relight reflections <0-2>      live reflection strength (0 off)\n"
                "mirv_relight lighteditor <on|off>   markers on every light; aim + F switches one off/on, Esc leaves\n"
                "mirv_relight lights reset           switch every light on this map back on\n"
                "mirv_relight stainedglass <0-2>     sunlight through stained glass (0 off)\n"
                "mirv_relight shadows 0|1             shadow maps for the budgeted lights\n"
                "mirv_relight shadowlights <n>        lights in the cached shadow tier\n"
                "mirv_relight actorshadows <0-512>    light faces with a moving player redrawn every frame\n"
                "mirv_relight lampsummary 0|1         lamp shadows skip their 32 taps where the atlas summary proves the answer (1)\n"
                "mirv_relight view off|lights|direct|coverage|probes\n"
                "mirv_relight gi 0|1 | giscale 0.5|1 | bounce <0-16> | albedo <r g b> | rebuild\n"
                "mirv_relight lightmapdiag 0|1     UV checker on world lightmap t2 (not GI)\n"
                "mirv_relight probecheck 0|1|2      normal, uniform fallback, uniform probe\n"
                "mirv_relight clusters 0|1|2|3      full mask, native tiles+slices, native slices, owned reference\n"
                "mirv_relight cost full|word|one    diagnostic: all, 32, or one record\n"
                "mirv_relight hoist 0|1            diagnostic: per-draw or per-view preparation\n"
                "mirv_relight reference 0|1        diagnostic: untouched native-input replay\n"
                "mirv_relight bindingcheck 0|1|2   diagnostic: normal / white coverage / black material\n"
                "mirv_relight light <index|identity> "
                "color <r g b>|intensity <n>|range <n>|size <n>|move <x y z>|on|off|shadows <0-2>\n"
                "mirv_relight sun angles <elevation> <azimuth>|color <r g b>|brightness <n>\n"
                "Nothing here is saved: a map change starts from the authored values again.\n");
            return;
        }
        if (op == "dumpprograms") {
            if (argc != 3) throw std::runtime_error("mirv_relight dumpprograms <directory>");
            std::error_code error;
            std::filesystem::create_directories(std::filesystem::u8path(arg(2)), error);
            const unsigned written = Filmmaker::Environment::DumpNativePrograms(arg(2).c_str())+DumpNativeClusterPrograms(arg(2));
            advancedfx::Message("relight: wrote %u clustered-light programs to %s\n", written, arg(2).c_str());
            return;
        }
        if (op == "load") {
            if (argc < 3) throw std::runtime_error("mirv_relight load <lights.json>");
            // The console tokenizer splits on spaces and colons, so an unquoted Windows path
            // arrives as "C" + "/Temp/...". Re-join drive-letter splits; quoted paths stay argc==3.
            std::string path = arg(2);
            for (int i = 3; i < argc; ++i) {
                if (path.size() == 1 &&
                    ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')))
                    path += ':';
                else
                    path += ' ';
                path += arg(i);
            }
            Load(path, Filmmaker::Environment::CurrentMapName());
            Status();
            return;
        }
        if (op == "on" || op == "off") {
            if (op == "on" && (g_scene.lights.empty() ||
                               g_scene.map != Filmmaker::Environment::CurrentMapName())) {
                const auto map=Filmmaker::Environment::CurrentMapName();
                const auto folder=std::filesystem::u8path(g_scene.source).parent_path();
                bool loaded=false;
                if(!g_scene.source.empty())for(const auto& name:{map,map.rfind("de_",0)==0?map.substr(3):map}) {
                    const auto candidate=folder/(name+"-authored-lights.json");
                    std::error_code error;
                    if(std::filesystem::is_regular_file(candidate,error)) {
                        Load(candidate.u8string(),map);loaded=true;break;
                    }
                }
                if(!loaded)throw std::runtime_error("Load this map's authored lights first; on also searches beside the last loaded export");
            }
            g_scene.enabled = op == "on";
            ++g_scene.generation;
            Status();
            return;
        }
        if (op == "status") {
            Status();
            return;
        }
        if (op == "list") {
            const auto filter = arg(2);
            unsigned shown = 0;
            for (unsigned i = 0; i < g_scene.lights.size(); ++i) {
                const auto &light = g_scene.lights[i];
                if (!filter.empty() && light.kind.find(filter) == std::string::npos &&
                    light.identity.find(filter) == std::string::npos)
                    continue;
                advancedfx::Message("  [%3u] %-16s %s lumens=%-7g range=%-7g size=%.1f pos=(%.0f %.0f %.0f) "
                                    "color=(%.2f %.2f %.2f) shadows=%d\n",
                                    i, light.kind.c_str(), light.enabled ? "on " : "off",
                                    light.lumens, light.range, light.sourceRadius,
                                    light.position.x, light.position.y,
                                    light.position.z, light.color.x, light.color.y, light.color.z,
                                    light.castShadows);
                if (++shown >= 200) {
                    advancedfx::Message("  ... %zu more; pass a filter\n",
                                        g_scene.lights.size() - shown);
                    break;
                }
            }
            if (!shown) advancedfx::Message("  no lights match\n");
            return;
        }
        if (op == "intensity" || op == "indirect" || op == "sunscale" || op == "ceiling" ||
            op == "ao") {
            if (argc != 3) throw std::runtime_error("mirv_relight <control> <value>");
            const float n = Number(args->ArgV(2));
            const float limit = op == "intensity" || op == "sunscale" ? 100.f
                                : op == "ceiling"                     ? 65504.f
                                : op == "ao"                          ? 2.f
                                                                      : 1.f;
            if (!(n >= 0) || n > limit) throw std::runtime_error("Value out of range");
            if (op == "intensity") g_scene.intensity = n;
            else if (op == "indirect") g_scene.indirect = n;
            else if (op == "sunscale") g_scene.sun = n;
            else if (op == "ao") g_scene.ao = n;
            else g_scene.ceiling = n;
            ++g_scene.generation;
            Status();
            return;
        }
        if(op=="reach") {
            if(argc!=3)throw std::runtime_error("mirv_relight reach <0-8>");
            const float value=Number(args->ArgV(2));
            if(!(value>=0)||value>8)throw std::runtime_error("Value out of range");
            g_scene.reach=value;++g_scene.generation;Status();return;
        }
        if(op=="cuberecords") {
            if(argc!=3)throw std::runtime_error("mirv_relight cuberecords 0|1");
            g_scene.cubeShadows=std::string(args->ArgV(2))!="0";++g_scene.generation;Status();return;
        }
        if(op=="shadowreach") {
            if(argc!=3)throw std::runtime_error("mirv_relight shadowreach <0-64>");
            const float value=Number(args->ArgV(2));
            if(!(value>=-1)||value>64)throw std::runtime_error("Value out of range");
            g_scene.shadowReach=value;++g_scene.generation;Status();return;
        }
        if(op=="skyocclusion") {
            if(argc!=3)throw std::runtime_error("mirv_relight skyocclusion <0-1>");
            const float value=Number(args->ArgV(2));
            if(!(value>=0)||value>1)throw std::runtime_error("Value out of range");
            g_scene.skyOcclusion=value;++g_scene.generation;Status();return;
        }
        if(op=="stainedglass") {
            if(argc!=3)throw std::runtime_error("mirv_relight stainedglass <0-2>");
            const float value=Number(args->ArgV(2));
            if(!(value>=0)||value>2)throw std::runtime_error("Value out of range");
            g_scene.stainedGlass=value;++g_scene.generation;Status();return;
        }
        if(op=="lighteditor") {
            // Lighting > Edit lights: markers on every map light, F switches the aimed one, Esc leaves.
            const auto value=arg(2);
            if(value=="on"||value=="1")SetLightEditor(true);
            else if(value=="off"||value=="0")SetLightEditor(false);
            else if(value=="toggle"||value.empty())SetLightEditor(!LightEditorActive());
            else throw std::runtime_error("mirv_relight lighteditor <on|off|toggle>");
            advancedfx::Message("light editor %s; %u light(s) switched off on this map\n",LightEditorActive()?"on (aim at a light, F switches it, Esc leaves)":"off",LightsSwitchedOff());
            return;
        }
        if(op=="dumpsun") {
            if(argc!=3)throw std::runtime_error("mirv_relight dumpsun <file>");
            Filmmaker::Environment::RequestSunCascadeDump(args->ArgV(2));advancedfx::Message("sun cascades will be written next frame\n");return;
        }
        if(op=="lights") {
            if(arg(2)!="reset")throw std::runtime_error("mirv_relight lights reset");
            ResetLightChoices();++g_scene.generation;advancedfx::Message("every light on this map is on again\n");return;
        }
        if(op=="reflections") {
            if(argc!=3)throw std::runtime_error("mirv_relight reflections <0-2>");
            const float value=Number(args->ArgV(2));
            if(!(value>=0)||value>2)throw std::runtime_error("Value out of range");
            g_scene.reflections=value;++g_scene.generation;Status();return;
        }
        if(op=="suncascade") {
            if(argc!=3)throw std::runtime_error("mirv_relight suncascade <-1|0-3>");
            const int value=std::atoi(args->ArgV(2));
            if(value<-1||value>3)throw std::runtime_error("Value out of range");
            g_scene.sunCascade=value;++g_scene.generation;Status();return;
        }
        if(op=="selfshadow") {
            if(argc!=3)throw std::runtime_error("mirv_relight selfshadow <0-128>");
            const float value=Number(args->ArgV(2));
            if(value<0||value>128)throw std::runtime_error("Value out of range");
            g_scene.characterSelfShadowUnits=value;++g_scene.generation;Status();return;
        }
        if(op=="shadowbias") {
            if(argc!=3)throw std::runtime_error("mirv_relight shadowbias <0-0.01>");
            const float value=Number(args->ArgV(2));
            if(value<0||value>.01f)throw std::runtime_error("Value out of range");
            g_scene.shadowBias=value;++g_scene.generation;Status();return;
        }
        if(op=="reference") {
            const auto value=arg(2);
            if(argc!=3||(value!="0"&&value!="1"))throw std::runtime_error("mirv_relight reference 0|1");
            g_scene.reference=value=="1";++g_scene.generation;Status();return;
        }
        if(op=="bindingcheck") {
            const auto value=arg(2);
            if(argc!=3||(value!="0"&&value!="1"&&value!="2"))throw std::runtime_error("mirv_relight bindingcheck 0|1|2");
            g_scene.bindingCheck=unsigned(value[0]-'0');++g_scene.generation;Status();return;
        }
        if(op=="giscale") {
            const auto value=arg(2);
            if(argc!=3||(value!="0.5"&&value!="1"))throw std::runtime_error("mirv_relight giscale 0.5|1");
            g_scene.screenGi=value=="0.5";++g_scene.generation;Status();return;
        }
        if(op=="gi") {
            const auto value=arg(2);
            if(argc!=3||(value!="0"&&value!="1"))throw std::runtime_error("mirv_relight gi 0|1");
            g_scene.gi=value=="1";++g_scene.generation;Status();return;
        }
        if(op=="lightmapdiag") {
            const auto value=arg(2);
            if(argc!=3||(value!="0"&&value!="1"))
                throw std::runtime_error("mirv_relight lightmapdiag 0|1");
            g_scene.lightmapDiag=value=="1";++g_scene.generation;Status();return;
        }
        if(op=="bounce") {
            if(argc!=3)throw std::runtime_error("mirv_relight bounce <0-16>");
            const float n=Number(args->ArgV(2));
            if(n<0||n>16)throw std::runtime_error("Value out of range");
            g_scene.bounce=n;++g_scene.generation;Status();return;
        }
        if(op=="albedo") {
            if(argc!=5)throw std::runtime_error("mirv_relight albedo <r g b>, each 0-1");
            const Vec3 n{Number(args->ArgV(2)),Number(args->ArgV(3)),Number(args->ArgV(4))};
            if(n.x<0||n.x>1||n.y<0||n.y>1||n.z<0||n.z>1)throw std::runtime_error("Value out of range");
            g_scene.albedo=n;++g_scene.generation;Status();return;
        }
        if(op=="rebuild") {
            if(argc!=2)throw std::runtime_error("mirv_relight rebuild");
            ++g_scene.probeRebuild;++g_scene.generation;Status();return;
        }
        if (op == "probecheck") {
            const auto value=arg(2);
            if(argc!=3||(value!="0"&&value!="1"&&value!="2"&&value!="3"&&value!="4"&&value!="5"&&value!="6"))
                throw std::runtime_error("mirv_relight probecheck 0|1|2|3");
            g_scene.probeCheck=unsigned(value[0]-'0');++g_scene.generation;Status();return;
        }
        if (op == "clusters") {
            const auto value=arg(2);
            if(argc!=3||(value!="0"&&value!="1"&&value!="2"&&value!="3"))
                throw std::runtime_error("mirv_relight clusters 0|1|2|3");
            g_scene.clusters=unsigned(value[0]-'0');++g_scene.generation;Status();return;
        }
        if (op == "cost" || op == "hoist") {
            const auto value = arg(2);
            if (argc != 3 || (op == "cost" && value != "full" && value != "word" && value != "one") ||
                (op == "hoist" && value != "0" && value != "1"))
                throw std::runtime_error("mirv_relight cost full|word|one | hoist 0|1");
            if (op == "cost") g_scene.cost = value == "word" ? 1u : value == "one" ? 2u : 0u;
            else g_scene.hoist = value == "1";
            ++g_scene.generation;
            Status();
            return;
        }
        if (op == "actorshadows") {
            if (argc != 3) throw std::runtime_error("mirv_relight actorshadows <0-512>");
            const long n = std::strtol(args->ArgV(2), nullptr, 10);
            if (n < 0 || n > 512) throw std::runtime_error("Value out of range");
            Filmmaker::Environment::SetActorShadowFacesPerFrame(unsigned(n));
            advancedfx::Message("mirv_relight: %u visible light faces with a moving player refresh every frame.\n", unsigned(n));
            return;
        }
        if (op == "lampsummary") {
            if (argc != 3) throw std::runtime_error("mirv_relight lampsummary 0|1");
            Filmmaker::Environment::SetLampShadowSummary(std::string(args->ArgV(2)) != "0");
            advancedfx::Message("mirv_relight: lamp shadow early-out %s (the picture is identical either way).\n",
                Filmmaker::Environment::LampShadowSummary() ? "on" : "off");
            return;
        }
        if (op == "shadowlights") {
            if (argc != 3) throw std::runtime_error("mirv_relight shadowlights <count>");
            const long n = std::strtol(args->ArgV(2), nullptr, 10);
            if (n < 0 || n > long(StaticShadowLimit))
                throw std::runtime_error("Value out of range");
            g_scene.staticShadows = unsigned(n);
            ++g_scene.generation;
            Status();
            return;
        }
        if (op == "lightmap") {
            if (argc < 3) throw std::runtime_error("mirv_relight lightmap <layout file> | off");
            std::string path = arg(2);
            for (int i = 3; i < argc; ++i) {
                if (path.size() == 1 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))) path += ':';
                else path += ' ';
                path += arg(i);
            }
            g_scene.lightmapFile = path == "off" ? std::string() : path;
            if (!g_scene.lightmapFile.empty()) Filmmaker::Relight::LightmapRequest(g_scene.lightmapFile);
            ++g_scene.generation;
            Status();
            return;
        }
        if (op == "programs") {
            const auto which = arg(2);
            if (argc != 3 || (which != "all" && which != "lm" && which != "other"))
                throw std::runtime_error("mirv_relight programs all|lm|other");
            g_scene.programFilter = which == "all" ? 0u : which == "lm" ? 1u : 2u;
            ++g_scene.generation;
            Status();
            return;
        }
        if (op == "lightmapmode") {
            const auto mode = arg(2);
            if (argc != 3 || (mode != "fill" && mode != "checker" && mode != "gi"))
                throw std::runtime_error("mirv_relight lightmapmode fill|checker|gi");
            g_scene.lightmapMode = mode == "fill" ? 0u : mode == "checker" ? 1u : 2u;
            ++g_scene.generation;
            Status();
            return;
        }
        if (op == "sunshadow" || op == "locallights") {
            const auto value = arg(2);
            if (argc != 3 || (value != "0" && value != "1"))
                throw std::runtime_error("mirv_relight sunshadow 0|1 | locallights 0|1");
            (op == "sunshadow" ? g_scene.sunShadows : g_scene.localLights) = value == "1";
            ++g_scene.generation;
            Status();
            return;
        }
        if (op == "shadows") {
            if (argc != 3) throw std::runtime_error("mirv_relight shadows 0|1");
            g_scene.shadows = Number(args->ArgV(2)) != 0;
            ++g_scene.generation;
            Status();
            return;
        }
        if (op == "view") {
            const auto mode = arg(2);
            g_scene.view = mode == "off"        ? 0
                           : mode == "lights"   ? 1
                           : mode == "direct"   ? 2
                           : mode == "coverage" ? 3
                           : mode == "probes"   ? 4
                                                : -1;
            if (g_scene.view < 0) {
                g_scene.view = 0;
                throw std::runtime_error("mirv_relight view off|lights|direct|coverage|probes");
            }
            ++g_scene.generation;
            Status();
            return;
        }
        if (op == "sun") {
            const auto what = arg(2);
            if (what == "angles" && argc == 5) {
                g_scene.sunAngles.x = std::clamp(Number(args->ArgV(3)), -90.f, 90.f);
                g_scene.sunAngles.y = Number(args->ArgV(4));
            } else if (what == "color" && argc == 6) {
                const Vec3 color{Number(args->ArgV(3)), Number(args->ArgV(4)),
                                 Number(args->ArgV(5))};
                if (!IsFiniteVec(color) || color.x < 0 || color.y < 0 || color.z < 0 || color.x > 1 ||
                    color.y > 1 || color.z > 1)
                    throw std::runtime_error("Sun colour is sRGB 0..1");
                g_scene.sunColor = color;
            } else if (what == "brightness" && argc == 4) {
                const float n = Number(args->ArgV(3));
                if (!(n >= 0) || n > 100) throw std::runtime_error("Value out of range");
                g_scene.sunBrightness = n;
            } else
                throw std::runtime_error(
                    "mirv_relight sun angles <elevation> <azimuth> | color <r g b> | brightness <n>");
            ++g_scene.generation;
            Status();
            return;
        }
        if (op == "light") {
            const int index = Resolve(arg(2));
            if (index == -2) throw std::runtime_error("That identity matches more than one light");
            if (index < 0) throw std::runtime_error("No such light; try mirv_relight list");
            auto &light = g_scene.lights[size_t(index)];
            const auto what = arg(3);
            if (what == "color" && argc == 7) {
                const Vec3 color{Number(args->ArgV(4)), Number(args->ArgV(5)),
                                 Number(args->ArgV(6))};
                if (!IsFiniteVec(color) || color.x < 0 || color.y < 0 || color.z < 0 || color.x > 1 ||
                    color.y > 1 || color.z > 1)
                    throw std::runtime_error("Light colour is sRGB 0..1");
                light.color = color;
            } else if (what == "intensity" && argc == 5) {
                const float n = Number(args->ArgV(4));
                if (!(n >= 0) || n > 1e7f) throw std::runtime_error("Value out of range");
                light.lumens = n;
            } else if (what == "range" && argc == 5) {
                const float n = Number(args->ArgV(4));
                if (!(n >= 1) || n > 10000) throw std::runtime_error("Range is 1..10000");
                light.range = n;
            } else if (what == "size" && argc == 5) {
                const float n = Number(args->ArgV(4));
                if (!(n >= 1) || n > 256) throw std::runtime_error("Emitter size is 1..256");
                light.sourceRadius = n;
            } else if (what == "move" && argc == 7) {
                const Vec3 position{Number(args->ArgV(4)), Number(args->ArgV(5)),
                                    Number(args->ArgV(6))};
                if (!IsFiniteVec(position)) throw std::runtime_error("Position must be finite");
                light.position = position;
            } else if ((what == "on" || what == "off") && argc == 4) {
                light.enabled = what == "on";
            } else if (what == "shadows" && argc == 5) {
                const float n = Number(args->ArgV(4));
                if (!(n >= 0) || n > 2) throw std::runtime_error("castshadows is 0, 1 or 2");
                light.castShadows = int(n);
            } else
                throw std::runtime_error("mirv_relight light <index|identity> "
                                         "color <r g b>|intensity <n>|range <n>|size <n>|"
                                         "move <x y z>|on|off|shadows <0-2>");
            ++g_scene.generation;
            advancedfx::Message("  [%3d] %-16s %s lumens=%g range=%g size=%.1f pos=(%.0f %.0f %.0f) "
                                "color=(%.2f %.2f %.2f) shadows=%d\n",
                                index, light.kind.c_str(), light.enabled ? "on " : "off",
                                light.lumens, light.range, light.sourceRadius,
                                light.position.x, light.position.y,
                                light.position.z, light.color.x, light.color.y, light.color.z,
                                light.castShadows);
            return;
        }
        throw std::runtime_error("Unknown subcommand; try mirv_relight help");
    } catch (const std::exception &e) {
        advancedfx::Warning("mirv_relight: %s\n", e.what());
        Filmmaker::MvmDebugLog_LinefAlways("relight.error", "%s", e.what());
    }
}
