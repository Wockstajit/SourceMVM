// FilmmakerDemoInfo (Go / demoinfocs-golang) - parse a CS2 .dem and print a JSON
// scoreboard summary on stdout. It emits the "<demo>.fmjson" schema the C++ side
// reads (DemoInfoHelper.{h,cpp}); the parser is a single self-contained binary
// with no runtime dependency.
//
// Usage:  FilmmakerDemoInfo <path-to-demo.dem> [accountId,accountId,...]
// Output: a single JSON object on stdout. On failure it still prints
//
//	{"ok":false,"error":"..."} and returns non-zero so the C++ caller can
//	cache the negative result and not retry forever.
//
// This ALWAYS does a full parse (demoinfocs is fast). The optional accountId list
// is accepted for command-line compatibility but ignored - the full parse is
// needed to settle the real end-of-match team sides and MVPs, which the .dem.info
// matchmaking sidecar gets wrong (it splits the roster at the halfway index) and
// cannot provide at all (no MVPs / no per-round timeline).
package main

import (
	"encoding/json"
	"fmt"
	"os"
	"sort"

	dem "github.com/markus-wa/demoinfocs-golang/v5/pkg/demoinfocs"
	"github.com/markus-wa/demoinfocs-golang/v5/pkg/demoinfocs/common"
	"github.com/markus-wa/demoinfocs-golang/v5/pkg/demoinfocs/events"
	"github.com/markus-wa/demoinfocs-golang/v5/pkg/demoinfocs/msg"
)

// Must match kSchemaVersion in DemoInfoHelper.cpp. A cached "<demo>.fmjson" whose
// "v" differs was produced by an older helper and is discarded.
//
//	v6: parser switched to demoinfocs-golang; "team" is the real end-of-match side
//	    (CT/T), MVPs and per-round MVP/side are populated for ALL parsed demos.
const schemaVersion = 10

const steamID64Base uint64 = 76561197960265728

type roundStat struct {
	K    int `json:"k"`
	HS   int `json:"hs"`
	D    int `json:"d"`
	W    int `json:"w"`
	Side int `json:"side"`
	MVP  int `json:"mvp"`
}

type playerOut struct {
	Name      string      `json:"name"`
	SteamID   string      `json:"steamId"`
	AccountID uint32      `json:"accountId"`
	Team      int         `json:"team"`
	K         int         `json:"k"`
	A         int         `json:"a"`
	D         int         `json:"d"`
	Score     int         `json:"score"`
	MVPs      int         `json:"mvps"`
	PerRound  []roundStat `json:"perRound"`
}

type eventOut struct {
	Tick      int    `json:"tick"`
	Type      string `json:"type"`
	Item      string `json:"item"`
	AccountID uint32 `json:"accountId"`
}

type result struct {
	OK            bool        `json:"ok"`
	V             int         `json:"v"`
	NamesOnly     bool        `json:"namesOnly"`
	Map           string      `json:"map"`
	DurationSecs  int         `json:"durationSeconds"`
	Rounds        int         `json:"rounds"`
	TeamScore0    int         `json:"teamScore0"`
	TeamScore1    int         `json:"teamScore1"`
	HasScoreboard bool        `json:"hasScoreboard"`
	Players       []playerOut `json:"players"`
	Events        []eventOut  `json:"events"`
}

// Per-player accumulator. Totals (k/a/d/score/mvps) are snapshotted from the
// player entity (the official scoreboard values); the per-round breakdown is
// derived from kill / mvp game events.
type agg struct {
	steamID   uint64
	name      string
	team      int // last seen side: 0 = CT, 1 = T, -1 = none
	finalTeam int // side held at the last scored round (the side "ended on")
	kills     int
	deaths    int
	assists   int
	score     int
	mvps      int

	curK, curHS, curD, curMVP int
	rounds                    [][6]int // [k, hs, died, won, side, mvp]
}

func teamIdx(t common.Team) int {
	switch t {
	case common.TeamCounterTerrorists:
		return 0
	case common.TeamTerrorists:
		return 1
	default:
		return -1
	}
}

func accountID(sid uint64) uint32 {
	if sid > steamID64Base {
		return uint32(sid - steamID64Base)
	}
	return 0
}

// lastSlash returns the index of the last '/' or '\\' in s, or -1 if none.
func lastSlash(s string) int {
	for i := len(s) - 1; i >= 0; i-- {
		if s[i] == '/' || s[i] == '\\' {
			return i
		}
	}
	return -1
}

func fail(msg string) {
	b, _ := json.Marshal(map[string]any{"ok": false, "error": msg})
	os.Stdout.Write(b)
}

func main() {
	if len(os.Args) < 2 {
		fail("usage: FilmmakerDemoInfo <demo.dem> [accountIds]")
		os.Exit(2)
	}
	path := os.Args[1]
	f, err := os.Open(path)
	if err != nil {
		fail("file not found")
		os.Exit(2)
	}
	defer f.Close()

	p := dem.NewParser(f)
	defer p.Close()

	// The CS2 demo header is internal in v5; the map name arrives via the
	// CSVCMsg_ServerInfo net message (clean short name, e.g. "de_mirage").
	var mapName string
	p.RegisterNetMessageHandler(func(m *msg.CSVCMsg_ServerInfo) {
		mn := m.GetMapName()
		if i := lastSlash(mn); i >= 0 {
			mn = mn[i+1:]
		}
		mapName = mn
	})

	roster := map[uint64]*agg{}
	// Resolve (and lazily create) the accumulator for a player, refreshing the
	// name + last-seen side every time we see them in an event.
	get := func(pl *common.Player) *agg {
		if pl == nil || pl.SteamID64 == 0 {
			return nil
		}
		a := roster[pl.SteamID64]
		if a == nil {
			a = &agg{steamID: pl.SteamID64, team: -1, finalTeam: -1}
			roster[pl.SteamID64] = a
		}
		if pl.Name != "" {
			a.name = pl.Name
		}
		if ti := teamIdx(pl.Team); ti >= 0 {
			a.team = ti
		}
		return a
	}

	var evs []eventOut
	addEvent := func(t string, pl *common.Player, item string) {
		var acc uint32
		if pl != nil {
			acc = accountID(pl.SteamID64)
		}
		evs = append(evs, eventOut{Tick: p.GameState().IngameTick(), Type: t, Item: item, AccountID: acc})
	}

	// Snapshot every connected player's cumulative totals. Called at each round
	// boundary and at the end so a player who disconnects in the post-match
	// wind-down still keeps the totals from the last round they were present.
	snapshot := func() {
		for _, pl := range p.GameState().Participants().All() {
			a := get(pl)
			if a == nil {
				continue
			}
			a.kills = pl.Kills()
			a.deaths = pl.Deaths()
			a.assists = pl.Assists()
			a.score = pl.Score()
		}
	}

	// Attribute MVPs from the official m_iMVPs entity property rather than the
	// round_mvp game event, which many GOTV/HLTV demos never emit. CS2 raises
	// m_iMVPs at round end, and that increment is already visible by the time the
	// RoundEnd handler runs -- so we detect it AFTER committing the round and pin
	// it to the round that just ended (rounds[len-1]). Also called once after
	// ParseToEnd as a safety net for a final-round MVP that lands post-RoundEnd.
	detectMvpDelta := func() {
		for _, pl := range p.GameState().Participants().All() {
			a := get(pl)
			if a == nil {
				continue
			}
			if m := pl.MVPs(); m > a.mvps {
				a.mvps = m
				if n := len(a.rounds); n > 0 {
					a.rounds[n-1][5] = 1
				} else {
					a.curMVP = 1
				}
			}
		}
	}

	// Kills / headshots / deaths for the per-round timeline + the Follow Camera's
	// weapon-drop-on-death event (CS2 exposes no weapon_drop game event).
	p.RegisterEventHandler(func(e events.Kill) {
		victim := get(e.Victim)
		if victim != nil {
			victim.curD = 1
		}
		attacker := get(e.Killer)
		if attacker != nil && attacker != victim {
			attacker.curK++
			if e.IsHeadshot {
				attacker.curHS++
			}
		}
		addEvent("weapon_drop", e.Victim, "")
	})

	// round_mvp (when a demo emits it) marks the player who earned MVP this round;
	// patch the round just committed on RoundEnd. This is a secondary signal - the
	// m_iMVPs delta (detectMvpDelta) is the reliable source.
	p.RegisterEventHandler(func(e events.RoundMVPAnnouncement) {
		if a := get(e.Player); a != nil {
			if n := len(a.rounds); n > 0 {
				a.rounds[n-1][5] = 1
			} else {
				a.curMVP = 1
			}
		}
	})

	// MatchStart fires when the live match begins (after warmup / knife round /
	// restarts); drop anything captured before it.
	p.RegisterEventHandler(func(e events.MatchStart) {
		for _, a := range roster {
			a.rounds = a.rounds[:0]
			a.curK, a.curHS, a.curD, a.curMVP = 0, 0, 0, 0
		}
	})

	// Commit one entry per scored round. Warmup rounds are skipped.
	p.RegisterEventHandler(func(e events.RoundEnd) {
		gs := p.GameState()
		if gs.IsWarmupPeriod() {
			return
		}
		snapshot()
		winner := teamIdx(e.Winner)
		for _, a := range roster {
			side := a.team
			if side >= 0 {
				a.finalTeam = side
			}
			won := 0
			if side >= 0 && side == winner {
				won = 1
			}
			a.rounds = append(a.rounds, [6]int{a.curK, a.curHS, a.curD, won, side, a.curMVP})
			a.curK, a.curHS, a.curD, a.curMVP = 0, 0, 0, 0
		}
		// Detect the MVP increment AFTER committing this round so it attributes to
		// the round that just ended, not the previous one. (Detecting before the
		// commit shifted every MVP star one round too early on the timeline.)
		detectMvpDelta()
	})

	// Loadout events for the Follow Camera's Preview-Tick jump. Skip pickups during
	// freezetime - those are the automatic round-start weapon grants (the old .NET
	// helper filtered these as "silent"), which would otherwise drown out the real
	// mid-round ground pickups with ~5x noise.
	p.RegisterEventHandler(func(e events.ItemPickup) {
		if p.GameState().IsFreezetimePeriod() {
			return
		}
		item := ""
		if e.Weapon != nil {
			item = e.Weapon.String()
		}
		addEvent("item_pickup", e.Player, item)
	})
	p.RegisterEventHandler(func(e events.BombDropped) { addEvent("bomb_dropped", e.Player, "c4") })
	p.RegisterEventHandler(func(e events.BombPickup) { addEvent("bomb_pickup", e.Player, "c4") })
	p.RegisterEventHandler(func(e events.BombPlanted) { addEvent("bomb_planted", e.Player, "c4") })

	var lastTick int
	p.RegisterEventHandler(func(e events.FrameDone) {
		if t := p.GameState().IngameTick(); t > lastTick {
			lastTick = t
		}
	})

	if err := p.ParseToEnd(); err != nil {
		fail(err.Error())
		os.Exit(1)
	}
	detectMvpDelta() // attribute the final round's MVP
	snapshot()

	gs := p.GameState()
	ctScore := gs.TeamCounterTerrorists().Score()
	tScore := gs.TeamTerrorists().Score()

	// Header playback time is internal in v5; derive duration from the last seen
	// in-game tick and the server tick time.
	durationSeconds := 0
	if tt := p.TickTime().Seconds(); tt > 0 && lastTick > 0 {
		durationSeconds = int(float64(lastTick) * tt)
	}

	// Only real players (CT/T), sorted by score desc so the scoreboard reads like CS2.
	recs := make([]*agg, 0, len(roster))
	for _, a := range roster {
		if a.team == 0 || a.team == 1 {
			recs = append(recs, a)
		}
	}
	sort.SliceStable(recs, func(i, j int) bool { return recs[i].score > recs[j].score })

	players := make([]playerOut, 0, len(recs))
	for _, a := range recs {
		reportTeam := a.finalTeam
		if reportTeam < 0 {
			reportTeam = a.team
		}
		pr := make([]roundStat, 0, len(a.rounds))
		for _, r := range a.rounds {
			pr = append(pr, roundStat{K: r[0], HS: r[1], D: r[2], W: r[3], Side: r[4], MVP: r[5]})
		}
		name := a.name
		if name == "" {
			name = "[unknown]"
		}
		players = append(players, playerOut{
			Name:      name,
			SteamID:   fmt.Sprintf("%d", a.steamID),
			AccountID: accountID(a.steamID),
			Team:      reportTeam,
			K:         a.kills,
			A:         a.assists,
			D:         a.deaths,
			Score:     a.score,
			MVPs:      a.mvps,
			PerRound:  pr,
		})
	}

	if evs == nil {
		evs = []eventOut{}
	}
	out := result{
		OK:            true,
		V:             schemaVersion,
		NamesOnly:     false,
		Map:           mapName,
		DurationSecs:  durationSeconds,
		Rounds:        ctScore + tScore,
		TeamScore0:    ctScore,
		TeamScore1:    tScore,
		HasScoreboard: len(players) > 0,
		Players:       players,
		Events:        evs,
	}
	b, err := json.Marshal(out)
	if err != nil {
		fail("encode: " + err.Error())
		os.Exit(1)
	}
	os.Stdout.Write(b)
}
