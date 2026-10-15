#pragma once

// Per-weapon FX overrides.
//
// Several CS2 weapons share one muzzle-flash system, so tuning that system for one weapon
// necessarily moves it for the others. Where the weapons emit DIFFERENT CS2 particle names the
// rules table in ParticleFxRules.cpp can already separate them (the Deagle is one of these:
// uweapon_muzflsh_deagle is its own name). Where they emit the SAME name there is nothing in the
// particle to tell them apart:
//
//     SCAR-20 and G3SG1  -> weapon_muzzleflash_snip_ar   (verified 2026-07-03 via `fx names`)
//     M249 and Negev     -> uweapon_muzflsh_mach
//
// For those, the swap has to look at which weapon is actually firing. That is what this is for:
// report "the M249 is misaligned but the Negev is fine" and it becomes one table entry plus a
// forked asset, instead of a change that moves both.
//
// Threading: the particle hook runs on whatever thread creates particles, where entity access is
// not safe (see the notes in ParticleFxHook.cpp). So the active weapon is sampled on the MAIN
// thread once per pump and only the cached value is read during a swap.

// Cache the spectated pawn's active weapon class. MAIN THREAD ONLY.
void PerWeaponFx_SampleMainThread();

// Per-weapon replacement for `baseTargetLower`, or nullptr to keep the base target.
// Safe from any thread. The returned pointer is thread-local and stays valid until this
// thread calls the function again.
const char* PerWeaponFx_Override(const char* baseTargetLower);

// The cached weapon class ("-" when nothing is resolved), for diagnostics.
const char* PerWeaponFx_ActiveWeaponClass();
