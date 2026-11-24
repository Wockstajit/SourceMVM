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
they are not painted over.

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

- Video playback is built and linked (`mfuuid.lib`) but has only been checked with still images in
  game.
