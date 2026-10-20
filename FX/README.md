# FX/: in-game particle FX asset pipeline

## What is not in this repository

The particle packs and the death animations are built from other games' material: Insurgency,
Insurgency: Sandstorm, Call of Duty (MW2019 and classic), a Garry's Mod pack and a CS:GO mod. That
material is not ours to publish, so the repository does not carry it:

| Folder | What it is |
|---|---|
| `FX/sources/` | the donor particle systems, textures, models and animations |
| `FX/compiled/` | the CS2 FX pack compiled from them (`source_mvm_fx`) |
| `FX/death-animations/ragdoll_animations.mvmanim` | the death-animation library baked from the donor animations |

SOURCE:MVM releases (the installer and runtime packages) carry the compiled pack and the
animation library, staged from the maintainer's local copy. Only the source tree leaves them out.

The repository keeps the conversion and bake tools (`tools/`), our own particles (`native/`), render
materials (`render-materials/`) and the hook's own Panorama textures (`ui/`: the outliner's Lucide
icons and the HC Tracker badge, each a power-of-two PNG with a `_png.vtex` beside it).
`tools/build-ui-textures.ps1` compiles them into `compiled/source_mvm_fx/panorama/images/` when
Workshop Tools are installed and stages them on every full build; after a hook-only build, run it
with `-StageRoot` to put new icons into the stage. The viewer's metadata
(`death-animations/clip-tags.json`, `removed-clips.txt`) sits in the local `death-animations/` folder with the baked library.

Without these folders, a clone still builds and runs. `build-source2.bat` reports that `FX\compiled` is
missing and CS2 shows its stock particles, Death Animations has no clips (the hook logs
"ragdoll animation file missing"), and the HC Tracker badge loads its logo from the web copy in
`Assets/hctracker/`. Release packaging (`build-release-payload.ps1`, `build-bundle.ps1`) still
requires the compiled pack and stops without it (unless told `-SkipFx`), so a release cannot
silently ship without its effects.

To work on them, copy the three folders into this checkout from a private copy. `.gitignore` keeps
them out of commits. The maintainer keeps that copy outside the repository.

## How the packs work

Environment illumination is separate from particle-pack selection. Real weapon-fire and
explosion events, held and projectile Molotov state, and inferno burning patches drive runtime
lights whichever pack is selected. Empty clicks emit no muzzle light. Native smoke can receive
these lights, but transparent sprites do not automatically cast opaque shadows.
See the [lighting contract](../Docs/effects/whole-map-relighting.md) for moving-slot limits, fire
timing, weapon response and remaining visual verification. A particle asset change alone adds no
light source and does not establish casing-shadow support.

Particle Packs also has Dynamic Lights: independent Molly, Flashbang, and Grenade buttons that
use CS2's native `C_OP_RenderOmni2Light`. Molly lights cover held bottles, flying Molotovs, and
burning ground patches. Flashbang and Grenade add brief light bursts at their detonation points.
They work with native map lighting without enabling Environment.
The authored sources are `native/particles/filmmaker/molly_light.vpcf` (bottles and HE),
`native/particles/filmmaker/molly_area.vpcf` (ground fire), and
`native/particles/filmmaker/flash_light.vpcf` (flashbangs); compile them with
`powershell -File FX/tools/build-molly-light.ps1` when CS2 Workshop Tools are installed.
Its compiled output is shipped in `compiled/source_mvm_fx/particles/filmmaker/`.
The full converter copies `native/` into its content tree before runtime closure pruning.
See the [Particle Packs contract](../Docs/features/filmmaker_effects_modifiers.md) for
the toggle, persistence, limits and cleanup behavior.

Converted muzzle smoke uses native scene lighting with a 0.20 visibility floor, which keeps the wisp
visible after the flash light expires.
The final `light-weapon-smoke.py` pass runs after all pack layering, including the Povarehok
weapon-smoke children, while retaining additive flame/glow, transparency and particle motion.
It does not add custom shadow receiving to sprite materials. Runtime spritecard capture
projects textured smoke coverage into the Environment shadow atlas, retaining native
world poses, animation and fade while excluding emissive flash/glow. On and Modern
muzzle-only AK checks are recorded in the lighting contract. Check the asset lighting pass with `python Automation/tests/weapon-smoke-lighting-tests.py`.

This folder owns the **custom particle FX packs** that the filmmaker's Config > Effects
section can swap in for CS2's stock particles. It is a build and release concern, kept separate
from `Automation/` (which is test-only).

| Pack | Modes | Source inputs | Runtime namespace |
|---|---|---|---|
| Insurgency 2 | On / Less | `sources/insurgency2/` (local only, ~52 MB) | `particles/filmmaker/insurgency/<pcf>/` |
| Modern (MW2019) | Modern | `sources/modern-warfare-gmod/` (local only, ~51 MB) | `particles/filmmaker/modern/...` |
| Money on headshot | Independent toggle | `sources/povarehok-money/` (local only, ~6 MB) | `particles/filmmaker/money/...` |

The three particle trees come from one converter run and are mounted at runtime by
`AfxHookSource2/Filmmaker/Movie/ParticleFx*.cpp` (swap tables: `ParticleFxRules.cpp`).

Death animations are baked clips, not particles. The game plays them through the Advanced
Ragdolls card ([Docs/features/filmmaker_death_animations.md](../Docs/features/filmmaker_death_animations.md));
the install step copies the one baked file to `bin/resources/ragdoll` when it is present. The baked
`death-animations/ragdoll_animations.mvmanim` and the donor animations it is built from
(`sources/*-deaths`) are local only (above); with them in place, `tools/extract-death-anims.py`
rebuilds the library. The Blender viewer reads the same file. Format 9 tags every clip with its source (MW2019 / Classic CoD / Extra), body area, hit side, stance and weapon; corrections made in the
viewer live in `death-animations/clip-tags.json` and deletions in `death-animations/removed-clips.txt`,
and the builder applies both. The viewer's *Put every clip in the Asset Browser* button bakes every
clip into its .blend as action assets, in folders Death Anims / source / stance / body area
(Asset Browser > Current File).

| Pack | Source input | Tool | Output |
|---|---|---|---|
| Death animations | `sources/brutal-deaths/` (GMod), `sources/cod-deaths/` (Call of Duty SEAnim + player skeleton) and `sources/mw2019-deaths/` (Cast exports + player skeleton), all local only | `tools/extract-death-anims.py` | `death-animations/ragdoll_animations.mvmanim` |

```powershell
py -3 FX/tools/extract-death-anims.py --report
```

Clips listed in `death-animations/removed-clips.txt` (written by the viewer's Delete button) are
left out of every bake.

With the local `sources/` in place, a conversion needs no donor game install, only CS2.
On/Less uses Insurgency 2's own asset library, one consistent donor. For the CS:GO "Povarehok" mod
conversion background and texture-failure patterns, see
[`Docs/porting/povarehok-csgo-mod-reference.md`](../Docs/porting/povarehok-csgo-mod-reference.md).

## Layout

```
FX/
  compiled/source_mvm_fx/             the compiled runtime pack (particles, materials incl. render
                                      materials, models); local only; staged by build-source2.bat
  death-animations/                   viewer tags, deletions and the baked library (local only)
  render-materials/                   render-pass material sources (+ prebuilt green-screen sky)
  tools/                              the conversion pipeline
    convert-fx-source1.ps1            driver: stages all packs, imports, compiles, validates
    extract-insurgency-fx.py          (re)extracts the Insurgency 2 source tree from the game
    extract-modern-particles-gmod.py  (re)extracts the Modern source tree from a GMod install
    extract-povarehok-money.py        one-time: money-on-headshot closure from the CS:GO mod
    export-source1-vtf.py             VTF -> vtex + sprite-sheet reconstruction
    postprocess_common.py             shared CS2 VPCF fixes (see Docs/porting/povarehok-csgo-mod-reference.md §10)
    postprocess_insurgency.py         CLI entry point (drives postprocess_common/_modern too)
    postprocess_modern.py             Modern (MW2019)-only fixes
    validate-fx-assets.py             runtime resource-closure check
  sources/                            local only (third-party donor material, see the top)
    insurgency2/                      Insurgency 2 Source 1 inputs (On / Less)
    modern-warfare-gmod/              Modern (MW2019) Source 1 inputs + casing templates
    povarehok-money/                  money-on-headshot inputs
    brutal-deaths/, cod-deaths/, mw2019-deaths/   death-animation donors
```

## Build

```powershell
powershell -File FX/tools/convert-fx-source1.ps1 -Compile
```

- A full rebuild does not reproduce the published pack yet. The script runs
  to the end, but its output has ~536 particle systems against the published ~1,280. The layered impacts, blood, the Modern HE explosion and
  other pieces come from generator steps the script does not run. The script therefore refuses to
  publish a pack more than 10% smaller than the one in `compiled/` (`-AllowSmallerPack` overrides).
  To add one effect, convert and compile just its systems and copy the new files into `compiled/`,
  for example the MW2019 flash grenade (`mw2019_explosions_pak/generic_explo_flash`).
- The pipeline's Python needs `srctools`, `vtf2img` and `vpk`; pass `-Python` when the one on
  PATH lacks them.
- No GMod or Insurgency install needed once the local `sources/` is in place.
- Compiling needs `resourcecompiler.exe`, which retail CS2 does not ship: install CS2 Workshop
  Tools. A normal `build.bat` does not compile; it stages the local `compiled/` pack.
- Add `-RefreshModernFromGmod` to re-pull the Modern inputs from a local GMod first.
- Add `-RefreshInsurgencyFromGame` to re-pull the Insurgency inputs from a local install first.
- Work happens under `build/fx/…` (ignored). A validated run publishes the compiled game dir to
  `compiled/source_mvm_fx` (render materials excepted: `tools/build-render-materials.ps1` owns
  `materials/filmmaker`). Update the private copy with the result. It is never committed.
- Before compiling, the content tree is pruned to the runtime closure derived from
  `ParticleFxRules.cpp`'s swap tables (the validator's `--emit-closure`). The donors ship far
  more than the DLL ever references: the converted tree holds 1,487 VPCFs,
  of which the swap tables reach 451 (580 content files total). Adding a new `FXRULE`
  keeps its assets automatically. There is no manual exclude list.

## Docs

- [`Docs/features/filmmaker_effects_modifiers.md`](../Docs/features/filmmaker_effects_modifiers.md): how the modes work in-game.
- [`Docs/porting/povarehok-csgo-mod-reference.md`](../Docs/porting/povarehok-csgo-mod-reference.md): Povarehok conversion and texture-failure patterns.
- [`Docs/porting/mw2019-fx-mapping-reference.md`](../Docs/porting/mw2019-fx-mapping-reference.md): how MW2019 effects map onto CS2 weapons.
