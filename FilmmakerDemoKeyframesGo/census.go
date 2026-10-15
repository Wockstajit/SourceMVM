package main

import (
	"fmt"
	"sort"

	"github.com/markus-wa/demoinfocs-golang/v5/pkg/demoinfocs/msg"
	"google.golang.org/protobuf/proto"
)

// census prints how often each net message type appears per demo command (diagnostics).
func census(frames []frame) {
	counts := map[string]int{}
	for _, f := range frames {
		var data []byte
		switch f.cmd {
		case demPacket, demSignonPacket:
			var m msg.CDemoPacket
			if proto.Unmarshal(f.payload, &m) != nil {
				continue
			}
			data = m.GetData()
		case demFullPacket:
			var m msg.CDemoFullPacket
			if proto.Unmarshal(f.payload, &m) != nil {
				continue
			}
			data = m.GetPacket().GetData()
		default:
			counts[fmt.Sprintf("frame cmd %d", f.cmd)]++
			continue
		}
		ms, _ := splitMessages(data)
		for _, x := range ms {
			name := fmt.Sprint(x.t)
			if n, ok := msg.SVC_Messages_name[int32(x.t)]; ok {
				name = n
			} else if n, ok := msg.NET_Messages_name[int32(x.t)]; ok {
				name = n
			}
			if x.t == svcUpdateTable {
				var ut msg.CSVCMsg_UpdateStringTable
				if proto.Unmarshal(x.body, &ut) == nil {
					name = fmt.Sprintf("%s(table %d)", name, ut.GetTableId())
				}
			}
			counts[fmt.Sprintf("cmd %d: %s", f.cmd, name)]++
		}
	}
	keys := make([]string, 0, len(counts))
	for k := range counts {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		fmt.Printf("%8d  %s\n", counts[k], k)
	}
}
