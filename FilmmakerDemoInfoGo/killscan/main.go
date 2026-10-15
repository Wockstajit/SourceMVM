// Standalone kill-tick scanner, for picking a demo tick worth screenshotting.
//
// Prints every kill with its tick, round, weapon, and the participants, so a capture can be aimed
// at a real mid-round firefight instead of a round-end or a blinded frame. Multi-kill moments are
// flagged, since those are the ticks where something is actually happening on screen.
//
// Deliberately its own program rather than a mode added to FilmmakerDemoInfoGo, which exists to
// build the scoreboard .fmjson cache and has nothing to do with choosing capture ticks
// (CLAUDE.md no-coupling rule). Same pattern as the sibling smokescan/namescan tools.
//
//	go run ./killscan <demo.dem>
package main

import (
	"fmt"
	"os"

	demoinfocs "github.com/markus-wa/demoinfocs-golang/v5/pkg/demoinfocs"
	events "github.com/markus-wa/demoinfocs-golang/v5/pkg/demoinfocs/events"
)

type kill struct {
	tick    int
	frame   int
	x, y, z float64
	round   int
	killer  string
	victim  string
	weapon  string
	hs      bool
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: killscan <demo.dem>")
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

	var kills []kill
	round := 0

	p.RegisterEventHandler(func(e events.RoundStart) { round++ })
	p.RegisterEventHandler(func(e events.Kill) {
		k := kill{tick: p.GameState().IngameTick(), frame: p.CurrentFrame(), round: round, hs: e.IsHeadshot}
		k.killer, k.victim = "?", "?"
		if e.Killer != nil {
			k.killer = e.Killer.Name
		}
		if e.Victim != nil {
			k.victim = e.Victim.Name
			pos := e.Victim.Position()
			k.x, k.y, k.z = pos.X, pos.Y, pos.Z
		}
		if e.Weapon != nil {
			k.weapon = e.Weapon.String()
		}
		kills = append(kills, k)
	})

	if err := p.ParseToEnd(); err != nil {
		fmt.Fprintln(os.Stderr, "parse:", err)
	}

	fmt.Printf("total kills: %d\n", len(kills))
	for i, k := range kills {
		// A kill within ~5 seconds (64 tick) of the previous one means a sustained fight rather
		// than an isolated pick -- those are the ticks worth capturing.
		burst := ""
		if i > 0 && k.tick-kills[i-1].tick < 320 {
			burst = "  <== BURST"
		}
		hs := ""
		if k.hs {
			hs = " (hs)"
		}
		fmt.Printf("event %3d frame %7d tick %7d round %2d %-16s -> %-16s %-14s%s%s position=(%.1f %.1f %.1f)\n",
			i, k.frame, k.tick, k.round, k.killer, k.victim, k.weapon, hs, burst, k.x, k.y, k.z)
	}
}
