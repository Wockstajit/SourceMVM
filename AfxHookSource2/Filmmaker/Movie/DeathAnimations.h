#pragma once

// Death Animations: a dying player plays a baked death animation EXACTLY, and the native ragdoll
// takes over only when the clip is over. The Call of Duty model (KisakCOD src/ragdoll): the corpse
// is pure animation until the ragdoll starts, and the ragdoll starts from the animation's final
// pose carrying the animation's own motion.
//
// How: every frame each of the victim's fifteen vphysics2 ragdoll bodies is placed exactly on the
// clip's transform, moving with the clip's own velocity, so whatever physics did in between is
// overwritten. The bodies stay dynamic: CS2 skips drawing a ragdoll with keyframed bodies on most
// frames. When the clip ends the placing stops and the ragdoll carries the last frames' velocity.
//
// Clips come from the baked library resources/ragdoll/ragdoll_animations.mvmanim
// (FX/tools/extract-death-anims.py), one source at a time: Modern Warfare 2019, Classic CoD
// (WaW / BO1 / BO2 / MW3), or Extra (GMod). Selection, placement and sampling live in
// DeathAnimationsMath.h.

#include <map>
#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }
namespace SOURCESDK { namespace CS2 { class CGameEvent; } }

namespace Filmmaker {

// Main/UI thread, from Filmmaker::RunMainThreadFrame. No-op while off and idle.
void DeathAnimations_RunFrame();
// After the client's own FRAME_RENDER_PASS work: read-only render-side evidence for mvm_debug.
void DeathAnimations_RunLateFrame();
void DeathAnimations_OnGameEvent(SOURCESDK::CS2::CGameEvent* event);
// The entity graph is gone: forget tracked corpses without touching them.
void DeathAnimations_LevelInit();

// A clip is playing on (or waiting for) this pawn's ragdoll bodies. Other features that move a corpse
// (the Nervous System) leave it alone until the clip lets go.
bool DeathAnimations_Owns(int pawnIndex);

bool DeathAnimations_Enabled();
void DeathAnimations_SetEnabled(bool enabled);
// 0 = Modern Warfare 2019, 1 = Classic CoD, 2 = Extra (GMod).
int  DeathAnimations_Source();
void DeathAnimations_SetSource(int source);
// How the clip plays on the body (Config > RAGDOLLS > Advanced Ragdolls):
//   blend in   seconds from the pose the player died in into the clip (0..1, default 0.1)
//   strength   how strong the body's "muscles" are (10..100, default 100): how fast the clip may
//              change the motion physics gives each body. 100 tracks the clip; 30 barely holds the
//              body up; 10 is a ragdoll the clip nudges. Exact playback: share of the way placed.
//   impact     how hard a grenade blast throws the body (0..300 %, default 100), outward and up; the
//              clip plays while it flies. Gun deaths are never pushed. Solid playback only.
//   blend out  seconds the clip crossfades the body into physics before letting go -- at the landing,
//              the end, or from a contact / balance loss (0..1, default 0.5)
//   walls      limbs that hit something bend or go limp; a torso that hits something stops the clip
double DeathAnimations_BlendIn();
void DeathAnimations_SetBlendIn(double seconds);
double DeathAnimations_Impact();
void DeathAnimations_SetImpact(double percent);
double DeathAnimations_Strength();
void DeathAnimations_SetStrength(double percent);
double DeathAnimations_BlendOut();
void DeathAnimations_SetBlendOut(double seconds);
bool DeathAnimations_Walls();
void DeathAnimations_SetWalls(bool on);
// Solid body: the clip steers the bodies by velocity so floors and walls stop them (default on);
// off places every body exactly on the clip, through anything in the way.
bool DeathAnimations_Solid();
void DeathAnimations_SetSolid(bool on);

// Per-player animations: a player's key (steam id, or name for bots) -> the clip every one of
// their deaths plays. Set from the Config panel's picker; persisted with the settings.
const std::map<std::string, std::string>& DeathAnimations_PlayerClips();
void DeathAnimations_SetPlayerClip(const std::string& playerKey, const std::string& clip);

// A script the Config panel should run (the picker's clip catalog), queued by the `catalog`
// command. Returns false when nothing is waiting.
bool DeathAnimations_TakeScript(std::string& script);

// One clip for the picker's 3D preview: its frame-0 body layout (kBodies * 3 floats, clip space,
// floor at z = 0), its length in seconds, and the fifteen clip-space body poses at `seconds`
// (clamped). Poses are DeathAnimationsMath::Pose, passed as p[3]+q[4] float runs so this header
// stays free of the math header. False when the clip does not exist.
bool DeathAnimations_ClipPose(const char* name, double seconds, float* poses7x15, const float** base, double* duration);

void DeathAnimations_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);
void DeathAnimations_AppendStateJson(std::ostringstream& o);

} // namespace Filmmaker
