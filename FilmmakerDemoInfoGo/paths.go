// Round paths mode: every living player's floor position through every round, for the round heat
// map (AfxHookSource2/Filmmaker/Heatmap/RoundHeatmap.cpp, format parsed by RoundHeatmapMath.h). The
// hook runs `FilmmakerDemoInfo <demo.dem> -paths` once per demo and caches stdout as
// roundpaths.fmbin in the demo's project folder.
//
// Little-endian binary on stdout (a heat map holds tens of thousands of samples per round, which
// JSON would make ten times larger):
//
//	"FMRP" u32 version u16 sampleStepTicks u16 mapLen map[mapLen] u32 roundCount
//	round:  i32 startTick i32 endTick(exclusive) u8 warmup u16 playerCount
//	player: u32 accountId u8 team(2 = T, 3 = CT) u32 sampleCount
//	sample: i16 x i16 y i16 z u8 flags
//
// Ticks are demo ticks: IngameTick reports the demo packet's tick, the clock the engine's demo tick
// (GetDemoTick, demo_gototick) counts in.
// A round runs from its round_start to the next one, so freeze time and the after-round walk are in
// it. Positions are the feet (m_vecAbsOrigin) rounded to whole units. Flag 1 starts a new stroke
// (first sample of a round, after a respawn, or a jump the player could not have walked), flag 2 is
// on ground, flag 4 is crouched. A failure prints {"ok":false,...} JSON instead of the magic.
package main

import (
	"bufio"
	"encoding/binary"
	"math"
	"os"

	dem "github.com/markus-wa/demoinfocs-golang/v5/pkg/demoinfocs"
	"github.com/markus-wa/demoinfocs-golang/v5/pkg/demoinfocs/common"
	"github.com/markus-wa/demoinfocs-golang/v5/pkg/demoinfocs/events"
	"github.com/markus-wa/demoinfocs-golang/v5/pkg/demoinfocs/msg"
)

const (
	pathsVersion    = 1
	pathsSampleStep = 2 // ticks between samples: 32 per second, about 8 units apart at full run speed

	flagStrokeStart = 1
	flagOnGround    = 2
	flagDucking     = 4

	// Farther than this between two samples is a teleport, not a step (full run speed covers about
	// 8 units per sample).
	pathsTeleportUnits = 96
)

type pathSample struct {
	x, y, z int16
	flags   uint8
}

type pathPlayer struct {
	accountID uint32
	team      uint8
	samples   []pathSample
	lastTick  int // tick of the last sample, for spotting a gap (death, disconnect)
}

type pathRound struct {
	start, end int
	warmup     bool
	players    map[uint64]*pathPlayer
	order      []uint64
}

func clampUnits(v float64) int16 {
	r := math.Round(v)
	if r > math.MaxInt16 {
		return math.MaxInt16
	}
	if r < math.MinInt16 {
		return math.MinInt16
	}
	return int16(r)
}

func pawnFlags(pl *common.Player) (ground, duck bool) {
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

func runPaths(path string) {
	f, err := os.Open(path)
	if err != nil {
		fail("file not found")
		os.Exit(2)
	}
	defer f.Close()

	p := dem.NewParser(f)
	defer p.Close()

	var mapName string
	p.RegisterNetMessageHandler(func(m *msg.CSVCMsg_ServerInfo) {
		mn := m.GetMapName()
		if i := lastSlash(mn); i >= 0 {
			mn = mn[i+1:]
		}
		mapName = mn
	})

	var rounds []*pathRound
	var cur *pathRound
	implicit := false // cur was opened by the first frame, not by a round_start
	lastTick := 0
	open := func(tick int) {
		if cur != nil {
			cur.end = tick
		}
		cur = &pathRound{start: tick, warmup: p.GameState().IsWarmupPeriod(), players: map[uint64]*pathPlayer{}}
		rounds = append(rounds, cur)
	}
	p.RegisterEventHandler(func(e events.RoundStart) {
		t := p.GameState().IngameTick()
		if implicit && t-cur.start < 5*64 {
			// A recording that starts a moment before the first round_start: the same freeze time,
			// not a round of its own.
			implicit = false
			cur.warmup = p.GameState().IsWarmupPeriod()
			return
		}
		implicit = false
		open(t)
	})

	lastSample := math.MinInt32
	p.RegisterEventHandler(func(e events.FrameDone) {
		gs := p.GameState()
		t := gs.IngameTick()
		if t > lastTick {
			lastTick = t
		}
		if cur == nil {
			open(t) // a demo that starts mid-round
			implicit = true
		}
		if t < lastSample+pathsSampleStep {
			return
		}
		lastSample = t
		for _, pl := range gs.Participants().Playing() {
			if pl == nil || pl.SteamID64 == 0 || !pl.IsAlive() {
				continue
			}
			tr := cur.players[pl.SteamID64]
			if tr == nil {
				team := uint8(0)
				switch pl.Team {
				case common.TeamTerrorists:
					team = 2
				case common.TeamCounterTerrorists:
					team = 3
				}
				tr = &pathPlayer{accountID: accountID(pl.SteamID64), team: team}
				cur.players[pl.SteamID64] = tr
				cur.order = append(cur.order, pl.SteamID64)
			}
			pos := pl.Position()
			s := pathSample{x: clampUnits(pos.X), y: clampUnits(pos.Y), z: clampUnits(pos.Z)}
			ground, duck := pawnFlags(pl)
			if ground {
				s.flags |= flagOnGround
			}
			if duck {
				s.flags |= flagDucking
			}
			if n := len(tr.samples); n == 0 || t-tr.lastTick > 4*pathsSampleStep {
				s.flags |= flagStrokeStart
			} else {
				prev := tr.samples[n-1]
				dx, dy, dz := float64(s.x-prev.x), float64(s.y-prev.y), float64(s.z-prev.z)
				if dx*dx+dy*dy+dz*dz > pathsTeleportUnits*pathsTeleportUnits {
					s.flags |= flagStrokeStart
				}
			}
			tr.lastTick = t
			tr.samples = append(tr.samples, s)
		}
	})

	if err := p.ParseToEnd(); err != nil {
		fail(err.Error())
		os.Exit(1)
	}
	if cur != nil {
		cur.end = lastTick + 1
	}

	kept := rounds[:0]
	for _, r := range rounds {
		if len(r.players) > 0 && r.end > r.start {
			kept = append(kept, r)
		}
	}

	w := bufio.NewWriterSize(os.Stdout, 1<<20)
	le := binary.LittleEndian
	put := func(v any) { binary.Write(w, le, v) }
	w.WriteString("FMRP")
	put(uint32(pathsVersion))
	put(uint16(pathsSampleStep))
	if len(mapName) > math.MaxUint16 {
		mapName = mapName[:math.MaxUint16]
	}
	put(uint16(len(mapName)))
	w.WriteString(mapName)
	put(uint32(len(kept)))
	for _, r := range kept {
		put(int32(r.start))
		put(int32(r.end))
		warm := uint8(0)
		if r.warmup {
			warm = 1
		}
		put(warm)
		put(uint16(len(r.order)))
		for _, id := range r.order {
			tr := r.players[id]
			put(tr.accountID)
			put(tr.team)
			put(uint32(len(tr.samples)))
			buf := make([]byte, 7*len(tr.samples))
			for i, s := range tr.samples {
				o := buf[7*i:]
				le.PutUint16(o[0:], uint16(s.x))
				le.PutUint16(o[2:], uint16(s.y))
				le.PutUint16(o[4:], uint16(s.z))
				o[6] = s.flags
			}
			w.Write(buf)
		}
	}
	w.Flush()
}
