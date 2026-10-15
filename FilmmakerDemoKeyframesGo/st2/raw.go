package sendtablescs2

// SOURCE:MVM patch (not upstream demoinfocs): every decoded field also keeps its ENCODED bits, so
// the current state of every entity can be written back out as a full (non-delta) entity update
// without re-quantizing a single value. Contract: Docs/features/filmmaker_demo_scrubbing.md.
// The rest of this package is demoinfocs-golang v5.2.0 sendtablescs2 (MIT, LICENSE-demoinfocs),
// patched only where marked SOURCE:MVM.

import (
	"fmt"
	"sort"
	"strings"
	"sync"
)

// rawNode mirrors one field-path level of an entity with the bits that last encoded it.
type rawNode struct {
	bits []byte // LSB-first
	n    int
	set  bool
	kids []*rawNode
}

func (n *rawNode) child(i int) *rawNode {
	for len(n.kids) <= i {
		n.kids = append(n.kids, nil)
	}
	if n.kids[i] == nil {
		n.kids[i] = &rawNode{}
	}
	return n.kids[i]
}

func (e *Entity) rawAt(fp *fieldPath) *rawNode {
	n := &e.raw
	for i := 0; i <= fp.last; i++ {
		n = n.child(fp.path[i])
	}
	return n
}

func copyBits(buf []byte, start, end int) []byte {
	n := end - start
	out := make([]byte, (n+7)/8)
	for i := 0; i < n; i++ {
		p := start + i
		if buf[p>>3]>>(p&7)&1 != 0 {
			out[i>>3] |= 1 << (i & 7)
		}
	}
	return out
}

const (
	kindValue = iota
	kindPointer
	kindCollection
)

func (s *serializer) kindAt(fp *fieldPath, pos int) int {
	return s.fields[fp.path[pos]].kindAt(fp, pos+1)
}

// kindAt mirrors field.getDecoderAndCollection.
func (f *field) kindAt(fp *fieldPath, pos int) int {
	switch f.model {
	case fieldModelFixedTable:
		if fp.last == pos-1 {
			return kindPointer
		}
		return f.serializer.kindAt(fp, pos)
	case fieldModelVariableArray:
		if fp.last == pos {
			return kindValue
		}
		return kindCollection
	case fieldModelVariableTable:
		if fp.last >= pos+1 {
			return f.serializer.kindAt(fp, pos+1)
		}
		return kindCollection
	}
	return kindValue
}

// recordRaw is called by readFields after each value is decoded.
// TraceField (diagnostics) prints every write whose field name contains it.
var TraceField string
var TraceTick uint64

func (e *Entity) recordRaw(fp *fieldPath, r *reader, start, end int, val any, collection bool) {
	if TraceField != "" && !e.verifyClone {
		if name := e.class.name + "." + e.class.getNameForFieldPath(fp); strings.Contains(name, TraceField) {
			fmt.Printf("  trace tick %d ent %d %s %v = %v (%d bits)\n", TraceTick, e.index, name, fp, val, end-start)
		}
	}
	node := e.rawAt(fp)
	node.bits = copyBits(r.buf, start, end)
	node.n = end - start
	node.set = true
	if collection {
		if newLen := int(val.(uint64)); len(node.kids) > newLen {
			node.kids = node.kids[:newLen]
		}
		return
	}
	if b, ok := val.(bool); ok && !b && e.class.serializer.kindAt(fp, 0) == kindPointer {
		node.kids = nil // a null pointer owns no sub-fields
	}
}

// RawField is one field path of an entity with its encoded bits.
type RawField struct {
	Path []int
	Bits []byte
	N    int
}

// RawFields lists the entity's current fields in ascending field-path order.
func (e *Entity) RawFields() []RawField {
	var out []RawField
	var walk func(n *rawNode, path []int)
	walk = func(n *rawNode, path []int) {
		for i, k := range n.kids {
			if k == nil {
				continue
			}
			p := append(append(make([]int, 0, len(path)+1), path...), i)
			if k.set {
				out = append(out, RawField{p, k.bits, k.n})
			}
			walk(k, p)
		}
	}
	walk(&e.raw, nil)
	return out
}

// ActiveEntities returns the live entities sorted by index.
func (p *Parser) ActiveEntities() []*Entity {
	out := make([]*Entity, 0, len(p.entities))
	for _, e := range p.entities {
		if e != nil && e.active {
			out = append(out, e)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].index < out[j].index })
	return out
}

func (e *Entity) ClassID() int32      { return e.class.classId }
func (e *Entity) CreateExtra() uint32 { return e.createExtra }
func (e *Entity) Index() int32        { return e.index }
func (e *Entity) Serial() int32       { return e.serial }

// ---- bit writer ----

type BitWriter struct {
	Buf []byte
	N   int
}

func (w *BitWriter) bit(b uint32) {
	if w.N&7 == 0 {
		w.Buf = append(w.Buf, 0)
	}
	if b&1 != 0 {
		w.Buf[w.N>>3] |= 1 << (w.N & 7)
	}
	w.N++
}

func (w *BitWriter) WriteBits(v uint32, n int) {
	for i := 0; i < n; i++ {
		w.bit(v >> i)
	}
}

func (w *BitWriter) WriteRaw(b []byte, n int) {
	for i := 0; i < n; i++ {
		w.bit(uint32(b[i>>3] >> (i & 7)))
	}
}

func (w *BitWriter) WriteBool(b bool) {
	if b {
		w.bit(1)
	} else {
		w.bit(0)
	}
}

// WriteUBitVar is the inverse of reader.readUBitVar.
func (w *BitWriter) WriteUBitVar(v uint32) {
	switch {
	case v < 16:
		w.WriteBits(v, 6)
	case v < 1<<8:
		w.WriteBits(v&15|16, 6)
		w.WriteBits(v>>4, 4)
	case v < 1<<12:
		w.WriteBits(v&15|32, 6)
		w.WriteBits(v>>4, 8)
	default:
		w.WriteBits(v&15|48, 6)
		w.WriteBits(v>>4, 28)
	}
}

// WriteUBitVarFP is the inverse of reader.readUBitVarFP.
func (w *BitWriter) WriteUBitVarFP(v uint32) {
	switch {
	case v < 1<<2:
		w.bit(1)
		w.WriteBits(v, 2)
	case v < 1<<4:
		w.WriteBits(0b10, 2)
		w.WriteBits(v, 4)
	case v < 1<<10:
		w.WriteBits(0b100, 3)
		w.WriteBits(v, 10)
	case v < 1<<17:
		w.WriteBits(0b1000, 4)
		w.WriteBits(v, 17)
	default:
		w.WriteBits(0, 4)
		w.WriteBits(v, 31)
	}
}

func (w *BitWriter) WriteVarUint32(v uint32) {
	for {
		b := v & 0x7f
		v >>= 7
		if v != 0 {
			b |= 0x80
		}
		w.WriteBits(b, 8)
		if v == 0 {
			return
		}
	}
}

func (w *BitWriter) WriteVarInt32(v int32) {
	w.WriteVarUint32(uint32(v<<1) ^ uint32(v>>31))
}

// ---- field path encoder ----

var (
	opCodesOnce sync.Once
	opCodes     [][]uint8
	opIndex     map[string]int
)

func buildOpCodes() {
	opCodes = make([][]uint8, len(fieldPathTable))
	opIndex = map[string]int{}
	for i, op := range fieldPathTable {
		opIndex[op.name] = i
	}
	var walk func(node int16, code []uint8)
	walk = func(node int16, code []uint8) {
		n := fpHuffNodes[node]
		if n.left < 0 {
			opCodes[n.value] = append([]uint8(nil), code...)
			return
		}
		walk(n.left, append(code, 0))
		walk(n.right, append(code, 1))
	}
	walk(0, nil)
}

func (w *BitWriter) op(name string) {
	opCodesOnce.Do(buildOpCodes)
	i, ok := opIndex[name]
	if !ok {
		panic("unknown field path op " + name)
	}
	for _, b := range opCodes[i] {
		w.bit(uint32(b))
	}
}

// OpStats counts the ops the encoder emitted (diagnostics).
var OpStats = map[string]int{}

func (w *BitWriter) useOp(name string) {
	OpStats[name]++
	w.op(name)
}

// encodeFieldPaths writes one op per path (every op except Finish yields a path that is then
// given a value, so a transition may never take two ops), then FieldPathEncodeFinish.
func encodeFieldPaths(w *BitWriter, fields []RawField) {
	cur := []int{-1}
	for _, f := range fields {
		b := f.Path
		la, lb := len(cur)-1, len(b)-1
		switch {
		case la == lb && equalPrefix(cur, b, la) && b[la] > cur[la]:
			d := b[la] - cur[la]
			switch d {
			case 1:
				w.useOp("PlusOne")
			case 2:
				w.useOp("PlusTwo")
			case 3:
				w.useOp("PlusThree")
			case 4:
				w.useOp("PlusFour")
			default:
				w.useOp("PlusN")
				w.WriteUBitVarFP(uint32(d - 5))
			}
		case lb == la+1 && equalPrefix(cur, b, la+1) && b[lb] == 0:
			w.useOp("PushOneLeftDeltaZeroRightZero")
		case lb < la && lb == 0 && b[0] > cur[0]:
			d := b[0] - cur[0]
			if d == 1 {
				w.useOp("PopAllButOnePlusOne")
			} else {
				w.useOp("PopAllButOnePlusN")
				w.WriteUBitVarFP(uint32(d - 1))
			}
		case lb >= la:
			// Adjust every existing level by a signed delta, then push the new levels.
			w.useOp("PushNAndNonTopological")
			for i := 0; i <= la; i++ {
				if cur[i] != b[i] {
					w.WriteBool(true)
					w.WriteVarInt32(int32(b[i] - cur[i] - 1))
				} else {
					w.WriteBool(false)
				}
			}
			w.WriteUBitVar(uint32(lb - la))
			for j := la + 1; j <= lb; j++ {
				w.WriteUBitVarFP(uint32(b[j]))
			}
		default:
			// Pop to the target depth, then adjust every remaining level by a signed delta.
			w.useOp("PopNAndNonTopographical")
			w.WriteUBitVarFP(uint32(la - lb))
			for i := 0; i <= lb; i++ {
				if cur[i] != b[i] {
					w.WriteBool(true)
					w.WriteVarInt32(int32(b[i] - cur[i]))
				} else {
					w.WriteBool(false)
				}
			}
		}
		cur = append(cur[:0], b...)
	}
	w.useOp("FieldPathEncodeFinish")
}

func equalPrefix(a, b []int, n int) bool {
	for i := 0; i < n; i++ {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// EncodeEntity appends one CREATE record (index delta, cmd, class, serial, extra, fields) and
// returns the bit length of the field section (field paths + values) for serialized_entities.
func (p *Parser) EncodeEntity(w *BitWriter, prevIndex int32, e *Entity) (headerStart, fieldsStart, end int) {
	headerStart = w.N
	w.WriteUBitVar(uint32(e.index - prevIndex - 1))
	w.WriteBits(2, 2) // create: bit0 = 0, bit1 = 1
	w.WriteBits(uint32(e.class.classId), int(p.classIdSize))
	w.WriteBits(uint32(e.serial), 17)
	w.WriteVarUint32(e.createExtra)
	fieldsStart = w.N
	fields := e.RawFields()
	encodeFieldPaths(w, fields)
	for _, f := range fields {
		w.WriteRaw(f.Bits, f.N)
	}
	return headerStart, fieldsStart, w.N
}

// CloneForVerify returns an empty parser sharing this one's classes, serializers and baselines,
// to decode a synthesized full update and compare it with the live state.
func (p *Parser) CloneForVerify() *Parser {
	q := NewParser(nil)
	q.verifyClone = true
	q.serializers = p.serializers
	q.classIdSize = p.classIdSize
	q.classesById = p.classesById
	q.classesByName = p.classesByName
	for k, v := range p.classBaselines {
		q.classBaselines[k] = v
	}
	return q
}

// CompareEntities reports the first difference between two parsers' live entities.
func CompareEntities(a, b *Parser) error {
	ea, eb := a.ActiveEntities(), b.ActiveEntities()
	if len(ea) != len(eb) {
		return fmt.Errorf("entity count %d != %d", len(ea), len(eb))
	}
	for i := range ea {
		x, y := ea[i], eb[i]
		if x.index != y.index || x.class.classId != y.class.classId || x.serial != y.serial || x.createExtra != y.createExtra {
			return fmt.Errorf("entity #%d header differs: idx %d/%d class %d/%d serial %d/%d extra %d/%d",
				i, x.index, y.index, x.class.classId, y.class.classId, x.serial, y.serial, x.createExtra, y.createExtra)
		}
		fx, fy := x.RawFields(), y.RawFields()
		if len(fx) != len(fy) {
			ka, kb := map[string]RawField{}, map[string]RawField{}
			for _, f := range fx {
				ka[fmt.Sprint(f.Path)] = f
			}
			for _, f := range fy {
				kb[fmt.Sprint(f.Path)] = f
			}
			var only []string
			for k, f := range ka {
				if _, ok := kb[k]; !ok {
					only = append(only, fmt.Sprintf("tracked-only %s %s (%d bits %x)", k, x.class.getNameForFieldPath(pathOf(f.Path)), f.N, f.Bits))
				}
			}
			for k, f := range kb {
				if _, ok := ka[k]; !ok {
					only = append(only, fmt.Sprintf("other-only %s %s (%d bits %x)", k, y.class.getNameForFieldPath(pathOf(f.Path)), f.N, f.Bits))
				}
			}
			return fmt.Errorf("entity %d (%s) field count %d != %d: %s", x.index, x.class.name, len(fx), len(fy), strings.Join(only, "; "))
		}
		for j := range fx {
			if fmt.Sprint(fx[j].Path) != fmt.Sprint(fy[j].Path) || fx[j].N != fy[j].N || string(fx[j].Bits) != string(fy[j].Bits) {
				return fmt.Errorf("entity %d (%s) field %v differs (%d vs %d bits)", x.index, x.class.name, fx[j].Path, fx[j].N, fy[j].N)
			}
		}
	}
	return nil
}

// Spans records, per update of the last OnPacketEntities, where its record started, where its
// fields started and where it ended (bit positions) -- to learn what serialized_entities counts.
type Span struct{ Start, Fields, End int }

var LastSpans []Span
var RecordSpans bool

func pathOf(p []int) *fieldPath {
	fp := newFieldPath()
	copy(fp.path, p)
	fp.last = len(p) - 1
	return fp
}

// DiffSummary tallies every difference between two parsers' entities by field name.
func DiffSummary(a, b *Parser) map[string]int {
	out := map[string]int{}
	eb := map[int32]*Entity{}
	for _, e := range b.ActiveEntities() {
		eb[e.index] = e
	}
	for _, x := range a.ActiveEntities() {
		y := eb[x.index]
		if y == nil {
			out["entity missing in other: "+x.class.name]++
			continue
		}
		ka, kb := map[string]RawField{}, map[string]RawField{}
		for _, f := range x.RawFields() {
			ka[fmt.Sprint(f.Path)] = f
		}
		for _, f := range y.RawFields() {
			kb[fmt.Sprint(f.Path)] = f
		}
		for k, f := range ka {
			g, ok := kb[k]
			name := x.class.name + "." + x.class.getNameForFieldPath(pathOf(f.Path))
			switch {
			case !ok:
				out["tracked-only "+name]++
			case g.N != f.N || string(g.Bits) != string(f.Bits):
				out["bits-differ "+name]++
				if TraceField != "" {
					fmt.Printf("  differ ent %d %s: tracked %d bits %x, other %d bits %x\n", x.index, name, f.N, f.Bits, g.N, g.Bits)
				}
			}
		}
		for k, f := range kb {
			if _, ok := ka[k]; !ok {
				out["other-only "+y.class.name+"."+y.class.getNameForFieldPath(pathOf(f.Path))]++
			}
		}
	}
	return out
}
