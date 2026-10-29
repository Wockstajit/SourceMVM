#pragma once

// The Source 1 hook's settings store: the camera feel (Smooth View, Handheld Camera, Weapon Recoil and
// its presets, Weapon Bob; AfxHookSource2/Filmmaker/Config/CameraFeelSettings.h, written and read as
// CS2's store writes and reads them) and Config's Keep between demos, in
// %APPDATA%\HLAE\filmmaker_settings_source1.json. Its own file, because CS2's store rewrites
// filmmaker_settings.json from its own modules and the two would drop each other's settings.
//
// As CS2's store: the modules' construction defaults are kept before the file is read (what a reset
// means), a snapshot is built every frame and a change written 500 ms after the last one, and the file
// is written once more when the game shuts down.
//
// Keep between demos (CS2's EffectsPersistence for the CAMERA tab): on, a new demo keeps the camera
// feel; off, each new demo starts from the defaults. That reset never reaches the file, so turning keep
// back on, or restarting, brings the saved feel back; the file is held until the user edits something.
//
// Main thread.

namespace advancedfx { class ICommandArgs; }

namespace SourceFilmmaker {

// Once, at the first frame: the defaults, then the file.
void Settings_Load();
// Every frame: a new demo with keep off, and the autosave.
void Settings_Frame();
// When the game shuts down.
void Settings_Flush();

bool Settings_KeepCamera();
void Settings_SetKeepCamera(bool keep);
// The TIMELINE dock's Cache (the scrub frame cache, CS2's camtl.scrubCache), saved with the rest.
bool Settings_ScrubCache();
void Settings_SetScrubCache(bool on);
// Changes whenever a saved setting does: the camera feel changes what a frame shows.
unsigned long long Settings_PictureRevision();

// mirv_filmmaker settings save | reset <smoothView|handheld|weaponRecoil|weaponBob|all> | state
void Settings_Command(int argc, advancedfx::ICommandArgs* args);

void Settings_PrintHealth();

} // namespace SourceFilmmaker
