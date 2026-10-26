#pragma once

// The mouse outside the workspaces while a demo plays, CS2's behaviour (MovieMode.h): the cursor
// stays free and the game sees no mouse; the demo bar takes its own clicks; a click anywhere else
// steps the spectated player (left next, right previous, CS2's binds in its demo cameras); holding
// a button or dragging looks around (chase and free cam) until the button comes up, and the cursor
// returns where the press was. CS:S does none of this itself, so the hook does it.
//
// Press and hold are told apart by CS2's look rule (UiClickMath.h RightPressBecomesLook: 200 ms or
// more than 4 px), the one the workspaces already use. While HLAE's free camera (mirv_input) flies
// the view, a press steers it at once and steps no player, as CS2's own free cam does.
//
// Engine-free and header-only; Automation/tests/source-director-math-tests.cpp pins it.

#include "UiTypes.h"

#include "../../../AfxHookSource2/Filmmaker/Movie/UiClickMath.h"

#include <cstdint>

namespace SourceFilmmaker {
namespace Ui {

class DirectorMouse {
public:
	enum class Mode { Free, Look };
	enum class Route { Ui, Game, Swallow };
	enum class Click { None, NextPlayer, PreviousPlayer };
	enum Button { Left = 0, Right = 1, Middle = 2 };

	Mode GetMode() const { return m_mode; }
	bool Pending() const { return m_pending >= 0; }
	// Where the press that became the current look was, client px.
	float LookX() const { return m_x; }
	float LookY() const { return m_y; }

	// Director mode began or ended: the cursor is free, nothing is pending.
	void Reset() {
		m_mode = Mode::Free;
		m_pending = -1;
		m_barPress[Left] = m_barPress[Right] = false;
	}

	// steer: the free camera flies the view, so the press looks at once.
	Route ButtonDown(int button, float x, float y, int64_t nowMs, const Rect& bar, bool steer = false) {
		if (button != Left && button != Right) return m_mode == Mode::Look ? Route::Game : Route::Swallow;
		if (m_mode == Mode::Look) return Route::Game;
		if (bar.Contains(x, y)) {
			m_barPress[button] = true; // its release goes to the bar too
			return Route::Ui;
		}
		if (m_pending >= 0) return Route::Swallow; // the other button while one waits: ignored
		if (steer) {
			m_mode = Mode::Look;
			m_lookButton = button;
			m_x = x;
			m_y = y;
			return Route::Swallow; // the press itself steps no player
		}
		m_pending = button;
		m_x = x;
		m_y = y;
		m_atMs = nowMs;
		return Route::Swallow;
	}

	Route Move(float x, float y, int64_t nowMs) {
		if (m_mode == Mode::Look) return Route::Game;
		CheckLook(nowMs, x, y, true);
		return m_mode == Mode::Look ? Route::Game : Route::Ui;
	}

	// Once a frame: a press held long enough looks even without moving.
	void Tick(int64_t nowMs) { CheckLook(nowMs, m_x, m_y, false); }

	Route ButtonUp(int button, int64_t nowMs, Click& click) {
		(void)nowMs;
		click = Click::None;
		if (button != Left && button != Right) return m_mode == Mode::Look ? Route::Game : Route::Swallow;
		if (m_barPress[button]) {
			m_barPress[button] = false;
			return Route::Ui;
		}
		if (m_mode == Mode::Look) {
			if (button != m_lookButton) return Route::Game;
			m_mode = Mode::Free;
			return Route::Swallow;
		}
		if (button == m_pending) {
			m_pending = -1;
			click = button == Left ? Click::NextPlayer : Click::PreviousPlayer;
		}
		return Route::Swallow;
	}

	// The window lost focus: a press whose release will never come is dropped, a look ends.
	void FocusLost() {
		m_pending = -1;
		m_barPress[Left] = m_barPress[Right] = false;
		m_mode = Mode::Free;
	}

private:
	void CheckLook(int64_t nowMs, float x, float y, bool moved) {
		if (m_pending < 0) return;
		const int heldMs = (int)(nowMs - m_atMs);
		const int dx = moved ? (int)(x - m_x) : 0;
		const int dy = moved ? (int)(y - m_y) : 0;
		if (!Filmmaker::UiClick::RightPressBecomesLook(heldMs, dx, dy)) return;
		m_lookButton = m_pending;
		m_pending = -1;
		m_mode = Mode::Look;
	}

	Mode m_mode = Mode::Free;
	int m_pending = -1;     // the button whose press waits to become a click or a look
	int m_lookButton = -1;  // the button holding the current look
	bool m_barPress[2] = { false, false };
	float m_x = 0, m_y = 0;
	int64_t m_atMs = 0;
};

} // namespace Ui
} // namespace SourceFilmmaker
