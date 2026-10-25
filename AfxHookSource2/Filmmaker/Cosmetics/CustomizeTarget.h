#pragma once

// Customize Player's ONE answer to "whose loadout is the modal editing, and who can be picked".
//
// CONFIG and the Camera Editor open the same modal (Panorama/CameraEditorCustomizeJs.h) and both
// read this module's state, so the two can no longer disagree about the target. The rules are in
// CustomizeTargetMath.h; this file feeds them from the game:
//
//   * The roster and the spectated player come from CS2's own demo bar (CameraTimelineHud reads
//     HudDemoController.GetDemoControllerState / GetDemoPlayers). That is correct in first AND third
//     person. The render-eye match it replaces (AfxGetSpectatedPawnIndex) only works in first person,
//     a GOTV demo reports no local observer at all, and the eye match could land on a dead body or on
//     a dead teammate's observer pawn sitting at the same eye (mvm_debug evidence in
//     Docs/features/customize-feature.md). The eye match stays only as the fallback for a demo whose
//     bar does not answer.
//   * A dead player stays in the roster. The controller's m_hPawn moves to an observer pawn when its
//     player dies; m_hPlayerPawn / m_hOriginalController still name the body that death animations
//     keep on screen. Their loadout is the last one read while they were alive (weapons are dropped
//     on death), else whatever the body still carries (gloves, agent).
//   * Picking a player switches the view to first person on them: CS2's own spectate for a living
//     player, our free cam on the body's eye for a dead one (CS2 refuses to spectate the dead).

#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Main thread, once per frame, after CameraTimelineHud (which reads CS2's demo bar) and before
// ConfigHud / CameraEditorHud build their state. `loadouts` asks for the per-player loadout reads,
// which only an open CONFIG or Camera Editor consumes; the target itself is tracked every frame.
void CustomizeTarget_RunFrame(bool loadouts);

// The demo's entity graph is gone: forget the pick, the last target and the cached loadouts.
void CustomizeTarget_LevelInit();

// {"target":<controller|-1>,"source":"pick|spectated|last|none","spectated":<controller|-1>,
//  "players":[{"controller","pawn","alive","team","slot","name","key","steamId","lastSeen",
//  "loadout":<BuildCustomizeTargetJson or null>}, ...]} -- T first, then CT, each by name.
const std::string& CustomizeTarget_StateJson();

// At least one T or CT player to customize. CONFIG greys its button when there is nobody.
bool CustomizeTarget_Available();

// The modal's player list: make this controller the target and put the view in first person on
// them. False (and nothing changes) when the controller is not a player of this demo.
bool CustomizeTarget_Pick(int controller);

// Console: "mirv_filmmaker cosmetics pick <controller|clear>" and "... cosmetics roster".
void CustomizeTarget_RunPickCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);
void CustomizeTarget_PrintRoster(const char* cmd);

} // namespace Filmmaker
