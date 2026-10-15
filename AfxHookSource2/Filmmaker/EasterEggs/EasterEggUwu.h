#pragma once

// UwU (E-girl) egg, the world side (the pastel UI is the overlay's): while it is on
//   * every player death plays `bang`, every gun shot one of four `pop` variations -- placed in the
//     world like CS2's own sounds: loud near the camera, fading with distance, gone across the
//     map, and panned to the side the player is on (EasterEggAudioMath.h);
//   * the game's own gunshots are muted (EasterEggGunMute);
//   * blood is off (violence_hblood / violence_ablood 0, put back when UwU turns off).
// The clips are staged beside the hook in bin\easter-eggs\uwu\ (Assets\easter-eggs\uwu\), decoded
// once through the bundled ffmpeg and mixed by XAudio2.

#include <string>

namespace SOURCESDK { namespace CS2 { class CGameEvent; } }

namespace Filmmaker {

enum class ModCategory;
// MAIN thread: persistence reads the user's colors while UwU temporarily owns the live tint.
void EasterEggUwu_GetUserTint(ModCategory category, int* r, int* g, int* b, bool* enabled);

// MAIN thread (EasterEggs_RunMainThreadFrame): follows UwU on/off (blood, gun mute, clip loading).
void EasterEggUwu_RunFrame();
// Game-event thread: player_death and weapon_fire, deduped across the server + client deliveries.
void EasterEggUwu_OnGameEvent(SOURCESDK::CS2::CGameEvent* event);
// The song's beat right now, 0..1 (peaks on its kicks, decaying between): the overlay's pink
// vignette pulses with it. 0 while the song is not playing.
double EasterEggUwu_MusicPulse();
// One status line for `mirv_filmmaker egg status`.
std::string EasterEggUwu_Status();

} // namespace Filmmaker
