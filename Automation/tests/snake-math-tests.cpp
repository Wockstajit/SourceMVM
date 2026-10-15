// SnakeMath.h (the long-render Snake) and RenderEtaMath.h (when it pops up).
#include "../../AfxHookSource2/Filmmaker/EasterEggs/SnakeMath.h"
#include "../../AfxHookSource2/Filmmaker/Render/RenderEtaMath.h"

#include <cmath>
#include <cstdio>

using namespace Filmmaker;
using namespace Filmmaker::Snake;

namespace {
int g_failed = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAILED line %d: %s\n", __LINE__, #c); ++g_failed; } } while (0)

void TestStartAndMove() {
	Game g;
	g.Reset(20, 12, 7, 10);
	CHECK(g.state == State::Ready && g.body.size() == 4 && !g.OnSnake(g.food));
	const Cell head = g.body.front();
	CHECK(!g.Step() && g.body.front() == head);   // waits for a key
	g.Turn(kRight);                                // the first key starts it (same direction: no turn)
	CHECK(g.state == State::Playing);
	g.Step();
	CHECK(g.body.front().x == head.x + 1 && g.body.size() == 4);
	g.Turn(kLeft);                                 // straight back into the neck: ignored
	g.Step();
	CHECK(g.body.front().x == head.x + 2);
	g.Turn(kUp); g.Turn(kLeft);                    // two quick turns both happen, one per step
	g.Step(); CHECK(g.body.front().y == head.y - 1);
	g.Step(); CHECK(g.body.front().x == head.x + 1);
}

void TestEatAndWalls() {
	Game g;
	g.Reset(10, 10, 3, 5);
	g.food = Move(g.body.front(), kRight);
	g.Turn(kRight);
	CHECK(g.Step() && g.score == 1 && g.body.size() == 5 && !g.OnSnake(g.food) && g.foodIcon < 5);
	for (int i = 0; i < 20 && g.state == State::Playing; ++i) { g.food = { 0, 0 }; g.Step(); }
	CHECK(g.state == State::Dead);                 // ran into the right wall
}

void TestSelfAndTail() {
	Game g;
	g.Reset(10, 10, 5, 1);
	g.body = { { 5, 5 }, { 5, 6 }, { 4, 6 }, { 4, 5 } }; // a square: the head faces its own tail
	g.dir = kUp; g.food = { 0, 0 }; g.state = State::Playing;
	g.Turn(kLeft);
	g.Step();                                      // into the tail cell as the tail leaves: alive
	const Cell tailCell{ 4, 5 };
	CHECK(g.state == State::Playing && g.body.front() == tailCell);
	Game h;
	h.Reset(10, 10, 5, 1);
	h.body = { { 5, 5 }, { 5, 6 }, { 4, 6 }, { 4, 5 }, { 4, 4 } };
	h.dir = kUp; h.food = { 0, 0 }; h.state = State::Playing;
	h.Turn(kLeft);
	h.Step();                                      // (4,5) is body, not the tail: dead
	CHECK(h.state == State::Dead);
}

void TestEta() {
	// 1000 renders total, 100 done in 18 s -> 180 s total: not longer than 3 minutes.
	RenderEta e = EstimateRenderTime(18, 100, 1000);
	CHECK(e.valid && std::fabs(e.totalS - 180.0) < 1e-9 && !IsLongRender(e));
	e = EstimateRenderTime(18, 90, 1000);          // a touch slower: 200 s -> qualifies
	CHECK(IsLongRender(e));
	e = EstimateRenderTime(8, 10, 1000);           // very slow but only 8 s in: not yet
	CHECK(!IsLongRender(e));
	e = EstimateRenderTime(10, 10, 1000);          // 10 s in, ~1000 s total: Snake
	CHECK(IsLongRender(e));
	CHECK(!EstimateRenderTime(30, 0, 1000).valid);
}
} // namespace

int main() {
	TestStartAndMove();
	TestEatAndWalls();
	TestSelfAndTail();
	TestEta();
	if (g_failed) { std::printf("%d check(s) failed\n", g_failed); return 1; }
	std::printf("SnakeMathTests passed\n");
	return 0;
}
