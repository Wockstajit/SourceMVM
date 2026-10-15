# Cosmetics recompose research: making the written skin actually render

> **Status.** Per-player demo weapon skins render through the Andromeda direct-composite call
> (held defIndex, fallback writes, and `composite once`). The current implementation lives in
> `AfxHookSource2/Filmmaker/Cosmetics/` (`CosmeticOverrideSystem.cpp` apply loop, `CosmeticDirectComposite.cpp`,
> `CosmeticPaintKitBridge.cpp`). The distilled recipes live in `../cosmetics/cosmetics-cs2-methodology-notes.md`, and the
> architecture in `../cosmetics/cosmetics-overview.md`.

This doc covers the visual-rebuild step that makes a written skin render. The repo resolves and clears
`C_EconEntity::m_bAttributesInitialized`, and the apply loop overwrites existing `m_NetworkedDynamicAttributes` paint,
wear, and seed values. The fallback-field write sticks (confirmed live: `m_iItemIDHigh=-1`,
`m_nFallbackPaintKit/m_flFallbackWear/m_nFallbackSeed` updated, `patched=2 reverted=0` every frame). The weapon
renders its skin once the direct composite call runs. The knife and agent model-swap problem is covered in
`Docs/archive/cosmetics-model-override-research.md`.

The code also exposes an experimental bridge, `mirv_filmmaker cosmetics paintkitbridge ...`. It writes the global
`cl_paintkit_override` cvar and restores the previous value when disabled. It is global, not a per-player or
per-entity skin hook. It affects only the next weapon deploy or create, because CS2 reads the cvar while building the
composite material at that point. The live verifier is `Automation/verify/verify-cosmetics-paintkit-bridge.ps1`, with
image-diff support in `Automation/tools/image_diff.py`.

## Per-player demo skins render through the Andromeda direct composite call

A per-player weapon skin override renders on the spectated demo weapon. Live checks with
cropped, paused-tick, noise-floor-controlled screenshot diffs and direct visual inspection confirmed it. Four
distinct paint kits produced four visibly distinct finishes on the same USP-S. The working mechanism:

1. The Andromeda composite functions (`UpdateCompositeMaterial` / `UpdateCompositeMaterialSet` / `UpdateSkin`)
   are wired into `cosmetics composite once`. Their AOB patterns write each wildcard byte as `??`.
   `Afx::BinUtils::FindPatternString` consumes two hex chars per byte, so a lone `?` is read as half a byte and
   shifts every later literal. `ResolveDirectCompositeFns()` in `CosmeticOverrideSystem.cpp` resolves all four
   AOBs with `??`. The four patterns are present and resolve uniquely in the current `client.dll`. The `updateSkin`
   prologue has two near-identical hits 0x50 apart. The first is taken, which is a minor ambiguity to watch.

2. The working recipe (verified live, demo paused, USP-S def 61):
   - Override the held weapon's defIndex. A held USP-S is def 61, not slot `0`.
   - Turn fallback identity ON (`m_iItemIDHigh = -1`) so the composite build consults the fallback fields
     (`fallback paint/wear/seed`), along with the networked `def6` write.
   - Then run `mirv_filmmaker cosmetics composite once 0x608`. This is the essential lever. Changing the
     profile paint and waiting on the per-frame apply alone does not update the render; the weapon keeps the prior
     composite. Firing `composite once` re-composites to the new paint immediately. The per-frame *vtable*
     `recompose` path faults and disarms (`recompose=0 faulted=1`) and is not the working path.

3. Proof, not a threshold artifact. Paint 504 gave dark and worn, 915 dark olive, 568 green, and 638 pink or
   purple camo. All were on the same paused tick, with only the paint number changing. The override also persists
   through short playback. The per-frame apply keeps the data written, and the cached composite survives until the
   weapon entity is recreated. Caveat: a weapon redeploy, round change, or demo seek recreates the entity from
   the authoritative item (796), so `composite once` must be fired again after seeking. That is natural for
   frame-by-frame filmmaking.

4. Whole-frame diffs are not valid evidence. A full 1600x1200 frame diff, with the demo advancing ~900ms
   (`demo_resume`/`demo_pause`) between shots, changes about 100k pixels from motion alone, whatever the skin.
   `Automation/verify/verify-cosmetics-composite-direct.ps1` stays paused, uses the resolved defIndex and fallback,
   and compares a viewmodel crop against a same-tick TAA noise floor (`image_diff.py --crop`). It passes on the real
   signal (weapon-crop mean ~1.5 vs noise ~0.28).

Net: the direct post-write composite call works per-player, offline, during demo playback. Its composite-function
targets come from the Andromeda base.

### UI auto-update wiring and per-slot status

The Cam Editor's Customize flow renders skins without a manual command. The UI (`Panorama/CameraEditorJs.h`
`applyCosmeticCommand`) sends `cosmetics player ... weapon <def> paint <pk>` and `enabled 1`. `m_autoComposite`
(default ON, set with `cosmetics autocomposite 0|1`) controls whether `RunFrame` passes
`fireDirectComposite=m_autoComposite` to `ApplyMatchedWeapons`. The direct composite is change-gated
(`needComposite || attr.changed`), so it fires once on a skin-data change and fires again after a seek or redeploy
recreates the entity. No fallback identity is needed. The composite renders with `m_iItemIDHigh` left valid. It reads
our `def6` networked attribute and the `setAttributeValueByName` call inside the refresh.

Verified live (cropped, paused, noise-floor-controlled), replaying *exactly* the UI's commands:

| Slot | Status | Notes |
|---|---|---|
| Primary (e.g. SSG08 def 40) | WORKS | autocomposite OFF→mean 0.24 (no render), ON→mean 6.36 (full weapon repaint). |
| Secondary (e.g. USP-S def 61) | WORKS | UI command path mean 2.97 vs noise 0.17. |
| Knife: paint, player already has a custom knife | Expected to work (same composite path) | NOT verified live; the test demo's spectated players carry default knives. |
| Knife: default knife / change knife TYPE | Model-swap path, opt-in | Default knife has `networkedAttrs count=0` (no attrs to overwrite) and is not a paintable composite target (engine logs `vCompMat ... did not affect any target materials on knife_default_ct`). Writing `m_iItemDefinitionIndex=507` alone does not swap the renderable model (worldModel stays `knife_default_ct`). The model is swapped by the model-swap path (see below). |
| Gloves | Model-swap path | Gloves are not a scanned weapon entity. They live on the pawn (`C_CSPlayerPawn::m_EconGloves`, an embedded `C_EconItemView`). The glove composite-owner pointer is unknown. Gloves are applied to the HUD-arms entity by the model-swap path (see below). |
| Agents | Model-swap path | Full player-model swap, not a weapon composite. Separate mechanism (`Docs/archive/cosmetics-model-override-research.md`, see below). |

Bottom line: weapon (primary/secondary) skins auto-apply and render through the UI. Knife paint should work for players
who already own a custom knife. Gloves, agents, and knife TYPE swaps are delivered by the model-swap path below.

### Model-swap path

The model-swap path (`CosmeticModelSwap.cpp`, `C_BaseModelEntity::SetModel` + `SetMeshGroupMask` + `UpdateSubclass`
+ glove body group, all resolved on the live build) fires `CGameSceneNode::PostDataUpdate` (vtable index 22,
from Andromeda) after each write. `CosmeticOverrideSystem::MaybeFireTickNudge` briefly resumes the demo about 10 ticks
after a change, then re-pauses ("let the game play ~10 ticks", automated). Together these make agent, glove,
knife-type, and legacy-mesh swaps render. The renderable refresh and live frames are what make the writes visible.
Verified live: the weapon skin is VISIBLE in place, and the agent `m_ModelName` flips `ctm_st6`->`ctm_fbi`
(authoritative). All model-swap functions resolve. Details are in `Docs/cosmetics/cosmetics-cs2-methodology-notes.md`. View body swaps show in third person (`mirv_filmmaker follow preset behind` + `follow place`). A first-person view
shows only the viewmodel.

### Knife type swaps are opt-in

A debug log records the crash path after a UI customizer pick: `knife steamId=... def=500`, then
`cosmetics.apply ... knifeSwap=1`, then a short tick nudge, then CS2 Breakpad reset messages when the player
switched weapons.

The paint/composite path remains usable. The knife TYPE model/subclass swap is not production-safe in demo
playback. It defaults OFF behind `mirv_filmmaker cosmetics modelswap knife 1` for focused testing. Normal customizer
use can still write the profile without firing `ApplyKnifeModelSwap`.

### Reset scope, knife resources, and viewmodels

Three separate behaviours are handled:

1. Cosmetic profiles are runtime-only. The compatibility JSON at `%APPDATA%\HLAE\cosmetic_profiles.json` is normalized
   to `{"enabled":false,"players":{}}`. State is cleared on demo path changes and on a large regression to the first
   second, so a close, reload, or demo restart does not reapply stored edits.
2. The Karambit fallback uses `knife_karambit/weapon_knife_karambit.vmdl`, which matches live `items_game.txt` and VPK
   contents. The Bowie, Navaja, and Talon fallbacks use the same current resource naming.
3. The apply loop resolves the original owner's pawn for knife and weapon model refresh. The first-person mesh branch
   refreshes weapon children under `m_hHudModelArms`, and again after the direct composite call. Gloves do not set the
   `first_or_third_person` body-group choice. The engine chooses the pawn body group, and the selected glove
   definition's `agents/models/shared/arms/...` model is applied to the HUD-arms entity.

These changes are build-verified. Final visual behavior requires live demo verification.

## Investigation tooling

Two console commands drive the investigation in game (both in `AfxHookSource2/Filmmaker/Cosmetics/`):

- `mirv_filmmaker cosmetics visualdiag` (`CosmeticDebug.cpp::Cosmetics_PrintVisualDiag`,
  read-only): dumps the spectated weapon's full visual-cache state: entity index and class, owner xuid,
  defIndex, `m_iItemIDHigh/Low`/`m_iAccountID`, the fallback paint/wear/seed/stattrak, the
  `m_NetworkedDynamicAttributes` values for def 6/7/8/81, the world-model path, and the active-weapon
  handle. It also prints the three visuals-cache flags and their resolved schema offsets
  (`m_bVisualsDataSet`, `m_bClearWeaponIdentifyingUGC`, `m_nCustomEconReloadEventId`). The printed offsets
  are what an IDA/Ghidra pass needs to find the consuming function (search for byte access at
  `weapon_base + <offset>`).  Run it before and after a skin apply or rebuild to compare.
- `mirv_filmmaker cosmetics rebuild once` / `rebuild auto 0|1`
  (`CosmeticOverrideSystem.cpp::RebuildOnce` / `SetRebuildAuto`): `rebuild once` re-asserts the
  visuals-stale field writes (`m_bVisualsDataSet=false`, `m_bClearWeaponIdentifyingUGC=true`, bump
  `m_nCustomEconReloadEventId`, clear init flags, bump attr parity) on every matched weapon
  entity right now, separate from per-frame change detection. It also fires the recompose vcall once if
  it is armed (`cosmetics recompose 1`). `rebuild auto` controls whether the per-frame loop does that
  stale-marking on change. Caveat: these commands re-issue field writes that stick but do not render on their own. They make the
  field-poke path triggerable by hand for A/B screenshots, and they provide an on-demand slot for firing a resolved
  rebuild function. By themselves they do not change the render; the composite call does.

- `mirv_filmmaker cosmetics paintkitbridge [0|1|auto|force <paint>]`
  (`CosmeticOverrideSystem.cpp`): a deploy-time workaround around CS2's own `cl_paintkit_override`. `auto`
  follows the paint kit of the currently spectated weapon that has a profile. `force <paint>` supports automation
  and manual proof-of-life without editing stored profiles. It is labeled experimental and global on purpose,
  because it cannot target one player after the composite has already been built.

## tl;dr

The skin's composited material is built from the weapon entity's lifecycle, not polled from the econ fields. It runs
from the weapon's `OnDataChanged()` or visibility-change callback, which fires on the *networked* update path. A raw
out-of-band memory write from this DLL does not trigger that callback, so the fallback fields are correct but are not
consumed by a rebuild. The working path runs the Andromeda direct composite functions after the write
(`cosmetics composite once`), which builds the composite directly.

## Binary analysis: the trigger is `OnDataChanged`

A static pass over the current-build `client.dll`
(`F:\SteamLibrary\...\csgo\bin\win64\client.dll`, 37 MB) used capstone and pefile. Findings, strongest first:

1. The CS:GO-legacy `cstrike15` weapon code is compiled into CS2's `client.dll`. The build embeds source-path
   strings such as `...\game\shared\cstrike15\weapon_csbase.cpp`, `weapon_knife.cpp`, and `weapon_csbasegun.cpp`.
   The same `OnDataChanged` and custom-material call sites exist as real functions in the running binary.
2. CS2 uses the Source 2 "composite material" (`.vcompmat`) system. The binary carries `CompositeMaterialGameSystem`
   and `CCompositeMaterialManager`, the paint-kit templates `workshop/paintkits/templates/*.vcompmat`, and a
   `composite_material_*` convar family. The rebuild is the (re)build of this weapon's composite material. The
   `m_vecCompositeMaterialAssemblyProcedures` / `CompositeMaterial_t` family is the relevant system.
3. The rebuild trigger is the weapon's `OnDataChanged`, which is virtual. It calls the non-virtual
   `UpdateCustomMaterial`. The networked field `m_nCustomEconReloadEventId` supports this. Its name reads "reload the
   custom econ visuals when this changes", and it is a token checked for changes in `OnDataChanged`. A vtable call
   (`SafeVCall`) can reach `OnDataChanged`.
4. `CosmeticOverrideSystem.h` documents that `vtArg=0` tests `OnDataChanged(DATA_UPDATE_CREATED)`, the candidate
   skin-rebuild trigger. The vtable index for `OnDataChanged` is slot 11 (see Live in-game verification).
5. The override does not render when it changes econ data after the composite build. Real demo weapons render their
   *real* skins, so the composite is built during demo playback, at entity creation. Nothing re-runs `OnDataChanged`
   after an out-of-band write. Re-running the create or data-changed path for an already-created weapon is the
   required step.
6. Independent confirmation from a working tool. `yuzhouUvU/cs2_weapons_skin` (client-side) applies the skin and
   then forces a visual refresh by destroying and recreating the weapon entity (`RemoveWeapon` → entity-system
   `EntityRemove` → `GiveNamedItem`). The natural creation-time composite then consumes the new fallback values in
   `OnEntitySpawned`. It never calls a manual rebuild. The lesson matches `cs2-WeaponPaints`' `!kill` requirement.
   Those are server-authoritative calls, unavailable during demo playback (no server simulating the world).

Concrete anchors in the current build:
`C_CSWeaponBase` main vftable `@ 0x181a2a798`; `C_EconEntity` main vftable `@ 0x181b5f570`;
`C_CSWeaponBase::GetEconWpnData` accessors `@ 0x180796d50` / `@ 0x180796de0` (cached at
`weapon+0x1cc0`); `m_bVisualsDataSet` / `m_nCustomEconReloadEventId` netvar strings
`@ 0x181a26240` / `@ 0x181a26268`. RTTI is walkable both statically (confirmed) and at runtime via
the repo's existing `FindClassVtable`/`getVTableFn`.

## Live in-game verification

Verification used a live demo (USP-S / SSG08 of a spectated pro). Artifacts are under
`Automation/output/ondatachanged_bisect/` (git-ignored). Tooling: `mirv_filmmaker cosmetics vtprobe <idx>`
(calls `weapon->vtable[idx](this,0)` after marking visuals stale, SEH-guarded), the `reloadevent` lever (sets
`m_nCustomEconReloadEventId` to `-1`, `CosmeticOverrideSystem.cpp`), and `Automation/verify/verify-ondatachanged-bisect.ps1`.
Findings:

1. The OnDataChanged slot is 11. `C_CSWeaponBase` vtable slot 11 (`0x1807aa6d0`) chains to the C_EconEntity base. Only if
   `m_nCustomEconReloadEventId < 0` does it call the composite manager's `"clientside_reload_custom_econ"` and
   store a fresh reload id. Live, after `vtprobe 11`, the field went `-1 → 272` (a deterministic id), which shows the
   reload block ran. The field must be `-1` for this block to run; a positive value takes the `jge skip` branch.
2. Slot 11 is safe to call. Blind probing is not. A probe of slot 4 (another DataUpdateType_t-taking virtual) returned
   `faulted=0`, but the game crashed (`ResetBreakpadAppId`) immediately after. SEH catches access violations, not
   logic corruption. Slot 11 was identified from the offset xref to `m_nCustomEconReloadEventId @+0x18bc` (the function
   that writes that field), not by crash-bisecting candidates.
3. The econ DATA path works end to end. With the override on the held weapon's slot (`weapon 0` is primary, and a
   held USP-S is *secondary*, so use `weapon 61`), the live `m_NetworkedDynamicAttributes` def6 became our paint and
   the fallback fields updated. In fallback mode (`m_iItemIDHigh=-1`), the in-game HUD weapon name showed the
   override skin (USP-S read "Kill Confirmed" for paint 504). The data, identity, and name layers fully consume the
   write.
4. But the rendered composite MATERIAL does not regenerate. The HUD name flipped and `def6` and the fallback fields
   all read our paint, yet the 3D view model kept its *original* composited skin. USP-S stayed visually "Check Engine",
   and direct crop A/B of view-model pixels were identical. This held for `arg=0` and `arg=1`, paused and resumed,
   fallback on and off, and after `mat_reloadallmaterials` (no effect). `m_bVisualsDataSet` reads 0 even for a
   correctly rendered stock skin, so it is not the rebuild gate.
5. Runtime composite override is achievable with the engine's own dev lever. `sv_cheats 1;
   cl_paintkit_override <N>` makes weapons render paint kit `<N>`. A freshly deployed SSG08 rendered the override
   finish, while `mat_reloadallmaterials` alone did nothing. `cl_paintkit_override` applies at composite build time
   (weapon deploy or create) and is global (all weapons). The composite is therefore built from the econ item at
   create or deploy and cached on the renderable. The per-frame poll writes *after* that build, and nothing
   invalidates the cached composite material instance.

Net: the slot-11 reload (`reloadId=-1`, `clientside_reload_custom_econ`) drives the econ identity and HUD name. It does
not force a composite-material regeneration on an already-built renderable. `cl_paintkit_override` does regenerate it,
at build time.

### Write-based approaches that do not render

None of the following renders a per-player override (each verified live with screenshot diffs):
- per-frame econ write (def6 + fallback): HUD and name only, never the composite;
- slot-11 reload (`reloadId=-1` + `OnDataChanged`), `arg=0` and `arg=1`, fallback on/off, paused/resumed;
- visuals-invalidate (`m_bVisualsDataSet=0`) + reload;
- `mat_reloadallmaterials`;
- seek-recreate with the override pre-written (clean A/B at one tick: cosmetics-off real skin
  `def6=796` vs cosmetics-on `def6=504` after recreate gave weapon-region pixel diff max=24, meaning no
  skin change; HUD still "Check Engine");
- `cl_paintkit_override <N>` on an already-deployed weapon (in place): no change (mean pixel diff 0.04).
  It affects a weapon only at its next deploy, and then globally.

Root cause, confirmed: the weapon's skin composite is built once, synchronously, at entity create or
deploy, from the *authoritative networked econ item* (the GC item behind `m_iItemIDHigh`, paint 796 here). It is
not built from our locally edited `m_NetworkedDynamicAttributes` def6. It is then cached on the renderable. Our
per-frame write lands after that build. The demo's full update at a seek re-sends 796 and builds before our
`RunMainThreadFrame` overwrites it to 504. Nothing out-of-band invalidates the cached composite.
`cl_paintkit_override` works because it is consulted inside the build and overrides the value the build uses.

A write alone does not render a per-player skin. The composite build has to run after the write, which the direct
composite call (`cosmetics composite once`) provides.

## Mechanism

### What we know for certain (CS:GO source, MIT/leak; direct ancestor of CS2's econ system)

CS2's `C_EconEntity`/`C_EconItemView` schema is a near-literal continuation of CS:GO's
`CEconEntity`/`CEconItemView`. The field names match (`m_nFallbackPaintKit`, `m_flFallbackWear`,
`m_nFallbackSeed`, `m_nFallbackStatTrak`, `m_AttributeManager`), as confirmed by comparing
`AfxHookSource2/SchemaSystem.h:93-119` with the CS:GO-era econ headers. In the legacy
`CWeaponCSBase::OnDataChanged`, the client builds the custom weapon material on
`DATA_UPDATE_CREATED` (`UpdateCustomMaterial()`), and re-checks it when the first-person
spectated state flips (`CheckCustomMaterial()`). The weapon renders with its original material for a
few frames until the custom one is ready.

Key facts this establishes:
- `UpdateCustomMaterial()` / `CheckCustomMaterial()` are private, non-virtual member functions
  of the weapon class (declared
  in a private `CLIENT_DLL`-only block alongside `bool m_bVisualsDataSet;`, with no `virtual`
  keyword). They are not reachable via a vtable call. `OnDataChanged`, which is virtual, is the vtable entry point
  that reaches them (slot 11, see Live in-game verification). A vtable index that lands on an unrelated virtual
  (e.g. `UpdateVisibility`) can still have side effects.
- The trigger is the entity lifecycle, not a per-frame field poll. The rebuild fires from exactly two
  conditions: (a) `DATA_UPDATE_CREATED`, meaning the entity was just (re)created on the client. This happens on
  weapon pickup, `!kill` plus respawn, or round start. (b) `bChangedCarryState`, meaning the weapon's carried or
  dropped state, or its first-person-spectated state, flipped since the last network update. Neither condition is
  met by silently overwriting `m_nFallbackPaintKit`/`m_iItemIDHigh` on an already fully created, already carried
  weapon outside the network-update path. `ApplyCosmeticWrite` does exactly that every frame. This is the root
  cause. The write is correct, but nothing downstream runs, because neither trigger fires.
- `m_bInitialized`/`m_bInitializedTags` are not material-rebuild flags. The CS:GO leak's
  `CEconItemView::Init()` sets `m_bInitialized = true`, then calls `MarkDescriptionDirty()`. That flag belongs to the
  inventory tooltip and description text cache, which is unrelated to the 3D render path. Toggling these to
  `false` (as `ApplyCosmeticWrite` does at `CosmeticOverrideSystem.cpp:120-123`) is harmless. It does not cause a
  re-render. At most it invalidates a cached UI string. This matches the in-game result: clearing these flags alone
  does not visually rebuild the skin.

### Real-world corroboration (CS2-specific, current build)

Two independent live-CS2 data points corroborate the "lifecycle trigger, not per-frame" model
above:

1. A documented player-facing fact: pressing Q (quick-switch) is known to force the engine to redraw the
   `C_WeaponCSBase` model. It is a manual fix when a weapon skin or viewmodel desyncs after fast
   `subclass_change` or skin-changer console spam. Swapping weapons causes `m_iState` and carry-state changes,
   including `WEAPON_NOT_CARRIED` transitions, which are exactly `bChangedCarryState` above. That makes it an
   established empirical "make the skin redraw" trick.
2. `Nereziel/cs2-WeaponPaints` (`https://github.com/Nereziel/cs2-WeaponPaints`), the most widely
   deployed CounterStrikeSharp (server-side) skin plugin, documents in its own setup instructions
   that knife skins require the player to `!kill` (die/respawn) to correctly apply visually.
   Respawn destroys and recreates the weapon entity, which forces `DATA_UPDATE_CREATED`. That is the first
   branch in `OnDataChanged` above. This is an independently documented confirmation, for the CS2 current build,
   that the create or recreate path is what makes a changed skin render. It holds even when the server plugin
   has already written the correct econ attributes.

Both are server-plugin-adjacent observations. The skin data itself arrives over the network in those cases. The
*visual-refresh trigger* they rely on is purely client-side entity lifecycle behavior, the same client.dll code path
this DLL would need to invoke or imitate.

### CS2 schema evidence (current-build-relevant, not CS:GO-only)

A live source2gen-style dump of current `client.dll` (`a2x/cs2-dumper`,
`https://github.com/a2x/cs2-dumper/blob/main/output/client_dll.hpp`) shows
`C_EconEntity` (via `cs2-sdk`'s mirror of the same generator,
`https://raw.githubusercontent.com/NotOfficer/cs2-sdk/master/client.hpp`) carries the following field,
which this repo's `SchemaSystem.h` resolves:

```cpp
class C_EconEntity : public C_BaseFlex
{
	...
	bool m_bAttributesInitialized;          // resolved in AfxHookSource2/SchemaSystem.h/.cpp
	C_AttributeContainer m_AttributeManager;
	uint32_t m_OriginalOwnerXuidLow;
	uint32_t m_OriginalOwnerXuidHigh;
	int32_t m_nFallbackPaintKit;
	int32_t m_nFallbackSeed;
	float m_flFallbackWear;
	int32_t m_nFallbackStatTrak;
	bool m_bClientside;
	bool m_bParticleSystemsCreated;
	...
};
```

`m_bAttributesInitialized` lives on `C_EconEntity` (the weapon or wearable entity itself). It is a structurally
different flag from `C_EconItemView::m_bInitialized` (the embedded item view, already wired up in
`g_clientDllOffsets.C_EconItemView.m_bInitialized`). The repo resolves it and clears it during writes. Live testing showed no visual rebuild from that lever alone.

The same current dump also shows `C_EconItemView::m_iItemID` (a single `uint64`), where this repo's
`SchemaSystem.h:109-111` still resolves separate `m_iItemIDHigh`/`m_iItemIDLow` (`uint32` each). Both namings may
coexist across builds and dump tools. A `uint64` field and a high/low pair can be the same bytes viewed two ways, or
the schema may differ between builds. This is flagged as a discrepancy to re-check with `Misc/sigscan.py` or the schema dumper
against the actual running build. This research could not resolve it from dumps alone.

## Flag semantics (confirmed vs. hypothesis)

| Field | Class | Confirmed meaning | Confirmed effect of writing it |
|---|---|---|---|
| `m_iItemIDHigh = -1` | `C_EconItemView` | "Don't trust networked econ item; use my fallback fields." This is the one part of the existing pipeline known-good (write sticks, matches CS:GO's "fallback when networked item missing" design intent). | Necessary, not sufficient. Confirmed by in-game test: write sticks every frame, skin still doesn't render. |
| `m_nFallbackPaintKit`/`m_flFallbackWear`/`m_nFallbackSeed`/`m_nFallbackStatTrak` | `C_EconEntity` | The actual values the (unknown, unreached) composite step is supposed to read once it runs. | Write sticks (confirmed). No visual effect observed because the consuming step never runs (per Mechanism section above). |
| `m_bInitialized` | `C_EconItemView` | Confirmed (CS:GO leak): inventory description-text cache flag, set by `CEconItemView::Init()`, consumed by `MarkDescriptionDirty()`/`EnsureDescriptionIsBuilt()`. Not part of the 3D render path. | Toggling it is harmless but inert for rendering, which matches this repo's empirical finding. |
| `m_bInitializedTags` | `C_EconItemView` | Hypothesis: a sibling flag for a second dirty-cache (e.g. "tag"/sticker description cache), by naming analogy with `m_bInitialized`. Not separately confirmed in the CS:GO leak excerpts. | Same as above. No evidence it touches rendering. |
| `m_bAttributesInitialized` | `C_EconEntity` | Current-build schema field, resolved by the repo. Plausibly the entity-level "have my attributes been consumed into visual/render state" flag. | Cleared during writes. Live testing showed it is insufficient by itself; no visible skin rebuild on an already-held default AK. |
| `m_NetworkedDynamicAttributes` | `C_EconItemView` | Confirmed present in current schema (already resolved, `AfxHookSource2/SchemaSystem.h`, `SchemaSystem.cpp`). Per the existing in-repo comment, this is where a networked/spectated item's live paint kit/wear/seed actually live, as opposed to `m_AttributeList` (the local/cooked list, empty for demo players). | The apply loop overwrites existing def 6/7/8/81 values. It cannot currently create missing attributes, so a default AK with an empty networked list still falls back to the unresolved material-rebuild problem. |

Is there a separate "composite generation token" that must be bumped? Not confirmed either
way. CS2's offset dumps do show an unrelated `CompositeMaterial_t`/
`CompositeMaterialAssemblyProcedure_t` enum/struct family (seen in `cs2-dumper`'s output via the
`m_vecCompositeMaterialAssemblyProcedures`/`m_vecCompositeMaterials` offsets surfaced during this
search), but the owning class could not be found or confirmed as the same
"composite" as econ skin compositing (Source 2's general-purpose material-compositing system is
also used for unrelated things, e.g. procedural decal/wear systems). Flagged as a loose thread,
not a finding.

## Viewmodel vs. world model

Per `C_EconItemView`, not per entity, but two separate entities each hold their own view.
Confirmed from the CS:GO architecture (the same `C_EconEntity`/`m_AttributeManager.m_Item`
relationship CS2 inherited): a weapon's first-person viewmodel and its third-person world model
are typically driven by two different client entities that both reference the same underlying
econ item data (this repo's model-override research, `Docs/archive/cosmetics-model-override-research.md`
§3, independently confirms CS2 has `C_BaseViewModel::m_hWeapon`/`m_hWeaponModel` as separate
handles, and that nSkinz on CS:GO explicitly set the model index on both `view_model` and
`world_model` as two separate writes; see that doc's §2). The practical implication for the
recompose problem specifically:

- If the real fix is "call the rebuild function on the weapon entity," it needs to run once per
  visible representation, i.e. potentially twice (viewmodel weapon entity and world-model
  weapon entity), not once. `CosmeticOverrideSystem::RunFrame`'s current loop
  (`CosmeticOverrideSystem.cpp:336-422`) walks all entities and applies to anything matching
  `LooksLikeWeaponEntity`. This would already iterate over both the viewmodel and world-model
  weapon entities if they are both present as separate indices in the entity list (consistent with
  how `m_OriginalOwnerXuidLow/High` ownership resolution already works generically per-entity in
  that loop). No code change needed here if the rebuild call is added at the same per-entity
  granularity the write already happens at, assuming the viewmodel weapon and world-model weapon are in fact separate
  `C_EconEntity`-derived entities in CS2 (consistent with, but not separately re-confirmed in, the
  model-override doc's findings).
- This is not separately confirmed for CS2's current build (no live introspection of whether
  CS2 demo-spectator playback actually instantiates two distinct weapon entities for viewmodel vs.
  world model, vs. a single entity with the engine handling both renders from one econ item). The
  existing `Cosmetics_RunFrame` loop already iterating "every entity that looks like a weapon"
  means this should be moot in practice. Whatever entity-level fix is found will naturally apply
  to both representations without additional code, *if* they are in fact separate entities. If
  CS2 uses a single entity for both, there is nothing extra to do at all.

## Confidence / risk note

- High confidence: the existing fallback-field write mechanism is correct and necessary
  (matches CS:GO's designed-for-this fallback path 1:1, and is independently confirmed sticking in
  live testing).
- High confidence: `m_bInitialized`/`m_bInitializedTags` are inventory-description-cache
  flags, not render triggers, based on directly-read CS:GO leak source, not inference.
- High confidence: the real trigger is entity-lifecycle-based (`OnDataChanged` /
  `DATA_UPDATE_CREATED` / carry-state change), not a per-frame poll of the fallback fields, based
  on the same leak source plus two independent CS2-current-build corroborations (Q-switch redraw
  folklore, cs2-WeaponPaints' documented `!kill` requirement for knives).
- High confidence: the rebuild function itself is non-virtual in the CS:GO architecture, so a
  vtable-index approach is very unlikely to be the right mechanism even if some index happens not
  to fault.
- Medium confidence / hypothesis: `C_EconEntity::m_bAttributesInitialized` is the relevant
  current-build gate. This is an informed guess from the field's name and its class (entity-level,
  not item-view-level), not confirmed by reading any function body, because no public source
  shows CS2's actual attribute-consumption code. Cheap and safe to test directly, because testing it
  costs nothing more than the same kind of write the existing pipeline already performs safely.
- Low confidence / open problem: no concrete signature, xref chain, or stable string reference
  for the actual rebuild function was found. This is the one piece of the research goal
  ("candidate signature for the composite function") that remains open. Closing this
  gap needs either disassembler access to a live `client.dll` or a not-yet-found public reversing
  resource. The search covered `a2x/cs2-dumper`, `sezzyaep/CS2-OFFSETS`, `NotOfficer/cs2-sdk`,
  `Aspasia1337/Aspasia`, `samyycX/awesome-cs2` and general web search without success.
- Server-side techniques excluded as non-viable, confirmed: CounterStrikeSharp/SwiftlyS2/
  Metamod skin plugins (`Nereziel/cs2-WeaponPaints`, `samyycX/WeaponSkins`,
  `K4ryuu/K4-AlwaysWeaponSkins`, `yuzhouUvU/cs2_weapons_skin`) all operate server-side and rely on
  the engine's own networked attribute-update path to drive the client's normal `OnDataChanged`
  dispatch, a mechanism categorically unavailable to a client-only DLL replaying an offline demo
  (no server is simulating the world during demo playback, per `CLAUDE.md`'s constraints). Their
  existence and design are useful evidence for the trigger mechanism (this doc relies on that), but
  none of their code is directly portable, since they never need to solve "force a rebuild
  without a network event." The network event is what they use instead.

## Sources

- CS2 current-build schema dumps (source2gen family, cross-referenced against each other):
  - `https://raw.githubusercontent.com/NotOfficer/cs2-sdk/master/client.hpp`:
    `C_EconEntity` full field list including `m_bAttributesInitialized`
  - `https://raw.githubusercontent.com/a2x/cs2-dumper/main/output/client_dll.hpp`:
    `C_EconItemView` field list (`m_bInitialized` @ 0x1E8, `m_bInitializedTags` @ 0x468,
    `m_NetworkedDynamicAttributes` @ 0x280, `m_iItemID` as single uint64. Note vs. this repo's
    `m_iItemIDHigh`/`m_iItemIDLow` split, flagged as unresolved discrepancy above)
  - `https://github.com/sezzyaep/CS2-OFFSETS` (cross-reference, not separately quoted)
- CS2-current corroboration (player/operator-facing, not source-level):
  - `https://github.com/Nereziel/cs2-WeaponPaints`: documented `!kill` requirement for knife
    skin application (entity-recreate-triggers-rebuild evidence)
  - Q-switch-forces-redraw folklore (general CS2 console-command community knowledge, surfaced via
    web search; no single authoritative URL. Treat as corroborating anecdote, not a
    primary source)
- Local repo (current state):
  - `AfxHookSource2/Filmmaker/Cosmetics/CosmeticOverrideSystem.cpp` (the apply loop;
    `SafeVCall`/`ApplyCosmeticWrite`/`RunFrame`)
  - `AfxHookSource2/SchemaSystem.h` / `AfxHookSource2/SchemaSystem.cpp` (`ClientDllOffsets_t`,
    `initCosmeticsOffsets`, `getOffsetsFromSchemaSystem`, the `getAddress`-based AOB scan used for
    the schema-system interface itself, the existing precedent for how a new sigscan would be
    wired in)
  - `Misc/sigscan.py` (the repo's existing pattern-validation tool)
  - `Docs/archive/cosmetics-model-override-research.md` (the separate model-swap
    problem; cross-referenced for the viewmodel/world-model entity split and the
    `C_EconEntity`/`C_EconItemView` field-name continuity argument)

## Reference comparison

The practical implementation guidance lives in `Docs/cosmetics/cosmetics-cs2-methodology-notes.md`,
section 9, after reviewing three reference repos. The takeaways are:

- The direct composite path remains necessary for weapon skins, but it is only one part of the visible
  update. The references also update item identity/cache flags, mesh group masks, viewmodels, HUD icon
  caches, and sometimes item descriptions.
- For demo playback, overwriting `m_NetworkedDynamicAttributes` works only when the demo entity already
  has def 6/7/8/81 attributes. If `attrWritten=0`, fallback fields alone are usually insufficient; we
  need an attribute-creation or `SetAttributeValueByName` fallback like the references.
- The deeper `real-time-internal-overlay-research-main` read adds one more likely cache gate:
  copied/synthetic item identity plus `C_EconItemView::m_bDisallowSOC = false`. Its source calls this
  flag write "the fix" before copying fake loadout item IDs into the live weapon item view. In demo
  mode we should not port fake inventory ownership, but we should test session-local synthetic item IDs
  and `m_bDisallowSOC=false` alongside the missing-attribute fallback.
- `real-time-internal-overlay-research-main` also proves StatTrak/nametag and knife viewmodel rebuilds
  are separate refresh surfaces: it calls `AddStattrakEntity()`, `AddNametagEntity()`, and hooks
  `C_BaseModelEntity::SetModel` to rewrite viewmodel knife models before the engine draws the default
  model. Composite refresh alone will not solve those visible surfaces.
- Default knives and gloves are special because they often have no paintable networked attribute vector
  and may not have target materials for the requested paint. Treat `attrWritten=0` as a first-class
  diagnostic result, not as "the composite call failed."
- Gloves and agents are not solved by weapon recomposition. Gloves are pawn `m_EconGloves` plus body
  group / HUD arms refresh; agents are pawn `SetModel`.
- The current demo architecture should stay per-SteamID/per-entity. Do not port the local-player
  inventory/loadout model wholesale.
