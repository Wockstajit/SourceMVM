#pragma once

#include <cstddef>
#include <cstdint>

namespace Deadlock {

// Deadlock ConCommandCreation_t, verified against AlliedModders' deadlock SDK and
// the installed tier0 registrar. This is a creation descriptor, not ConCommandData.
struct ConsoleCommandCreation {
	const char* name = nullptr;
	const char* help = nullptr;
	std::uint64_t flags = 0;
	void* callback = nullptr;
	std::uint8_t callbackFlags = 1; // ICommandCallback interface; not a flat function.
	std::uint8_t callbackPadding[7] = {};
	void* completion = nullptr;
	std::uint8_t completionFlags = 0;
	std::uint8_t completionPadding[7] = {};
};
static_assert(sizeof(ConsoleCommandCreation) == 56, "Deadlock command creation ABI");
static_assert(offsetof(ConsoleCommandCreation, callback) == 24, "Deadlock callback offset");
static_assert(offsetof(ConsoleCommandCreation, completion) == 40, "Deadlock completion offset");

// MSVC's member-function struct return is a hidden RDX pointer. The engine reads
// the descriptor from R8 and additional flags from R9 (tier0+0x69ff0, build 6774).
// A three-argument pointer-returning function shifts these arguments and crashes.
using ConsoleRegisterFn = void*(__fastcall*)(void*, std::uint64_t*, const ConsoleCommandCreation*, std::uint64_t);
inline std::uint64_t ConsoleRegister(ConsoleRegisterFn fn, void* cvar,
	const ConsoleCommandCreation& creation) {
	std::uint64_t result = 0xffff;
	if (fn && cvar) fn(cvar, &result, &creation, 0);
	return result;
}

} // namespace Deadlock
