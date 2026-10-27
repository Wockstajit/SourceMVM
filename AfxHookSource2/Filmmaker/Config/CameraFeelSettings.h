#pragma once

// The camera feel both games keep between sessions: Smooth View, Handheld Camera, Weapon Recoil with
// its presets, and Weapon Bob, the Config CAMERA tab's motion and weapon cards. CS2's settings store
// (FilmmakerSettingsStore.cpp) writes them into filmmaker_settings.json among its other settings, and
// the Source 1 hook's (AfxHookSource/Filmmaker/SourceSettings.cpp) into its own file, both through
// these, so a group is written and read the same in either.
//
// Each reader applies the values first and the switch last, and leaves a module as it is when its
// group is missing. Main thread.

namespace Filmmaker {

class JsonBuilder;
struct JsonValue;

// "smoothView", "handheld", "weaponRecoil", "weaponRecoilPresets" and "weaponBob", in that order.
void CameraFeel_Write(JsonBuilder& b);
// The same groups from a settings file's root object.
void CameraFeel_Load(const JsonValue& root);

// One group each, for a store's per-group reset (null leaves the module as it is).
void CameraFeel_WriteSmoothView(JsonBuilder& b);
void CameraFeel_WriteHandheld(JsonBuilder& b);
void CameraFeel_WriteWeaponBob(JsonBuilder& b);
void CameraFeel_LoadSmoothView(const JsonValue* group);
void CameraFeel_LoadHandheld(const JsonValue* group);
void CameraFeel_LoadWeaponBob(const JsonValue* group);

} // namespace Filmmaker
