#pragma once

// osu easter egg hitsounds: short samples mixed by XAudio2 (low latency, any number at once --
// the song itself plays through OsuAudio). Samples resolve the way osu! stable does:
//   custom index N>=1  the beatmap's own "<name><N>.wav|ogg|mp3" (N=1 unnumbered), only when
//                      SetBeatmapSamples(true)
//   otherwise          the player's skin (OsuSkin: lazer's selected skin, else stable's)
//   otherwise          a small built-in synthesized click
// Prepare() loads everything a beatmap can ask for on a worker thread (ogg/mp3 are decoded once
// through the bundled ffmpeg into %APPDATA%\HLAE\osu\cache); Play() is then a voice restart.

#include "OsuMath.h"

#include <functional>
#include <string>

namespace Filmmaker {
namespace OsuHitsounds {

using Resolver = std::function<std::wstring(const std::string&)>; // beatmap-relative name -> file

void Prepare(const Osu::Beatmap& b, const Resolver& beatmapFiles);
bool Ready();
void Play(const Osu::SampleRef& s);
void PlayNamed(const char* skinName, double volume01); // e.g. "combobreak" (skin only)
// The map's full hitsounding (additions, sample sets, ticks, its own sample files). Off by default:
// the game plays only the skin's plain click on circles and slider heads (OsuGame PlayNode).
// The map's own files load from the next map start.
void SetBeatmapSamples(bool on);
bool BeatmapSamples();
void SetVolume(double volume01);
double Volume();

} // namespace OsuHitsounds
} // namespace Filmmaker
