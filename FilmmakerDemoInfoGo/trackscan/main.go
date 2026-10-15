// Player track exporter for planning camera-path shots offline.
//
// Writes every player's position, eye angles, ground/crouch state and velocity for each demo tick
// in a range, plus the shots and kills inside it, as one JSON document. The cinematic planner
// (Automation/cinematics/shotplan.py) frames, collision-checks and previews camera paths against
// this track without playing the demo in game.
//
// Its own program for the same reason as the sibling killscan/smokescan tools: the main
// FilmmakerDemoInfoGo binary only builds the scoreboard cache.
//
//	go run ./trackscan <demo.dem> <startTick> <endTick> <out.json>
package main

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
	"strconv"

	demoinfocs "github.com/markus-wa/demoinfocs-golang/v5/pkg/demoinfocs"
	common "github.com/markus-wa/demoinfocs-golang/v5/pkg/demoinfocs/common"
	events "github.com/markus-wa/demoinfocs-golang/v5/pkg/demoinfocs/events"
	msg "github.com/markus-wa/demoinfocs-golang/v5/pkg/demoinfocs/msg"
)

type sample struct {
	Tick   int        `json:"tick"`
	Pos    [3]float64 `json:"pos"`
	Eye    [3]float64 `json:"eye"`    // eye position (pos + view offset)
	Angles [2]float64 `json:"angles"` // pitch, yaw (Source convention: pitch down positive)
	Vel    [3]float64 `json:"vel"`
	Ground bool       `json:"ground"`
	Duck   bool       `json:"duck"`
	Alive  bool       `json:"alive"`
	Weapon string     `json:"weapon"`
}

type player struct {
	Name    string   `json:"name"`
	SteamID uint64   `json:"steamid"`
	Team    int      `json:"team"`
	Samples []sample `json:"samples"`
}

type event struct {
	Tick   int        `json:"tick"`
	Kind   string     `json:"kind"`
	Player string     `json:"player"`
	Victim string     `json:"victim,omitempty"`
	Weapon string     `json:"weapon,omitempty"`
	Pos    [3]float64 `json:"pos"`
	HS     bool       `json:"hs,omitempty"`
}

type document struct {
	Demo    string             `json:"demo"`
	Map     string             `json:"map"`
	Start   int                `json:"start"`
	End     int                `json:"end"`
	Players map[string]*player `json:"players"`
	Events  []event            `json:"events"`
}

func flags(pl *common.Player) (ground, duck bool) {
	pawn := pl.PlayerPawnEntity()
	if pawn == nil {
		return true, false
	}
	v, ok := pawn.PropertyValue("m_fFlags")
	if !ok {
		return true, false
	}
	var fl uint64
	switch x := v.Any.(type) {
	case uint32:
		fl = uint64(x)
	case uint64:
		fl = x
	case int32:
		fl = uint64(x)
	}
	return fl&1 != 0, fl&2 != 0
}

func round2(v float64) float64 { return math.Round(v*100) / 100 }

func main() {
	if len(os.Args) < 5 {
		fmt.Fprintln(os.Stderr, "usage: trackscan <demo.dem> <startTick> <endTick> <out.json>")
		os.Exit(2)
	}
	start, _ := strconv.Atoi(os.Args[2])
	end, _ := strconv.Atoi(os.Args[3])
	f, err := os.Open(os.Args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	defer f.Close()

	p := demoinfocs.NewParser(f)
	defer p.Close()
	doc := document{Demo: os.Args[1], Start: start, End: end, Players: map[string]*player{}}
	lastTick := -1

	inRange := func() bool { t := p.GameState().IngameTick(); return t >= start && t <= end }
	p.RegisterEventHandler(func(e events.WeaponFire) {
		if !inRange() || e.Shooter == nil {
			return
		}
		pos := e.Shooter.Position()
		w := ""
		if e.Weapon != nil {
			w = e.Weapon.String()
		}
		doc.Events = append(doc.Events, event{Tick: p.GameState().IngameTick(), Kind: "shot", Player: e.Shooter.Name, Weapon: w, Pos: [3]float64{round2(pos.X), round2(pos.Y), round2(pos.Z)}})
	})
	p.RegisterEventHandler(func(e events.Kill) {
		if !inRange() || e.Victim == nil {
			return
		}
		k := event{Tick: p.GameState().IngameTick(), Kind: "kill", Victim: e.Victim.Name, HS: e.IsHeadshot}
		if e.Killer != nil {
			k.Player = e.Killer.Name
		}
		if e.Weapon != nil {
			k.Weapon = e.Weapon.String()
		}
		pos := e.Victim.Position()
		k.Pos = [3]float64{round2(pos.X), round2(pos.Y), round2(pos.Z)}
		doc.Events = append(doc.Events, k)
	})
	// Smoke clouds and fires that are up during the range, including ones that started before it
	// (a smoke lasts ~20 s): the planner keeps the camera out of them and its sightline clear of them.
	smokeWindow := func() bool { t := p.GameState().IngameTick(); return t >= start-1600 && t <= end }
	nade := func(kind string, e events.GrenadeEvent) {
		if !smokeWindow() {
			return
		}
		q := e.Position
		doc.Events = append(doc.Events, event{Tick: p.GameState().IngameTick(), Kind: kind,
			Player: fmt.Sprint(e.GrenadeEntityID), Pos: [3]float64{round2(q.X), round2(q.Y), round2(q.Z)}})
	}
	p.RegisterEventHandler(func(e events.SmokeStart) { nade("smoke_start", e.GrenadeEvent) })
	p.RegisterEventHandler(func(e events.SmokeExpired) { nade("smoke_end", e.GrenadeEvent) })
	p.RegisterEventHandler(func(e events.FireGrenadeStart) { nade("fire_start", e.GrenadeEvent) })
	p.RegisterEventHandler(func(e events.FireGrenadeExpired) { nade("fire_end", e.GrenadeEvent) })
	// Detonations inside the range: war shots aim at them and jolt the camera on the blast.
	blast := func(kind string, e events.GrenadeEvent) {
		if inRange() {
			nade(kind, e)
		}
	}
	p.RegisterEventHandler(func(e events.HeExplode) { blast("he", e.GrenadeEvent) })
	p.RegisterEventHandler(func(e events.FlashExplode) { blast("flash", e.GrenadeEvent) })
	p.RegisterEventHandler(func(e events.FrameDone) {
		gs := p.GameState()
		t := gs.IngameTick()
		if t == lastTick || t < start || t > end {
			return
		}
		lastTick = t
		for _, pl := range gs.Participants().Playing() {
			if pl == nil {
				continue
			}
			key := strconv.FormatUint(pl.SteamID64, 10)
			tr := doc.Players[key]
			if tr == nil {
				tr = &player{Name: pl.Name, SteamID: pl.SteamID64, Team: int(pl.Team)}
				doc.Players[key] = tr
			}
			pos := pl.Position()
			var vel struct{ X, Y, Z float64 }
			if n := len(tr.Samples); n > 0 {
				// Velocity from the previous sample (units/s at 64 ticks per second).
				prev := tr.Samples[n-1]
				dt := float64(t-prev.Tick) / 64
				vel.X, vel.Y, vel.Z = (pos.X-prev.Pos[0])/dt, (pos.Y-prev.Pos[1])/dt, (pos.Z-prev.Pos[2])/dt
			}
			ground, duck := flags(pl)
			eyeZ := 64.06
			if duck {
				eyeZ = 46.04
			}
			w := ""
			if aw := pl.ActiveWeapon(); aw != nil {
				w = aw.String()
			}
			pitch := float64(pl.ViewDirectionY())
			if pitch > 180 {
				pitch -= 360
			}
			tr.Samples = append(tr.Samples, sample{
				Tick:   t,
				Pos:    [3]float64{round2(pos.X), round2(pos.Y), round2(pos.Z)},
				Eye:    [3]float64{round2(pos.X), round2(pos.Y), round2(pos.Z + eyeZ)},
				Angles: [2]float64{round2(pitch), round2(float64(pl.ViewDirectionX()))},
				Vel:    [3]float64{round2(vel.X), round2(vel.Y), round2(vel.Z)},
				Ground: ground, Duck: duck, Alive: pl.IsAlive(), Weapon: w,
			})
		}
		if t >= end {
			p.Cancel()
		}
	})
	p.RegisterNetMessageHandler(func(m *msg.CSVCMsg_ServerInfo) { doc.Map = m.GetMapName() })
	if err := p.ParseToEnd(); err != nil && err != demoinfocs.ErrCancelled {
		fmt.Fprintln(os.Stderr, "parse:", err)
	}
	out, _ := json.Marshal(doc)
	if err := os.WriteFile(os.Args[4], out, 0o644); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Printf("map=%s players=%d events=%d ticks=%d..%d -> %s\n", doc.Map, len(doc.Players), len(doc.Events), start, end, os.Args[4])
}
