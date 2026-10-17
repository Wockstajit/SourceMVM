#pragma once

#include <atomic>

namespace Filmmaker {

// A practice session: Clippy's onboarding tutorial runs the filmmaker on its defaults and keeps
// everything the user changes in memory only (Onboarding/PracticeSession.cpp starts and ends it).
// Every store that writes the user's data to disk checks this flag: while it is on, writes are
// skipped (reported as done, so nothing queues a retry) and per-demo reads find nothing. The
// user's files are therefore exactly as they were when the session ends, and also when the game
// exits or crashes in the middle of it.
//
// Header-only so any unit can check it, including the ones the native tests build on their own.
// Any thread.
inline std::atomic<bool>& PracticeSessionFlag() {
	static std::atomic<bool> on{ false };
	return on;
}
inline bool PracticeSession_Active() { return PracticeSessionFlag().load(std::memory_order_acquire); }

} // namespace Filmmaker
