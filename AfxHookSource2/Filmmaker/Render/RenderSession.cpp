#include "RenderSession.h"

namespace Filmmaker {

namespace {

using S = RenderSessionState;

// The legal edges, in one place. The happy path is a straight line; the interesting parts are the
// escapes, so those are called out rather than left implicit:
//
//   * ANY non-terminal state may go to Failed. A job can die at any point.
//   * ANY non-terminal state may go to CancelRequested. That is the user pressing cancel.
//   * CancelRequested goes ONLY to RestoringState or Canceled -- never straight back into work.
//     This is the cooperative half of the handshake: the worker acknowledges, unwinds, and only
//     then declares the session Canceled.
//   * Failed and Canceled both route through RestoringState in normal operation, but are allowed
//     as direct terminals too, because a failure early enough to have changed nothing has nothing
//     to restore.
bool IsLegal(S from, S to) {
	if (from == to) return true; // idempotent re-assert is harmless

	// Universal escapes from any live state.
	if (to == S::Failed || to == S::CancelRequested) {
		return !RenderSession::IsTerminal(from);
	}

	switch (from) {
		case S::Idle:              return to == S::Validating;
		case S::Validating:        return to == S::PreparingOutput;
		case S::PreparingOutput:   return to == S::SavingState;
		case S::SavingState:       return to == S::SeekingPreRoll;
		case S::SeekingPreRoll:    return to == S::WarmingUp;
		case S::WarmingUp:         return to == S::Capturing;
		case S::Capturing:         return to == S::FlushingGpu;
		case S::FlushingGpu:       return to == S::FinalizingStreams;
		case S::FinalizingStreams: return to == S::PostProcessing;
		case S::PostProcessing:    return to == S::RestoringState;
		case S::RestoringState:    return to == S::Completed || to == S::Canceled || to == S::Failed;

		// Cooperative cancel: acknowledge, then unwind. Never resumes work.
		case S::CancelRequested:   return to == S::RestoringState || to == S::Canceled;

		// Terminal. Only Reset() leaves these, and it goes to Idle directly.
		case S::Completed:
		case S::Failed:
		case S::Canceled:          return false;
	}
	return false;
}

} // namespace

bool RenderSession::IsTerminal(RenderSessionState s) {
	return s == S::Completed || s == S::Failed || s == S::Canceled;
}

bool RenderSession::IsBusy() const {
	const S s = State();
	return s != S::Idle && !IsTerminal(s);
}

bool RenderSession::TryTransition(RenderSessionState to, const char** reason) {
	const S from = State();
	if (!IsLegal(from, to)) {
		if (reason) *reason = "illegal transition";
		return false;
	}
	m_state.store(to, std::memory_order_release);
	if (reason) *reason = nullptr;
	return true;
}

void RenderSession::RequestCancel() {
	m_cancelRequested.store(true, std::memory_order_release);
	// Only nudge the state when there is live work to cancel. A terminal or idle session has
	// nothing to acknowledge, and forcing CancelRequested onto it would strand the machine.
	const S s = State();
	if (s != S::Idle && !IsTerminal(s) && s != S::CancelRequested)
		TryTransition(S::CancelRequested);
}

bool RenderSession::Reset() {
	if (!IsTerminalNow() && State() != S::Idle)
		return false; // a running session must not be reset out from under its worker
	m_state.store(S::Idle, std::memory_order_release);
	m_cancelRequested.store(false, std::memory_order_release);
	return true;
}

const char* RenderSession::StateName(RenderSessionState s) {
	switch (s) {
		case S::Idle:              return "Idle";
		case S::Validating:        return "Validating";
		case S::PreparingOutput:   return "PreparingOutput";
		case S::SavingState:       return "SavingState";
		case S::SeekingPreRoll:    return "SeekingPreRoll";
		case S::WarmingUp:         return "WarmingUp";
		case S::Capturing:         return "Capturing";
		case S::FlushingGpu:       return "FlushingGpu";
		case S::FinalizingStreams: return "FinalizingStreams";
		case S::PostProcessing:    return "PostProcessing";
		case S::RestoringState:    return "RestoringState";
		case S::Completed:         return "Completed";
		case S::Failed:            return "Failed";
		case S::CancelRequested:   return "CancelRequested";
		case S::Canceled:          return "Canceled";
	}
	return "Unknown";
}

RenderSession& RenderSessionRef() {
	static RenderSession s;
	return s;
}

} // namespace Filmmaker
