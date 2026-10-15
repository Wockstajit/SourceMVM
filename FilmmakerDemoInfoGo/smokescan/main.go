// Standalone smoke-tick scanner for render-pass occlusion acceptance.
//
// Prints the tick window of every smoke cloud in a demo, so a render test can pick a range where a
// player is genuinely hidden BY SMOKE (the half of the "not visible in beauty => not visible
// anywhere" rule that depth preservation cannot fix, because smoke writes no depth).
//
// Deliberately its own program, not a mode bolted onto FilmmakerDemoInfoGo: that tool builds the
// scoreboard .fmjson cache and has nothing to do with render acceptance (CLAUDE.md no-coupling
// rule). Build and run:
//
//	go -C FilmmakerDemoInfoGo run ./smokescan <demo.dem>
package main

import (
	"fmt"
	"os"
	"sort"

	demoinfocs "github.com/markus-wa/demoinfocs-golang/v5/pkg/demoinfocs"
	events "github.com/markus-wa/demoinfocs-golang/v5/pkg/demoinfocs/events"
)

type cloud struct {
	start, end int
	x, y, z    float64
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: find-smoke-ticks <demo.dem>")
		os.Exit(2)
	}
	f, err := os.Open(os.Args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	defer f.Close()

	p := demoinfocs.NewParser(f)
	defer p.Close()

	open := map[int64]*cloud{}
	var done []cloud

	p.RegisterEventHandler(func(e events.SmokeStart) {
		c := &cloud{start: p.GameState().IngameTick()}
		c.x, c.y, c.z = e.Position.X, e.Position.Y, e.Position.Z
		open[int64(e.GrenadeEntityID)] = c
	})
	p.RegisterEventHandler(func(e events.SmokeExpired) {
		if c, ok := open[int64(e.GrenadeEntityID)]; ok {
			c.end = p.GameState().IngameTick()
			done = append(done, *c)
			delete(open, int64(e.GrenadeEntityID))
		}
	})

	if err := p.ParseToEnd(); err != nil {
		fmt.Fprintln(os.Stderr, "parse:", err)
	}
	for _, c := range open { // clouds still alive at demo end
		c.end = p.GameState().IngameTick()
		done = append(done, *c)
	}
	sort.Slice(done, func(i, j int) bool { return done[i].start < done[j].start })

	fmt.Println("startTick\tendTick\tdurationTicks\tx\ty\tz")
	for _, c := range done {
		fmt.Printf("%d\t%d\t%d\t%.0f\t%.0f\t%.0f\n", c.start, c.end, c.end-c.start, c.x, c.y, c.z)
	}
	fmt.Fprintf(os.Stderr, "%d smoke cloud(s)\n", len(done))
}
