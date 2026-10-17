// FilmmakerDemoKeyframes writes a copy of a CS2 demo with a synthesized DEM_FullPacket every
// -interval ticks. CS2 seeks BACKWARD by restoring the newest full packet at or before the target
// and replaying from there (CDemoPlayer::SkipToTick / FullPacketGoto), and Valve writes one only
// every 3840 ticks -- so going back can replay up to a minute of demo. With a keyframe every 32
// ticks the replay is at most 32 ticks. Contract: Docs/features/filmmaker_demo_scrubbing.md.
//
// Every synthesized entity update is decoded again and compared field-by-field (encoded bits)
// with the live state before it is written, and the live state is checked against each of
// Valve's own full packets, so a mistake fails the run instead of producing a demo that
// restores a wrong world.
package main

import (
	"flag"
	"fmt"
	"os"
	"sort"
	"strings"
	"time"

	"github.com/markus-wa/demoinfocs-golang/v5/pkg/demoinfocs/msg"
	"google.golang.org/protobuf/proto"

	st2 "filmmakerdemokeyframes/st2"
)

var (
	svcServerInfo     = uint32(msg.SVC_Messages_svc_ServerInfo)
	svcCreateTable    = uint32(msg.SVC_Messages_svc_CreateStringTable)
	svcUpdateTable    = uint32(msg.SVC_Messages_svc_UpdateStringTable)
	svcPacketEntities = uint32(msg.SVC_Messages_svc_PacketEntities)
	svcUserCmds       = uint32(msg.SVC_Messages_svc_UserCmds)
	netTick           = uint32(msg.NET_Messages_net_Tick)
)

type netMsg struct {
	t    uint32
	body []byte
}

func splitMessages(data []byte) ([]netMsg, error) {
	var out []netMsg
	r := newBitReader(data)
	for r.bitsLeft() > 8 {
		t, ok := r.readUBitVar()
		if !ok {
			return out, fmt.Errorf("truncated message type")
		}
		size, ok := r.readVarUInt32()
		if !ok {
			return out, fmt.Errorf("truncated message size")
		}
		body, ok := readBytesAt(r.buf, r.pos, int(size))
		if !ok {
			return out, fmt.Errorf("truncated message body (type %d, %d bytes)", t, size)
		}
		r.skipBits(int(size) * 8)
		out = append(out, netMsg{t, body})
	}
	return out, nil
}

func joinMessages(ms []netMsg) []byte {
	var w st2.BitWriter
	for _, m := range ms {
		w.WriteUBitVar(m.t)
		w.WriteVarUint32(uint32(len(m.body)))
		for _, b := range m.body {
			w.WriteBits(uint32(b), 8)
		}
	}
	return w.Buf
}

type state struct {
	ents           *st2.Parser
	tables         *stringTables
	lastPE         *msg.CSVCMsg_PacketEntities // header fields of the newest entity update
	tickMsgs       []netMsg                    // the newest DEM_Packet's messages
	nonTransmit    *msg.CSVCMsg_PacketEntitiesNonTransmittedEntitiesT
	haveFull       bool // the first full entity update has been applied
	valveDiffs     map[string]int
	valveFields    int
	valveDiffering int // unexplained differences only (valveDiffExplained)
	valveExplained int
}

// valveDiffExplained says whether a difference from Valve's full packet is one of the kinds normal
// playback is known to hold (see checkAgainstValve): the scoreboard bookkeeping the server stops
// sending once a round is over -- per-round stats, the match totals, the damage list -- and stale
// animation and simulation times. None of them reach a rendered frame, but the per-round stats
// grow with every round played, so counting them against the 1% bar failed long matches outright:
// 350 of 27805 fields at tick 65281 of a 10-player match, every one of them of these kinds.
// They are still listed; only differences outside these kinds count toward the bar.
func valveDiffExplained(key string) bool {
	for _, kind := range []string{".m_perRoundStats.", ".m_pActionTrackingServices.", ".m_pDamageServices.",
		".m_flAnimTime", ".m_flSimulationTime"} {
		if strings.Contains(key, kind) {
			return true
		}
	}
	return false
}

func (s *state) onPacket(data []byte, isFull bool) error {
	ms, err := splitMessages(data)
	if err != nil {
		return err
	}
	if !isFull {
		s.tickMsgs = ms
	}
	for _, m := range ms {
		switch m.t {
		case svcServerInfo:
			var si msg.CSVCMsg_ServerInfo
			if err := proto.Unmarshal(m.body, &si); err != nil {
				return err
			}
			if err := s.ents.OnServerInfo(&si); err != nil {
				return err
			}
		case svcCreateTable:
			var ct msg.CSVCMsg_CreateStringTable
			if err := proto.Unmarshal(m.body, &ct); err != nil {
				return err
			}
			if err := s.tables.create(&ct); err != nil {
				return err
			}
			s.refreshBaselines(ct.GetName())
		case svcUpdateTable:
			var ut msg.CSVCMsg_UpdateStringTable
			if err := proto.Unmarshal(m.body, &ut); err != nil {
				return err
			}
			if err := s.tables.update(&ut); err != nil {
				return err
			}
			if id := int(ut.GetTableId()); id < len(s.tables.byID) {
				s.refreshBaselines(s.tables.byID[id].name)
			}
		case svcPacketEntities:
			if isFull {
				continue // the stream is complete without the full packets' copies
			}
			var pe msg.CSVCMsg_PacketEntities
			if err := proto.Unmarshal(m.body, &pe); err != nil {
				return err
			}
			if !pe.GetLegacyIsDelta() && s.haveFull {
				continue
			}
			if err := safeEntities(s.ents, &pe); err != nil {
				return err
			}
			if !pe.GetLegacyIsDelta() {
				s.haveFull = true
				s.nonTransmit = pe.GetNonTransmittedEntities()
			}
			s.lastPE = &pe
		}
	}
	return nil
}

func safeEntities(p *st2.Parser, pe *msg.CSVCMsg_PacketEntities) (err error) {
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("entity update: %v", r)
		}
	}()
	return p.OnPacketEntities(pe)
}

func (s *state) refreshBaselines(table string) {
	if table != "instancebaseline" {
		return
	}
	for _, it := range s.tables.byName[table].items {
		var id int
		if _, err := fmt.Sscanf(it.key, "%d", &id); err == nil && !strings.Contains(it.key, ":") && it.data != nil {
			s.ents.SetInstanceBaseline(id, it.data)
		}
	}
}

// buildEntities writes the full (non-delta) entity update for the current state.
func (s *state) buildEntities(verify bool) (*msg.CSVCMsg_PacketEntities, error) {
	var w st2.BitWriter
	var counts []byte
	prev := int32(-1)
	ents := s.ents.ActiveEntities()
	for _, e := range ents {
		_, fieldsStart, end := s.ents.EncodeEntity(&w, prev, e)
		counts = appendUvarint(counts, uint64(end-fieldsStart))
		prev = e.Index()
	}
	l := s.lastPE
	n := int32(len(ents))
	f := false
	zero := uint32(0)
	base := int32(0)
	pe := &msg.CSVCMsg_PacketEntities{
		MaxEntries:                    l.MaxEntries,
		UpdatedEntries:                &n,
		LegacyIsDelta:                 &f,
		UpdateBaseline:                &f,
		Baseline:                      &base,
		EntityData:                    w.Buf,
		ActiveSpawngroupHandle:        l.ActiveSpawngroupHandle,
		MaxSpawngroupCreationsequence: l.MaxSpawngroupCreationsequence,
		LastCmdNumberExecuted:         l.LastCmdNumberExecuted,
		ServerTick:                    l.ServerTick,
		SerializedEntities:            counts,
		NonTransmittedEntities:        s.nonTransmit,
		CqStarvedCommandTicks:         &zero,
		CqDiscardedCommandTicks:       &zero,
	}
	if verify {
		q := s.ents.CloneForVerify()
		if err := safeEntities(q, pe); err != nil {
			return nil, fmt.Errorf("re-decoding the synthesized update: %v", err)
		}
		if err := st2.CompareEntities(s.ents, q); err != nil {
			return nil, fmt.Errorf("synthesized update does not round-trip: %v", err)
		}
	}
	return pe, nil
}

func (s *state) buildFullPacket(tick uint64, verify bool) ([]byte, error) {
	pe, err := s.buildEntities(verify)
	if err != nil {
		return nil, err
	}
	peBody, err := proto.Marshal(pe)
	if err != nil {
		return nil, err
	}
	// Same messages as Valve's: the tick's net_Tick, the entities, the tick's user commands.
	var ms []netMsg
	for _, m := range s.tickMsgs {
		if m.t == netTick {
			ms = append(ms, m)
		}
	}
	ms = append(ms, netMsg{svcPacketEntities, peBody})
	for _, m := range s.tickMsgs {
		if m.t == svcUserCmds {
			ms = append(ms, m)
		}
	}
	fp := &msg.CDemoFullPacket{StringTable: &msg.CDemoStringTables{}, Packet: &msg.CDemoPacket{Data: joinMessages(ms)}}
	names := make([]string, 0)
	for name, t := range s.tables.byName {
		if t.dirty {
			names = append(names, name)
		}
	}
	sort.Strings(names)
	for _, name := range names {
		fp.StringTable.Tables = append(fp.StringTable.Tables, s.tables.byName[name].snapshot())
		s.tables.byName[name].dirty = false
	}
	return proto.Marshal(fp)
}

// checkAgainstValve decodes one of Valve's full packets and compares it with the tracked state.
func (s *state) checkAgainstValve(fp *msg.CDemoFullPacket) error {
	ms, err := splitMessages(fp.GetPacket().GetData())
	if err != nil {
		return err
	}
	for _, m := range ms {
		if m.t != svcPacketEntities {
			continue
		}
		var pe msg.CSVCMsg_PacketEntities
		if err := proto.Unmarshal(m.body, &pe); err != nil {
			return err
		}
		q := s.ents.CloneForVerify()
		if err := safeEntities(q, &pe); err != nil {
			return fmt.Errorf("decoding Valve's full packet: %v", err)
		}
		// Valve's snapshot is the SERVER's view; a keyframe must reproduce what normal playback has
		// in the CLIENT, which is the tracked state. The two legitimately differ in a few fields:
		// the server omits stale weapon timing fields (m_flSimulationTime, m_flAnimTime) and
		// per-round stats the client still holds, and a value the server changes BACK to an earlier
		// one is sometimes never sent as a delta (engine2 carries a comment about exactly that), so
		// normal playback keeps the stale value. Those are reported. The entity list itself must
		// match, and field differences must stay rare -- widespread ones mean a tracking bug.
		ea, eb := s.ents.ActiveEntities(), q.ActiveEntities()
		if len(ea) != len(eb) {
			return fmt.Errorf("tracked %d entities, Valve's full packet has %d", len(ea), len(eb))
		}
		total := 0
		for i := range ea {
			if ea[i].Index() != eb[i].Index() || ea[i].ClassID() != eb[i].ClassID() || ea[i].Serial() != eb[i].Serial() {
				return fmt.Errorf("entity #%d differs from Valve's full packet (index %d/%d class %d/%d serial %d/%d)",
					i, ea[i].Index(), eb[i].Index(), ea[i].ClassID(), eb[i].ClassID(), ea[i].Serial(), eb[i].Serial())
			}
			total += len(ea[i].RawFields())
		}
		sum := st2.DiffSummary(s.ents, q)
		differing, explained := 0, 0
		for k, n := range sum {
			if valveDiffExplained(k) {
				explained += n
			} else {
				differing += n
			}
			s.valveDiffs[k] += n
		}
		if differing*100 > total {
			keys := make([]string, 0, len(sum))
			for k := range sum {
				keys = append(keys, k)
			}
			sort.Strings(keys)
			for _, k := range keys {
				fmt.Printf("    %4d  %s\n", sum[k], k)
			}
			return fmt.Errorf("%d of %d fields differ from Valve's full packet (more than 1%%; %d more are scoreboard or stale-time fields)",
				differing, total, explained)
		}
		s.valveFields += total
		s.valveDiffering += differing
		s.valveExplained += explained
	}
	var mine []string
	for name, t := range s.tables.byName {
		if t.dirty {
			mine = append(mine, name)
		}
	}
	var theirs []string
	for _, t := range fp.GetStringTable().GetTables() {
		theirs = append(theirs, t.GetTableName())
		mt := s.tables.byName[t.GetTableName()]
		if mt == nil {
			return fmt.Errorf("Valve's full packet carries table %s that was never created", t.GetTableName())
		}
		if len(mt.items) != len(t.GetItems()) {
			return fmt.Errorf("table %s: %d items tracked, Valve has %d", t.GetTableName(), len(mt.items), len(t.GetItems()))
		}
		for i, it := range t.GetItems() {
			if mt.items[i].key != it.GetStr() || string(mt.items[i].data) != string(it.GetData()) {
				return fmt.Errorf("table %s item %d differs (key %q vs %q, %d data bytes %x vs %d bytes %x)", t.GetTableName(), i,
					mt.items[i].key, it.GetStr(), len(mt.items[i].data), mt.items[i].data, len(it.GetData()), it.GetData())
			}
		}
	}
	sort.Strings(mine)
	sort.Strings(theirs)
	if strings.Join(mine, ",") != strings.Join(theirs, ",") {
		fmt.Printf("  note: changed tables tracked [%s], Valve's full packet carries [%s]\n", strings.Join(mine, ","), strings.Join(theirs, ","))
	}
	for _, t := range s.tables.byName {
		t.dirty = false
	}
	return nil
}

func main() {
	interval := flag.Int("interval", 32, "ticks between keyframes (divides 3840 so Valve's own stay on the grid)")
	verifyEvery := flag.Int("verify-every", 1, "re-decode every Nth synthesized keyframe (1 = all)")
	dense := flag.String("dense", "", "first-last tick windows, comma separated, that get the -dense-every step")
	denseEvery := flag.Int("dense-every", 2, "ticks between keyframes inside the -dense windows (divides -interval)")
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "usage: %s [flags] <in.dem> <out.dem>\n", os.Args[0])
		flag.PrintDefaults()
	}
	flag.Parse()
	if flag.NArg() != 2 {
		flag.Usage()
		os.Exit(2)
	}
	g, err := newGrid(*interval, *denseEvery, *dense)
	if err != nil {
		fmt.Fprintln(os.Stderr, "FAILED:", err)
		os.Exit(2)
	}
	if err := run(flag.Arg(0), flag.Arg(1), g, *verifyEvery); err != nil {
		fmt.Fprintln(os.Stderr, "FAILED:", err)
		os.Exit(1)
	}
}

func run(inPath, outPath string, g grid, verifyEvery int) error {
	t0 := time.Now()
	data, err := os.ReadFile(inPath)
	if err != nil {
		return err
	}
	frames, err := readFrames(data)
	if err != nil {
		return err
	}
	if os.Getenv("KF_CENSUS") != "" {
		census(frames)
		return nil
	}
	s := &state{ents: st2.NewParser(nil), tables: newStringTables(), valveDiffs: map[string]int{}}
	st2.TraceField = os.Getenv("KF_TRACE")
	out := make([]byte, 0, len(data)+len(data)/3)
	out = append(out, data[:demHeaderSize]...)
	type insertion struct{ at, n int }
	var inserts []insertion
	made, verified, valveChecks := 0, 0, 0
	for i, f := range frames {
		st2.TraceTick = f.tick
		out = append(out, f.raw...)
		switch f.cmd {
		case demSendTables:
			var m msg.CDemoSendTables
			if err := proto.Unmarshal(f.payload, &m); err != nil {
				return err
			}
			if err := s.ents.ParsePacket(m.GetData()); err != nil {
				return err
			}
		case demClassInfo:
			var m msg.CDemoClassInfo
			if err := proto.Unmarshal(f.payload, &m); err != nil {
				return err
			}
			if err := s.ents.OnDemoClassInfo(&m); err != nil {
				return err
			}
		case demStringTables:
			var m msg.CDemoStringTables
			if err := proto.Unmarshal(f.payload, &m); err != nil {
				return err
			}
			s.tables.load(&m)
			s.refreshBaselines("instancebaseline")
		case demPacket, demSignonPacket:
			var m msg.CDemoPacket
			if err := proto.Unmarshal(f.payload, &m); err != nil {
				return err
			}
			if err := s.onPacket(m.GetData(), false); err != nil {
				return fmt.Errorf("tick %d: %v", f.tick, err)
			}
		case demFullPacket:
			var m msg.CDemoFullPacket
			if err := proto.Unmarshal(f.payload, &m); err != nil {
				return err
			}
			if s.haveFull {
				if err := s.checkAgainstValve(&m); err != nil {
					return fmt.Errorf("tick %d: %v", f.tick, err)
				}
				valveChecks++
			}
		}
		// A keyframe goes right after the tick's DEM_Packet, where Valve puts its own -- unless
		// Valve's is the next frame.
		if f.cmd != demPacket || !s.haveFull || f.tick == tickNone || !g.keyframeAt(int(f.tick)) {
			continue
		}
		if i+1 < len(frames) && frames[i+1].cmd == demFullPacket && frames[i+1].tick == f.tick {
			continue
		}
		verify := verifyEvery > 0 && made%verifyEvery == 0
		payload, err := s.buildFullPacket(f.tick, verify)
		if err != nil {
			return fmt.Errorf("keyframe at tick %d: %v", f.tick, err)
		}
		fr := encodeFrame(demFullPacket, f.tick, payload)
		inserts = append(inserts, insertion{f.off + len(f.raw), len(fr)})
		out = append(out, fr...)
		made++
		if verify {
			verified++
		}
	}
	// FileInfo and SpawnGroups follow the Stop frame; the header points at them.
	if len(frames) > 0 {
		last := frames[len(frames)-1]
		out = append(out, data[last.off+len(last.raw):]...)
	}
	patchHeader(out[:demHeaderSize], func(old int) int {
		n := old
		for _, in := range inserts {
			if in.at <= old {
				n += in.n
			}
		}
		return n
	})
	if err := os.WriteFile(outPath, out, 0o644); err != nil {
		return err
	}
	fmt.Printf("keyframes: %d written %s (%d re-decoded and matched)\n", made, g.describe(), verified)
	fmt.Printf("valve: %d full packets checked, same entities, %d of %d fields differ, plus %d scoreboard or stale-time fields (client-held stale values, listed below)\n",
		valveChecks, s.valveDiffering, s.valveFields, s.valveExplained)
	fmt.Printf("size: %.1f MB -> %.1f MB in %.1fs\n", float64(len(data))/1e6, float64(len(out))/1e6, time.Since(t0).Seconds())
	keys := make([]string, 0, len(s.valveDiffs))
	for k := range s.valveDiffs {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		fmt.Printf("  %5d  %s\n", s.valveDiffs[k], k)
	}
	return nil
}
