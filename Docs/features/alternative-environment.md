# Alternative environment

A CS2 demo can play on a different map: a night or rain remake from the Workshop, another map that
ships with CS2, or a neutral room. Every player, weapon, grenade and effect from the demo plays on
top of it. Requested in [issue #88](https://github.com/Wockstajit/SourceMVM/issues/88).

Open it from **CONFIG > ENVIRONMENT > World > MAP**, or drive it with `mirv_filmmaker altenv`.

## How it works

CS2 loads whatever world a demo tells it to load. A demo names its world in five places:

| Where | Field | Example |
| --- | --- | --- |
| `CDemoFileHeader`, the first frame | `map_name` (5), `addons` (10) | `de_inferno`, empty |
| `CSVCMsg_ServerInfo` (net message 40), in the signon | `map_name` (15), `addon_name` (18), and `game_session_config` (19) field `s1_mapname` (11) | `de_inferno`, empty |
| `CSVCMsg_ClearAllStringTables` (net message 51), in the signon | `mapname` (1) | `de_inferno` |
| `CNETMsg_SpawnGroup_Load` (net message 8) for the main world | `worldname` (1), parent handle 0 | `de_inferno` |
| The same message for the 3D skybox | `worldname`, world group `skyboxWorldGroup0` | `maps/prefabs/de_inferno/inferno_skybox` |

The spawn groups choose which world loads. Net message 51 sets the client's level name, which
`GetLevelNameShort` and the hook's own map lookups read, so a copy that left it alone would load
the new world under the old name.

A GOTV demo sends the spawn groups in its first packet. A demo recorded with `record` keeps them
in the `DEM_SpawnGroups` index at the end of the file, which the demo player preloads. No later
frame repeats any of them, full packets included.

So the hook writes a copy of the demo that names the replacement map in all five places and sets
its Workshop item as the addon, which makes the engine mount it the way it does for a demo
recorded on a Workshop map. The skybox spawn group gets the replacement map's own 3D skybox, read
from its `skybox_reference` entity. Every other frame and message is copied byte for byte.
Rewritten frames are stored uncompressed, and the header's FileInfo and SpawnGroups offsets are
moved to match.

Then the demo reopens from that copy at the same tick, paused or playing as before. The switch
pauses the reopened demo before it seeks, because a seek sent while the demo plays lands under a
pause that the engine lifts about two seconds later. After the seek it puts the demo back to
playing if it was, and checks the pause state for 64 frames. The replacement map loads during that
level load, so its assets are all in memory before playback continues and nothing stutters while
the demo plays.

A demo started from the console (`playdemo`) or by the automation launchers opens on its own map
first. With its setting on, it moves to the saved map as soon as the demo has been checked. Once no
demo has played for two seconds (a map was loaded, or the main menu is up), opening the same demo
again counts as a new open and moves to the saved map again. Before, a second `playdemo` of the same
demo stayed on its own map. Seeking, pausing and scrubbing are the
engine's own. When the scrub keyframe copy is ready, the environment copy is written from it, so
backward scrubbing stays fast.

The engine cannot change the world under a demo that is already playing. Changing the map or the
alignment always reopens the demo from a new copy. Writing a copy of a 450 MB demo takes about a
second.

### What moves

The demo's coordinates do not change. Players, weapons, grenades, networked props, the hook's
camera paths, follow cameras and markers all stay where the demo recorded them. Only the
replacement world moves. `CNETMsg_SpawnGroup_Load` can place a world at an offset and yaw (CS2
already uses this for its team-intro prefabs), and the copy places the replacement world with the
inverse of the alignment. Its static geometry, baked lighting and client-side entities move with
it. Its server-side entities are not in the demo at all (see Limits).

## Alignment

The alignment says where the demo lands on the replacement map: a demo point `p` is drawn at
`R(yaw) * p + (x, y, z)` in the replacement map's own coordinates. Yaw turns counter-clockwise
seen from above. A remake of the same map in the same place needs no offset.

The engine moves the world instead, so the copy uses the inverse:
`world_offset_angle.y = -yaw` and `world_offset_pos = -R(-yaw) * (x, y, z)`
([`AltEnvironmentMath.h`](../../AfxHookSource2/Filmmaker/AltEnvironment/AltEnvironmentMath.h)
`WorldOffsetFor`). The 3D skybox gets the same transform, because its `sky_camera` lives in the
skybox's own package. Rotation then stays correct. A translation leaves the distant skyline off by
the same distance, which is hard to see for small offsets.

### The map view

**CHOOSE MAP** or **SHOW MAP VIEW** opens the map view over the preview area. It shows while the
MAP card is selected; another card or tab gets the preview back, and closing CONFIG closes it.

While it is open, the view owns the mouse over the preview. It publishes `mapviewopen` on CONFIG's
root, and C++ treats that like an open popup (`ConfigHud_PopupOpen`): the wheel zooms the view
instead of cycling CS2's camera, a right press never becomes the ENVIRONMENT tab's look-around, and
`MovieMode::OnMouseButton` swallows the right button there so CS2's spectator does not step to the
previous player. The scene-object picker and the light and mesh arrows ignore the preview under it.

- **Left**: the maps the demo can play on. **WORKSHOP** lists every map inside an installed Workshop
  item, with its published title. **INSTALLED** lists the maps in `csgo/maps`, without menu, preview
  and character scenes, and without a map that a Workshop item also holds: a map package copied
  into `csgo/maps` on its own has none of the materials and models its item ships beside it (El
  Dorado's copy holds only `maps/de_eldorado/`, and every material drew as missing), so the copy
  always names the item, and a demo saved with such a map gets the item named when it opens. Two
  items can hold a map of the same name (Nightly Cache and Rainy Cache are both `de_cache_n_r`), so
  a pick carries its item and that item is the one mounted. A Workshop item whose map package is smaller than 1 MB is flagged as a
  placeholder. Some "night" items are only a box that tells you to join a server.
- **Right**: both maps seen from above. The replacement map is grey, lighter where it is higher.
  Over it, the demo's own map: its walls as light lines, its floor and the paths its players walked
  through the whole demo, and dots for where the players are now (orange T, blue CT). Before a
  replacement map is chosen, the walls and paths are orange.

The floor and paths are coloured by where they land on the replacement map under the alignment
being edited, so it shows which walked areas the new map can carry:

| Colour | Lands | Test (each of the cell's up to four floor heights) |
| --- | --- | --- |
| Green | On the map | a floor from 48 units under the point to 16 over it |
| Yellow | Floating | only floors further down: players would walk in the air |
| Purple | Under the map | only floors above: inside or beneath the geometry |
| Red | Outside the map | no floor in that place: the void |

A row of chips over the view gives the walked paths' shares of each, and "walked paths on the map"
in the card is the green share. The hook redraws the coloured layer whenever the alignment changes
while the view is open: one job at a time on a background thread (`LandingPicture` in the math
header, about 30 ms), at most every 50 ms. Between redraws the view moves the last drawing. Setting
a new image on a Panorama panel blanks it until the download lands, which made the layer flicker on
every redraw, so the view keeps two image panels: the next drawing loads into the hidden one and
takes over on `ImageLoaded`. One load runs at a time, and drawings published meanwhile are skipped
for the newest. The fit readout is scored on a background thread too, at most ten times a second.

Dragging the demo layer moves the demo. Right-dragging turns it about its middle (Shift snaps
to 15 degrees). The wheel zooms about the cursor, and Ctrl-drag pans a zoomed view. The card holds
the same numbers as sliders (**Move east**, **Move north**, **Move up**, **Turn**), **TURN -90 /
+90**, the **Demo map props** switch, the fit readout and **PLAY ON**. The card's header **Reset**
puts the alignment back to no offset on the map it plays on; **BACK TO ...** is the way back to the
demo's own map. The tab's **RESET ENVIRONMENT** runs the same reset.

The sliders and drags change the alignment that **PLAY ON** writes; the world in the game moves
when the demo reloads from the new copy. The engine places a loaded world once, at load (the
spawn-group offset above). Every world scene object does keep that placement as a matrix at `+0x30`
(and aggregates a second copy at `+0x118`), but rewriting those matrices for every world object being
drawn was measured not to move the world: the renderer draws it from data built at load. Moving a
loaded world live would need those load-time buffers, most likely the aggregates' per-fragment GPU
transforms. `altenv sceneprobe` (Diagnosis) is the tool for that research.

Opening CONFIG on the ENVIRONMENT tab starts reading both maps and the players' paths in the
background, so the view and AUTO ALIGN usually have them by the time the MAP card is opened.

The pictures are top-down floor rasters of each map's collision: the highest upward-facing surface
per cell, 8 units per pixel (coarser for big maps). Each cell also keeps up to four floor heights
(surfaces within 16 units count as one), so a street under an arch or a balcony still has its
floor when the fit is scored. The hook draws them as PNGs and serves them to Panorama on
`127.0.0.1` ([`LocalImageServer.h`](../../AfxHookSource2/Filmmaker/Platform/LocalImageServer.h)),
because Panorama shows http images but not loose files. The collision comes from the
[map data cache](../effects/map-data-extraction.md). A map that is not loaded is extracted for the
view first, one extraction at a time with the live loader.

### Fit and AUTO ALIGN

The fit readout is two shares:

- **Floor match**: how much of the demo map's top floor lands on one of the replacement map's
  floors at the same height (within 24 units).
- **Walked paths on the map**: how many of the players' recorded feet land on a floor that could
  carry them (48 units under their feet to 16 over), on any of the cell's floors. This is the map
  view's green share.

**AUTO ALIGN** searches offset and yaw on 16-unit rasters of both maps:

1. If the maps' own coordinates already fit (score 0.85 or better), it keeps them exactly: no
   offset, or only the height step between the two floors when that fits better. This is the night
   and rain remake case, and it keeps the hook's map-data effects on.
2. Otherwise every yaw in 3-degree steps is tried on 64-unit cells. For each yaw, FFT
   cross-correlation scores every shift at once: the turned demo floor against the replacement floor,
   +1 on floor and -1 off it, so landing on empty space costs as much as landing on floor gains.
3. The best few places are refined at 32-unit cells and half-degree steps, then on the full
   16-unit raster in quarter-cell and quarter-degree steps. Each candidate is judged by the fit
   above, with the players' feet when the demo's paths are available. A result within two cells
   and one degree of the maps' own coordinates that scores no better than them becomes exactly
   zero, so a fraction of a unit never counts as a moved world.
4. If even the best place scores under 0.4 (floor match and players on a floor, averaged), the two
   maps do not share a layout. AUTO ALIGN says so and leaves the offset alone: Inferno on Dust2
   Night scores 0.30 and Inferno on Anubis Night 0.01, while the remakes above score 0.87 or more.

On the real collision of four remakes (Inferno Night, Rainy Mirage, Dust2 Night, Ancient Night)
the first step keeps the maps' own coordinates in 26 to 158 ms, with floor matches of 0.90 to 1.00
and 0.97 to 0.99 of the players on a floor. With the remake's collision moved by a known offset
and turn (up to 2,500 units and 180 degrees), the full search finds it to within 4 units and the
exact yaw in 0.5 to 1.7 seconds. Each pass runs its yaws on up to eight threads below the game's
priority (`AutoAlignOptions::threads`, `workerInit`), merged in a fixed order so the answer does not
depend on the thread count, and the FFTs run in single precision. It cannot find a mirrored layout,
and a map with a symmetric layout can line up the wrong way round. Check the picture before playing.

## Settings and files

The choice and the alignment are saved in the demo's project file (`project.mvm`, section
`environment`). Opening the demo again plays it on the saved map. **BACK TO ...** plays it on its
own map again and turns the setting off.

The copy is written to the demo's project folder as `environment.dem` or `environment-b.dem`. There
are two slots because the engine keeps the playing file open while a new copy is written. Each copy
has a `.src` marker that records the file it was written from (with its size and write time), the
copy format, the map, addon, skybox and alignment. A copy is used only while all of those still
match. Raise `kCopyFormat` in `AltEnvironment.cpp` whenever the rewrite changes, so old copies are
written again. Opening a
demo deletes every other demo's environment copies, as it does for scrub keyframe copies.

## Limits

- **The hook's own map-data effects stay off while the world is moved.** The Environment
  renderer's shadows, relighting and surface passes read the replacement map's data in its own
  coordinates. With no offset (the remake case) that matches the world. With an offset it would
  not, so the map data is held off and the Environment panel says why. Moving that data with the
  world is follow-up work.
- **The demo map's networked props are hidden unless you show them.** Doors, breakables, physics
  props, chickens and brush entities are part of the demo, so on another map they float where the
  demo recorded them, and any whose model lives in the demo map's own package (brush entities
  compile to `maps/<map>/entities/*.vmdl`) draws as ERROR, because that package is not mounted.
  `AltEnvironmentProps.cpp` picks them by model (anything under `models/`, plus the demo map's own
  package; on the Inferno demo that was 488 entities, 396 of them breakables that report no entity
  class at all) and hides them at the draw: every entity's scene object carries its owner handle at
  `+0xC0`, and the scene-system draw detour skips a batch whose draws all belong to hidden entities,
  shadows included. A batch that mixes them with shown draws has the shown elements moved to its
  front and only those drawn, then the engine's batch is put back as it was. Mixed batches are
  common where the replacement map uses the same models: 4 to 6 a frame on Rainy Inferno, against
  about one on El Dorado. Drawing them whole left hidden props and ERRORs on screen.
  Writing the entity's own `m_bEnableRendering`, `EF_NODRAW`, scene-node scale or a
  mesh-group mask that selects nothing was measured not to reach the renderer. **Demo map props**
  brings the model props back (on a remake of the same map they sit in the right place); the ones
  from the demo map's own package stay hidden. The demo map's networked particles and lights are not
  covered yet.
- **The replacement map's own server-side entities are missing, and its lamps are among them.** A
  demo only carries the entities its server sent, so the replacement map's doors, buttons and
  moving props do not exist in the copy. Neither do its dynamic lights, sky, fog or
  post-processing volume, which CS2 also sends from the server: the demo brings the demo map's
  `env_sky`, `env_cubemap_fog`, `env_gradient_fog`, `post_processing_volume` and `sky_camera`
  instead (`altenv props lights`, measured 2026-10-25). A map whose world lighting is baked looks
  right: the Inferno remakes, El Dorado. A night map lit by lamps does not. Shoots Night natively
  has 20 `light_rect` and `light_barn` entities from its server, and the Inferno demo carries none
  (Inferno's lighting is all baked), so its world draws black under Inferno's bright sky. Raising
  exposure only brightens the sky, and `mirv_relight` does not engage there (`cost=0`, its views
  unchanged), because it swaps inputs the engine binds only when real light entities exist.
  Lighting such a map needs its lights spawned on the client from its entity lump, or relighting
  that works without native light records. Neither exists yet.
- **A Workshop map must be installed.** The copy names the Workshop item, and the engine mounts it
  from the local Workshop folder.
- **No 3D skybox on the replacement map** leaves the demo map's own skybox in place, moved with the
  world.

## Console

```text
mirv_filmmaker altenv status                    what the open demo plays on
mirv_filmmaker altenv maps [rescan]             the maps it can play on
mirv_filmmaker altenv pick <map> [item]|none    choose the replacement map (and its Workshop item)
mirv_filmmaker altenv align <x> <y> <z> <yaw>   where the demo lands on it
mirv_filmmaker altenv move <dx> <dy> [dz]       shift that
mirv_filmmaker altenv turn <deg> [px py]        turn it about the demo map's middle (or a demo point)
mirv_filmmaker altenv reset | revert            no offset | back to what is saved
mirv_filmmaker altenv auto                      AUTO ALIGN
mirv_filmmaker altenv apply                     play the demo on the picked map
mirv_filmmaker altenv off                       play it on its own map again
mirv_filmmaker altenv props on|off|toggle       the demo map's own doors, breakables and physics props
mirv_filmmaker altenv props census              every entity class, with an example model (altenv.props)
mirv_filmmaker altenv props lights              the lighting, sky, fog and post-processing entities (altenv.lights)
mirv_filmmaker altenv view on|off|toggle        the map view in CONFIG
mirv_filmmaker altenv sceneprobe [n]            research: dump n scene objects the world draws through
```

`mirv_filmmaker health` prints one `altenv:` line: the map, its addon, the loaded level and whether
the copy is playing.

## Diagnosis

`mvm_debug` channels:

- `altenv`: the demo and its saved setting, picks, each picture (triangles, raster sizes, addon,
  skybox, time), the players' paths, AUTO ALIGN (result, fit, time), apply, every copy build (the
  world offset written, how many headers, ServerInfo, worlds and skyboxes were rewritten, bytes,
  time), copy resolution on open, `restore` when a demo opened on its own map moves to its saved
  one, and `world loaded level=... expected=... copy=... ok|MISMATCH` once each switch has landed.
- `keyframes.switch`: the reopen itself (`switching`, `seeking`, `landed`, `done`). The
  alternative environment shares it with the scrub keyframes switch.
- `altenv.probe`: once a second while a demo is open, the level, whether the copy plays, the tick,
  pause, and the alignment. Use it to check that the result holds through seeks and pauses.
- `altenv.layer`: each redraw of the map view's demo layer, with the map it was coloured for, the
  alignment, the walked feet by where they land, and the milliseconds it took (about 30 on
  Inferno's 58,000 feet). `altenv status` repeats the last shares.
- `image.server`: each picture Panorama downloads from the hook, with its version and size.
  `mirv_filmmaker config eval $.Msg(JSON.stringify($.ConfigHudMapView.stats()))` prints the map
  view's side: pictures asked for, shown, failed, shown after the 1.5 s fallback, and load times.
- `environment.occlusion`: extraction for the map view, and `held:` when map data is off.
- `altenv.props`: each scan of the demo map's props (how many hidden, how many from its own
  package, and how many draw batches mixed hidden and shown draws, of which only the shown ones
  were drawn), the first few hidden entities with their class and model, and every one from the demo
  map's package with its position. `altenv props census` lists every entity class with an example
  model.
- `altenv.lights`: `altenv props lights` lists every light, probe, cubemap, sky, fog and
  post-processing entity with its index (under 16384 the server sent it) and position.
- `altenv` also logs `demo closed` when no demo has played for two seconds.
- `altenv.sceneprobe`: `altenv sceneprobe` logs the world offset the playing copy applied, then the
  memory of the next scene objects the base and aggregate draw detours see (`SceneSystem.cpp`).
  Read-only. It found the placement matrix and the owner handle described above.

## Code

| Piece | File |
| --- | --- |
| Alignment, world offset, floor rasters, pictures, fit, AUTO ALIGN | [`AltEnvironmentMath.h`](../../AfxHookSource2/Filmmaker/AltEnvironment/AltEnvironmentMath.h) |
| State, workers, project settings, playback, commands | [`AltEnvironment.cpp`](../../AfxHookSource2/Filmmaker/AltEnvironment/AltEnvironment.cpp) |
| The copies on disk: slots, `.src` markers, clean-up (the only side that reads the raw playing file) | [`AltEnvironmentCopies.cpp`](../../AfxHookSource2/Filmmaker/AltEnvironment/AltEnvironmentCopies.cpp) |
| The demo copy | [`DemoWorldSwap.cpp`](../../AfxHookSource2/Filmmaker/Demo/DemoWorldSwap.cpp), [`SnappyDecode.h`](../../AfxHookSource2/Filmmaker/Platform/SnappyDecode.h) |
| Reopening the demo from a copy on the same tick | `SwitchPlayingCopy` in [`Filmmaker.cpp`](../../AfxHookSource2/Filmmaker/Filmmaker.cpp) |
| Card and map view | [`ConfigHudAltEnvironmentJs.h`](../../AfxHookSource2/Filmmaker/Panorama/ConfigHudAltEnvironmentJs.h) |
| Hiding the demo map's props | [`AltEnvironmentProps.cpp`](../../AfxHookSource2/Filmmaker/AltEnvironment/AltEnvironmentProps.cpp) |
| Scene-object research probe | [`AltEnvironmentSceneProbe.cpp`](../../AfxHookSource2/Filmmaker/AltEnvironment/AltEnvironmentSceneProbe.cpp) |
| Pictures over http | [`LocalImageServer.cpp`](../../AfxHookSource2/Filmmaker/Platform/LocalImageServer.cpp), [`PngWrite.h`](../../AfxHookSource2/Filmmaker/Platform/PngWrite.h) |
| Collision for any map | `ReadMapCollision` in [`EnvironmentOcclusion.cpp`](../../AfxHookSource2/Filmmaker/Environment/EnvironmentOcclusion.cpp) |

## Tests

```batch
ctest --test-dir build/x64-source2-release -C Release -R "AltEnvironmentMathTests|DemoWorldSwapTests|DemoProjectStoreMathTests|DemoProjectFileMathTests"
```

- `alt-environment-math-tests.cpp`: yaw, the alignment round trip, the world offset as the exact
  inverse, turning about a pivot, raster building, pictures, the fit, the PNG encoder, and AUTO
  ALIGN on synthetic levels: no offset, four offsets and turns with an unrelated extra area, with
  and without players' feet, the same answer on one thread and on four, a refusal for an unrelated
  level, and where points land (on a floor, floating, under it, outside, a street under an arch)
  with the counts and colours of the map view's demo layer.
- `environment-ui-tests.mjs`: the MAP card's commands, its Reset and the tab reset's question,
  `mapviewopen` only while the card is selected, the input routing that keeps the open view's
  clicks, drags and wheel from CS2 and the object picker, the map view's landing chips and legend,
  and the picture swap (no blank frame, newest first, a failed load keeps the last picture).
- `demo-world-swap-tests.cpp`: a synthetic demo with a compressed signon, unaligned messages, GOTV
  spawn groups and a `DEM_SpawnGroups` index; every rewritten field, the untouched bytes, the
  header offsets, a missing skybox, and refusals.

The UI is reviewed in the [CS2 mockup](../../UiMockup/README.md) with the `altenv_card`,
`altenv_view`, `altenv_aligned` and `altenv_other_card` captures
(`node UiMockup/capture-mock.mjs altenv_view`). In game,
check with `mvm_debug` running: pick a Workshop remake, apply, and read the `world loaded` line and
the probe through a seek and a pause.
