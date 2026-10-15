package main

import (
	"fmt"
	"os"

	demoinfocs "github.com/markus-wa/demoinfocs-golang/v5/pkg/demoinfocs"
)

func main() {
	f, err := os.Open(os.Args[1])
	if err != nil { fmt.Fprintln(os.Stderr, err); os.Exit(1) }
	defer f.Close()
	p := demoinfocs.NewParser(f)
	defer p.Close()
	seen := map[uint64]string{}
	target := 26291
	for {
		ok, err := p.ParseNextFrame()
		if err != nil || !ok { break }
		if p.GameState().IngameTick() >= target {
			for _, pl := range p.GameState().Participants().Playing() {
				seen[pl.SteamID64] = pl.Name
			}
			break
		}
	}
	for id, n := range seen { fmt.Printf("%d\t%q\n", id, n) }
}
