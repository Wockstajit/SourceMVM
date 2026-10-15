#include "EnvironmentWorld.h"
#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"
#include <cmath>
#include <stdexcept>
namespace Filmmaker::Environment {
const std::vector<WorldField> &WorldFields() {
    static const std::vector<WorldField> fields = {
        {"exposure", "lighting", {-1}, -1, 10},
        {"lightColor", "lighting", {255, 255, 255}, 0, 255, true},
        {"lightAmount", "lighting", {1}, 0, 1},
        {"lightUniform", "lighting", {1}, 0, 1, true},
        {"lightDirection", "lighting", {0, 0}, -360, 360},
        {"sunPositionOverride", "lighting", {0}, 0, 1, true},
        {"sunPosition", "lighting", {0, 0}, -360, 360},
        {"sunSize", "lighting", {1}, .05, 20},
        {"sunBrightness", "lighting", {1}, 0, 5},
        {"sunGlow", "lighting", {1}, 0, 3},
        {"sunGlowSize", "lighting", {1}, .05, 8},
        {"sunVisible", "lighting", {1}, 0, 1, true},
        {"shadows", "lighting", {1}, 0, 1, true},
        {"engineLights", "lighting", {1}, 0, 1, true},
        {"shadowLayers", "lighting", {1, 1}, 0, 1, true},
        {"extraShadows", "lighting", {0}, 0, 1, true},
        {"shadowStrength", "lighting", {.65}, 0, 1},
        {"shadowStep", "lighting", {2.5}, .25, 16},
        {"shadowThickness", "lighting", {.0008}, .00001, .05},
        {"shadowSteps", "lighting", {16}, 1, 32, true},
        {"skyDirection", "sky", {0, 0}, -360, 360},
        {"skyMirror", "sky", {0}, 0, 1, true},
        {"skyTargets", "sky", {1, 1}, 0, 1, true},
        {"skyTint", "sky", {0}, 0, 1, true},
        {"skyColor", "sky", {255, 255, 255, 255}, 0, 255, true},
        {"cloudTint", "sky", {0}, 0, 1, true},
        {"cloudColor", "sky", {255, 255, 255}, 0, 255, true},
        {"cloudsVisible", "sky", {1}, 0, 1, true},
        {"cloudDensity", "sky", {1}, 0, 3},
        {"cloudBrightness", "sky", {1}, 0, 3},
        {"cloudSize", "sky", {1}, .1, 4},
        {"cloudDrift", "sky", {1}, 0, 8},
        {"worldTint", "atmosphere", {0, 0, 0, 0}, 0, 1, true},
        {"worldColors",
         "atmosphere",
         {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
         0,
         255,
         true},
        {"worldStrength", "atmosphere", {1}, 0, 1},
        {"smokeTint", "atmosphere", {0, 0}, 0, 1, true},
        {"smokeColors", "atmosphere", {255, 255, 255, 255, 255, 255}, 0, 255, true},
    };
    return fields;
}
WorldLook DefaultWorldLook() {
    WorldLook result;
    for (const auto &field : WorldFields())
        result.fields[field.name] = field.defaults;
    return result;
}
void ResetWorldPage(WorldLook &look, const std::string &page) {
    if (page == "all" || page == "sky")
        look.sky.clear();
    for (const auto &field : WorldFields())
        if (page == "all" || page == field.page)
            look.fields[field.name] = field.defaults;
}
void WriteWorldLook(JsonBuilder &out, const WorldLook &look) {
    out.BeginObject();
    out.StringField("sky", look.sky);
    for (const auto &field : WorldFields()) {
        out.Key(field.name);
        out.BeginArray();
        auto found = look.fields.find(field.name);
        for (double n : found == look.fields.end() ? field.defaults : found->second)
            out.ValueDouble(n);
        out.EndArray();
    }
    out.EndObject();
}
bool ReadWorldLook(const JsonValue &value, WorldLook &out, std::string &error) {
    try {
        WorldLook pending;
        auto sky = value.Find("sky");
        if (!sky || sky->type != JsonValue::Type::String || sky->str.size() > 128)
            throw std::runtime_error("Invalid sky identifier");
        pending.sky = sky->str;
        for (const auto &field : WorldFields()) {
            auto v = value.Find(field.name);
            // Optional independent controls within format 14. A format-14 scene written before
            // these controls existed had exactly the master shadow state and enabled direct
            // lights. Preserve that behavior; pre-14 fog/quality schemas are still rejected.
            if (!v && std::string(field.name) == "engineLights") {
                pending.fields[field.name] = {1}; continue;
            }
            if (!v && std::string(field.name) == "shadowLayers") {
                const auto &master = pending.fields.at("shadows");
                pending.fields[field.name] = {master[0], master[0]}; continue;
            }
            if (!v || v->type != JsonValue::Type::Array || v->arr.size() != field.defaults.size())
                throw std::runtime_error(std::string("Invalid environment field: ") + field.name);
            auto &numbers = pending.fields[field.name];
            for (const auto &item : v->arr) {
                double n = item.number;
                if (item.type != JsonValue::Type::Number || !std::isfinite(n) || n < field.min ||
                    n > field.max || (field.integer && std::floor(n) != n))
                    throw std::runtime_error(std::string("Invalid environment value: ") + field.name);
                numbers.push_back(n);
            }
        }
        out = std::move(pending);
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
} // namespace Filmmaker::Environment
