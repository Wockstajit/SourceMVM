#pragma once

// Deadlock's ICvar (CCvar) slots, re-derived on 2026-10-12 for build 6774.
// The current tier0 has 46 entries. The older 48-entry layout shifted these methods by two;
// continuing to call that layout silently sent registration to GetCmd after the game update.
//
// Automation/verify/derive-deadlock-icvar-slots.py resolves RTTI and compares actual bodies.
// Current CS2 -> Deadlock mapping (identical bytes out of 512):
//   GetCvar 41 -> 41 (510), RegisterConCommand 42 -> 42 (511),
//   UnregisterConCommand 43 -> 43 (508), GetCmd 44 -> 44 (508).
// The current registrar's hidden return pointer and descriptor layout were also checked in
// disassembly. Re-run derivation after updates; matching interface names are not ABI evidence.

#include "../../Deps/release/prop/cs2/sdk_src/public/icvar.h"
#include "DeadlockConsoleAbi.h"

namespace Deadlock {

const size_t kIcvarSlot_GetCvar = 41;
const size_t kIcvarSlot_RegisterConCommand = 42;
const size_t kIcvarSlot_UnregisterConCommand = 43;
const size_t kIcvarSlot_GetCmd = 44;

// Keep the Deadlock adapter explicit even when this build matches CS2. Each game updates
// independently. These take the interface pointer and reach the measured slot directly.
inline void* Icvar_Slot(SOURCESDK::CS2::ICvar* cvar, size_t slot) {
	if (!cvar) return nullptr;
	void** vtbl = *(void***)cvar;
	return vtbl ? vtbl[slot] : nullptr;
}

inline std::uint64_t Icvar_RegisterConCommand(SOURCESDK::CS2::ICvar* cvar,
	const ConsoleCommandCreation& creation) {
	return ConsoleRegister((ConsoleRegisterFn)Icvar_Slot(cvar, kIcvarSlot_RegisterConCommand), cvar, creation);
}

inline SOURCESDK::CS2::CCmd* Icvar_GetCmd(SOURCESDK::CS2::ICvar* cvar, size_t i) {
	typedef SOURCESDK::CS2::CCmd*(__fastcall * Fn)(void* self, size_t i);
	Fn fn = (Fn)Icvar_Slot(cvar, kIcvarSlot_GetCmd);
	return fn ? fn(cvar, i) : nullptr;
}

inline SOURCESDK::CS2::Cvar_s* Icvar_GetCvar(SOURCESDK::CS2::ICvar* cvar, size_t i) {
	typedef SOURCESDK::CS2::Cvar_s*(__fastcall * Fn)(void* self, size_t i);
	Fn fn = (Fn)Icvar_Slot(cvar, kIcvarSlot_GetCvar);
	return fn ? fn(cvar, i) : nullptr;
}

} // namespace Deadlock
