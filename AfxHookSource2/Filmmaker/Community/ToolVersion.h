#pragma once

// Which SOURCE:MVM build is running, and what changed in it.
//
// The version comes from the launcher's installed.json, one folder above the runtime the hook was
// loaded from ("runtime": {"version": "0.3.1-hotfix1"}). A hook built and staged by hand has no
// such file and reports itself as a dev build. The changelog is the repository's CHANGELOG.md,
// installed as resources/CHANGELOG.md next to the hook (ChangelogMath.h reads it).
//
// The What's new window (WhatsNewJs.h, CS2's news popup) opens from the WHAT'S NEW button in the
// main menu's top bar, and by itself once per installed version, the first time the main menu is
// up after an update; the version it was shown for is kept as the UI setting whatsnew.seen.

#include <string>

namespace Filmmaker {

// "0.3.1-hotfix1" as installed.json names it; empty for a dev build.
const std::string& ToolVersion_Runtime();

// "0.3.1 hotfix 1", or "dev build".
std::string ToolVersion_Label();

// The What's new payload for the main menu script (FilmmakerMenu::PushWhatsNew):
//   {"label":..,"dev":bool,"open":bool,"sections":[{"title","date","top","groups":[{"name","items":[..]}]}]}
// `open` asks the page to show the window by itself; `top` marks the sections it opens with (the
// ones newer than the version last seen), the rest sit under EARLIER VERSIONS.
std::string ToolVersion_WhatsNewJson();

// The window was closed: it stays shut until the next update.
void ToolVersion_MarkWhatsNewSeen();
// Forget the version last seen, so the window opens again by itself (testing, or to re-read).
void ToolVersion_ForgetWhatsNewSeen();

// The full changelog on GitHub, in the user's browser.
void ToolVersion_OpenChangelogOnline();

} // namespace Filmmaker
