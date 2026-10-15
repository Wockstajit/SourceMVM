#pragma once

// Knife-type-swap CRASH fix via a targeted client.dll detour ("approach #3", the one that actually works).
//
// Root cause (proven, see Docs/cosmetics/cosmetics-cs2-methodology-notes.md §11): a knife TYPE swap SetModels the
// demo world weapon to a .vmdl whose per-model ANIMATION/sequence resource was never loaded in this demo
// (the player never carried that knife). The engine's animation pass then asks its builder
// (client.dll FUNC @ AOB, internally "find-or-build per-model anim data") for the model's sequence list,
// gets a NULL out-param (the model isn't registered in the anim system), and the caller dereferences it
// unchecked -> `client.dll+0x3399cc  mov r15,[rdi+8]` reads [null+8] -> crash on the anim worker thread.
//
// Loading the model resource is NOT enough (CResourceSystem::PreCache(.vmdl) loads the model but does not
// register its anim data), and the registration path is a deep engine-internal we can't cleanly invoke.
// So instead we make the null HARMLESS: detour the builder and, when it hands back a NULL sequence-list
// out-param, substitute a static EMPTY (count=0) list. The caller's loop (`cmp r15, data+count*8; je`)
// then sees an empty list, processes zero sequences, and falls through to the normal finalize -- exactly
// what happens for any model that legitimately has no active sequences. No crash; the swapped knife model
// still renders (it just contributes no animation sequences from the missing resource).
//
// SEH/null-safe and gated. The detour is installed lazily (first knife swap) and the null-fill is toggled
// by `mirv_filmmaker cosmetics animfix 0|1` (default ON) so it can be A/B'd against the crash live.

namespace Filmmaker {

// Installs the builder detour once (idempotent). AOB-resolved; a pattern miss is non-fatal (warns, the
// fix is simply inactive and knife-type swaps to unloaded models will crash as before). Returns true if
// the detour is installed (or already was).
bool EnsureAnimCrashFixInstalled();

// Toggle the null-fill behavior (default ON). The detour stays installed; OFF makes the hook a pass-through
// so the crash reproduces (for A/B verification).
void SetAnimCrashFix(bool enabled);
bool AnimCrashFix();

// True once the detour is installed.
bool AnimCrashFixInstalled();

// Number of times the hook substituted an empty list for a null out-param (i.e. crashes prevented).
unsigned long long AnimCrashFixFills();

} // namespace Filmmaker
