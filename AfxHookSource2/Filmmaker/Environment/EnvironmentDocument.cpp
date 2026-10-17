#include "EnvironmentScene.h"
#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"
#include <limits>
#include <set>
#include <stdexcept>

namespace Filmmaker::Environment {
namespace {
void Vector(JsonBuilder &b, const char *key, Vec3 v) {
    b.Key(key);
    b.BeginArray();
    b.ValueDouble(v.x);
    b.ValueDouble(v.y);
    b.ValueDouble(v.z);
    b.EndArray();
}
const JsonValue &Field(const JsonValue &v, const char *key, JsonValue::Type type) {
    auto p = v.Find(key);
    if (!p || p->type != type)
        throw std::runtime_error(std::string("Invalid or missing field: ") + key);
    return *p;
}
float Number(const JsonValue &v, const char *key, float lo, float hi) {
    double n = Field(v, key, JsonValue::Type::Number).number;
    if (!std::isfinite(n) || n < lo || n > hi)
        throw std::runtime_error(std::string("Out of range: ") + key);
    return float(n);
}
int Integer(const JsonValue &v, const char *key, int lo, int hi) {
    double n = Field(v, key, JsonValue::Type::Number).number;
    if (!std::isfinite(n) || n < lo || n > hi || std::floor(n) != n)
        throw std::runtime_error(std::string("Expected bounded integer: ") + key);
    return int(n);
}
Vec3 Vector(const JsonValue &v, const char *key, float lo, float hi) {
    auto &a = Field(v, key, JsonValue::Type::Array).arr;
    if (a.size() != 3)
        throw std::runtime_error(std::string("Expected three values: ") + key);
    float n[3];
    for (int i = 0; i < 3; ++i) {
        if (a[i].type != JsonValue::Type::Number || !std::isfinite(a[i].number) || a[i].number < lo ||
            a[i].number > hi)
            throw std::runtime_error(std::string("Invalid vector: ") + key);
        n[i] = float(a[i].number);
    }
    return {n[0], n[1], n[2]};
}
bool Boolean(const JsonValue &v, const char *key) { return Field(v, key, JsonValue::Type::Bool).boolean; }
} // namespace
void ResetScenePage(Scene &next, const std::string &resetPage) {
    if (resetPage == "all") {
        next = Scene{};
        ResetClimatePage(next.climate, "all");
    }
    else {
        ResetWorldPage(next.world, resetPage);
        ResetClimatePage(next.climate, resetPage);
        if (resetPage == "sky") {
            next.climate[CL_sky] = 0;
            next.climate[CL_clouds] = 0;
        }
        if (resetPage == "atmosphere") {
            next.exposure = 1;
            next.tint = {1, 1, 1};
            next.fogColor = Scene{}.fogColor;
        }
        if (resetPage == "weather")
            next.weather = Weather{};
        if (resetPage == "lighting" || resetPage == "props") {
            bool lights = resetPage == "lighting";
            next.objects.erase(std::remove_if(next.objects.begin(), next.objects.end(),
                                              [&](const Object &o) { return lights ? o.IsLight() : o.IsProp(); }),
                               next.objects.end());
            if (lights)
                next.ambient = Scene{}.ambient;
        }
        if (next.objects.empty() && next.weather.follow && next.climate[CL_surfaceRadius] == 0 &&
            next.climate[CL_localFogRadius] == 0 && next.climate[CL_puddles] == 0 &&
            next.climate[CL_worldGi] == 0 && next.climate[CL_storm] == 0)
            next.map.clear();
    }
}

bool ValidName(const std::string &n) {
    if (n.empty() || n.size() > 48 || n.front() == ' ' || n.back() == ' ')
        return false;
    for (unsigned char c : n)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' ||
              c == '-' || c == '_'))
            return false;
    return true;
}
std::string SerializeScene(const Scene &s) {
    JsonBuilder b;
    b.BeginObject();
    b.IntField("version", 14);
    b.Key("climate");
    b.BeginObject();
    for (int i = 0; i < ClimateCount; ++i)
        b.DoubleField(ClimateSpecs()[i].key, s.climate.values[i]);
    b.EndObject();
    b.Key("world");
    WriteWorldLook(b, s.world);
    b.BoolField("enabled", s.enabled);
    b.StringField("map", s.map);
    b.DoubleField("exposure", s.exposure);
    b.DoubleField("ambient", s.ambient);
    Vector(b, "fogColor", s.fogColor);
    Vector(b, "tint", s.tint);
    b.Key("weather");
    b.BeginObject();
    auto &w = s.weather;
    b.IntField("type", w.type);
    b.IntField("count", w.count);
    b.IntField("seed", w.seed);
    b.DoubleField("radius", w.radius);
    b.DoubleField("height", w.height);
    b.DoubleField("speed", w.speed);
    b.DoubleField("size", w.size);
    b.DoubleField("windX", w.windX);
    b.DoubleField("windY", w.windY);
    b.BoolField("follow", w.follow);
    b.BoolField("shelter", w.shelter);
    Vector(b, "center", w.center);
    Vector(b, "color", w.color);
    b.EndObject();
    b.Key("objects");
    b.BeginArray();
    for (auto &o : s.objects) {
        b.BeginObject();
        b.IntField("id", o.id);
        b.StringField("kind", o.kind);
        b.StringField("asset", o.asset);
        b.BoolField("enabled", o.enabled);
        Vector(b, "position", o.position);
        Vector(b, "angles", o.angles);
        Vector(b, "scale", o.scale);
        Vector(b, "color", o.color);
        b.DoubleField("intensity", o.intensity);
        b.DoubleField("range", o.range);
        b.DoubleField("cone", o.cone);
        b.BoolField("shadows", o.shadows);
        b.DoubleField("width", o.studio.width); b.DoubleField("height", o.studio.height);
        b.DoubleField("sourceRadius", o.studio.sourceRadius);
        b.IntField("pattern", o.studio.pattern); b.IntField("profile", o.studio.profile);
        b.IntField("cue", o.studio.cue); b.DoubleField("patternScale", o.studio.patternScale);
        b.DoubleField("rate", o.studio.rate); b.DoubleField("amount", o.studio.amount);
        b.DoubleField("phase", o.studio.phase);
        b.BoolField("cameraVisible",o.cameraVisible);b.BoolField("materialEnabled",o.material.enabled);
        b.StringField("group",o.group);b.BoolField("render",o.render);b.BoolField("locked",o.locked);
        if(!o.clip.empty()){b.StringField("clip",o.clip);b.DoubleField("clipSpeed",o.clipSpeed);}
        if(o.IsCamera())b.DoubleField("fov",o.fov);
        b.DoubleField("roughness",o.material.roughness);b.DoubleField("metallic",o.material.metallic);
        b.DoubleField("coat",o.material.coat);b.DoubleField("coatRoughness",o.material.coatRoughness);
        b.DoubleField("anisotropy",o.material.anisotropy);b.DoubleField("emission",o.material.emission);
        b.DoubleField("normalStrength",o.material.normalStrength);b.DoubleField("uvScale",o.material.uvScale);
        b.StringField("albedoTexture",o.material.albedoTexture);b.StringField("normalTexture",o.material.normalTexture);
        b.StringField("roughnessTexture",o.material.roughnessTexture);
        b.EndObject();
    }
    b.EndArray();
    { int next = s.nextObjectId; for (const auto &o : s.objects) next = (std::max)(next, o.id + 1); b.IntField("nextObjectId", next); } // what a load computes, so a round trip is exact
    b.Key("held");
    b.BeginObject();
    for (auto &h : s.held)
        b.DoubleField(h.first.c_str(), h.second);
    b.EndObject();
    b.EndObject();
    return b.Str();
}
bool ParseScene(const std::string &json, Scene &out, std::string &error) {
    // Bound nesting before the general recursive parser sees untrusted preset files.
    if (json.size() > 1024 * 1024) {
        error = "Preset exceeds 1 MB";
        return false;
    }
    int depth = 0;
    bool quoted = false, escaped = false;
    for (char c : json) {
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '\"')
                quoted = false;
        } else {
            if (c == '\"')
                quoted = true;
            else if (c == '{' || c == '[') {
                if (++depth > 8) {
                    error = "Preset nesting exceeds limit";
                    return false;
                }
            } else if (c == '}' || c == ']')
                --depth;
        }
    }
    try {
        JsonValue root;
        if (!JsonParse(json, root))
            throw std::runtime_error("Malformed JSON");
        int version = Integer(root, "version", 1, 1000000);
        if (version != 14)
            throw std::runtime_error("This environment preset uses an incompatible format. Create a new scene with the current editor.");
        Scene s;
        if (!ReadWorldLook(Field(root, "world", JsonValue::Type::Object), s.world, error))
            throw std::runtime_error(error);
        const auto &values = Field(root, "climate", JsonValue::Type::Object);
        for (int i = 0; i < ClimateCount; ++i) {
            double n = Field(values, ClimateSpecs()[i].key, JsonValue::Type::Number).number;
            if (!ClimateValueValid(i, n))
                throw std::runtime_error(std::string("Invalid climate value: ") + ClimateSpecs()[i].key);
            s.climate.values[i] = float(n);
        }
        s.climate[CL_ssr] = 0;
        s.climate[CL_gi] = 0;
        s.enabled = Boolean(root, "enabled");
        s.map = Field(root, "map", JsonValue::Type::String).str;
        if (s.map.size() > 128)
            throw std::runtime_error("Invalid map name");
        s.exposure = Number(root, "exposure", .05f, 4);
        s.ambient = Number(root, "ambient", 0, 2);
        s.fogColor = Vector(root, "fogColor", 0, 1);
        s.tint = Vector(root, "tint", 0, 1);
        // Absent in scenes saved before anything was held: nothing is held.
        if (const JsonValue *held = root.Find("held")) {
            if (held->type != JsonValue::Type::Object || held->obj.size() > ClimateCount + 1)
                throw std::runtime_error("Invalid held values");
            for (auto &h : held->obj) {
                if (h.second.type != JsonValue::Type::Number)
                    throw std::runtime_error("Invalid held value: " + h.first);
                if (h.first == HeldWeatherTypeKey()) {
                    if (h.second.number < 0 || h.second.number > 6 || std::floor(h.second.number) != h.second.number)
                        throw std::runtime_error("Invalid held weather type");
                } else {
                    int field = -1;
                    for (int i = 0; i < ClimateCount; ++i)
                        if (h.first == ClimateSpecs()[i].key) field = i;
                    if (!ClimateValueValid(field, h.second.number))
                        throw std::runtime_error("Invalid held value: " + h.first);
                }
                s.held[h.first] = float(h.second.number);
            }
        }
        auto &v = Field(root, "weather", JsonValue::Type::Object);
        auto &w = s.weather;
        w.type = Integer(v, "type", 0, 6);
        w.count = Integer(v, "count", 0, 6000);
        w.seed = Integer(v, "seed", 0, 65535);
        w.radius = Number(v, "radius", 64, 4096);
        w.height = Number(v, "height", 64, 4096);
        w.speed = Number(v, "speed", 0, 1500);
        w.size = Number(v, "size", .1f, 12);
        w.windX = Number(v, "windX", -1000, 1000);
        w.windY = Number(v, "windY", -1000, 1000);
        w.follow = Boolean(v, "follow");
        w.shelter = version>=10 ? Boolean(v,"shelter") : false;
        w.center = Vector(v, "center", -32768, 32768);
        w.color = Vector(v, "color", 0, 1);
        auto &objects = Field(root, "objects", JsonValue::Type::Array).arr;
        if (objects.size() > 80)
            throw std::runtime_error("Scene exceeds object limit");
        std::set<int> ids;
        int lights = 0, props = 0, cameras = 0, shadowLights = 0;
        for (auto &v : objects) {
            Object o;
            o.id = Integer(v, "id", 1, 1000000);
            if (!ids.insert(o.id).second)
                throw std::runtime_error("Duplicate object ID");
            o.kind = Field(v, "kind", JsonValue::Type::String).str;
            o.asset = Field(v, "asset", JsonValue::Type::String).str;
            o.enabled = Boolean(v, "enabled");
            // Placed puddles and snow drifts were removed (they are weather, Weather > Ground):
            // a scene saved with them loads without them.
            if (o.kind == "snowdrift" || o.kind == "puddle")
                continue;
            if (o.kind != "box" && o.kind != "sphere" && o.kind != "cylinder" && o.kind != "beveledbox" && o.kind != "obj" &&
                !o.IsLight() && !o.IsCamera())
                throw std::runtime_error("Unknown object kind");
            if (o.kind == "obj" && !ValidName(o.asset))
                throw std::runtime_error("Invalid mesh asset name");
            if (o.IsLight())
                ++lights;
            else if (o.IsCamera())
                ++cameras;
            else
                ++props;
            o.position = Vector(v, "position", -32768, 32768);
            o.angles = Vector(v, "angles", -360, 360);
            o.scale = Vector(v, "scale", .1f, 2048);
            o.color = Vector(v, "color", 0, 1);
            o.intensity = Number(v, "intensity", 0, 8);
            o.range = Number(v, "range", 16, 4096);
            o.cone = Number(v, "cone", 5, 170);
            if (version >= 5)
                o.shadows = Boolean(v, "shadows");
            if (version >= 7) {
                o.studio.width=Number(v,"width",1,1024); o.studio.height=Number(v,"height",1,1024);
                o.studio.sourceRadius=Number(v,"sourceRadius",0,64);
                o.studio.pattern=Integer(v,"pattern",0,4); o.studio.profile=Integer(v,"profile",0,3);
                o.studio.cue=Integer(v,"cue",0,4); o.studio.patternScale=Number(v,"patternScale",.1f,32);
                o.studio.rate=Number(v,"rate",0,20); o.studio.amount=Number(v,"amount",0,1);
                o.studio.phase=Number(v,"phase",0,1);
                if(o.kind=="point" && (o.studio.pattern || o.studio.profile))
                    throw std::runtime_error("Projectors and directional profiles require a spot or area light");
            }
            if(version>=8){
                o.cameraVisible=Boolean(v,"cameraVisible");o.material.enabled=Boolean(v,"materialEnabled");
                auto &m=o.material;m.roughness=Number(v,"roughness",.05f,1);m.metallic=Number(v,"metallic",0,1);
                m.coat=Number(v,"coat",0,1);m.coatRoughness=Number(v,"coatRoughness",.05f,1);
                m.anisotropy=Number(v,"anisotropy",-.9f,.9f);m.emission=Number(v,"emission",0,16);
                m.normalStrength=Number(v,"normalStrength",0,4);m.uvScale=Number(v,"uvScale",.01f,100);
                m.albedoTexture=Field(v,"albedoTexture",JsonValue::Type::String).str;
                m.normalTexture=Field(v,"normalTexture",JsonValue::Type::String).str;
                m.roughnessTexture=Field(v,"roughnessTexture",JsonValue::Type::String).str;
                for(const auto &name:{m.albedoTexture,m.normalTexture,m.roughnessTexture})
                    if(!name.empty()&&!ValidName(name))throw std::runtime_error("Invalid texture asset name");
                if(!o.IsProp()&&(!o.cameraVisible||m.enabled))throw std::runtime_error("Surface material and camera visibility belong to props");
            }
            // Outliner fields: optional, so a scene saved before the outliner existed keeps its
            // objects ungrouped, rendered and unlocked.
            if(auto g=v.Find("group");g&&g->type==JsonValue::Type::String){
                if(g->str.size()>40)throw std::runtime_error("Group names are at most 40 characters");
                o.group=g->str;
            }
            if(auto r=v.Find("render"))o.render=r->AsBool(true);
            if(auto l=v.Find("locked"))o.locked=l->AsBool(false);
            // A GLB's animation (optional: absent = the rest pose).
            if(auto c=v.Find("clip");c&&c->type==JsonValue::Type::String){
                if(c->str.size()>128)throw std::runtime_error("Clip names are at most 128 characters");
                o.clip=c->str;
            }
            if(auto s=v.Find("clipSpeed"))o.clipSpeed=std::clamp(float(s->AsNumber(1)),-4.f,4.f);
            if(o.IsCamera())o.fov=v.Find("fov")?Number(v,"fov",1,179):90.f;
            if (o.shadows && !o.IsLight())
                throw std::runtime_error("Shadow enable belongs to lights");
            if (o.shadows)
                ++shadowLights;
            s.objects.push_back(o);
        }
        if (lights > 16 || props > 64 || cameras > 16)
            throw std::runtime_error("Maximum 16 lights, 64 props and 16 cameras");
        // Optional (absent = one past the highest id); never below one past the highest id.
        s.nextObjectId = 1;
        for (const auto &o : s.objects) s.nextObjectId = (std::max)(s.nextObjectId, o.id + 1);
        if (const auto* next = root.Find("nextObjectId")) {
            if (next->type != JsonValue::Type::Number || !std::isfinite(next->number) || next->number < 1 ||
                next->number > 1000001 || next->number != std::floor(next->number))
                throw std::runtime_error("Invalid next object ID");
            s.nextObjectId = (std::max)(s.nextObjectId, int(next->number));
        }
        if (shadowLights > 4)
            throw std::runtime_error(
                "Maximum four shadow-casting added lights; disable shadows on another light first");
        out = std::move(s);
        error.clear();
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
} // namespace Filmmaker::Environment
