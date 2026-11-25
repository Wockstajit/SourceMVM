#pragma once

// Runtime actor animation playback for CS2:
//  - Live local-player animation playback & third-person preview
//  - Independent multi-actor bot animation playback during Bot Takes passes
//  - 3D MapPlayerPreviewPanel preview inside the Animation Picker
//  - Synchronized demo-companion (.mvmanimseq) playback during GOTV demos

#include "ActorAnimationsMath.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Filmmaker::ActorAnim {

// Ensures the MVMACT01 pack is loaded from disk (lazy, idempotent).
bool EnsureLoaded();
const Library &GetLibrary();

// Catalog JSON array: [[id, displayName, categoryIdx, durationSec, loopable, rootMotion], ...]
std::string CatalogJson();

// Live local-player animation playback (works in third-person preview and during recording).
bool PlayLocal(const Cue &cue);
void StopLocal();
bool LocalActive();
std::string LocalClip();
void SetLocalUpperBody(bool upperBody);
bool LocalUpperBody();

// Multi-actor performer tracks (live private match playback).
void ClearPerformers();
void SetPerformer(uint64_t steamId, int delayTicks, bool placedActor, const std::vector<Cue> &cues, int controllerSlot = -1);
void SetSceneElapsed(double elapsedSec, bool running);

// 3D Animation Picker preview (MapPlayerPreviewPanel).
void SetPreview(const std::string &clipId, float speed, bool loop, float startTime, float duration);
void StopPreview();
bool PreviewActive();
std::string PreviewClip();

// Demo companion sequence save/load (reproduces custom animations during GOTV demo playback).
bool SaveCompanionForDemo(const std::string &demoName, const CompanionSequence &seq);
bool LoadCompanionForDemo(const std::string &demoName);
void ClearCompanion();
std::string CompanionStatus();

// Late-frame skeleton pose pass: runs on main thread AFTER AnimGraph2 (Filmmaker::RunMainThreadLateFrame).
void RunLateFrame(bool panelOpen);

// Script for the Config panel (picker catalog), queued by `takes anim catalog`.
void QueueScript(const std::string &js);
bool TakeScript(std::string &js);

// How fast the local player really moves along the ground (inches per second), fed every frame by Takes.
// A locomotion clip played on the player runs at that speed over the clip's own root speed, so the
// feet do not slide. DesiredLocalMaxSpeed returns >0 for locomotion speed cap, 0 to lock stationary
// clips (reactions/gestures/reloads/static idle) in place, or -1 when inactive / untouched.
void SetLocalSpeed(float speed);
void SetLocalSpeedScale(float speed);
float LocalSpeedScale();
void SetLocalEquippedWeaponClass(uint8_t weaponClass);
void SetLocalMotion(float fwdSpeed, float rightSpeed, bool isWalking = false, bool isDucking = false);
bool SampleLocalRootVelocity(float outLocalVelXY[2]);
float DesiredLocalMaxSpeed();

// What hangs off the local pawn (weapons, holsters) and which bone each is nearest, for `takes anim attach`.
std::string DumpAttachments();

// Test switches for tracking down draw problems: see ActorAnimations.cpp.
void SetDebug(int mask, int mod, int k);

// Status summary for `mirv_filmmaker takes anim status`.
std::string StatusLine();

} // namespace Filmmaker::ActorAnim
