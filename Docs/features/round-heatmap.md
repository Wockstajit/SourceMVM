# Round heat map

**CONFIG → ENVIRONMENT → Round heat map** (its own card under Scene) paints the floor red where the players
walked during the round being watched. It covers the whole round, from its start to the next
round's start, not just the part already played. It only paints around the camera and follows the
camera as you move. A spot nobody stood on stays unpainted, so you can see where a placed object
will not get in anyone's way. A strip is as wide as a player's body (CS2's 32-unit hull), so the
paint shows the exact area people covered, not a soft blob. Brighter red means more separate
visits: one walk-through is a clear dark red, and a choke point the whole team crossed is full
red.

It is a placement aid. Recordings never show it, and it is not part of a saved scene or preset.

## Controls

The switch is the card's header master, and the same switch sits on its rail row, so it can be
flipped without opening the card. Its settings:

- **Area** (256 to 4096 units, default 1500) is how far around the camera the floor is painted.
- **Players** paints everyone, T only or CT only.
- The status line names the round (`Round 4 of 18.`), says when nobody walked near the camera,
  and says when the demo is still being read or could not be read.

```text
mirv_filmmaker heatmap on|off|toggle
mirv_filmmaker heatmap radius 1500
mirv_filmmaker heatmap teams all|t|ct
mirv_filmmaker heatmap status
```

The settings last for the session. A new demo keeps the switch on and reads that demo's rounds.

## Where the paths come from

The game only holds the players' current positions, and seeking replays the world. The heat map
therefore reads the demo file itself. `FilmmakerDemoInfo <demo> -paths`
([`paths.go`](../../FilmmakerDemoInfoGo/paths.go)) parses the demo once, in about three seconds
for a full match, and writes every living player's feet position every 2 ticks for every round. The
hook caches the output as `roundpaths.fmbin` in the demo's project folder (beside
`scoreboard.fmjson`) and reuses it while it is newer than the demo and the helper. A failed parse is
cached too, so an unreadable demo is not parsed again.

- A round runs from its `round_start` to the next one, so freeze time and the walk after the
  round ends are in it. A recording that starts a moment before the first `round_start` folds
  that moment into the first round.
- Warmup rounds, and every round before the match went live (a knife round, a restart), are
  marked pre-match. Their paths are shown while you watch them, the status line says Warmup, and
  `Round N of M` counts the match's own rounds the way the scoreboard does.
- Ticks are demo ticks, the clock `demo_gototick` and the engine's demo tick use. The round shown
  is the last one that started at or before the current demo tick.
- A stroke breaks at a respawn and at any jump between two samples longer than 96 units, so no
  line is drawn across a teleport.

## How the floor is painted

[`RoundHeatmapMath.h`](../../AfxHookSource2/Filmmaker/Heatmap/RoundHeatmapMath.h) is engine-free
and tested by `round-heatmap-math-tests.cpp`. Run that executable with a `roundpaths.fmbin` path
to see each round's grid size and build time on a real demo. Add a round number and a `.ppm` path
to write that round as a picture.

- The grid is a square of up to 1024 × 1024 texels around the camera, 2 units per texel at the
  finest and about 3.7 at the default area. It reaches 25 % past the area, so a short camera move
  keeps it. A move out of it, a new round, a new area or a new team filter builds a new one on a
  worker thread (20 to 35 ms for a full round of a competitive match).
- Each sample stamps the player's body along the segment from the previous sample. A texel counts
  one visit per player until that player has been away from it for 1.5 seconds, so standing still
  is one visit and coming back later is another. Coming back after a broken stroke (a respawn, a
  reconnect, a teleport) is always another visit.
- Each texel keeps two floor heights with their own counts, so traffic on an upper floor does not
  paint the floor under it. In the air, a sample keeps the floor the player left, unless they fell
  below it.
- The pass ([`RoundHeatmapRender.cpp`](../../AfxHookSource2/Filmmaker/Heatmap/RoundHeatmapRender.cpp))
  runs after the Environment composite and the lens effects, before the recorder and the UI. It
  rebuilds each pixel's world position from the scene depth and paints it only when the surface
  faces up, sits within a step of a stored floor height, and lies inside the area. It leaves the
  sky, the 3D skybox, the first-person weapon and the live players' shoes alone. The live players
  come from the Environment's per-frame player list, which runs while the heat map is on.
- Toggling it, a new area or team filter, and a newly read demo drop the scrub frame cache.

## Known gaps

- Spectating a scoped sniper in first person paints the floor over the scope's black edge.
- Smoke writes no depth, so the paint shows through a smoke cloud.
- Far from the camera a texel is smaller than a pixel, and thin strips can shimmer.
- The heat map needs a demo. Offline practice and live sessions have no rounds to read.
