#pragma once

// Method Acting: blinking, firing squints, idle/nearby-player gaze and relaxed death eyes.
// Eye motion is independent of body physics and Advanced Ragdolls. Models must expose the
// relevant named flex controllers / eye bones; unsupported facial rigs are skipped.
// Runs around the main-thread engine frame handler, driven by demo time (pause holds still).
// See Docs/features/filmmaker_method_acting.md for controls and model limitations.

#include <sstream>
namespace SOURCESDK { namespace CS2 { class CGameEvent; } }

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Main/UI thread, before/after animation preparation. Cheap no-op while
// disabled: it does not even scan the entity list.
void MethodActing_RunLateFrame(bool afterAnimation);
void MethodActing_OnGameEvent(SOURCESDK::CS2::CGameEvent* event);

bool MethodActing_Enabled();
void MethodActing_SetEnabled(bool enabled);

bool MethodActing_BlinkEnabled();
void MethodActing_SetBlinkEnabled(bool enabled);

// 0..1. How far the lids close while firing; 1.0 would shut the eyes completely.
double MethodActing_SquintLevel();
void   MethodActing_SetSquintLevel(double level);

// Milliseconds after the last round before the face starts relaxing -- the "buffer".
double MethodActing_HoldMs();
void   MethodActing_SetHoldMs(double ms);

// Milliseconds for the lids to reach the squint once firing starts, and to return afterwards.
double MethodActing_AttackMs();
void   MethodActing_SetAttackMs(double ms);
double MethodActing_ReleaseMs();
void   MethodActing_SetReleaseMs(double ms);

// Mean seconds between idle blinks. Real gaps land at roughly 0.4x-1.6x this.
double MethodActing_BlinkIntervalSec();
void   MethodActing_SetBlinkIntervalSec(double seconds);

// 0..1. Probability that a blink is a quick pair rather than a single, the way people often
// blink twice. Deterministic per player and per blink, so a pair reproduces on a re-render.
double MethodActing_DoubleBlinkChance();
void   MethodActing_SetDoubleBlinkChance(double chance);

// While firing, hold the model's eye_look_up / eye_look_down controllers at neutral so the
// squint reads as a squint. Without this the lid is shaped for wherever the eyes happen to
// be pointing AND squinted at the same time, which reads as the eyes rolling up.
//
// Those two controllers are normally driven by the model's own AnimConstraintDotToMorph off
// the eye_target bone, so this write is competing with the constraint. If it turns out the
// constraint wins, this switch is the thing to turn off.
bool MethodActing_SettleEyes();
void MethodActing_SetSettleEyes(bool enabled);

bool MethodActing_GazeEnabled();
void MethodActing_SetGazeEnabled(bool enabled);
bool MethodActing_DeathEyesEnabled();
void MethodActing_SetDeathEyesEnabled(bool enabled);

void MethodActing_ResetAll();

void MethodActing_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

// Appends this module's fields (leading comma included) to the Config panel state JSON, the same
// contract RagdollPhysics_AppendStateJson uses.
void MethodActing_AppendStateJson(std::ostringstream& o);

} // namespace Filmmaker
