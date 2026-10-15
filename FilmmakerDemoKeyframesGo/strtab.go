package main

import (
	"fmt"

	"github.com/golang/snappy"
	"github.com/markus-wa/demoinfocs-golang/v5/pkg/demoinfocs/msg"
)

// String tables as the engine sees them, so a keyframe can carry each table that changed since
// the previous full packet (that is what Valve's full packets carry: FullPacketGoto restores each
// table from the newest full packet that holds it).

type stItem struct {
	key  string
	data []byte
}

type stringTable struct {
	name          string
	flags         int32
	userDataFixed bool
	userDataSize  int32
	varintBits    bool
	items         []stItem
	dirty         bool
}

type stringTables struct {
	byID   []*stringTable
	byName map[string]*stringTable
}

func newStringTables() *stringTables { return &stringTables{byName: map[string]*stringTable{}} }

func (s *stringTables) create(m *msg.CSVCMsg_CreateStringTable) error {
	t := &stringTable{name: m.GetName(), flags: m.GetFlags(), userDataFixed: m.GetUserDataFixedSize(),
		userDataSize: m.GetUserDataSize(), varintBits: m.GetUsingVarintBitcounts()}
	s.byID = append(s.byID, t)
	s.byName[t.name] = t
	data := m.GetStringData()
	if m.GetDataCompressed() {
		d, err := snappy.Decode(nil, data)
		if err != nil {
			return fmt.Errorf("string table %s: %v", t.name, err)
		}
		data = d
	}
	return t.apply(data, int(m.GetNumEntries()))
}

func (s *stringTables) update(m *msg.CSVCMsg_UpdateStringTable) error {
	id := int(m.GetTableId())
	if id >= len(s.byID) {
		return fmt.Errorf("update for unknown string table %d", id)
	}
	t := s.byID[id]
	t.dirty = true
	return t.apply(m.GetStringData(), int(m.GetNumChangedEntries()))
}

// load replaces tables from a CDemoStringTables snapshot (signon DEM_StringTables).
func (s *stringTables) load(m *msg.CDemoStringTables) {
	for _, tt := range m.GetTables() {
		t := s.byName[tt.GetTableName()]
		if t == nil {
			t = &stringTable{name: tt.GetTableName()}
			s.byName[t.name] = t
			s.byID = append(s.byID, t)
		}
		t.flags = tt.GetTableFlags()
		t.items = t.items[:0]
		for _, it := range tt.GetItems() {
			t.items = append(t.items, stItem{it.GetStr(), append([]byte(nil), it.GetData()...)})
		}
	}
}

// apply mirrors demoinfocs' parseStringTable (Valve's string table update format).
func (t *stringTable) apply(buf []byte, numUpdates int) (err error) {
	if len(buf) == 0 {
		return nil
	}
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("string table %s: %v", t.name, r)
		}
	}()
	r := &stReader{buf: buf}
	index := -1
	keys := make([]string, 0, 33)
	for i := 0; i < numUpdates; i++ {
		if r.ReadBit() {
			index++
		} else {
			index = int(r.ReadVarInt32()) + 1
		}
		key, hasKeyNow := "", false
		if r.ReadBit() {
			hasKeyNow = true
			if r.ReadBit() {
				pos, size := int(r.ReadInt(5)), int(r.ReadInt(5))
				if pos >= len(keys) {
					key = r.ReadString()
				} else if s := keys[pos]; size > len(s) {
					key = s + r.ReadString()
				} else {
					key = s[:size] + r.ReadString()
				}
			} else {
				key = r.ReadString()
			}
			keys = append(keys, key)
			if len(keys) > 32 {
				keys = keys[1:]
			}
		}
		var value []byte
		hasValue := r.ReadBit()
		if hasValue {
			bits, compressed := 0, false
			if t.userDataFixed {
				bits = int(t.userDataSize)
			} else {
				if t.flags&1 != 0 {
					compressed = r.ReadBit()
				}
				if t.varintBits {
					bits = int(r.ReadUBitInt()) * 8
				} else {
					bits = int(r.ReadInt(17)) * 8
				}
			}
			value = r.ReadBits(bits)
			if compressed {
				if value, err = snappy.Decode(nil, value); err != nil {
					return err
				}
			}
		}
		for len(t.items) <= index {
			t.items = append(t.items, stItem{})
		}
		if hasKeyNow {
			t.items[index].key = key
		}
		// The entry's user data is always replaced: an update that carries no value clears it
		// (a player leaving empties their userinfo; Valve's full packets agree).
		t.items[index].data = value
	}
	return nil
}

func (t *stringTable) snapshot() *msg.CDemoStringTablesTableT {
	name, flags := t.name, t.flags
	out := &msg.CDemoStringTablesTableT{TableName: &name, TableFlags: &flags}
	for _, it := range t.items {
		k := it.key
		out.Items = append(out.Items, &msg.CDemoStringTablesItemsT{Str: &k, Data: it.data})
	}
	return out
}

// stReader is the LSB-first bit reader the string table format needs; it panics past the end
// (apply recovers).
type stReader struct {
	buf []byte
	pos int
}

func (r *stReader) ReadBit() bool {
	b := r.buf[r.pos>>3]>>(r.pos&7)&1 != 0
	r.pos++
	return b
}

func (r *stReader) ReadInt(n int) uint {
	var v uint
	for i := 0; i < n; i++ {
		if r.ReadBit() {
			v |= 1 << i
		}
	}
	return v
}

func (r *stReader) readByte() byte { return byte(r.ReadInt(8)) }

func (r *stReader) ReadVarInt32() uint32 {
	var res uint32
	for count := uint(0); count < 5; count++ {
		b := uint32(r.readByte())
		res |= (b & 0x7f) << (7 * count)
		if b&0x80 == 0 {
			break
		}
	}
	return res
}

func (r *stReader) ReadUBitInt() uint {
	res := r.ReadInt(6)
	switch res & 48 {
	case 16:
		res = res&15 | r.ReadInt(4)<<4
	case 32:
		res = res&15 | r.ReadInt(8)<<4
	case 48:
		res = res&15 | r.ReadInt(28)<<4
	}
	return res
}

func (r *stReader) ReadString() string {
	var out []byte
	for i := 0; i < 4096; i++ {
		b := r.readByte()
		if b == 0 {
			break
		}
		out = append(out, b)
	}
	return string(out)
}

func (r *stReader) ReadBits(n int) []byte {
	out := make([]byte, (n+7)/8)
	for i := 0; i < n; i++ {
		if r.ReadBit() {
			out[i>>3] |= 1 << (i & 7)
		}
	}
	return out
}
