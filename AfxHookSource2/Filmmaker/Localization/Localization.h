#pragma once

// Localization of the CS2 panels (Docs/panorama/localization.md).
//
// The scripts write English and wrap it in T('...') (Panorama/LocalizationJs.h). This module owns
// which language is active and gets that language's table into Panorama:
//
//   setting   the UI key `language` in the settings store: "auto" (the default) or a CS2
//             language id from LocalizationLanguages.h. `mirv_filmmaker language <id|auto>`
//             writes it, and so does the SOURCE:MVM tab in CS2's settings menu.
//   auto      follows the language CS2 itself runs in, asked from Panorama ($.Language()).
//   tables    lang/<id>.json, compiled into the DLL as resources (LocalizationTables.rc), so a
//             hook-only build needs no extra files staged.
//   contexts  the menu and the in-game HUD are separate Panorama contexts with separate script
//             globals. Each gets the table pushed on its own, and re-pushed when a context reload
//             dropped it.
//
// A language change bumps the UI epoch. Every panel driver compares it and rebuilds its script,
// which is what re-evaluates the T() calls.

#include <cstdint>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// First Panorama pump of the main-thread frame: the table has to be in a context before any
// panel script builds there. Also drives the SOURCE:MVM settings tab.
void Localization_RunMainThreadFrame();

// True when the UI language changed since `seen` was last updated, and updates it. A driver keeps
// one `seen` beside its built flag and clears that flag when this returns true.
bool Localization_TakeUiEpoch(uint64_t& seen);

// The language id whose table is active ("english" until the first frame resolved it).
const char* Localization_ActiveLanguage();

// `mirv_filmmaker language [id|auto]`.
void Localization_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
