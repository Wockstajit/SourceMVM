#pragma once

// ALTERNATIVE ENVIRONMENT, the demo side: writes a copy of a CS2 demo that plays on another map.
// Docs/features/alternative-environment.md owns the contract.
//
// A demo names its world in five places, all read and rewritten here:
//   * CDemoFileHeader.map_name / .addons (the first frame);
//   * CSVCMsg_ServerInfo.map_name / .addon_name (net message 40, in the signon), and its
//     game_session_config's s1_mapname;
//   * CSVCMsg_ClearAllStringTables.mapname (net message 51, in the signon): the level name the
//     client reports (GetLevelNameShort). Without it the new world loads under the old name;
//   * CNETMsg_SpawnGroup_Load (net message 8) for the main world: worldname is the map, its
//     parent handle is 0; world_offset_pos / world_offset_angle place it. A GOTV demo sends these
//     in its first packets; a client-recorded demo keeps them in the DEM_SpawnGroups index that
//     CDemoPlayer::LoadSpawnGroups preloads, so both are rewritten;
//   * the 3D skybox spawn group (worldgroupname "skyboxWorldGroup*"), renamed to the replacement
//     map's skybox and given the same offset so it stays put around the moved sky_camera.
// The players, weapons, grenades and networked props stay in the demo's coordinates; the
// replacement world is placed with the inverse of the alignment (AltEnvironmentMath.h
// WorldOffsetFor). Every other frame and every other message is copied byte for byte, rewritten
// frames are stored uncompressed, and the header's FileInfo / SpawnGroups offsets are remapped.
//
// Engine-free; Automation/tests/demo-world-swap-tests.cpp.

#include "../AltEnvironment/AltEnvironmentMath.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>

namespace Filmmaker {

struct DemoWorldSwap {
	std::string sourceMap; // the demo's own map ("de_inferno"): the world being replaced
	std::string map;       // the replacement map ("de_inferno_night")
	std::string addon;     // its workshop id; empty for a map the game ships
	std::string skybox;    // the replacement's 3D skybox world ("maps/prefabs/..."); empty keeps the demo's
	AltEnvironment::WorldOffset offset;
};

struct DemoWorldSwapResult {
	bool ok = false;
	std::string error;
	int headers = 0, serverInfos = 0, levelNames = 0, worlds = 0, skyboxes = 0; // messages rewritten
	uint64_t frames = 0, bytesIn = 0, bytesOut = 0;
	std::string headerMap; // the map the demo's own header named
};

// `progress` (optional) gets the share of the input read so far, 0..1, every few megabytes.
DemoWorldSwapResult SwapDemoWorld(std::istream& in, std::ostream& out, const DemoWorldSwap& spec,
	const std::atomic<bool>* cancel = nullptr, const std::function<void(double)>& progress = {});

// Writes `outPath` (via `outPath`.part, renamed when complete). Never touches `inPath`.
DemoWorldSwapResult SwapDemoWorldFile(const std::filesystem::path& inPath, const std::filesystem::path& outPath,
	const DemoWorldSwap& spec, const std::atomic<bool>* cancel = nullptr, const std::function<void(double)>& progress = {});

} // namespace Filmmaker
