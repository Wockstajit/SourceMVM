#include "DeadlockSettings.h"

#include "DeadlockFollowCamera.h"
#include "DeadlockRender.h"
#include "DeadlockShot.h"
#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"
#include "../Platform/TextEncoding.h"
#include "../Debug/MvmDebugLog.h"
#include "../../../Shared/AfxConsole.h"

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace Filmmaker {
namespace {

namespace fs = std::filesystem;

bool g_loaded = false;
ULONGLONG g_nextPoll = 0;
std::string g_lastSnapshot;

std::wstring SettingsPath() {
	wchar_t appData[32768] = {};
	const DWORD n = GetEnvironmentVariableW(L"APPDATA", appData, (DWORD)_countof(appData));
	if (!n || n >= _countof(appData)) return {};
	return (fs::path(std::wstring(appData, n)) / L"HLAE" / L"deadlock-filmmaker.json").wstring();
}

std::string Snapshot() {
	JsonBuilder b;
	b.BeginObject();
	b.IntField("version", 1);

	const FollowSettings& f = FollowCamera_Get();
	b.Key("follow"); b.BeginObject();
	b.IntField("mode", (int)f.mode);
	b.StringField("attach", f.attach);
	b.DoubleField("offX", f.offX); b.DoubleField("offY", f.offY); b.DoubleField("offZ", f.offZ);
	b.DoubleField("rotPitch", f.rotPitch); b.DoubleField("rotYaw", f.rotYaw); b.DoubleField("rotRoll", f.rotRoll);
	b.DoubleField("fov", f.fov);
	b.DoubleField("look", f.look); b.DoubleField("pos", f.pos); b.DoubleField("predict", f.predict);
	b.DoubleField("deadzone", f.deadzone); b.DoubleField("maxTurn", f.maxTurn);
	b.DoubleField("deathHold", f.deathHold);
	b.BoolField("releaseOnDeath", f.releaseOnDeath);
	b.BoolField("holdLast", f.holdLast); b.BoolField("retarget", f.retarget);
	b.EndObject();

	b.Key("shotView"); b.BeginObject();
	// drawPath / drawMarkers are deliberately NOT saved: the in-world path draws automatically
	// like CS2's, and a stale "off" from an old file was overriding that on every launch.
	b.BoolField("freeze", Shot_Freeze());
	b.EndObject();

	// Handheld camera shake is not saved. It was removed from Deadlock entirely 2026-09-14 -- no
	// UI exposes it, and a settings file is exactly how it got stuck ON in the first place (an old
	// file's "enabled": true re-armed it every launch with no control anywhere to turn it back
	// off). A field this repo does not read is a field that cannot silently reintroduce it.

	const RenderSettings r = Render_GetSettings();
	b.Key("render"); b.BeginObject();
	b.IntField("fps", r.fps); b.IntField("startTick", r.startTick); b.IntField("endTick", r.endTick);
	b.StringField("outputDir", WideToUtf8(r.outputDir));
	b.StringField("range", r.range); b.StringField("camSrc", r.camSrc);
	b.StringField("hud", r.hud); b.StringField("format", r.format);
	b.IntField("quality", r.quality); b.IntField("encodingVersion", 2);
	b.IntField("width", r.width); b.IntField("height", r.height); b.IntField("bitrateKbps", r.bitrateKbps);
	b.StringField("encoder", r.encoder); b.StringField("speed", r.speed);
	b.BoolField("names", r.names); b.BoolField("damage", r.damage);
	b.BoolField("pauseOnDrop", r.pauseOnDrop); b.BoolField("beep", r.beep);
	b.EndObject();
	b.EndObject();
	return b.Str();
}

void LoadNumber(const JsonValue* obj, const char* key, const char* setting) {
	if (!obj) return;
	if (const JsonValue* v = obj->Find(key); v && v->type == JsonValue::Type::Number)
		FollowCamera_SetValue(setting, v->number);
}

void Load() {
	const std::wstring path = SettingsPath();
	if (path.empty()) return;
	std::ifstream in(path, std::ios::binary);
	if (!in) return;
	const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	JsonValue root;
	if (!JsonParse(text, root) || root.type != JsonValue::Type::Object) {
		advancedfx::Warning("AfxHookDeadlock: ignored malformed filmmaker settings: %s\n",
			WideToUtf8(path).c_str());
		return;
	}

	if (const JsonValue* f = root.Find("follow")) {
		if (const JsonValue* v = f->Find("mode"))
			FollowCamera_SetMode(v->AsInt(0) == 1 ? FollowMode::Attach : FollowMode::LockOn);
		if (const JsonValue* v = f->Find("attach")) FollowCamera_SetAttach(v->AsString("chest").c_str());
		// A settings file saved by the pre-P5 build has "yaw"/"pitch"/"dist"/"height"/"lateral"
		// instead -- none of those keys exist here any more, so an old file just falls back to the
		// new defaults for framing rather than applying stale orbit values under new names.
		LoadNumber(f, "offX", "offx"); LoadNumber(f, "offY", "offy"); LoadNumber(f, "offZ", "offz");
		LoadNumber(f, "rotPitch", "rotpitch"); LoadNumber(f, "rotYaw", "rotyaw"); LoadNumber(f, "rotRoll", "rotroll");
		LoadNumber(f, "fov", "fov");
		LoadNumber(f, "look", "look"); LoadNumber(f, "pos", "pos"); LoadNumber(f, "predict", "predict");
		LoadNumber(f, "deadzone", "deadzone"); LoadNumber(f, "maxTurn", "maxturn");
		LoadNumber(f, "deathHold", "deathhold");
		if (const JsonValue* v = f->Find("releaseOnDeath")) FollowCamera_SetFlag("releaseondeath", v->AsBool(true));
		if (const JsonValue* v = f->Find("holdLast")) FollowCamera_SetFlag("holdlast", v->AsBool(false));
		if (const JsonValue* v = f->Find("retarget")) FollowCamera_SetFlag("retarget", v->AsBool(true));
	}

	if (const JsonValue* s = root.Find("shotView")) {
		// drawPath / drawMarkers from older files are ignored on purpose -- see Save above.
		if (const JsonValue* v = s->Find("freeze")) Shot_SetFreeze(v->AsBool(false));
	}

	// A "handheld" block from an older settings file is ignored outright -- see the comment in
	// Save() above. There is no HandheldCam in Deadlock any more to apply it to.

	if (const JsonValue* r = root.Find("render")) {
		if (const JsonValue* v = r->Find("fps")) Render_SetFps(v->AsInt(32));
		const int start = r->Find("startTick") ? r->Find("startTick")->AsInt(0) : 0;
		const int end = r->Find("endTick") ? r->Find("endTick")->AsInt(0) : 0;
		Render_SetRange(start, end);
		if (const JsonValue* v = r->Find("outputDir")) Render_SetOutputDir(Utf8ToWide(v->AsString("")));
		if (const JsonValue* v = r->Find("range")) Render_SetOption("range", v->AsString("all").c_str());
		if (const JsonValue* v = r->Find("camSrc")) Render_SetOption("camsrc", v->AsString("current").c_str());
		if (const JsonValue* v = r->Find("hud")) Render_SetOption("hud", v->AsString("game").c_str());
		if (const JsonValue* v = r->Find("format")) Render_SetOption("format", v->AsString("png").c_str());
		if (r->Find("encodingVersion")) { if (const JsonValue* v = r->Find("quality")) Render_SetQuality(v->AsInt(18)); }
        Render_SetSize(r->Find("width") ? r->Find("width")->AsInt(0) : 0, r->Find("height") ? r->Find("height")->AsInt(0) : 0);
        if (const JsonValue* v = r->Find("bitrateKbps")) Render_SetBitrate(v->AsInt(0));
        if (const JsonValue* v = r->Find("encoder")) Render_SetOption("encoder", v->AsString("auto").c_str());
        if (const JsonValue* v = r->Find("speed")) Render_SetOption("speed", v->AsString("balanced").c_str());
		if (const JsonValue* v = r->Find("names")) Render_SetFlag("names", v->AsBool(true));
		if (const JsonValue* v = r->Find("damage")) Render_SetFlag("damage", v->AsBool(true));
		if (const JsonValue* v = r->Find("pauseOnDrop")) Render_SetFlag("pauseondrop", v->AsBool(true));
		if (const JsonValue* v = r->Find("beep")) Render_SetFlag("beep", v->AsBool(false));
	}

	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("deadlock.settings", "loaded path=%ls bytes=%d", path.c_str(), (int)text.size());
}

bool Save(const std::string& snapshot) {
	const std::wstring path = SettingsPath();
	if (path.empty()) return false;
	std::error_code ec;
	fs::create_directories(fs::path(path).parent_path(), ec);
	const std::wstring temp = path + L".tmp";
	{
		std::ofstream out(temp, std::ios::binary | std::ios::trunc);
		if (!out) return false;
		out.write(snapshot.data(), (std::streamsize)snapshot.size());
		out.flush();
		if (!out.good()) return false;
	}
	if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		DeleteFileW(temp.c_str());
		return false;
	}
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("deadlock.settings", "saved path=%ls bytes=%d", path.c_str(), (int)snapshot.size());
	return true;
}

} // namespace

void DeadlockSettings_RunFrame() {
	if (!g_loaded) {
		g_loaded = true;
		Load();
		g_lastSnapshot = Snapshot();
		g_nextPoll = GetTickCount64() + 250;
		return;
	}
	const ULONGLONG now = GetTickCount64();
	if (now < g_nextPoll) return;
	g_nextPoll = now + 250;
	const std::string snapshot = Snapshot();
	if (snapshot == g_lastSnapshot) return;
	if (Save(snapshot)) g_lastSnapshot = snapshot;
}

} // namespace Filmmaker
