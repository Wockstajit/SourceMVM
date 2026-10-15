#pragma once

// Reads map name + playback duration from a CS2 (.dem) demo file header.
// Validated against real CS2 demos:
//   - 8 byte magic "PBDEMS2\0"
//   - int32 at offset 8 = byte offset of the DEM_FileInfo packet
//   - first packet (cmd DEM_FileHeader = 1) payload is CDemoFileHeader, map_name = field 5
//   - DEM_FileInfo (cmd = 2) payload is CDemoFileInfo, playback_time (seconds) = field 1 (float)
//   - CDemoFileHeader also carries network_protocol = field 2 and build_num = field 13, which
//     is what lets a demo be CLASSIFIED before anything parses it end to end (DemoRepair).

#include <string>

namespace Filmmaker {

struct DemoHeaderInfo {
	bool ok = false;
	std::string map;
	int durationSeconds = 0;
	int playbackTicks = 0; // CDemoFileInfo playback_ticks (0 when the packet is missing/unparsable)
	// Both 0 when the field is absent. Cheap: they come from the same first packet as the map,
	// so reading them costs nothing extra and avoids a multi-second full parse to classify.
	int networkProtocol = 0; // CDemoFileHeader network_protocol (field 2)
	int buildNum = 0;        // CDemoFileHeader build_num (field 13)
};

DemoHeaderInfo ReadDemoHeader(const std::wstring& path);

} // namespace Filmmaker
