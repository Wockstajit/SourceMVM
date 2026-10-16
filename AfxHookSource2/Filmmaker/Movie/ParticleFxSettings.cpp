// ParticleFx persistence: ParticleFx::LoadSettings / SaveSettings against
// %APPDATA%\HLAE\filmmaker_fx.json (master switch, moneyshot, per-category modes,
// custom block/swap rules).

#include "ParticleFxInternal.h"

#include "../Config/PracticeSessionFlag.h" // a practice session saves nothing

#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"
#include "../../hlaeFolder.h" // GetHlaeRoamingAppDataFolderW

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace Filmmaker {
namespace fx {

namespace {

std::wstring SettingsPath() {
	std::wstring path = GetHlaeRoamingAppDataFolderW();
	if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
		path += L'\\';
	path += L"filmmaker_fx.json";
	return path;
}

} // namespace

} // namespace fx

using namespace fx;

namespace {

// Everything filmmaker_fx.json holds, as its JSON.
std::string SettingsJson() {
	JsonBuilder b;
	std::lock_guard<std::mutex> lock(g_mx);
	b.BeginObject();
	b.BoolField("enabled", g_enabled);
	b.BoolField("moneyHeadshot", g_moneyHeadshot);
	b.BoolField("noFlashBlind", g_noFlashBlind.load());
	b.BoolField("mollyLights",g_mollyLights.load());
	b.BoolField("heLights",g_heLights.load());
	b.BoolField("flashLights",g_flashLights.load());
	b.Key("modes");
	b.BeginObject();
	for (int i = 0; i < kFxCategoryCount; ++i)
		b.StringField(kCategoryKeys[i], kModeNames[(int)g_modes[i]]);
	b.EndObject();
	b.Key("custom");
	b.BeginArray();
	for (const CustomRule& r : g_customRules) {
		b.BeginObject();
		b.StringField("match", r.match);
		if (!r.target.empty())
			b.StringField("target", r.target);
		b.EndObject();
	}
	b.EndArray();
	b.EndObject();
	return b.Str();
}

// The construction defaults as that JSON, taken the first time the settings are loaded, before the
// file is read. A practice session (Clippy's tutorial) puts them back.
std::string g_defaultSettingsJson;

bool ReadSettingsFile(JsonValue& root) {
	std::ifstream f{ std::filesystem::path(SettingsPath()) };
	if (!f.is_open())
		return false;
	std::stringstream ss;
	ss << f.rdbuf();
	return JsonParse(ss.str(), root) && root.type == JsonValue::Type::Object;
}

void ApplySettings(const JsonValue& root) {
	std::lock_guard<std::mutex> lock(g_mx);
	if (const JsonValue* v = root.Find("enabled"))
		g_enabled = v->AsBool(false);
	if (const JsonValue* v = root.Find("moneyHeadshot"))
		g_moneyHeadshot = v->AsBool(false);
	if (const JsonValue* v = root.Find("noFlashBlind"))
		g_noFlashBlind.store(v->AsBool(false));
	if(const JsonValue* v=root.Find("mollyLights"))g_mollyLights.store(v->AsBool(false));
	if(const JsonValue* v=root.Find("heLights"))g_heLights.store(v->AsBool(false));
	if(const JsonValue* v=root.Find("flashLights"))g_flashLights.store(v->AsBool(false));
	if (const JsonValue* modes = root.Find("modes"); modes && modes->type == JsonValue::Type::Object) {
		for (int i = 0; i < kFxCategoryCount; ++i) {
			if (const JsonValue* m = modes->Find(kCategoryKeys[i])) {
				const int mi = ModeFromName(m->AsString("off").c_str());
				g_modes[i] = NormalizeMode(i, mi >= 0 ? (FxMode)mi : FxMode::Off);
			}
		}
	}
	g_customRules.clear();
	if (const JsonValue* rules = root.Find("custom"); rules && rules->type == JsonValue::Type::Array) {
		for (const JsonValue& r : rules->arr) {
			if (r.type != JsonValue::Type::Object)
				continue;
			CustomRule cr;
			if (const JsonValue* m = r.Find("match"))
				cr.match = m->AsString();
			if (const JsonValue* t = r.Find("target"))
				cr.target = t->AsString();
			std::transform(cr.match.begin(), cr.match.end(), cr.match.begin(),
				[](unsigned char c) { return (char)std::tolower(c); });
			if (!cr.match.empty())
				g_customRules.push_back(std::move(cr));
		}
	}
	RebuildActiveSwapTargetsLocked(false);
}

void ApplyDefaults() {
	JsonValue root;
	if (!g_defaultSettingsJson.empty() && JsonParse(g_defaultSettingsJson, root)
		&& root.type == JsonValue::Type::Object)
		ApplySettings(root);
}

} // namespace

void ParticleFx::LoadSettings() {
	if (g_defaultSettingsJson.empty())
		g_defaultSettingsJson = SettingsJson();
	JsonValue root;
	if (ReadSettingsFile(root))
		ApplySettings(root);
}

bool ParticleFx::SaveSettings() const {
	// A practice session keeps its effects in memory; the file stays the user's.
	if (PracticeSession_Active())
		return true;
	const std::string text = SettingsJson();
	std::ofstream f{ std::filesystem::path(SettingsPath()), std::ios::trunc };
	if (!f.is_open())
		return false;
	f << text;
	return true;
}

void ParticleFx::ResetSettingsToDefaults() {
	ApplyDefaults();
	AfterSettingsReplaced();
}

void ParticleFx::ReloadSettings() {
	// Defaults first, then the file over them: a missing file means the user had the defaults.
	ApplyDefaults();
	JsonValue root;
	if (ReadSettingsFile(root))
		ApplySettings(root);
	AfterSettingsReplaced();
}

} // namespace Filmmaker
