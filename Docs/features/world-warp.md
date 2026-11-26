# World Effects (World Warp)

`CONFIG > EFFECTS > WORLD EFFECTS` bends the whole scene. Every mode is off by default and costs
nothing while off.

| Mode | How it works |
| --- | --- |
| Little Planet | Six views of the scene, one per cubemap face, projected stereographically so the ground sits in the middle and the sky wraps the rim |
| Inception, Rolling Horizon, World Wave, World Fold | CS2's own world, prop and player vertex shaders are patched to bend real geometry before it is drawn, so nothing is stretched in screen space |

Settings cover direction, wavelength, speed, softness, axis yaw and the origin (the camera, or a
point placed in the scene).

## Little Planet and demo time

The six faces are rendered on six frames, one face each. A planet built from faces of different
moments smears anything that moves, so the faces count only as a set taken at one demo time
(`CubemapSetTracker` in [WorldWarpMath.h](../../AfxHookSource2/Filmmaker/WorldWarp/WorldWarpMath.h)):

- While a set is being taken and the demo plays, the demo is held paused for faces 1 to 5 and
  plays on when face 5 lands. A pause the user made is left alone.
- If the demo time moves in the middle of a set anyway, the set is dropped and starts again at
  face 0.
- The six faces are captured into a working cubemap and copied to the one the planet reads only
  when the set is complete, so the screen never shows half of a new set.
- Without a demo there is no clock to hold, and a complete set is shown as it is.

Playback with Little Planet on therefore runs slower than real time. A render job cannot hold the
demo (it records every frame), so starting a render with Little Planet on is refused with a
message. Rendering it needs the render scheduler to take six views per output frame.

## Console

```text
mirv_filmmaker worldwarp mode <off|littleplanet|inception|horizon|wave|fold> [preset]
mirv_filmmaker worldwarp set <key> <value>
mirv_filmmaker worldwarp origin camera|clear|<x> <y> <z>
mirv_filmmaker worldwarp reset | status
```

`mvm_debug` logs dropped faces and each hold under `worldwarp.cube`.

## Code and tests

- [WorldWarp/](../../AfxHookSource2/Filmmaker/WorldWarp) (`WorldWarp.cpp` state and the demo hold,
  `WorldWarpRender.cpp` capture and composite, `WorldWarpDxbcMath.h` the vertex shader patcher)
- `WorldWarpMathTests` (projection, deformations, the cubemap set rule) and `WorldWarpDxbcTests`
  (patched shaders are created on D3D11 WARP)

## Known limits

- Extreme World Fold angles can open thin cracks at single-sided roof brushes on the map's outer
  shell.
- Little Planet cannot be rendered yet (above).
