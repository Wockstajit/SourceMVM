# Legacy vs CS2 weapon model variants: how CS2 picks a model per skin

Status: investigation and implemented support. The `mirv_filmmaker cosmetics mesh ...` knob sets the
mesh mode. The automatic econ-schema legacy read `PaintKitLegacyModel` lives in `CosmeticFnResolver.cpp`,
and the mesh-mask apply lives in `CosmeticModelSwap.cpp`. The file map is in
`Docs/cosmetics/cosmetics-overview.md`. The apply path is live-verified to execute and resolve on the
current build. The *visual* mesh switch depends on the demo-playback caveats in §6.

Companion docs: `Docs/cosmetics/cosmetics-cs2-methodology-notes.md` (the source-derived recipes) and
`Docs/archive/cosmetics-recompose-research.md` (the skin composite path).

## 1. The mechanism: one model, two (or more) mesh groups

A CS2 weapon `.vmdl` does not ship as separate "legacy" and "modern" files. Each weapon model
contains several mesh groups. A per-entity mesh-group mask on the scene node
(`CGameSceneNode::SetMeshGroupMask(uint64 mask)`) chooses which group renders. Weapons re-modelled
between CS:GO and CS2 (new high-poly geometry and a new UV layout) keep the legacy mesh (the old
geometry and UVs) as a separate group, so skins authored for the old UV map still line up on it.
A "legacy AK vs CS2 AK" is therefore the same loaded `.vmdl` with a different mesh group selected,
not a different model resource.

The paint kit, not the weapon, decides the variant. The paint kit definition carries a boolean
`IsUseLegacyModel`. A skin made for the old UVs sets it, and a skin made for the new CS2 UVs does
not. At weapon build time CS2 reads the flag and sets the mask, so the finish renders on matching
geometry.

Confirmed in source (both reference cheats, `Docs/cosmetics/cosmetics-cs2-methodology-notes.md` §5):
- `CPaintKit::IsUseLegacyModel()` reads a `uint8` at paint kit + 0xAE (Andromeda
  `g_CCPaintKit_IsUseLegacyModel`; nerv `c_paint_kit::uses_old_model`).
- The paint kit object comes from the econ item schema:
  `GetEconItemSystem() -> +0x8 (schema) -> CUtlMap<int,CPaintKit*> -> find by paint-kit id`. The map
  sits at a different schema offset on each game build.
  `CosmeticFnResolver.cpp` discovers the map and the flag byte at runtime rather than using fixed offsets,
  and validates them against fixed paint kits from `cs2_weapon_skins_legacy_index.csv` (see §7).
- The mesh-group mask is then `SetMeshGroupMask(node, mask)`.

## 2. Which weapons have legacy and CS2 variants

The check is per paint kit, not per weapon. Read the paint kit's `IsUseLegacyModel` flag to learn
whether a *finish* wants the legacy mesh. The repo does this (see §4). A hard-coded weapon list
is fragile, because Valve adds finishes and occasionally re-models more weapons.

Weapons with a distinct legacy mesh group are the ones re-modelled in the CS:GO "weapon model
overhaul" and again for CS2. Most are older rifles, pistols and SMGs with many skins authored on the
old UVs (e.g. AK-47, M4A4, M4A1-S, AWP, USP-S, Glock, Desert Eagle and similar). Newer weapons, and
weapons never re-UV'd, ship a single mesh group. For them the legacy flag does not matter, and
toggling the mask does nothing.

Live data point on this build: the USP-S (def 61) has both groups. The apply path set the mask
without faulting (the `weaponMesh` apply counter incremented). See §6 for the verification caveat
about *seeing* the switch.

## 3. How a skin determines the variant (the rule)

```
legacyMesh = paintKit.IsUseLegacyModel        // read from the econ schema, per finish
meshGroupMask = legacyMesh ? <legacy bits> : <modern bits>
SetMeshGroupMask(weaponSceneNode, meshGroupMask)
```

- A finish with the flag set must render on the legacy mesh. Otherwise its pattern and wear are
  UV-misaligned (smeared, offset, or mapped to the wrong faces).
- A finish with the flag clear renders on the modern CS2 mesh.
- The default/vanilla finish (paint 0) uses the modern mesh.

### The mask bit values (and a real source disagreement)

The two reference implementations disagree on the bit polarity. This is the biggest edge case:

| Case | Andromeda | nerv (weapon skins) | nerv (knife) |
|---|---|---|---|
| modern (CS2) | `1` | `1` | `2` |
| legacy | `2` | `2` | `1` |

Both agree on weapons: modern=1, legacy=2. For knives nerv inverts the values (modern=2,
legacy=1). This repo follows the Andromeda polarity for both (modern=`m_maskModern`, default 1;
legacy=`m_maskLegacy`, default 2). The values are tunable, so knife and edge cases can be A/B
tested in game without a rebuild:

```
mirv_filmmaker cosmetics mesh auto|modern|legacy        # selection mode
mirv_filmmaker cosmetics mesh masks <modernBits> <legacyBits>   # tune the bit values
```

## 4. How the player customizer chooses the model (implementation)

The model-swap subsystem (`PaintKitLegacyModel` in `CosmeticFnResolver.cpp`; the rest in
`CosmeticModelSwap.cpp`):
- `PaintKitLegacyModel(paintKitId)` does the econ-schema lookup above (SEH-guarded). It returns
  `1` for legacy, `0` for modern and `-1` for unknown.
- `ResolveMeshMask(paintKit, knife, mode, maskModern, maskLegacy)` maps that result to a mask:
  - `mode = auto` uses the schema flag. An unknown result returns 0, which leaves the mesh
    untouched. The code never guesses, so an unrecognised finish keeps the mesh the demo already set.
  - `mode = modern|legacy` forces the mesh (for A/B tests).
- In the apply loop (`CosmeticOverrideSystem::ApplyMatchedWeapons`):
  - Weapon skin override: if `paintKit > 0`, the code computes the mask and calls
    `ApplyWeaponMeshMask`. The customizer corrects the legacy or CS2 mesh to match the chosen
    finish. An unknown legacy result leaves the mesh untouched.
  - Knife type swap: a knife always needs a mesh group, so an unknown or auto result falls
    back to `maskModern`, not 0.
- This runs change-gated, right before the skin re-composite, so the composite rebuilds onto the
  selected mesh.

The schema read resolved on the live build (`cosmetics status` shows `modelSwapFns: ... econSchema=1`).

## 5. Edge cases

- Knife mask polarity (Andromeda and nerv disagree): tune with `cosmetics mesh masks` if a
  swapped knife renders the wrong geometry.
- Unknown paint kit (not in the schema map, or schema unavailable): auto mode leaves the mesh
  untouched. Only a forced `mesh legacy|modern` moves it.
- Weapons with a single mesh group: toggling the mask is a no-op. This is expected, not a bug.
- Default finish (paint 0): modern mesh, no legacy correction needed.
- A legacy skin left on the modern mesh (or the reverse): the finish renders, but the pattern is
  UV-misaligned. The auto-selection exists to prevent this symptom.
- Mesh change not visible while the demo is PAUSED: see §6. The mask write runs, but the
  rendered mesh group is not re-evaluated until the engine refreshes the renderable.

## 6. How automation should verify (and a trap to avoid)

Do not verify the legacy/CS2 switch by comparing two different paint kits (e.g. paint 38
vs paint 44). The diff you measure is the paint change, not the mesh switch, which gives a
false "it works".

The correct test holds the paint kit, wear, seed, weapon, tick, and camera constant, and toggles
only the mesh mode:
```
cosmetics player current weapon <def> paint <pk> wear <w> seed 0
cosmetics mesh modern   ; <recapture>    # screenshot A
cosmetics mesh legacy   ; <recapture>    # screenshot B
image_diff A B --crop <weapon-region>
```
A real legacy-to-modern switch on a weapon that has both groups changes the silhouette and UVs of the
weapon crop, well above the same-tick TAA noise floor. `Automation/verify/verify-cosmetics-customizer.ps1`
contains this same-paint toggle.

Verification note for paused demo playback: the same-paint toggle can measure at the noise floor
(about 0.16 mean) even when the mesh switch works. The mask write runs: the `weaponMesh` apply
counter increments, there is no crash, and the function resolves. Verify transitions with the
`visualdiag` fields and a live NetCon no-crash watch, not a paused crop alone (see the recipe below).

Current-client recipe: do not call `CGameSceneNode::PostDataUpdate` after a cosmetic
mesh or model write. On this client it queues invalid renderer work and causes asynchronous crashes or
hangs during demo seeks. `ApplyWeaponMeshMask` writes the resolved model-state mesh mask directly and
reasserts it each frame. The demo tick nudge supplies the live frames the renderer needs. Knife swaps
follow the same rule: precache, `SetModel`, then mesh and subclass writes. The animation-builder detour
substitutes an empty sequence list when an unloaded knife has no animation resource. The viewmodel
mirror also avoids `PostDataUpdate` and the animgraph reset. Verify legacy and modern transitions with
`visualdiag` (`paintLegacy` and `worldMask`) and a live NetCon no-crash watch, not a paused crop alone.

Third-person and playback reselect: live demo packets can reset the WORLD renderable's
cached mesh selection while `CModelState::m_MeshGroupMask` still reads the requested value. A direct
field hold keeps the diagnostics correct, but third-person geometry can stay reverted unless the world
is reselected. On each paused-to-playing transition, the late frame pass runs a three-frame native
scratch-to-target world reselect after CS2's own `FrameStageNotify`. Stable frames keep the
non-invalidating field hold. The first-person HUD-model path is handled separately. Seeks keep SteamID
profiles. They discard the pre-seek late queue at once and clear the per-entity mesh memo, so recreated
weapons get a fresh native selection after the seek-settle window.

Picks made during playback: a skin that needs the other mesh (Glock Moonrise, from legacy
to AXIA, modern) switches mesh (`meshswitch live=2 wanted=1`, `postMask=1`). The direct composite runs
only while paused, so a pick made while the demo plays uses nudge phase 2 (`CosmeticDemoSync.cpp`):
`demo_pause`, then one paused frame runs the mesh write and the direct composite
(`m_postNudgeMeshRefreshPending`), then 12 settle frames for the async material, then `demo_resume`.
A 240-frame cap and a seek abort always hand playback back. The log shows
`cosmetics.nudge START live-composite` and `DONE live-composite`.

The first-person gun is a separate HUD model (`C_CS2HudModelWeapon`) that CS2 builds anew whenever
the gun is drawn, on the demo's own mesh group. The one native switch above mirrors the group onto
the HUD copy that exists at that moment, and only when the gun is in hand. So a gun drawn later
showed a legacy finish on the CS2 mesh in first person. That happened when the gun was holstered at a
seek's landing or a loop's In, or was put away and drawn again. This was the AWP Dragon Lore and
Gungnir report. On de_inferno, every landing with the AWP drawn afterwards read world mask 2 and HUD
mask 1. Each time the gun comes into first-person view, the apply loop now mirrors the group again
(`MirrorWeaponMeshToViewmodel`): when the viewed player draws it, or when the view moves to a player
holding it. The mirror runs for 24 frames, because the HUD copy appears a few frames after the
draw. It only writes a copy whose mask differs, so it never re-invalidates a material that is
already right. It is limited to the viewed player's gun. HUD copies are matched by model path, so
another player's draw would otherwise put that player's group on the viewed player's copy. After
the change both masks read 2 on every landing. The `cosmetics.weapon.vmmirror` log line shows the
copies found per frame.

A gun that is holstered at the pick (the other weapon is in hand) is still owed its composite. The
composite runs only for the deployed weapon, so `m_compositeOwedIdx` remembers the entity. The first
frame it is active while the demo plays requests the same pause-composite pass
(`cosmetics.nudge owed composite idx=...`). In the Inferno demo (starry skies, Glock Moonrise to
AXIA) the log shows `liveMeshMask=2` at the draw, then `postMask=1` and a composite stage
on the paused frame. The rendered texture was not captured.


## 7. CS2 build 2000930 support

On build 2000930 the implementation handles four engine details:

1. Paint-kit map location: the layout is found at runtime (`EnsurePaintKitLayout`). It scans the schema
   for the `CUtlMap` whose nodes hold 14 calibration paint kits (id and name string), then picks the
   byte that reads 1 on every legacy kit and 0 on every CS2 kit. It fails closed if any calibration
   kit disagrees. `mirv_filmmaker cosmetics mesh` prints the discovered layout, and
   `mesh lookup <paintKit>...` classifies finishes. Live result: 1266/1266 indexed finishes match the CSV.
2. Composite owner: `CosmeticDirectComposite.cpp` reads the `CCompositeMaterialOwner` base
   displacement from the entity's MSVC RTTI class hierarchy rather than a fixed offset.
3. Composite timing: between frames the field holds the demo-recorded group (pre-arm), so a
   composite fired then would be built for the group being left. Every composite is held for 8 paused
   frames after a native switch. Then exactly one runs, with the target mask written into the field first. The post-seek stability clock
   also advances while no override exists, so the first pick after a seek switches in the pick frame.
4. Default (paint 0) resolves to the modern mask, which switches a legacy gun back to the CS2 mesh,
   and releases the HUD copy's composite materials.

A pick made while the demo plays is queued. The demo pauses, the pick commits on the paused
frame (attributes, mesh and composite together), and playback resumes (`cosmetics.livepick` log lines).
A gun holstered during a switch stays owed its composite until it is drawn. Clearing overrides forgets
the per-entity mesh memo. `cosmetics.composite.trace` logs world and HUD masks and composite counts for
90 frames after each pick.

Verified live on Inferno, Mirage and Cache match demos (AK-47, M4A1-S, Desert Eagle, AWP, including
guns with no demo skin). Sequences of legacy, CS2, Default, CS2, legacy switched the world and
first-person masks every time and rendered the chosen finish. This includes picks made during 0.5x
playback and across a holster and redraw.


## 8. Knives and gloves

A knife swap (SetModel + mesh + subclass) follows the same rule as a legacy/CS2 switch: composites
are held for 8 paused frames, then one composite runs on the new model. A swap that lands on a playing frame, such as the deploy-edge re-fire when the knife is
redrawn, owes its composite. That composite pauses the demo for one pass. A knife pick made during
playback is deferred like a weapon pick.

Gloves on demo players: the third-person and preview glove is a `C_WorldModelGloves` entity. The
pawn's per-tick client think spawns it from `m_EconGloves` when `m_bNeedToReApplyGloves` is set. A paused
demo runs no think, so `UpdatePawnGlovesNow` (CosmeticGloveSwap.cpp) runs that glove step directly.
It resolves the step from its call site in the think and validates the owner pointer. It also sets
`m_bRenderEvenWhenNotSuccessfullyInterpolated` on a glove entity created while paused. Without that
flag, the renderer skips the entity until a tick. The `first_or_third_person` body group decides whether
the agent's own default hands draw. It is 1 only for a custom finish and 0 for the plain team gloves.

Customize preview: CS2 agents have no hands of their own. Only the glove model
provides them. The preview pawn (`C_CSGO_PreviewPlayer`) equips its gloves inside its own client think
(vtable slot of the `C_CSPlayerPawn` think, 146 in build 2000930, resolved at runtime). That think
does not run while the demo is paused, so `PreviewAnimTick.cpp` runs it for the preview pawns on paused frames. `RunPreviewPawnThink`
refuses anything whose vtable is not the preview player's. `PreviewRenderFlag.cpp` holds the
render-even-if-not-interpolated flag on each preview pawn's glove entity, as it already did for the pawn.
Going from a finish back to the plain team gloves rebuilds the preview panel, because the panel never
clears an equipped glove model.

Verified live (paused) on Cache and Mirage: preview hands for demo gloves, Pandora's Box, Crimson Kimono,
Moto, default CT and default T. First-person demo-player gloves were checked across the same list. Knife
swaps (M9, Butterfly, Karambit, Bayonet, Flip, default) were checked with finishes, paused and during
0.5x playback (video). Third-person glove changes on demo players are hard to see at normal camera
distance. They were confirmed through the log (glove entity rebuilt, flag cleared) rather than by a
close-up screenshot.

## Sources
- `Docs/cosmetics/cosmetics-cs2-methodology-notes.md` (econ-schema + mesh-mask recipes, source disagreement table)
- Andromeda `CEconItemSchema.hpp` (`GetPaintKits` @ 0x2F0, `CPaintKit::IsUseLegacyModel` @ 0xAE),
  `CInventoryChanger.cpp` (`meshGroupMask = 1 + isLegacy`)
- nerv `i_econ_item_system.hpp` (`c_paint_kit::uses_old_model` @ 0xAE), `skin_changer.cpp`
  (knife mesh polarity inverted vs weapons)
- Local: `AfxHookSource2/Filmmaker/Cosmetics/CosmeticModelSwap.cpp`
  (`ResolveMeshMask`/`ApplyWeaponMeshMask`) + `CosmeticFnResolver.cpp` (`PaintKitLegacyModel`),
  `Automation/verify/verify-cosmetics-customizer.ps1`
