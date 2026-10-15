#pragma once

// UwU mutes the game's own gunshots (its pops replace them). Every sound event the client starts --
// demo/network CMsgSosStartSoundEvent and client-side EmitSound alike -- passes through one
// client.dll function ("DoStartSoundEvent"); a detour there resolves the event's NAME from its hash
// the same way the game does and drops gun-fire events while UwU is on.

namespace Filmmaker {

// MAIN thread: installs the detour the first time it is wanted (UwU on). Idempotent.
void EasterEggGunMute_Ensure();
void EasterEggGunMute_SetEnabled(bool on);
// For `mirv_filmmaker egg status`: installed, events seen, events dropped.
void EasterEggGunMute_Stats(bool& installed, unsigned long long& seen, unsigned long long& dropped);

} // namespace Filmmaker
