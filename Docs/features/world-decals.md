# Decals and media

`CONFIG > ENVIRONMENT > SCENE OBJECTS > Decals & Media` projects pictures onto the map's surfaces
using the scene depth, so they follow walls and floors.

| Kind | What it shows |
| --- | --- |
| Text | A line of text |
| Image | PNG, JPG, BMP or TIFF, decoded with Windows Imaging Component |
| Video | MP4, MOV, WMV or AVI, played with Media Foundation |
| Arrow, line | Painted marks |
| Route | A path that draws itself along the ground (`routeProgress`, `routeSpeed`) |

A decal can be attached to a scene object or player so it moves with them, and can skip players so
they are not painted over (on by default, and wide enough to cover arms and a held weapon).

## Placement

`add` and `place` cast a ray from the camera and put the decal on the surface they hit. The decal is
aligned to that surface's normal, so a wall decal placed from an angle stays flat on its wall and
does not turn toward the camera when it moves. Walls keep world up, floors keep the yaw the camera had.
Only surfaces that face the projector take the decal, so a wall decal does not spill onto the floor
or the far side of an object. Without map collision data the decal falls back to a spot ahead of the
camera.

In the console, put a path with a drive letter in quotes: `decals set 1 media "C:/pics/logo.png"`.

## Console

```text
mirv_filmmaker decals add <text|image|video|arrow|route> [floor|wall]
mirv_filmmaker decals select <id> | remove <id> | duplicate <id> | clear
mirv_filmmaker decals set <id> <field> <value...>
mirv_filmmaker decals place <id> <wall|floor>
mirv_filmmaker decals pickmedia <id> <image|video>
mirv_filmmaker decals route <id> addpoint|undo|clear|startnow
mirv_filmmaker decals attach <id> <objectId|off>
```

## Code and tests

- [WorldDecals/](../../AfxHookSource2/Filmmaker/WorldDecals) (state and media decoding, the
  depth-projected renderer)
- `WorldDecalsMathTests`

## Known limits

- Video decals follow demo time and loop. Checked in game with an H.264 MP4 whose picture changes each
  second. A seek has to decode forward from the clip's previous keyframe, so jumping far on a clip with
  a long keyframe interval can cost a short hitch.
