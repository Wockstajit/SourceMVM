#include "stdafx.h"

#include "SourceSettings.h"
#include "SourceDemoIndex.h"

#include "hlaeFolder.h"

#include <AfxHookSource2/Filmmaker/Config/CameraFeelSettings.h>
#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <AfxHookSource2/Filmmaker/Movie/RecoilPivotMarker.h>
#include <AfxHookSource2/Filmmaker/Movie/WeaponPivot.h>
#include <AfxHookSource2/Filmmaker/Movie/WeaponRecoil.h>
#include <AfxHookSource2/Filmmaker/Platform/JsonBuilder.h>
#include <AfxHookSource2/Filmmaker/Platform/JsonParser.h>
#include <AfxHookSource2/Filmmaker/Platform/TextEncoding.h>
#include <AfxHookSource2/Filmmaker/Render/RenderDoneSound.h>
#include <AfxHookSource2/Filmmaker/Render/RenderOutput.h>
#include <Shared/AfxConsole.h>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace SourceFilmmaker {

namespace {

constexpr unsigned long long kSaveDebounceMs = 500;

bool g_loaded = false;
bool g_keepCamera = true;
bool g_scrubCache = true; // the TIMELINE dock's Cache: backward drags over played ticks from kept frames
unsigned long long g_pictureRevision = 0; // the latest snapshot's hash: the camera feel changes the picture
std::string g_defaults;  // the modules as constructed, before the file
std::string g_saved;     // what the file holds
std::string g_pending;   // the latest snapshot
unsigned long long g_changedAt = 0;
bool g_dirty = false;
// A keep-off reset holds the file until the live state moves away from what the reset left.
bool g_quiet = false;
std::string g_quietSnapshot;
unsigned g_generation = 0; // the demo the keep rule last saw
unsigned long long g_saves = 0;
std::string g_lastError;

std::wstring SettingsPath() {
	std::wstring path = GetHlaeRoamingAppDataFolderW();
	if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
	return path + L"filmmaker_settings_source1.json";
}

// render false leaves out the Render workspace's fields, which change no picture (the revision).
std::string BuildSnapshot(bool render = true) {
	Filmmaker::JsonBuilder b;
	b.BeginObject();
	b.IntField("version", 1);
	Filmmaker::CameraFeel_Write(b);
	b.BoolField("keepCameraAcrossDemos", g_keepCamera);
	b.BoolField("scrubCache", g_scrubCache);
	if (render) {
		// CS2's keys (FilmmakerSettingsStore.cpp): the workspace opens on the last output folder.
		b.StringField("renderOutputFolder", Filmmaker::WideToUtf8(Filmmaker::RenderOutputSettingsRef().folder));
		b.BoolField("renderDoneSound", Filmmaker::RenderDoneSound_Enabled());
		b.IntField("renderDoneSoundVolume", Filmmaker::RenderDoneSound_Volume());
	}
	b.EndObject();
	return b.Str();
}

bool Save(const std::string& snapshot) {
	std::ofstream file{ std::filesystem::path(SettingsPath()), std::ios::trunc };
	if (!file.is_open()) {
		g_lastError = "could not open the file for writing";
		return false;
	}
	file << snapshot;
	if (!file.good()) {
		g_lastError = "the write failed";
		return false;
	}
	++g_saves;
	g_lastError.clear();
	return true;
}

const Filmmaker::JsonValue* Object(const Filmmaker::JsonValue& parent, const char* key) {
	const Filmmaker::JsonValue* value = parent.Find(key);
	return value && value->type == Filmmaker::JsonValue::Type::Object ? value : nullptr;
}

bool Parse(const std::string& text, Filmmaker::JsonValue& root) {
	return Filmmaker::JsonParse(text, root) && root.type == Filmmaker::JsonValue::Type::Object;
}

// Puts groups of the defaults back, through the same readers the file goes through. Weapon Recoil's
// presets are not a setting and stay. True when the group is known.
bool ResetGroup(const char* group) {
	Filmmaker::JsonValue root;
	if (!group || !Parse(g_defaults, root)) return false;
	const bool all = 0 == _stricmp(group, "all");
	bool matched = all;
	if (all || 0 == _stricmp(group, "smoothView")) { Filmmaker::CameraFeel_LoadSmoothView(Object(root, "smoothView")); matched = true; }
	if (all || 0 == _stricmp(group, "handheld")) { Filmmaker::CameraFeel_LoadHandheld(Object(root, "handheld")); matched = true; }
	if (all || 0 == _stricmp(group, "weaponRecoil")) { Filmmaker::WeaponRecoilRef().LoadState(Object(root, "weaponRecoil")); matched = true; }
	if (all || 0 == _stricmp(group, "weaponBob")) { Filmmaker::CameraFeel_LoadWeaponBob(Object(root, "weaponBob")); matched = true; }
	return matched;
}

// Keep between demos is off and a new demo started: the CAMERA tab back to its defaults, as CS2's
// RESET CAMERA, without the file hearing of it.
void QuietCameraReset() {
	ResetGroup("all");
	for (int owner = 0; owner < Filmmaker::kWeaponPivotOwnerCount; ++owner) {
		Filmmaker::WeaponPivotRef((Filmmaker::WeaponPivotOwner)owner).ResetToDefaults();
		Filmmaker::RecoilPivotMarkerRef().SetEnabled((Filmmaker::WeaponPivotOwner)owner, false);
	}
	g_quiet = true;
	g_quietSnapshot = BuildSnapshot();
	g_pending = g_quietSnapshot;
	g_dirty = false;
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("settings", "keep between demos is off: camera reset for the new demo");
}

} // namespace

void Settings_Load() {
	if (g_loaded) return;
	g_loaded = true;
	// First: the modules still hold their construction defaults here, and only here.
	g_defaults = BuildSnapshot();
	std::ifstream file{ std::filesystem::path(SettingsPath()) };
	if (file.is_open()) {
		std::stringstream text;
		text << file.rdbuf();
		Filmmaker::JsonValue root;
		if (Parse(text.str(), root)) {
			Filmmaker::CameraFeel_Load(root);
			if (const Filmmaker::JsonValue* keep = root.Find("keepCameraAcrossDemos"); keep && keep->type == Filmmaker::JsonValue::Type::Bool)
				g_keepCamera = keep->boolean;
			if (const Filmmaker::JsonValue* cache = root.Find("scrubCache"); cache && cache->type == Filmmaker::JsonValue::Type::Bool)
				g_scrubCache = cache->boolean;
			// Empty never overwrites: a settings reset must not forget where renders go.
			if (const Filmmaker::JsonValue* folder = root.Find("renderOutputFolder");
				folder && folder->type == Filmmaker::JsonValue::Type::String && !folder->str.empty())
				Filmmaker::RenderOutputSettingsRef().folder = Filmmaker::Utf8ToWide(folder->str);
			if (const Filmmaker::JsonValue* sound = root.Find("renderDoneSound"); sound && sound->type == Filmmaker::JsonValue::Type::Bool)
				Filmmaker::RenderDoneSound_SetEnabled(sound->boolean);
			if (const Filmmaker::JsonValue* volume = root.Find("renderDoneSoundVolume"); volume && volume->type == Filmmaker::JsonValue::Type::Number)
				Filmmaker::RenderDoneSound_SetVolume((int)volume->number);
		} else {
			g_lastError = "the file did not parse; the defaults are in use";
		}
	}
	g_saved = BuildSnapshot();
	g_pending = g_saved;
	g_generation = DemoIndex_Get().generation;
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("settings", "loaded %s, keep camera %s", file.is_open() ? "the file" : "no file (defaults)",
			g_keepCamera ? "on" : "off");
}

void Settings_Frame() {
	if (!g_loaded) return;
	const unsigned generation = DemoIndex_Get().generation;
	if (generation != g_generation) {
		g_generation = generation;
		if (!g_keepCamera && generation != 0) QuietCameraReset();
	}
	const std::string snapshot = BuildSnapshot();
	g_pictureRevision = std::hash<std::string>()(BuildSnapshot(false));
	const unsigned long long now = GetTickCount64();
	if (g_quiet) {
		if (snapshot == g_quietSnapshot) return; // still what the reset left: the file keeps the user's feel
		g_quiet = false;                         // the user moved something: that is saved as usual
		g_quietSnapshot.clear();
	}
	if (snapshot != g_pending) {
		g_pending = snapshot;
		g_changedAt = now;
		g_dirty = true;
	}
	if (g_dirty && now - g_changedAt >= kSaveDebounceMs) {
		if (g_pending == g_saved || Save(g_pending)) g_saved = g_pending;
		g_dirty = false;
	}
}

void Settings_Flush() {
	if (!g_loaded || g_quiet) return; // a keep-off reset never reaches the file, not even at exit
	g_pending = BuildSnapshot();
	if (g_pending != g_saved && Save(g_pending)) g_saved = g_pending;
	g_dirty = false;
}

bool Settings_KeepCamera() { return g_keepCamera; }

bool Settings_ScrubCache() { return g_scrubCache; }

void Settings_SetScrubCache(bool on) { g_scrubCache = on; }

unsigned long long Settings_PictureRevision() { return g_pictureRevision; }

void Settings_SetKeepCamera(bool keep) {
	g_keepCamera = keep;
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("settings", "keep camera between demos %s", keep ? "on" : "off");
}

void Settings_Command(int argc, advancedfx::ICommandArgs* args) {
	const char* action = argc >= 3 ? args->ArgV(2) : "state";
	if (0 == _stricmp(action, "save")) {
		g_quiet = false; // an explicit save saves what is on screen
		g_quietSnapshot.clear();
		Settings_Flush();
		advancedfx::Message("settings: saved to %ls\n", SettingsPath().c_str());
		return;
	}
	if (0 == _stricmp(action, "reset")) {
		const char* group = argc >= 4 ? args->ArgV(3) : "";
		if (ResetGroup(group)) advancedfx::Message("settings: %s back to the defaults.\n", group);
		else advancedfx::Message("settings: unknown group \"%s\". Groups: smoothView, handheld, weaponRecoil, weaponBob, all.\n", group);
		return;
	}
	if (0 != _stricmp(action, "state")) {
		advancedfx::Message("mirv_filmmaker settings save | reset <smoothView|handheld|weaponRecoil|weaponBob|all> | state\n");
		return;
	}
	Settings_PrintHealth();
}

void Settings_PrintHealth() {
	advancedfx::Message("  settings:   %ls, %s; %llu saves, keep camera between demos %s%s%s\n", SettingsPath().c_str(),
		!g_loaded ? "not loaded yet" : (g_quiet ? "held after a keep-off reset" : (g_dirty ? "change pending" : "saved")), g_saves,
		g_keepCamera ? "on" : "off", g_lastError.empty() ? "" : "; ", g_lastError.c_str());
}

} // namespace SourceFilmmaker
