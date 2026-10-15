#pragma once
#include <map>
#include <string>
#include <vector>
namespace Filmmaker {
struct JsonValue;
class JsonBuilder;
} // namespace Filmmaker
namespace Filmmaker::Environment {
// Named numeric data, not executable console text. One schema owns defaults and validation.
struct WorldLook {
    std::string sky;
    std::map<std::string, std::vector<double>> fields;
};
struct WorldField {
    const char *name;
    const char *page;
    std::vector<double> defaults;
    double min, max;
    bool integer = false;
};
const std::vector<WorldField> &WorldFields();
WorldLook DefaultWorldLook();
void WriteWorldLook(JsonBuilder &out, const WorldLook &look);
bool ReadWorldLook(const JsonValue &value, WorldLook &out, std::string &error);
void ResetWorldPage(WorldLook &look, const std::string &page);
enum class WorldLookCapture { Live, User };
// Rendering uses live effects; persistence/undo/UI preferences use the user's original lighting.
WorldLook CaptureWorldLook(WorldLookCapture source = WorldLookCapture::Live);
bool PrepareWorldLook(const WorldLook &look, std::string &error);
void ApplyWorldLook(const WorldLook &look, const std::string &page = "all");
void WorldLook_OnLevelInit();
} // namespace Filmmaker::Environment
