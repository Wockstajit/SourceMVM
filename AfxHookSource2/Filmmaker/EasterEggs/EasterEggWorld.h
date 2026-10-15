#pragma once

// The in-world halves of the easter eggs. EasterEggs.cpp owns which eggs are on; these modules
// only apply and undo their effect. One file per family so nothing couples to anything else:
//
//   EasterEggLighting.cpp  Disco Inferno (sun colour on the beat, spinning shadows) and
//                          Wes Anderson (warm pastel light). Both drive LightFx, so they share
//                          one stash of the user's own values and are mutually exclusive.
//   EasterEggBodies.cpp    Big Head (head_0 local-pose scale, late pump) and Moon Gravity
//                          (ragdoll_gravity_scale through RagdollPhysics).
//   EasterEggChickens.cpp  Chicken Storm (client prop_dynamic chickens that fall and bounce).
//   EasterEggBay.cpp       Michael Bay (an MW2019 explosion on every death).
//
// All entry points are MAIN THREAD except EasterEggBay_OnGameEvent (game-event thread; it only
// queues).

namespace SOURCESDK { namespace CS2 { class CGameEvent; } }

namespace Filmmaker {

// Lighting --------------------------------------------------------------------------------------
// False means activation was refused because the recovery stash could not be committed.
bool EasterEggLighting_SetDisco(bool on);
bool EasterEggLighting_SetWes(bool on);
void EasterEggLighting_RunFrame();
// Last startup recovery result; emits the same diagnostic to mvm_debug when queried.
const char* EasterEggLighting_RecoveryStatus();

struct EasterEggLightingState {
	int r = 255, g = 255, b = 255;
	float amount = 0.0f;
	bool uniform = false;
	float yaw = 0.0f;
};
// Persistence reads the user's stashed values while an egg owns live LightFx values.
// Same main-thread contract as the lighting setters and the settings-store snapshot.
EasterEggLightingState EasterEggLighting_UserState();

// Bodies ----------------------------------------------------------------------------------------
void EasterEggBodies_SetBigHead(bool on);
bool EasterEggBodies_SetMoon(bool on); // activation requires a committed recovery file
const char* EasterEggBodies_MoonRecoveryStatus();
void EasterEggBodies_RunLateFrame(); // after the engine's animation, before the draw
void EasterEggBodies_LevelInit(); // forget old model pointers without touching expired entities
int EasterEggBodies_CachedModels(); // bounded runtime enumeration, main thread

// Chickens --------------------------------------------------------------------------------------
void EasterEggChickens_Set(bool on);
void EasterEggChickens_RunFrame();
void EasterEggChickens_RunLateFrame(); // the pose write: after the client's own transform rebuild
void EasterEggChickens_LevelInit(); // the map's entities are gone: forget ours, do not touch them
int EasterEggChickens_Count();
void EasterEggChickens_SetClass(const char* cls); // spawn entity class (prop_physics / prop_dynamic)
const char* EasterEggChickens_Class();

// Michael Bay -----------------------------------------------------------------------------------
void EasterEggBay_Set(bool on);
void EasterEggBay_OnGameEvent(SOURCESDK::CS2::CGameEvent* event);
void EasterEggBay_RunFrame();
int EasterEggBay_BlastCount(); // increases per explosion: the overlay flashes on each new one
void EasterEggBay_TestBlast();  // one blast 300 units in front of the camera (console test)

// Wes Anderson chapters: a round_start bumps the chapter (deduplicated against seek replays).
void EasterEggWes_OnGameEvent(SOURCESDK::CS2::CGameEvent* event);

} // namespace Filmmaker
