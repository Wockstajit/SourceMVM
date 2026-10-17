package main

import (
	"fmt"
	"strconv"
	"strings"
)

// grid decides which ticks get a synthesized keyframe: the base grid everywhere (the same phase as
// Valve's own full packets, tick % interval == 1) plus a denser step inside the windows the hook
// passes with -dense -- the ticks around the camera-path markers, where scrubbing happens. See
// Docs/features/filmmaker_demo_scrubbing.md, "Dense keyframes around the shot".
type grid struct {
	interval   int
	denseEvery int
	dense      []window
}

type window struct{ first, last int } // inclusive ticks

func parseWindows(s string) ([]window, error) {
	var out []window
	s = strings.TrimSpace(s)
	if s == "" {
		return nil, nil
	}
	for _, part := range strings.Split(s, ",") {
		ab := strings.SplitN(strings.TrimSpace(part), "-", 2)
		if len(ab) != 2 {
			return nil, fmt.Errorf("dense window %q is not first-last", part)
		}
		a, errA := strconv.Atoi(ab[0])
		b, errB := strconv.Atoi(ab[1])
		if errA != nil || errB != nil || a < 0 || b < a {
			return nil, fmt.Errorf("dense window %q is not first-last", part)
		}
		out = append(out, window{a, b})
	}
	return out, nil
}

func newGrid(interval, denseEvery int, dense string) (grid, error) {
	if interval < 1 {
		return grid{}, fmt.Errorf("-interval must be at least 1")
	}
	ws, err := parseWindows(dense)
	if err != nil {
		return grid{}, err
	}
	// The dense step has to divide the base interval so every base keyframe is also a dense one:
	// a window edge then never leaves a longer gap than the base grid would.
	if denseEvery < 1 || interval%denseEvery != 0 {
		return grid{}, fmt.Errorf("-dense-every %d must divide -interval %d", denseEvery, interval)
	}
	return grid{interval: interval, denseEvery: denseEvery, dense: ws}, nil
}

func (g grid) keyframeAt(tick int) bool {
	if tick%g.interval == 1%g.interval {
		return true
	}
	if tick%g.denseEvery != 1%g.denseEvery {
		return false
	}
	for _, w := range g.dense {
		if tick >= w.first && tick <= w.last {
			return true
		}
	}
	return false
}

func (g grid) describe() string {
	if len(g.dense) == 0 {
		return fmt.Sprintf("every %d ticks", g.interval)
	}
	return fmt.Sprintf("every %d ticks, every %d in %d dense window(s)", g.interval, g.denseEvery, len(g.dense))
}
