package main

import (
	"encoding/binary"
	"fmt"

	"github.com/golang/snappy"
)

// CS2 demo framing: "PBDEMS2\0", int32 FileInfo offset, int32 SpawnGroups offset, then frames of
// varint(cmd | 64 if snappy) varint(tick) varint(size) payload.

const (
	demStop         = 0
	demFileHeader   = 1
	demFileInfo     = 2
	demSendTables   = 4
	demClassInfo    = 5
	demStringTables = 6
	demPacket       = 7
	demSignonPacket = 8
	demFullPacket   = 13
	demIsCompressed = 64
	demHeaderSize   = 16
	tickNone        = 0xFFFFFFFF
)

type frame struct {
	cmd     uint64 // without the compressed flag
	tick    uint64
	off     int    // offset of the frame in the source file
	raw     []byte // the whole frame, verbatim
	payload []byte // decompressed payload
}

func uvarint(b []byte, i int) (uint64, int, error) {
	var r uint64
	var s uint
	for {
		if i >= len(b) {
			return 0, i, fmt.Errorf("varint past end at %d", i)
		}
		c := b[i]
		i++
		r |= uint64(c&0x7f) << s
		s += 7
		if c&0x80 == 0 {
			return r, i, nil
		}
		if s > 63 {
			return 0, i, fmt.Errorf("varint overflow at %d", i)
		}
	}
}

func appendUvarint(dst []byte, v uint64) []byte {
	for v >= 0x80 {
		dst = append(dst, byte(v)|0x80)
		v >>= 7
	}
	return append(dst, byte(v))
}

func readFrames(data []byte) ([]frame, error) {
	if len(data) < demHeaderSize || string(data[:8]) != "PBDEMS2\x00" {
		return nil, fmt.Errorf("not a CS2 demo (PBDEMS2 header missing)")
	}
	var out []frame
	i := demHeaderSize
	for i < len(data) {
		off := i
		cmd, j, err := uvarint(data, i)
		if err != nil {
			return out, err
		}
		tick, j, err := uvarint(data, j)
		if err != nil {
			return out, err
		}
		size, j, err := uvarint(data, j)
		if err != nil {
			return out, err
		}
		end := j + int(size)
		if end > len(data) {
			return out, fmt.Errorf("frame at %d runs past the end of the file", off)
		}
		p := data[j:end]
		if cmd&demIsCompressed != 0 {
			if p, err = snappy.Decode(nil, p); err != nil {
				return out, fmt.Errorf("frame at %d: %v", off, err)
			}
		}
		out = append(out, frame{cmd &^ demIsCompressed, tick, off, data[off:end], p})
		i = end
		if cmd&^demIsCompressed == demStop {
			break
		}
	}
	return out, nil
}

func encodeFrame(cmd, tick uint64, payload []byte) []byte {
	c := snappy.Encode(nil, payload)
	out := appendUvarint(nil, cmd|demIsCompressed)
	out = appendUvarint(out, tick)
	out = appendUvarint(out, uint64(len(c)))
	return append(out, c...)
}

// patchHeader moves the two header offsets past the bytes inserted before them.
func patchHeader(header []byte, shift func(old int) int) {
	for _, at := range []int{8, 12} {
		old := int(binary.LittleEndian.Uint32(header[at:]))
		if old > 0 {
			binary.LittleEndian.PutUint32(header[at:], uint32(shift(old)))
		}
	}
}
