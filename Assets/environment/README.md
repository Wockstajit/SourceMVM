# Environment assets

Files the hook ships beside itself in `bin/resources/environment/` (install rules in
`AfxHookSource2/CMakeLists.txt`). The PNGs and the GLB are stored in Git LFS.

## `sand/`

The sand ground coating's textures, read by `EnvironmentGameTextures.cpp`. Each style is three maps
packed the way CS2's own snow material is (colour with cavities in alpha, a hemi-octahedral normal with
roughness in blue, and a height map). `FX/tools/build-sand-texture.py` builds them.

| Style | Files | Source |
| --- | --- | --- |
| Dusty Mix (default) | `sand_*.png` | A blend of the three scans below |
| Cracked Crust | `sand_fine_*.png` | Poly Haven `sand_01`, CC0 1.0 |
| Pebbly Grit | `sand_coarse_*.png` | Poly Haven `coast_sand_02`, CC0 1.0 |
| Trampled | `sand_rough_*.png` | Poly Haven `park_sand`, CC0 1.0 |

## `tumbleweed/`

`tumbleweed.glb` is a Russian thistle generated from scratch by `FX/tools/build-tumbleweed.py` (no
third-party mesh or texture). Run the script again to rebuild it; `--preview <png>` writes a quick
shaded sheet.
