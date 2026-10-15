#pragma once

// Snake, the rules (the long-render easter egg; EasterEggSnake.cpp drives it, the overlay draws it).
// Engine-free so Automation/tests/snake-math-tests.cpp can pin it.
//
// Classic rules: the snake moves one cell per step; walls and its own body end the game; eating
// grows it by one and puts new food (a random game icon) on a random empty cell. It waits in
// Ready until the first direction key. Up to two turns queue between steps so quick inputs are
// not lost, and a turn straight back into the neck is ignored.

#include <cstdint>
#include <deque>
#include <vector>

namespace Filmmaker {
namespace Snake {

enum Dir : int { kUp = 0, kRight = 1, kDown = 2, kLeft = 3 };
enum class State { Ready, Playing, Dead, Won };
struct Cell { int x = 0, y = 0; };
inline bool operator==(Cell a, Cell b) { return a.x == b.x && a.y == b.y; }

inline Cell Move(Cell c, int dir) {
	static const int dx[4] = { 0, 1, 0, -1 }, dy[4] = { -1, 0, 1, 0 };
	return { c.x + dx[dir & 3], c.y + dy[dir & 3] };
}
inline bool Opposite(int a, int b) { return ((a + 2) & 3) == (b & 3); }

struct Game {
	int w = 28, h = 18;
	std::deque<Cell> body; // front = head
	int dir = kRight;
	std::deque<int> turns;
	Cell food;
	int foodIcon = 0, iconCount = 1;
	int score = 0;
	State state = State::Ready;
	uint32_t rng = 1;

	uint32_t Rand() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

	bool OnSnake(Cell c) const { for (const Cell& b : body) if (b == c) return true; return false; }

	void PlaceFood() {
		std::vector<Cell> free;
		free.reserve((size_t)(w * h));
		for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) if (!OnSnake({ x, y })) free.push_back({ x, y });
		if (free.empty()) { state = State::Won; return; }
		food = free[Rand() % free.size()];
		foodIcon = (int)(Rand() % (uint32_t)(iconCount > 0 ? iconCount : 1));
	}

	void Reset(int width, int height, uint32_t seed, int icons) {
		w = width; h = height; iconCount = icons;
		rng = seed ? seed : 1;
		body.clear(); turns.clear();
		dir = kRight; score = 0; state = State::Ready;
		const Cell head{ w / 2, h / 2 };
		for (int i = 0; i < 4; ++i) body.push_back({ head.x - i, head.y });
		PlaceFood();
	}

	// A direction key. The first one starts the game.
	void Turn(int d) {
		if (state == State::Dead || state == State::Won) return;
		const int last = turns.empty() ? dir : turns.back();
		if (state == State::Ready) state = State::Playing;
		if (d == last || Opposite(d, last) || turns.size() >= 2) return;
		turns.push_back(d);
	}

	// One move. Returns true when the snake ate this step.
	bool Step() {
		if (state != State::Playing) return false;
		if (!turns.empty()) { dir = turns.front(); turns.pop_front(); }
		const Cell next = Move(body.front(), dir);
		if (next.x < 0 || next.y < 0 || next.x >= w || next.y >= h) { state = State::Dead; return false; }
		const bool eat = next == food;
		// Moving into the tail's cell is fine when the tail moves away this step.
		for (size_t i = 0; i < body.size(); ++i) {
			if (!(body[i] == next)) continue;
			if (i + 1 == body.size() && !eat) continue;
			state = State::Dead;
			return false;
		}
		body.push_front(next);
		if (eat) { score++; PlaceFood(); } else body.pop_back();
		return eat;
	}

	// Milliseconds per step: brisk at the start, quicker as it grows, capped so it stays playable.
	double StepMs() const { const double ms = 135.0 - 2.5 * score; return ms < 65.0 ? 65.0 : ms; }
};

} // namespace Snake
} // namespace Filmmaker
