package main

import "errors"

// Minimal protobuf wire helpers.
//
// We never need a generated schema here: the repair only has to find one length-delimited
// field inside two wrapper messages (CDemoPacket.data = field 3, CDemoFullPacket.packet =
// field 2) and put a modified copy back, leaving every other field byte-identical. Parsing
// generically and re-emitting fields in their original order does exactly that, and keeps this
// tool independent of any .proto/codegen.

const (
	wireVarint = 0
	wireFixed64 = 1
	wireBytes  = 2
	wireFixed32 = 5
)

var errTruncated = errors.New("truncated protobuf")

type pbField struct {
	num  int
	wire int
	raw  []byte // the full field bytes INCLUDING tag, as they appeared
	val  []byte // for wireBytes: just the payload
}

func readVarint(b []byte, i int) (uint64, int, error) {
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

func appendVarint(dst []byte, v uint64) []byte {
	for v >= 0x80 {
		dst = append(dst, byte(v)|0x80)
		v >>= 7
	}
	return append(dst, byte(v))
}

// parseFields splits a message into its top-level fields, preserving raw bytes.
func parseFields(b []byte) ([]pbField, error) {
	var out []pbField
	i := 0
	for i < len(b) {
		start := i
		key, n, err := readVarint(b, i)
		if err != nil {
			return nil, err
		}
		i = n
		f := pbField{num: int(key >> 3), wire: int(key & 7)}
		switch f.wire {
		case wireVarint:
			_, n, err := readVarint(b, i)
			if err != nil {
				return nil, err
			}
			i = n
		case wireFixed64:
			if i+8 > len(b) {
				return nil, errTruncated
			}
			i += 8
		case wireBytes:
			ln, n, err := readVarint(b, i)
			if err != nil {
				return nil, err
			}
			i = n
			if uint64(len(b)-i) < ln {
				return nil, errTruncated
			}
			f.val = b[i : i+int(ln)]
			i += int(ln)
		case wireFixed32:
			if i+4 > len(b) {
				return nil, errTruncated
			}
			i += 4
		default:
			return nil, errors.New("unsupported wire type")
		}
		f.raw = b[start:i]
		out = append(out, f)
	}
	return out, nil
}

// replaceBytesField re-serialises `fields`, swapping field `num`'s payload for newVal. Every
// other field is copied through byte-for-byte in its original order.
func replaceBytesField(fields []pbField, num int, newVal []byte) []byte {
	var out []byte
	for _, f := range fields {
		if f.num == num && f.wire == wireBytes {
			out = appendVarint(out, uint64(num)<<3|wireBytes)
			out = appendVarint(out, uint64(len(newVal)))
			out = append(out, newVal...)
			continue
		}
		out = append(out, f.raw...)
	}
	return out
}

// isWellFormed reports whether b parses cleanly as protobuf and consumes exactly len(b).
// Used as the fail-closed guard before dropping a message.
func isWellFormed(b []byte) bool {
	_, err := parseFields(b)
	return err == nil
}
