#pragma once

// Keeps the Customize modal's player-preview pawns DRAWABLE while the demo is paused.
//
// The problem this solves, measured live (CS2 build 2000879, client 14172) and NOT guessed:
// a preview pawn created while the demo is paused is fully present -- it resolves, has its agent
// model, is posed, and its animgraph advances every frame (that part is PreviewAnimTick's job) --
// but the renderer draws nothing of it. Only its held weapon appears, which is why the modal looked
// like "a gun floating in a black box". Open the modal while the demo PLAYS and the same pawn draws
// correctly, and keeps drawing after pausing; close and reopen while paused and it vanishes again.
//
// ROOT CAUSE: `C_BaseEntity::m_bHasSuccessfullyInterpolated`. The client only draws an entity once
// it has interpolated at least once, and interpolation needs the clock to advance. A paused demo
// pins the clock, so an entity SPAWNED DURING THE PAUSE never sets that flag and is skipped for as
// long as the pause lasts. Proven by a three-point differential of the same pawn (broken / broken
// later / after a brief play) with `mvm_previewprobe snap`: of the whole entity the only latched
// difference was this one byte, and forcing it made the agent render immediately.
//
// THE FIX is the engine's own escape hatch, not a forged interpolation state:
// `C_BaseEntity::m_bRenderEvenWhenNotSuccessfullyInterpolated` exists for precisely this situation.
// Live-verified: with m_bHasSuccessfullyInterpolated left at 0 and only the override set, the agent
// renders. Nothing else is written; playback, demo tick and every other entity are untouched.
//
// Scope: only entities whose class is `*previewplayer*` (the UI preview pawns -- buy menu, team
// select, vanity loadout and this modal), and only while the Customize modal is open. Those pawns
// never exist in gameplay, so the override cannot affect a real player, and it is cleared again on
// close. All preview pawns are covered rather than "the newest index", because the panel reuses
// POOLED pawns and picking the wrong one has already cost this project a session.
//
// Own module on purpose: PreviewAnimTick makes the paused preview ANIMATE (an animgraph tick
// delta); this makes it VISIBLE (an entity render flag). Different mechanism, different failure
// mode, different console command -- see the "never couple a new thing into an existing thing"
// rule in CLAUDE.md.
//
// Console: `mvm_previewvis auto|on|off|status`. Default auto = follow the Customize modal.

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Main-thread pump. Pass whether the Customize modal is currently open.
void PreviewRenderFlag_RunFrame(bool customizeModalOpen);

// `mvm_previewvis` dispatch.
void PreviewRenderFlag_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
