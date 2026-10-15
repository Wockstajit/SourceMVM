package main

// Source 2 netmessage bitstream primitives.
//
// Inside a CDemoPacket the net messages are packed into an UNALIGNED bit stream, LSB-first
// within each byte:
//
//	msgType = ReadUBitVar()      // 6 bits, with 3 possible extensions
//	size    = ReadVarUInt32()    // 8-bit groups, high bit = continue
//	payload = ReadBytes(size)    // NOT byte-aligned
//
// We only ever need to FIND message boundaries, never to re-encode one: kept messages are
// copied out as verbatim bit spans (see CopyBits). That is deliberate -- re-encoding a ubitvar
// or varint into a different-but-equivalent form would silently rewrite bytes we were asked to
// leave alone, and a mistake there corrupts a multi-hundred-megabyte demo in a way that is very
// hard to notice. Copying spans means the only bits that change are the ones we drop.

type bitReader struct {
	buf []byte
	pos int // absolute bit position
}

func newBitReader(b []byte) *bitReader { return &bitReader{buf: b} }

func (r *bitReader) bitsLeft() int { return len(r.buf)*8 - r.pos }

// readBits reads n bits (n <= 32) LSB-first.
func (r *bitReader) readBits(n int) (uint32, bool) {
	if n < 0 || n > 32 || r.bitsLeft() < n {
		return 0, false
	}
	var out uint32
	for i := 0; i < n; i++ {
		byteIdx := (r.pos + i) >> 3
		bitIdx := uint((r.pos + i) & 7)
		bit := (r.buf[byteIdx] >> bitIdx) & 1
		out |= uint32(bit) << uint(i)
	}
	r.pos += n
	return out, true
}

// readUBitVar mirrors Source 2's ReadUBitVar / gobitread's ReadUBitInt.
func (r *bitReader) readUBitVar() (uint32, bool) {
	v, ok := r.readBits(6)
	if !ok {
		return 0, false
	}
	switch v & 0x30 {
	case 0x10:
		ext, ok := r.readBits(4)
		if !ok {
			return 0, false
		}
		v = (v & 0x0F) | (ext << 4)
	case 0x20:
		ext, ok := r.readBits(8)
		if !ok {
			return 0, false
		}
		v = (v & 0x0F) | (ext << 4)
	case 0x30:
		ext, ok := r.readBits(28)
		if !ok {
			return 0, false
		}
		v = (v & 0x0F) | (ext << 4)
	}
	return v, true
}

// readVarUInt32 reads a protobuf-style varint out of the BIT stream (8 bits per group).
func (r *bitReader) readVarUInt32() (uint32, bool) {
	var res uint32
	for i := uint(0); i < 5; i++ {
		b, ok := r.readBits(8)
		if !ok {
			return 0, false
		}
		res |= (b & 0x7F) << (7 * i)
		if b&0x80 == 0 {
			return res, true
		}
	}
	return res, true
}

// skipBits advances without decoding.
func (r *bitReader) skipBits(n int) bool {
	if n < 0 || r.bitsLeft() < n {
		return false
	}
	r.pos += n
	return true
}

// readBytesAt copies n bytes starting at an arbitrary bit offset (used for schema checks).
func readBytesAt(buf []byte, bitPos, n int) ([]byte, bool) {
	if n < 0 || bitPos < 0 || len(buf)*8-bitPos < n*8 {
		return nil, false
	}
	out := make([]byte, n)
	for i := 0; i < n; i++ {
		var v byte
		for b := 0; b < 8; b++ {
			p := bitPos + i*8 + b
			bit := (buf[p>>3] >> uint(p&7)) & 1
			v |= bit << uint(b)
		}
		out[i] = v
	}
	return out, true
}

type bitWriter struct {
	buf []byte
	pos int // absolute bit position
}

func newBitWriter() *bitWriter { return &bitWriter{} }

func (w *bitWriter) writeBit(bit byte) {
	if w.pos>>3 >= len(w.buf) {
		w.buf = append(w.buf, 0)
	}
	if bit != 0 {
		w.buf[w.pos>>3] |= 1 << uint(w.pos&7)
	}
	w.pos++
}

// copyBits copies n bits out of src starting at srcBitPos. This is the only way messages are
// emitted, so kept messages are bit-for-bit identical to the input.
func (w *bitWriter) copyBits(src []byte, srcBitPos, n int) bool {
	if srcBitPos < 0 || n < 0 || len(src)*8-srcBitPos < n {
		return false
	}
	for i := 0; i < n; i++ {
		p := srcBitPos + i
		w.writeBit((src[p>>3] >> uint(p&7)) & 1)
	}
	return true
}

// bytes returns the written stream, zero-padded up to the next byte boundary (which is what the
// engine's own writer does -- trailing pad bits are never decoded as a message because a
// truncated ubitvar simply ends the read loop).
func (w *bitWriter) bytes() []byte {
	if w.buf == nil {
		return []byte{}
	}
	return w.buf
}
