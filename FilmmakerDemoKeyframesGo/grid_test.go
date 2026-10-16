package main

import "testing"

func TestGridBaseOnly(t *testing.T) {
	g, err := newGrid(32, 2, "")
	if err != nil {
		t.Fatal(err)
	}
	for _, tick := range []int{1, 33, 3841, 65281} {
		if !g.keyframeAt(tick) {
			t.Errorf("tick %d is on the base grid", tick)
		}
	}
	for _, tick := range []int{0, 2, 3, 32, 34} {
		if g.keyframeAt(tick) {
			t.Errorf("tick %d is off the base grid", tick)
		}
	}
}

func TestGridDenseWindows(t *testing.T) {
	g, err := newGrid(32, 2, "5500-6217, 9000-9100")
	if err != nil {
		t.Fatal(err)
	}
	// Inside a window every odd tick gets one -- the base grid's phase, so base ticks stay keyframes.
	for _, tick := range []int{5501, 5503, 5757, 6217, 9001, 9099} {
		if !g.keyframeAt(tick) {
			t.Errorf("tick %d is inside a dense window", tick)
		}
	}
	for _, tick := range []int{5500, 5756, 6218, 6219, 9101, 9103} {
		if g.keyframeAt(tick) {
			t.Errorf("tick %d should have no keyframe", tick)
		}
	}
	if !g.keyframeAt(6241) { // 6241 % 32 == 1: outside the window, still the base grid
		t.Error("the base grid continues outside the windows")
	}
}

func TestGridEveryTick(t *testing.T) {
	g, err := newGrid(32, 1, "100-103")
	if err != nil {
		t.Fatal(err)
	}
	for tick := 100; tick <= 103; tick++ {
		if !g.keyframeAt(tick) {
			t.Errorf("-dense-every 1 keys tick %d", tick)
		}
	}
}

func TestGridRejectsBadInput(t *testing.T) {
	for _, c := range []struct {
		interval, every int
		dense           string
	}{
		{32, 3, ""},      // does not divide the base interval
		{32, 0, ""},      // no step
		{32, 2, "10-5"},  // reversed
		{32, 2, "100"},   // not a range
		{32, 2, "a-b"},   // not numbers
		{32, 2, "-5-10"}, // negative
		{0, 1, ""},       // no base interval
	} {
		if _, err := newGrid(c.interval, c.every, c.dense); err == nil {
			t.Errorf("newGrid(%d, %d, %q) should fail", c.interval, c.every, c.dense)
		}
	}
}
