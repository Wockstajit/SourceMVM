#pragma once

// The "osu" easter egg: an osu!-style rhythm game that plays real osu!standard beatmaps (.osu /
// .osz) over the dimmed game, drawn by the easter-egg overlay (Panorama/EasterEggOsuJs.h).
//
// Beatmaps: drop .osz files (or extracted beatmap folders) into %APPDATA%\HLAE\osu\ -- they are
// imported into osu\Songs\ on the next scan. Bundled maps ship in <HLAE folder>\osu\Songs\.
// Audio plays through Windows MCI on a thread of its own (an mp3 opens without stalling a frame);
// .ogg audio is converted to mp3 once at import, with the bundled ffmpeg.
//
// Input while it is open: Z / X or the mouse buttons hit, the mouse aims, Esc pauses/backs out.
// Everything typed or clicked is swallowed so the demo underneath never reacts.
//
// Gameplay math (formulas, slider paths, judging) lives in OsuMath.h.

#include <cstdint>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

bool Osu_Active();               // open in any state (song select, playing, results)
void Osu_Open();
void Osu_Close();

// MAIN thread, once per frame.
void Osu_RunFrame();

// Window-proc thread. Returns true when the message belongs to the game and must not reach CS2.
bool Osu_OnWindowMessage(unsigned msg, unsigned long long wParam, long long lParam);
// True while the OS pointer should be hidden (the game draws its own cursor).
bool Osu_HidesOsCursor();

// Overlay feed. `frame` changes every frame; `objs` and `list` only when their revision moves.
void Osu_BuildJson(std::string& frame, std::string& objs, uint64_t& objsRev, std::string& list, uint64_t& listRev);

// mirv_filmmaker osu ...
void Osu_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
