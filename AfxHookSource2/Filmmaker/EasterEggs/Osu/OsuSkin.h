#pragma once

// The player's own osu! skin for the osu easter egg: the one selected in osu!lazer (game.ini
// `Skin = <id>` -> the realm's Skin row -> its files in the lazer store), else osu!stable's
// (`Skin =` in osu!.<user>.cfg -> <install>\Skins\<name>). Built-in lazer skins carry no files and
// fall back to the skin shipped with the mod (bin/osu/Skin: ExpressoKuns japan (WB) by Cappuccino),
// which is also what everyone without osu! plays with.
//
// Panorama cannot show a loose PNG (file:// wants a compiled .vtex_c) but it does load http
// images, so the skin's images are served to the overlay by a tiny HTTP server bound to
// 127.0.0.1 on a random port, under a random per-session path. Only the skin's own files are
// served; nothing else on disk is reachable through it.

#include <string>
#include <vector>

namespace Filmmaker {

class JsonBuilder;

namespace OsuSkin {

void Load();          // resolve the skin (worker thread); cheap when already loaded
void Reload();        // forget it and resolve again (skin changed in osu!)
bool Ready();
std::string Name();   // "" = no skin files (the built-in look)

// A skin file by name with extension ("normal-hitnormal.wav"), case-insensitive; "" if absent.
std::wstring File(const std::string& name);

// [Colours] ComboN as 0xRRGGBB; empty when the skin defines none.
void ComboColours(std::vector<int>& out);
// osu!lazer's "Beatmap skins" setting (game.ini BeatmapSkins): off = the skin's combo colours
// replace the beatmap's.
bool BeatmapSkins();

// "skin": {...} for the overlay (image URLs, sizes, colours, flags). Nothing when not ready.
void AppendJson(JsonBuilder& j);

std::string Status();

} // namespace OsuSkin
} // namespace Filmmaker
