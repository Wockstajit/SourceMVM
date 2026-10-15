// FilmmakerDemoRepair - strip the legacy net message that breaks CS2 demo playback after the
// 2026-07-20 update.
//
// THE BUG (Valve's, not ours): CS2 build 14172 no longer accepts entity message type 138
// (CEntityMessageRemoveAllDecals). Any demo recorded before the update that contains one makes
// the client's netmessage reader desync the moment it hits it:
//
//	Netchan 'DEMO' closed unexpectedly: Unknown message type 138
//	Netchan 'DEMO' closed unexpectedly: Error parsing message type 9 (CNETMsg_SpawnGroup_ManifestUpdate)
//	Disconnect reason: NETWORK_DISCONNECT_MESSAGE_PARSE_ERROR
//
// Everything after the bad message decodes as garbage, so the tail of that log is noise -- the
// first "Unknown message type 138" is the real event. This reproduces on a clean client with no
// mods, which is how we know it is not an AfxHookSource2 problem.
//
// THE FIX: walk the demo container, and inside each packet frame drop only those type-138
// messages whose payload is a well-formed protobuf (the fail-closed guard). Every other byte of
// the file is copied through untouched, and kept messages are copied as verbatim BIT SPANS so
// nothing is re-encoded. Output is a separate file; the input is never modified.
//
// Usage:
//
//	FilmmakerDemoRepair -in <demo.dem> -out <repaired.dem>
//	FilmmakerDemoRepair -in <demo.dem> -check          (report only, writes nothing)
//
// Always prints a single JSON object on stdout so the C++ caller can parse the result, and
// returns non-zero on failure.
//
// Standalone by design: its own module, its own binary, no dependency on FilmmakerDemoInfo.
package main

import (
	"encoding/binary"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"

	"github.com/golang/snappy"
)

// Demo command ids (EDemoCommands). Only the packet-bearing ones matter here.
const (
	demSignonPacket = 8
	demPacket       = 7
	demFullPacket   = 13

	demIsCompressed = 64
)

// The offending entity message id.
const legacyRemoveAllDecals = 138

// Protobuf field numbers: CDemoPacket.data = 3, CDemoFullPacket.packet = 2.
const (
	fieldDemoPacketData   = 3
	fieldFullPacketPacket = 2
)

var demoMagic = []byte("PBDEMS2\x00")

type result struct {
	OK            bool   `json:"ok"`
	Error         string `json:"error,omitempty"`
	Input         string `json:"input"`
	Output        string `json:"output,omitempty"`
	NeededRepair  bool   `json:"neededRepair"`
	MessagesFound int    `json:"messagesFound"`
	FramesTouched int    `json:"framesTouched"`
	FramesTotal   int    `json:"framesTotal"`
	BytesIn       int    `json:"bytesIn"`
	BytesOut      int    `json:"bytesOut"`
}

func main() {
	inPath := flag.String("in", "", "path to the source .dem")
	outPath := flag.String("out", "", "path to write the repaired .dem")
	check := flag.Bool("check", false, "only report whether a repair is needed")
	flag.Parse()

	res := result{Input: *inPath}
	if *inPath == "" || (!*check && *outPath == "") {
		res.Error = "usage: -in <demo.dem> (-out <repaired.dem> | -check)"
		emit(res, 2)
	}

	data, err := os.ReadFile(*inPath)
	if err != nil {
		res.Error = err.Error()
		emit(res, 1)
	}
	res.BytesIn = len(data)

	out, stats, err := repair(data, *check)
	if err != nil {
		res.Error = err.Error()
		emit(res, 1)
	}
	res.MessagesFound = stats.messages
	res.FramesTouched = stats.framesTouched
	res.FramesTotal = stats.framesTotal
	res.NeededRepair = stats.messages > 0

	if !*check && stats.messages > 0 {
		if err := os.WriteFile(*outPath, out, 0o644); err != nil {
			res.Error = err.Error()
			emit(res, 1)
		}
		res.Output = *outPath
		res.BytesOut = len(out)
	}
	res.OK = true
	emit(res, 0)
}

func emit(r result, code int) {
	b, _ := json.Marshal(r)
	fmt.Println(string(b))
	os.Exit(code)
}

type stats struct {
	messages      int
	framesTouched int
	framesTotal   int
}

func repair(data []byte, checkOnly bool) ([]byte, stats, error) {
	var st stats
	if len(data) < len(demoMagic)+8 {
		return nil, st, errors.New("file too small to be a CS2 demo")
	}
	if string(data[:len(demoMagic)]) != string(demoMagic) {
		return nil, st, errors.New("not a PBDEMS2 demo (wrong magic)")
	}

	// Header: magic + two int32 offsets. Those offsets point at the fileinfo/spawngroups
	// frames; we keep every frame (only shrinking packet payloads), so they stay valid only if
	// nothing before them moved. They DO move, so they are rewritten at the end.
	headerLen := len(demoMagic) + 8
	out := make([]byte, 0, len(data))
	out = append(out, data[:headerLen]...)

	// Original absolute offsets recorded in the header, so we can remap them.
	origFileInfo := int(int32(binary.LittleEndian.Uint32(data[len(demoMagic):])))
	origSpawnGroups := int(int32(binary.LittleEndian.Uint32(data[len(demoMagic)+4:])))
	newFileInfo, newSpawnGroups := origFileInfo, origSpawnGroups

	pos := headerLen
	for pos < len(data) {
		frameStart := pos

		cmd, n, err := readUvarint(data, pos)
		if err != nil {
			break // trailing garbage / clean EOF
		}
		pos = n
		tick, n, err := readUvarint(data, pos)
		if err != nil {
			break
		}
		pos = n
		size, n, err := readUvarint(data, pos)
		if err != nil {
			break
		}
		pos = n
		if uint64(len(data)-pos) < size {
			break
		}
		payload := data[pos : pos+int(size)]
		pos += int(size)
		st.framesTotal++

		// Remap the header offsets as frames shift.
		if frameStart == origFileInfo {
			newFileInfo = len(out)
		}
		if frameStart == origSpawnGroups {
			newSpawnGroups = len(out)
		}

		compressed := cmd&demIsCompressed != 0
		baseCmd := cmd &^ demIsCompressed

		if baseCmd != demPacket && baseCmd != demSignonPacket && baseCmd != demFullPacket {
			out = append(out, data[frameStart:pos]...) // untouched, byte-for-byte
			continue
		}

		raw := payload
		if compressed {
			dec, err := snappy.Decode(nil, payload)
			if err != nil {
				out = append(out, data[frameStart:pos]...) // can't read it -> leave it alone
				continue
			}
			raw = dec
		}

		newRaw, found, err := stripFromFrame(raw, baseCmd)
		if err != nil || found == 0 {
			out = append(out, data[frameStart:pos]...)
			continue
		}
		st.messages += found
		st.framesTouched++
		if checkOnly {
			out = append(out, data[frameStart:pos]...)
			continue
		}

		newPayload := newRaw
		if compressed {
			newPayload = snappy.Encode(nil, newRaw)
		}
		out = appendUvarint(out, cmd)
		out = appendUvarint(out, tick)
		out = appendUvarint(out, uint64(len(newPayload)))
		out = append(out, newPayload...)
	}

	if !checkOnly && st.messages > 0 {
		binary.LittleEndian.PutUint32(out[len(demoMagic):], uint32(int32(newFileInfo)))
		binary.LittleEndian.PutUint32(out[len(demoMagic)+4:], uint32(int32(newSpawnGroups)))
	}
	return out, st, nil
}

// stripFromFrame rewrites one decompressed frame payload, returning the new bytes and how many
// messages were dropped.
func stripFromFrame(raw []byte, baseCmd uint64) ([]byte, int, error) {
	fields, err := parseFields(raw)
	if err != nil {
		return nil, 0, err
	}

	if baseCmd == demFullPacket {
		// CDemoFullPacket.packet (field 2) wraps a CDemoPacket.
		for i, f := range fields {
			if f.num != fieldFullPacketPacket || f.wire != wireBytes {
				continue
			}
			inner, found, err := stripFromFrame(f.val, demPacket)
			if err != nil || found == 0 {
				return nil, 0, err
			}
			_ = i
			return replaceBytesField(fields, fieldFullPacketPacket, inner), found, nil
		}
		return nil, 0, nil
	}

	// CDemoPacket.data (field 3) is the netmessage bitstream.
	for _, f := range fields {
		if f.num != fieldDemoPacketData || f.wire != wireBytes {
			continue
		}
		newData, found := stripMessages(f.val)
		if found == 0 {
			return nil, 0, nil
		}
		return replaceBytesField(fields, fieldDemoPacketData, newData), found, nil
	}
	return nil, 0, nil
}

// stripMessages walks the unaligned netmessage bitstream and copies every message through
// except well-formed type-138 ones.
func stripMessages(data []byte) ([]byte, int) {
	r := newBitReader(data)
	w := newBitWriter()
	dropped := 0

	for {
		msgStart := r.pos
		msgType, ok := r.readUBitVar()
		if !ok {
			break
		}
		size, ok := r.readVarUInt32()
		if !ok {
			break
		}
		payloadStart := r.pos
		if r.bitsLeft() < int(size)*8 {
			// Truncated tail: copy the remainder verbatim and stop.
			r.pos = msgStart
			w.copyBits(data, msgStart, r.bitsLeft())
			break
		}
		r.skipBits(int(size) * 8)
		msgEnd := r.pos

		if msgType == legacyRemoveAllDecals {
			// Fail-closed: only drop it if the payload really is a protobuf message. A
			// mis-framed stream could land us anywhere, and dropping the wrong span would
			// corrupt the demo far worse than the original bug.
			pl, ok := readBytesAt(data, payloadStart, int(size))
			if ok && isWellFormed(pl) {
				dropped++
				continue // skip: not copied to the output
			}
		}
		w.copyBits(data, msgStart, msgEnd-msgStart)
	}

	if dropped == 0 {
		return nil, 0
	}
	return w.bytes(), dropped
}

func readUvarint(b []byte, i int) (uint64, int, error) {
	var v uint64
	for s := uint(0); ; s += 7 {
		if i >= len(b) || s > 63 {
			return 0, 0, errTruncated
		}
		c := b[i]
		i++
		v |= uint64(c&0x7F) << s
		if c&0x80 == 0 {
			return v, i, nil
		}
	}
}

func appendUvarint(dst []byte, v uint64) []byte { return appendVarint(dst, v) }
