#pragma once

// Skybox library -- the pickable list behind CONFIG > EFFECTS > WEATHER & SKY.
//
// Two sources, one list:
//   * CUSTOM  -- skyboxes built from your own image by FX/tools/build-skybox.ps1, discovered by
//                reading the index that tool writes to
//                %APPDATA%\HLAE\filmmaker\skyboxes\skyboxes.json.
//   * BUILTIN -- a curated set of CS2's own sky materials, enumerated from pak01_dir.vpk rather
//                than guessed (Docs/effects/skybox.md 2 lists the enumeration command).
//
// This module OWNS NO RENDERING. Applying a skybox is one call out to the pre-existing
// `mirv_sky material <path>` backend in SceneSystem.cpp, which is genuinely the home for that
// work -- writing a second skybox swapper here would be the coupling mistake the repo rule
// warns about. What this module adds is discovery, naming, thumbnails and selection state.
//
// See Docs/effects/skybox.md.

// <sstream> in full, not a forward declaration: std::ostringstream is a TYPEDEF, so declaring it
// by hand is ill-formed (hit for real earlier in this feature's development).
#include <sstream>
#include <string>
#include <vector>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

struct SkyboxEntry {
	std::string name;      // stable id, unique across the list ("belfast_sunset", "sky_de_mirage")
	std::string label;     // what the card shows ("Belfast Sunset", "Mirage")
	std::string material;  // game-relative .vmat path handed to `mirv_sky material`
	std::string image;     // s2r:// thumbnail, or empty for a name-only card
	// Original image this was built from, recorded by build-skybox.ps1. Empty for stock skies.
	// SkyboxBuilder needs it to rebuild the skybox at a new rotation.
	std::string sourcePath;
	double rotation = 0.0;
	bool flipped = false;
	bool builtin = false;
};

// Re-reads skyboxes.json. Safe to call at any time; cheap enough to call when the panel opens,
// which is how a skybox built while CS2 is running shows up without a restart.
void SkyboxLibrary_Refresh();

const std::vector<SkyboxEntry>& SkyboxLibrary_Entries();

// Applies by `name`. False if no such entry, so the caller can say WHICH name was wrong instead
// of failing silently.
bool SkyboxLibrary_Apply(const char* name);

// Back to the map's own sky.
void SkyboxLibrary_Default();

// Empty when the map's own sky is in use.
const std::string& SkyboxLibrary_Current();

// Deletes a CUSTOM skybox: its entry in skyboxes.json, its preview, and the compiled material,
// texture and thumbnail. Refuses on a stock CS2 sky. If the deleted one was active, the map's
// own sky is restored first so the view never sits on a material that no longer exists.
// False with a reason on refusal or failure.
bool SkyboxLibrary_Delete(const char* name, std::string* outReason);

// Appends ",\"skyList\":[...],\"skyCurrent\":\"...\"" for the Panorama state mirror.
void SkyboxLibrary_AppendStateJson(std::ostringstream& o);

void SkyboxLibrary_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
