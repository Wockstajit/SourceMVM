# Map data extraction

Runtime map lighting, sun shadows, smoke shelter, surface props and the death-weapon ground drop
all read one per-map cache. The cache holds the map's authored lights and environment entities,
its opaque collision, and its placed render geometry as shadow casters. The hook builds that cache
itself, from the installed game files, the first time a map loads. Nothing else is needed: no VRF
CLI, no .NET, no download and no manual preparation step. It works the same for official and
Workshop maps.

Code: [`AfxHookSource2/Filmmaker/MapData/`](../../AfxHookSource2/Filmmaker/MapData) (engine-free,
also built into `MapDataTool.exe`), started by
[`EnvironmentOcclusion.cpp`](../../AfxHookSource2/Filmmaker/Environment/EnvironmentOcclusion.cpp).
The format readers are ported from ValveResourceFormat 20.0. Its MIT license ships beside them in
[`LICENSE-ValveResourceFormat.txt`](../../AfxHookSource2/Filmmaker/MapData/LICENSE-ValveResourceFormat.txt).

## What it reads

| Part | Source in the map package | Cache payload |
| --- | --- | --- |
| Authored lights, probes, cubemaps, sky, fog, exposure and other environment entities | every `.vents_c` entity lump | `<sha256>.lights.json` |
| Opaque collision | `maps/<map>/world_physics` (hulls, meshes, spheres, capsules) | `<sha256>.mvmocclusion` (`MVMOCC01`) |
| Placed props, world fragments, foliage, cables, light-blocker brushes | every `.vwnod_c` world node, through its models, meshes, materials and textures | `<sha256>.mvmcasters` (`MVMCAST6`) |
| 3D skybox casters and `sky_camera` scale/origin | the package named by `skybox_reference` | a second `.mvmcasters` |

By name, the map package is the official `game/csgo/maps/<map>.vpk`, then the newest installed
Workshop item that holds `maps/<map>.vpk`. A caller can ask for one Workshop item instead
(`ExtractOptions::addon`, `OpenMapSources(..., addon)`), because two items, or an item and a loose
package, can hold a map of the same name. The alternative environment does this for the item it
mounts. There is still one cache per map name: a request for an item treats a cache built from
another source as stale (`CacheCurrent(..., addon)`), and a request by name takes whichever source
is cached.

Models, materials and textures resolve through the map package, its Workshop addon, the
selected map's `game/csgo_community_addons/<map>/<map>_dir.vpk` asset mount, then every
`Game` search path in `game/csgo/gameinfo.gi`. Community directory packages and numbered
archives participate in cache freshness. Referenced skybox packages also resolve through
these mounts. The decoder reads only what these payloads need:
VPK directories (including packages nested inside another VPK), compiled-resource blocks, binary
KeyValues3 (legacy and v1-5, LZ4/ZSTD), entity lumps, physics shapes, mesh vertex/index buffers
(meshoptimizer, ZSTD), material parameters and the top mip of the colour textures that cutouts,
albedo and stained glass use. The decoders are vendored under [`Deps/release/`](../../Deps/README.md).

Extraction and runtime loading share one visual payload budget: 1 GiB, up to
16 million rigid vertices and 64 million rigid indices. Opaque material batches merge
into that rigid mesh and use its limits. Alpha-tested, animated and stained-glass batches
keep their smaller per-batch limits. This keeps large community-map walls instead of dropping
an entire material batch. Missing caster materials are reported in extraction warnings.

### Caster rules

Casters follow these rules:

- casters are materials of the CS2 world, model, foliage and cable shaders, plus
  `toolsblocklight` brushes. Overlays, translucent, additive and `F_DO_NOT_CAST_SHADOWS`
  materials never cast;
- `F_ALPHA_TEST` materials keep their alpha mask and cutoff (an all-opaque mask is stored as
  plain geometry); `csgo_foliage` sway/flutter parameters drive animated leaves;
- materials named stained glass also transmit their colour image as sunlight;
- each batch carries a linear albedo (texture average weighted by alpha, times the material and
  instance tint) for bounce lighting;
- collision drops `clip`, `sky` and `window` groups and glass surface properties.

A problem with one asset (a missing texture, an unsupported vertex format, a non-triangle draw
call) leaves that asset out or keeps it static, and records a warning. It never fails the map.
A map fails only when its package or world collision is missing, its data is malformed, or the
payloads exceed the runtime's limits.

## Cache

`%APPDATA%\HLAE\source_mvm\environment\occlusion\<map>.json` (manifest format 4) names the
digest-named payloads and lists every game file that was read, with its size and write time.
The cache is current while every one of those files is unchanged and the manifest's
`extractor` version matches the build (`kExtractorVersion` in
[`MapExtract.h`](../../AfxHookSource2/Filmmaker/MapData/MapExtract.h); bump it whenever output
changes). Otherwise the next load extracts again. Payloads are written before the manifest and
verified by SHA-256 on load. Manifests of other formats and unreferenced payloads are deleted
after each extraction.

The binary payloads (`.mvmocclusion`, `.mvmcasters`) are stored losslessly compressed
(manifest format 4, [`MapCacheCodec.h`](../../AfxHookSource2/Filmmaker/MapData/MapCacheCodec.h)).
They use 4 MB blocks of Windows XPRESS-Huffman, decoded block by block while loading. A payload's
digest is the SHA-256 of the file as stored. Inferno's stored payloads total 140 MB, and
decoding them adds about 0.9 s to a map load. Python tools read them through
[`Automation/lib/mvm_cache.py`](../../Automation/lib/mvm_cache.py). `.lights.json` stays plain text.

Extraction runs on a below-normal-priority worker and is cancelled when the map changes. The
Environment panel shows `Preparing map data: ...` progress. Measured times (Release, NVMe):
Inferno 16 s, Dust II 8 s, Ancient 11 s, Nuke 9 s. Most maps take 3 to 20 s.

## MapDataTool

`build/x64-source2-release/AfxHookSource2/Release/MapDataTool.exe` runs the same code outside
the game:

```batch
MapDataTool extract de_inferno
MapDataTool extract de_dogtown --out Automation\runs\map-data
MapDataTool entities de_inferno > lights.json
MapDataTool lightmap de_inferno --out de_inferno.mvmlightmap
```

`extract` writes to the AppData cache unless `--out` is given. `lightmap` writes the
`MVMLMAP2` atlas layout for `mirv_relight lightmap <file>`. `--cs2 <root>` overrides the Steam
install lookup.

## Verification

- Native unit checks: `ctest --test-dir build/x64-source2-release -C Release -R MapDataTests`.
- Whole-map parity: extract into a folder and compare with a reference extraction of the same game
  files using fixed tolerances (counts, area by normal direction, bounds, albedo, cutout masks,
  stained glass, every light and environment property):

  ```batch
  py -3 Automation/verify/compare-map-data.py <reference-dir> <candidate-dir> de_inferno de_dust2
  ```

  Against a VRF-pipeline reference extraction, Inferno, Dust II, Ancient and Nuke pass all 170
  checks, with identical triangle, batch, light and entity counts.
- Live: load a map with an empty cache and check the `environment.occlusion` lines in the
  `mvm_debug` log (`extracted map=... warnings=0`, then `Map data ready`).

The collision cache also supports [interior camera validation](interior-lighting-testing.md) before
live lighting captures. Collision bounds alone do not establish that a camera is indoors.
