#pragma once

// Owns the render session state machine: the 15 states, which transitions are legal, and the
// cooperative cancellation handshake.
//
// WHY THIS EXISTS. The state was previously a bare enum plus a setter, which meant any caller
// could move the session anywhere -- Idle straight to Capturing, Completed back to Capturing,
// or out of a terminal state entirely. Plan section 17 says "a second job cannot start until the
// previous session reaches a terminal state" and "the UI must never infer state from button
// text"; neither is enforceable while the setter is unguarded.
//
// Two invariants this type exists to hold:
//
//   1. TRANSITIONS ARE CHECKED. TryTransition() rejects an illegal move and says so, rather than
//      silently corrupting the session. Legal edges are declared in one table in the .cpp.
//   2. CANCELLATION IS COOPERATIVE. RequestCancel() only raises a flag. The worker observes it at
//      a safe point and drives the session to Canceled itself. Nothing is torn down underneath a
//      render thread that is still using it.
//
// THREADING. The state is atomic so the UI thread can read it while the render thread advances
// it. Transitions themselves should be driven from one thread (the session owner); the atomic is
// for safe observation, not for concurrent mutation from several writers.

#include "RenderTypes.h"

#include <atomic>

namespace Filmmaker {

class RenderSession {
public:
	RenderSessionState State() const { return m_state.load(std::memory_order_acquire); }

	// True once the session has reached a state it can never leave without a Reset().
	static bool IsTerminal(RenderSessionState s);
	bool IsTerminalNow() const { return IsTerminal(State()); }

	// True while a job occupies the session -- i.e. not Idle and not terminal. A new job must not
	// start while this is true.
	bool IsBusy() const;

	// Attempts a transition. Returns false and leaves the state untouched when the edge is not
	// legal. `reason` (optional) receives a short explanation for logging.
	bool TryTransition(RenderSessionState to, const char** reason = nullptr);

	// Cooperative cancel. Raises the flag and, if the session is in a state that can acknowledge
	// it immediately, moves to CancelRequested. Never forces a terminal state on its own -- the
	// worker must observe CancelRequested and finish the handshake by transitioning to Canceled
	// once it has stopped touching GPU/encoder resources.
	void RequestCancel();
	bool IsCancelRequested() const { return m_cancelRequested.load(std::memory_order_acquire); }

	// Clears back to Idle. Only legal from a terminal state; returns false otherwise, so a job
	// cannot be reset out from under a running session.
	bool Reset();

	static const char* StateName(RenderSessionState s);

private:
	std::atomic<RenderSessionState> m_state{ RenderSessionState::Idle };
	std::atomic<bool> m_cancelRequested{ false };
};

RenderSession& RenderSessionRef();

} // namespace Filmmaker
