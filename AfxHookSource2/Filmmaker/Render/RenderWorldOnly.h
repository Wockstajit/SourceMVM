#pragma once

// World Only capture mode: the map, and nothing that is a person or a gun.
//
// WHAT IT IS. A render pass that keeps the world exactly as CS2 lit and coloured it -- normal
// materials, normal lighting, props, decals, sky -- while removing every player, every piece of
// player-worn or player-held gear, and the first-person viewmodel. The delivered plate is the
// empty map from the shot's camera, so a comp can put its own subjects into a real environment.
//
// WHY IT IS ITS OWN MODULE (CLAUDE.md "never couple a new thing into an existing thing"). This is
// a separable capability with its own policy, its own state and its own console surface. It owns
// the ONE question the scene draw hook asks it -- "is this draw part of the world?" -- and nothing
// else. SceneSystem keeps owning the draw plumbing; it does not learn what a player is.
//
// HOW IT DECIDES. Purely from the draw's material path. Source 2's material vocabulary is
// deterministic per draw, whereas per-draw entity/render-state pointers were measured to be shared
// and unreliable (that finding is why the earlier pass system needed an entity-attachment probe at
// all). World Only does not need that probe: it rejects held gear AND dropped gear AND the
// viewmodel, so the "is this gun in someone's hands?" question never has to be answered.
//
// SCOPE. Draw rejection only. It never substitutes materials, never touches depth, and never
// changes any engine state -- so arming it costs one relaxed atomic load per draw when idle, and
// disarming it restores the frame exactly.

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Main thread, once per frame. Refreshes the one piece of game state the filter consults: whether
// the C4 is currently PLANTED. A planted bomb is scene furniture and stays in the plate; a carried
// or loose one is a weapon and goes with every other weapon. Costs nothing while disarmed.
void RenderWorldOnly_RunMainThreadFrame();

// Main thread. `reason` is recorded for `status` / mvm_debug so a mode left on by a failed job is
// traceable to who armed it.
void RenderWorldOnly_SetArmed(bool armed, const char* reason);
bool RenderWorldOnly_IsArmed();

// Render thread, hot path. True while the live preview is armed OR the recorded render currently
// executing is a job's World Only render (AfxStreams_CurrentPassRenderKey). Never touches the player-draw convar for a job: the Main render of the same frame
// must stay the full scene.
bool RenderWorldOnly_Active();
// Main thread. A job with a World Only pass keeps the planted-bomb verdict fresh for its whole
// life, exactly as the live arm does.
void RenderWorldOnly_SetJobUsesPass(bool uses);

// Render thread, hot path. True when this draw is a player, player gear, a weapon or the
// first-person viewmodel and must not reach the frame. `materialName` is the caller's already
// SEH-guarded read; a null/empty name is never rejected (an unclassifiable draw is kept, because
// removing the world by accident is a worse failure than leaving one subject draw in).
bool RenderWorldOnly_RejectsMaterial(const char* materialName);

// Same question, tagged with the submission path that asked ("base", "agg", ...). scenesystem
// submits drawable batches through several descriptor types and they do NOT all reach the same
// detour; when something survives the plate, the FIRST thing to establish is which path drew it.
// The tag is carried into the probe log for exactly that.
bool RenderWorldOnly_RejectsMaterialFrom(const char* materialName, const char* source);

// The real question the draw hook has to ask. A drawable batch is an ARRAY of elements, each with
// its own material, and the hook can only skip the batch as a WHOLE -- so classifying element 0
// and skipping on that answer silently takes any world geometry sharing the batch with it. That is
// not hypothetical: it cost a potted plant its colour pass while its depth still wrote, leaving a
// black silhouette on de_mirage (measured 2026-08-10, tick 5136).
//
// So the batch is rejected only when EVERY element is a subject. A mixed batch is kept, because
// leaving one subject in the plate is a far smaller failure than punching a hole in the map -- and
// mixed batches are rare (one object in a whole frame).
//
// `names` may contain nulls; `count` is clamped by the caller. Returns true to skip the batch.
bool RenderWorldOnly_RejectsBatchFrom(const char* const* names, int count, const char* source);

// Arms a bounded material-verdict probe on the mvm_debug channel `render.worldonly`. Logs each
// DISTINCT material path once with its class and verdict, up to `limit` names, so a leftover
// object in the plate can be traced to the exact material that survived (or the exact world
// material that was wrongly dropped). Zero disarms. Never changes scene state.
void RenderWorldOnly_ArmProbe(int limit);

// Console: `mirv_filmmaker render worldonly ...`
void RenderWorldOnly_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
