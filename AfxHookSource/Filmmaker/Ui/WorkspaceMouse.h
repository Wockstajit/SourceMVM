#pragma once

// Who has the mouse while a workspace is open, CS2's rules (UiClickMath.h) as a state machine, so the
// game host and the browser mockup decide the same way. Engine-free and header-only;
// Automation/tests/source-workspace-mouse-tests.cpp pins it.
//
//   Ui    the cursor is free, panels take clicks, the game sees no mouse. The default.
//   Game  the game has the mouse (a left click in the preview); G gives it back.
//   Look  a right press in the preview that was held or dragged: the game looks around until the
//         button is released, then the mouse returns to the UI.
//
// Each input returns where it goes: to the UI (feed it to ImGui), to the game (let it through), or
// nowhere (the workspace used it). The host applies the cursor side effects of a mode change.

#include "UiTypes.h"

#include "../../../AfxHookSource2/Filmmaker/Movie/UiClickMath.h"

#include <cstdint>

namespace SourceFilmmaker {
namespace Ui {

class WorkspaceMouse {
public:
	enum class Mode { Ui, Game, Look };
	enum class Route { Ui, Game, Swallow };
	enum Button { Left = 0, Right = 1, Middle = 2 };

	Mode GetMode() const { return m_mode; }
	bool RightPending() const { return m_rightPending; }
	// Where the right press that became the current look started, client px.
	float LookX() const { return m_rightX; }
	float LookY() const { return m_rightY; }

	// The workspace opened or closed: the UI has the mouse, nothing is pending.
	void Reset() {
		m_mode = Mode::Ui;
		m_rightPending = false;
	}

	// G: the UI and the game trade the mouse. A look in progress is left alone.
	void ToggleGame() {
		if (m_mode == Mode::Look) return;
		m_mode = m_mode == Mode::Ui ? Mode::Game : Mode::Ui;
		m_rightPending = false;
	}

	Route Move(float x, float y, int64_t nowMs) {
		if (m_mode != Mode::Ui) return Route::Game;
		CheckLook(nowMs, x, y, true);
		return m_mode == Mode::Look ? Route::Game : Route::Ui;
	}

	// Once a frame: a right press held long enough looks even without moving.
	void Tick(int64_t nowMs) { CheckLook(nowMs, m_rightX, m_rightY, false); }

	Route ButtonDown(int button, float x, float y, int64_t nowMs, const Rect& preview) {
		if (m_mode != Mode::Ui) return Route::Game;
		const bool inPreview = preview.Contains(x, y);
		if (button == Left) {
			Filmmaker::UiClick::Input in;
			in.uiModeOn = true;
			in.workspace = true;
			in.preview.valid = preview.Valid();
			in.preview.x0 = preview.x0; in.preview.y0 = preview.y0;
			in.preview.x1 = preview.x1; in.preview.y1 = preview.y1;
			in.x = x; in.y = y;
			if (Filmmaker::UiClick::ClickSwitchesToGameMode(in)) {
				m_mode = Mode::Game;
				return Route::Game; // the click carries on to the game
			}
			return Route::Ui;
		}
		if (button == Right && inPreview) {
			m_rightPending = true;
			m_rightX = x;
			m_rightY = y;
			m_rightAtMs = nowMs;
			return Route::Swallow;
		}
		return Route::Ui;
	}

	Route ButtonUp(int button, int64_t nowMs) {
		(void)nowMs;
		if (button == Right && m_mode == Mode::Look) {
			m_mode = Mode::Ui;
			return Route::Swallow;
		}
		if (m_mode != Mode::Ui) return Route::Game;
		if (button == Right && m_rightPending) {
			// Released before it became a look: a click. CS2 opens the picked object's menu here.
			m_rightPending = false;
			return Route::Swallow;
		}
		return Route::Ui;
	}

	// The wheel over the preview is the game's camera; over a panel it scrolls the panel.
	Route Wheel(float x, float y, const Rect& preview) const {
		if (m_mode != Mode::Ui) return Route::Game;
		return preview.Contains(x, y) ? Route::Game : Route::Ui;
	}

	// The window lost focus: a press whose release will never come is dropped, a look ends.
	void FocusLost() {
		m_rightPending = false;
		if (m_mode == Mode::Look) m_mode = Mode::Ui;
	}

private:
	void CheckLook(int64_t nowMs, float x, float y, bool moved) {
		if (!m_rightPending) return;
		const int heldMs = (int)(nowMs - m_rightAtMs);
		const int dx = moved ? (int)(x - m_rightX) : 0;
		const int dy = moved ? (int)(y - m_rightY) : 0;
		if (!Filmmaker::UiClick::RightPressBecomesLook(heldMs, dx, dy)) return;
		m_rightPending = false;
		m_mode = Mode::Look;
	}

	Mode m_mode = Mode::Ui;
	bool m_rightPending = false;
	float m_rightX = 0, m_rightY = 0;
	int64_t m_rightAtMs = 0;
};

} // namespace Ui
} // namespace SourceFilmmaker
