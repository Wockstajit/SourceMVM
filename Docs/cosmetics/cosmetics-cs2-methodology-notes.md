# Cosmetics CS2 methodology notes: knife, glove, and agent model swap (from working source)

Status: methodology and implementation notes from working source. Sections 1 to 5 hold the reference
comparison and recipes. Section 6 covers the renderable refresh, section 7 the demo-playback behaviour,
and section 8 the knife-swap safeguards. Sections 9 and 10 cover the demo-playback design rules and the glove
HUD arms. For the current file map and supported user surface, start with
[`cosmetics-overview.md`](cosmetics-overview.md).

> **Implementation file map.** The Cosmetics code is split into
> single-purpose translation units. See
> [cosmetics-overview.md](cosmetics-overview.md) for the full map:
> - AOB and vtable resolution, plus the SEH wrappers (`SafePostDataUpdate`, `SafePrecacheModel`, `GetStaticData`
>   lookups): `CosmeticFnResolver.cpp` (shared `Fns` table in `CosmeticModelSwapInternal.h`)
> - knife-type swap (`ApplyKnifeModelSwap`, the `knife.swap` and `knife.vm` breadcrumbs): `CosmeticKnifeSwap.cpp`
> - gloves (`SafeApplyGloves`): `CosmeticGloveSwap.cpp`
> - seek detection and tick nudge (`MaybeFireTickNudge`, `OnDemoSeekDetected`): `CosmeticDemoSync.cpp`
> - `cl_paintkit_override` bridge: `CosmeticPaintKitBridge.cpp`
> - direct composite (`ResolveDirectCompositeFns`, `FireDirectCompositeRefresh`): `CosmeticDirectComposite.cpp`
> - `CosmeticModelSwap.cpp` keeps the HUD-arms and viewmodel machinery, the weapon mesh mask and the agent swap.
>   `CosmeticOverrideSystem.cpp` keeps the profile match and apply loop.

> **Client note:** for a paint-only weapon refresh, do **not** append
> `CGameSceneNode::PostDataUpdate` to the direct composite chain. NetCon stage isolation on client build
> 14172 showed that the named attribute setters, `UpdateCompositeMaterial(weapon + 0x608)`,
> `UpdateCompositeMaterialSet`, and `UpdateSkin` are each stable. `PostDataUpdate` on that weapon scene
> node returns, then hangs the game asynchronously. The current safe chain is those first four stages
> only. Verified on a fresh current-build demo: FAMAS paint kit 1184 rendered visibly, four consecutive
> manual refreshes returned, and CS2 stayed responsive. Other model and body swap paths may still use
> their own guarded `PostDataUpdate`. This finding applies to the weapon paint-composite path.

> **Model swap is client-side.** Both reference implementations resolve
> **`C_BaseModelEntity::SetModel(const char*)` as a normal `client.dll` function by AOB**, and they call
> it every frame to change knife and agent models. The signature is identical in both code bases (see
> the table in section 5). The model swap is a client-side call, not a server call. The knife-type swap
> and the agent swap use this lever.

Both tools operate on the **local player's own loadout** in live and offline play. They read the real
`CCSPlayerInventory` and copy item identity into the live weapon entities. The *rendering mechanism* they
use is fully client-side and independent of the server, and that is the part that ports. The
demo-playback adaptation (applying to a **spectated** pawn or weapon instead of the local pawn) and its
caveats are covered in §7.

---

## 1. Two sources, what each covers

| | Andromeda-CS2-Base-master | nerv |
|---|---|---|
| Knife model+skin swap | Yes: `CInventoryChanger::OnFrameStageNotify` | Yes: `c_skin_changer::process_knife` |
| Weapon skin (paint) | Yes: via `SetAttributeValueByName` + fallback fields | Yes: via hand-built `m_AttributeList` |
| Gloves | Yes: `CInventoryChanger::SetGlove` | Yes: `c_glove_changer::run` |
| **Agent / player model** | Yes: `CInventoryChanger::SetAgent` | No (has `is_agent()` helper, no feature) |
| Function resolution style | central `CFunctionList` (AOB table) | inline `g_opcodes->scan(...)` per call site |
| Frame hook | `FrameStageNotify`, `FrameStage == 6` | `FrameStageNotify`, `stage == 7` |

Source files read for this comparison:
- Andromeda: `AndromedaClient/Features/CInventoryChanger/CInventoryChanger.cpp`,
  `CS2/SDK/Types/CEntityData.{hpp,cpp}`, `CS2/SDK/CFunctionList.{hpp,cpp}`,
  `CS2/SDK/FunctionListSDK.hpp`, `CS2/SDK/Update/Offsets.hpp`,
  `CS2/SDK/Econ/CEconItemDefinition.{hpp,cpp}`.
- nerv: `features/skin_changer/skin_changer.cpp`, `features/glove_changer/glove_changer.cpp`,
  `features/shared/econ_item_attribute_manager.cpp`, `features/shared/item_schema.cpp`,
  `valve/classes/c_cs_player_pawn.hpp`.
- real-time-internal-overlay-research-main: `src/game/skins/skin_changer.cpp`,
  `src/game/game_hooks.cpp`, `src/game/menu/menu.cpp`,
  `src/sdk/source2-sdk/econ/ceconitem.{hpp,cpp}`,
  `src/sdk/source2-sdk/entity/c_econitemview.hpp`,
  `src/sdk/source2-sdk/entity/c_csweaponbase.{hpp,cpp}`.

The references agree on the client-side rendering mechanisms below. Where one repo adds an extra
mechanism, it is called out explicitly.

---

## 2. The knife model + skin swap (client-only)

This is the recipe the repo docs call the "deferred model-swap path." It is a per-weapon-entity
sequence that runs each frame for the held knife.

### Call order (Andromeda `OnFrameStageNotify`, lines 184-220; nerv `process_knife`, lines 99-161)

1. **Point the econ item view at the desired knife identity.**
   - `item->m_iItemDefinitionIndex() = selectedKnifeDef;` (e.g. 500 = Bayonet)
   - `item->m_iEntityQuality() = QUALITY_UNUSUAL;` (nerv; 3)
   - Andromeda copies the whole identity from the loadout item view: `m_iItemID`, `m_iItemIDHigh`,
     `m_iItemIDLow`, `m_iAccountID`, and sets `m_bDisallowSOC=false`,
     `m_bRestoreCustomMaterialAfterPrecache=true`.

2. **Swap the actual model resource (the model lever).** Call the client `SetModel` on both the
   world/weapon entity and the first-person viewmodel knife entity:
   ```cpp
   weapon->SetModel( knifeDef->m_pszModelName() );          // e.g. "weapons/models/knife/knife_bayonet/weapon_knife_bayonet.vmdl"
   if (auto* hud = GetKnifeViewModel(weapon, pawn))
       hud->SetModel( knifeDef->m_pszModelName() );
   ```
   `m_pszModelName` is `CEconItemDefinition + 0x148`. A live dump in Andromeda confirms that def 500
   maps to `weapons/models/knife/knife_bayonet/weapon_knife_bayonet.vmdl`. The viewmodel-knife entity
   is found by walking the HUD-arms scene-node children and matching the one whose owner is the weapon
   (Andromeda `GetKnifeModel`/`GetViewModel`; nerv `get_hud_weapon`).

3. **Fix the mesh group for legacy vs. new models.**
   ```cpp
   weapon->m_pGameSceneNode()->SetMeshGroupMask(meshMask);
   hud->m_pGameSceneNode()->SetMeshGroupMask(meshMask);
   ```
   The mask comes from the paint kit's "uses old/legacy model" flag. **The two code bases disagree on the
   exact value, and the disagreement is specific to knives:**
   - Andromeda (weapons and knife): `meshMask = 1 + isLegacyModel`, so legacy=2 and modern=1.
   - nerv weapon skins: `uses_old_model ? 2 : 1` (same as Andromeda).
   - nerv **knife**: `uses_old_model ? 1 : 2` (**inverted** for knives, so modern=2).
   Treat this as "try 1, if the mesh is wrong try 2"; the legacy flag is read from the paint kit
   (Andromeda `g_CCPaintKit_IsUseLegacyModel = 0xAE`; nerv `pk->uses_old_model()`).

4. **Re-derive the weapon subclass from the new def index.** This makes the weapon adopt the new knife's
   VData/animation set:
   ```cpp
   uint32_t hash = CUtlStringToken(std::to_string(knifeDef).c_str()).GetHashCode(); // murmur2, seed 0x31415926, lowercased
   weapon->m_nSubclassID().SetHashCode(hash);
   weapon->UpdateSubclass();
   ```
   nerv does the same with a hand-rolled `string_token_hash` (murmur2, seed `0x31415926`, input
   lowercased). It writes `m_nSubclassID` and then calls `UpdateSubclass()`. **This is the CS2
   `CUtlStringToken` hash** (see `c_cs_player_pawn.hpp:11-53` for the
   reference murmur2).

5. **Rebuild the composite material (the skin).**
   ```cpp
   weapon->UpdateCompositeMaterial( (CCompositeMaterialOwner*)((PBYTE)weapon + 0x608) );
   weapon->UpdateCompositeMaterialSet();
   weapon->UpdateSkin();          // nerv: vtable[110](force=true)
   ```
   Some references also call `weapon->m_pGameSceneNode()->PostDataUpdate()` after these. Do not add that
   call to the paint-only path, because it hangs CS2 on the current client.
   `0x608` (`g_CompositeMaterialOffset`) is the `CCompositeMaterialOwner` embedded in the weapon. nerv
   also calls `update_weapon_data()` (vtable[195]) and a global `regenerate_skins()`.

6. **Clear the stale HUD weapon icon** so the selection panel redraws with the new knife
   (`CCSGO_HudWeaponSelection::ClearHudWeaponIcon`; both set `item->pCEconItemDescription()/m_name_description_ptr() = 0`).

### Paint attributes (two interchangeable ways to attach paint/seed/wear)

- **Andromeda: engine setter + fallback fields:**
  ```cpp
  weapon->m_nFallbackPaintKit() = loadout->GetCustomPaintKitIndex();
  weapon->m_nFallbackSeed()     = seed;
  weapon->m_flFallbackWear()    = wear;
  C_EconItemView_SetAttributeValueByName(item, "set item texture prefab", paintKit);
  C_EconItemView_SetAttributeValueByName(item, "set item texture seed",  (float)seed);
  C_EconItemView_SetAttributeValueByName(item, "set item texture wear",  wear);
  ```
- **nerv: hand-built `m_AttributeList` vector** (no engine call): allocate 3
  `econ_item_attribute_t` via the game allocator and write the vector pointer/size directly
  (`econ_item_attribute_manager::create`). Attribute def indices: **paint=6, pattern/seed=7, wear=8**.
  Struct layout: `def_index` at `+0x30`, `value` (float) at `+0x34`, `init_value` at `+0x38`. Frees the
  previous block via `GameFree` first. This is the cleaner path when there is no real owned item to copy
  from. For demo playback, the spectated player's networked attributes already exist, so overwrite
  def 6/7/8 in place, rather than allocating.

---

## 3. Gloves (client-only)

Gloves are **not a weapon entity**. They live on the pawn as an embedded `C_EconItemView`
(`C_CSPlayerPawn::m_EconGloves`). The repo handles them in `CosmeticGloveSwap.cpp`, outside the weapon apply loop.

### Recipe (Andromeda `SetGlove` 339-377; nerv `c_glove_changer::run`)

```cpp
auto& glove = pawn->m_EconGloves();          // embedded C_EconItemView on the pawn
glove.m_iItemDefinitionIndex() = gloveDef;   // e.g. 5027/5030/... (NOT 5028/5029 = default)
glove.m_iItemID()  = loadout->m_iItemID();   // Andromeda copies full identity
glove.m_iItemIDHigh() = ...; glove.m_iItemIDLow() = ...; glove.m_iAccountID() = ...;
glove.m_bDisallowSOC() = false;
glove.m_bRestoreCustomMaterialAfterPrecache() = true;

// attach paint (same two options as §2; nerv builds the attribute list, also sets quality=UNUSUAL)
glove.m_iEntityQuality() = QUALITY_UNUSUAL;   // nerv

glove.m_bInitialized() = true;                // <-- gloves use m_bInitialized = TRUE (note: opposite of the "clear init" intuition)
pawn->SetBodyGroup();                          // C_CSPlayerPawn::SetBodyGroup(this, "first_or_third_person", 1)
C_BaseEntity_UpdateBodyGroupChoice(pawn);      // Andromeda only; not called by this repo, see section 6
pawn->m_bNeedToReApplyGloves() = true;         // the engine's own "regenerate gloves next frame" flag
```

### Frame timing matters (both use a multi-frame counter)

Gloves do **not** apply in a single frame. Both run the write for several consecutive frames after a
change:
- Andromeda: on a spawn-time change or `m_bApplyGloves`, set `uUpdateFrames = 3`. Each of the next 3
  frames re-asserts `m_bInitialized=true`, `SetBodyGroup`, `UpdateBodyGroupChoice`, and
  `m_bNeedToReApplyGloves=true`.
- nerv: `m_update_frames = 4` on a config, team, spawn or engine-reset change. A separate
  `m_clear_frames = 2` on a team change first *removes* the attributes and zeroes the def index, then
  the update frames re-apply. Tracked triggers: def-index reset by the engine, `!m_initialized`,
  `m_need_to_reapply_gloves`, spawn-time change, team change, and buy-menu open.

The engine resets gloves on spawn, round and team change. The apply must therefore watch
`m_flLastSpawnTimeIndex`, the team and `m_bNeedToReApplyGloves`, and re-fire.

---

## 4. Agent / player model (Andromeda only; the cleanest swap of the three)

Agents are a **full player-model swap**. Andromeda does it with a **single `SetModel` call on the
pawn**, with no body group, composite or subclass step:

```cpp
auto* loadout = inventory->GetItemInLoadout(pawn->m_iTeamNum(), LOADOUT_SLOT_CLOTHING_CUSTOMPLAYER /*38*/);
auto* def     = loadout->GetStaticData();
const char* modelName = def->m_pszModelName();    // the agent .vmdl path from the item definition
if (modelName && *modelName)
    pawn->SetModel(modelName);                    // same C_BaseModelEntity::SetModel as the knife
```

Guarded by a hash so it only re-sets when the chosen agent changes (`hash_64_fnv1a(modelName)` cached in
a static). Requires `pawn->m_pGameSceneNode()->GetSkeletonInstance()` to be valid first.

`IsAgent` is detected by item type string `#Type_CustomPlayer` (FNV1a-32 hashed); default agents are
def `5036`/`5037` and are skipped (`CEconItemDefinition::IsAgent`). The agent model path comes from the
same `m_pszModelName() @ 0x148` used for weapons/knives.

> The skeleton and animgraph caveat applies in principle (a swapped pawn model must
> be animgraph-compatible). In practice, Andromeda shows that Valve's agent models swap cleanly via a bare
> `SetModel`, because they share the player skeleton family. No sequence-remap hack (the nSkinz
> Source-1 problem) is needed for agents here.

---

## 5. Consolidated signature / offset table (current CS2 build, from both sources)

All in `client.dll` unless noted. Andromeda patterns are the `CFunctionList.hpp` entries; nerv patterns
are the inline `g_opcodes->scan` strings. **Where both list a function, the byte pattern is identical**,
strong cross-confirmation.

| Function | AOB pattern | Resolve note |
|---|---|---|
| `C_BaseModelEntity::SetModel(this, const char*)` | `40 53 48 83 EC ? 48 8B D9 4C 8B C2 48 8B 0D ? ? ? ? 48 8D 54 24 40` | direct (both identical) |
| `CGameSceneNode::SetMeshGroupMask(this, uint64)` | `48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8D 99 ? ? ? ? 48 8B 71` | direct (both identical) |
| `C_CSWeaponBase::UpdateSubclass(this)` | `4C 8B DC 53 48 81 EC ?? ?? ?? ?? 48 8B 41` | direct (both identical) |
| `C_CSWeaponBase::UpdateSkin(this, bool)` | `48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 E8 ? ? ? ? F6 C3 01 74 0A 33 D2 48 8B CF E8 ? ? ? ? 48 8D 8F 60 19 00 00` | Andromeda AOB / nerv vtable[110] |
| `C_CSWeaponBase::UpdateCompositeMaterial(owner, bool)` | `E8 ? ? ? ? 48 8D 8B ? ? ? ? 48 89 BC 24` | CALL-relative (Andromeda) |
| `C_CSWeaponBase::UpdateCompositeMaterialSet(this, bool)` | `40 55 53 41 57 48 8D AC 24 00 FE ? ?` | direct (Andromeda) |
| `C_CSPlayerPawn::SetBodyGroup(this, "first_or_third_person", 1)` | `E8 ? ? ? ? EB 0C 48 8B CF` | CALL-relative, +1 (both identical) |
| `C_BaseEntity::UpdateBodyGroupChoice(this)` | `E8 ? ? ? ? 4C 8B AC 24 ? ? ? ? 48 8B BC 24` | CALL-relative (Andromeda) |
| `C_EconItemView::SetAttributeValueByName(view, name, float)` | `E8 ? ? ? ? 66 41 0F 6E D4` | CALL-relative (Andromeda) |
| `C_EconItemView::GetStaticData(view)` | `40 56 48 83 EC ? 48 89 5C 24 ? 48 8B F1 48 8B 1D ? ? ? ? ...` | direct (Andromeda) |
| `C_EconItemView::GetCustomPaintKitIndex(view)` | `48 89 5C 24 ? 57 48 83 EC ? 8B 15 ? ? ? ? 48 8B F9 ...` | direct (Andromeda) |
| `C_EconItemView::construct_paint_kit(view)` | `48 89 5C 24 ? 56 48 83 EC ? 48 8B 01 FF 50` | direct (nerv) |
| `CCSGO_HudWeaponSelection::ClearHudWeaponIcon` | `E8 ? ? ? ? 8B F8 C6 84 24` | CALL-relative (both) |
| `FindHudElement(name)` | `4C 8B DC 53 48 83 EC ? 48 8B 05` (nerv) / `4C 8B DC 53 48 83 EC 50 48 8B 05` (Andromeda) | direct |
| `CCSPlayerInventory::GetItemInLoadout(inv, team, slot)` | `40 55 48 83 EC ? 49 63 E8` | direct (Andromeda) |
| `CCSInventoryManager::EquipItemInLoadout` | `48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 89 54 24 ? 57 41 54 41 55 41 56 41 57 48 83 EC ? 0F B7 FA` | direct (Andromeda) |
| `regenerate_skins()` (global) | `48 83 EC ? E8 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 48 8B 10` | direct (nerv) |

Constant offsets:
| Offset | Value | Meaning |
|---|---|---|
| `CCompositeMaterialOwner` in weapon | `+0x608` | `g_CompositeMaterialOffset` (Andromeda), the owner passed to `UpdateCompositeMaterial` |
| `CEconItemDefinition::m_pszModelName` | `+0x148` | the `.vmdl` path for knife/glove/agent/weapon |
| `CEconItemDefinition::LoadoutSlot` getter offset | `0x338` | `g_CEconItemDefinition_GetLoadoutSlot` |
| `CCPaintKit::IsUseLegacyModel` | `+0xAE` | legacy-model flag, which selects the mesh group mask |
| Econ attribute def indices | paint=`6`, seed/pattern=`7`, wear=`8` | for hand-built attribute list |
| Loadout slots | melee=`0`, hands(gloves)=`41`, custom-player(agent)=`38`, musickit=`54` | `LoadOutSlot_t` enum (Andromeda hpp) |
| Default items to skip | gloves `5028/5029`, agents `5036/5037` | "default, don't override" |

Schema fields used (resolved via the repo's existing schema system, not hardcoded):
`C_EconItemView`: `m_iItemDefinitionIndex`, `m_iItemID`/`High`/`Low`, `m_iAccountID`, `m_iEntityQuality`,
`m_bInitialized`, `m_bDisallowSOC`, `m_bRestoreCustomMaterialAfterPrecache`, `m_AttributeList`,
`m_szCustomName`. `C_EconEntity`: `m_nFallbackPaintKit/Seed`, `m_flFallbackWear`, `m_nFallbackStatTrak`,
`m_AttributeManager`. `C_BaseEntity`: `m_nSubclassID`, `m_pGameSceneNode`. `C_CSPlayerPawn`:
`m_EconGloves`, `m_bNeedToReApplyGloves`, `m_hHudModelArms`, `m_flLastSpawnTimeIndex`.
`CModelState`: `m_hModel`, `m_ModelName`. `CSkeletonInstance`: `m_modelState`, `m_nHitboxSet`.

---

## 6. Renderable refresh and tick nudge

Writing a model or mesh does not make it render by itself. Two levers make swaps visible:

1. **`CGameSceneNode::PostDataUpdate` (vtable index 22, called `node->vtable[22](node, 0, 0)`).**
   Extracted from Andromeda (`SDK::VMT_Index::CGameSceneNode::PostDataUpdate = 22`,
   `CEntityData.hpp`). The call is SEH-guarded in `SafePostDataUpdate` (`CosmeticFnResolver.cpp`) and
   exposed as `PostDataUpdate` in `CosmeticModelSwap.cpp`. It runs after the agent `SetModel` on the pawn
   and after the mesh-mask apply on the first-person viewmodel weapon. It is not part of the paint-only
   composite chain (see the client note above), and it is not called on the knife-type swap path or on
   the HUD arms entity.

2. **Auto play-out "tick nudge"** (`CosmeticDemoSync.cpp`, `CosmeticOverrideSystem::MaybeFireTickNudge`).
   `PostDataUpdate` alone does not make a third-person **body** swap (agent, gloves, knife type, legacy
   mesh) appear while the demo is paused. Those re-derive only during live rendered frames. So after a
   profile change, if the demo is paused, the system briefly issues `demo_resume`, lets about
   `m_tickNudgeTicks` (default 10) ticks render, then re-pauses. It does nothing when the demo is already
   playing. It is debounced, so a slider drag becomes one nudge. Toggle or tune it with
   `mirv_filmmaker cosmetics ticknudge [on|off|<ticks>]`. The `m_ModelName` readback is the authoritative
   check for which model is set, because it is the model the renderer draws, whatever the camera framing.

Net: weapon skins, wear and seed apply in place (composite chain). Agent, gloves and legacy-mesh swaps go
through the model-swap path. Knife-type swaps are **default-ON** (`m_knifeModelSwap = true`). They fire
only when all of these hold:
- The owner's knife is the target of a re-fire trigger: a new entity index (recreated after a seek or
  quick switch), a new target def, a holstered-to-deployed edge, or a live world model that differs from
  the target (`modelMismatch`). The periodic composite re-assert never triggers it.
- The post-seek stability window has passed (`kKnifeSwapStableFrames`, 8 frames). A suppressed attempt is
  logged as `SUPPRESS-settle`.

The world (third-person) model swap runs for holstered knives too. The first-person viewmodel mirror runs
only for the deployed knife. Disable the knife-type swap live with
`mirv_filmmaker cosmetics modelswap knife 0`. To VIEW body swaps, use a third-person camera
(`mirv_filmmaker follow preset behind` and `follow place`). A first-person view shows only the viewmodel.

## 7. Demo-playback caveats (the main unknowns for this tool)

Both sources drive the **local player's** entities in a live or offline match. This repo applies to a
**spectated** player during **demo playback**. The rendering calls are generic per-entity `client.dll`
code, so they should work on any weapon or pawn entity. Three things are unverified and must be tested
in-game before trusting them:

1. **Re-application after a networked snapshot.** In a demo, the next delta or full update can overwrite
   the econ identity and, for `SetModel`, possibly the model handle. As with the existing composite path,
   expect to **re-fire on every relevant frame and after every seek**. The repo already gates re-fire
   on change for skins. Extend the same gating to SetModel and SetBodyGroup. Andromeda and nerv re-assert
   every frame for exactly this reason: the engine resets gloves on spawn, and demos reset on seek.
2. **Resource precache.** `SetModel` takes a `.vmdl` path string, and the engine resolves it through the
   resource system. During live play the model is precached. During demo playback, a knife or agent
   model the demo never referenced may not be loaded. The repo therefore blocking-loads the target
   `.vmdl` with `CResourceSystem::PreCache` (`SafePrecacheModel`, SEH-guarded) before the knife world
   `SetModel` and before the agent `SetModel`. Precache loads the model resource only. The animation-table
   gap is covered by the anim-builder fallback in section 8. Toggle with `mirv_filmmaker cosmetics precache 0|1`.
3. **Viewmodel vs. world model entities.** The knife swap must hit both the weapon's world entity and
   the first-person viewmodel knife entity. Both sources do this through the HUD-arms scene-node walk
   (`GetKnifeModel`/`get_hud_weapon`). For a spectated player, the first-person viewmodel exists only
   when first-person spectating. In third person, only the world model entity is present. Apply to
   whatever weapon or pawn entities the spectated player actually has.

---

## 8. Knife-swap safeguards

A knife-type swap to a model whose animation data the demo never loaded would make the engine's anim
builder return a null table, and the caller dereferences it on a worker thread. Two layers guard the swap:

- **Model precache** (`SafePrecacheModel`): blocking load of the target `.vmdl` before `SetModel`. It loads
  the model resource so the mesh renders promptly.
- **Anim-builder fallback** (`CosmeticAnimFix.{h,cpp}`): a detour on the client.dll per-model animation
  builder (`0x3561c0`, validated unique in the live `client.dll`). When the out-param is null, the detour
  points it at a static empty (count 0) sequence list, so the caller takes its existing empty-list path.
  The detour is installed on the first knife swap. Toggle with `mirv_filmmaker cosmetics animfix 0|1`
  (default on). Breadcrumb: `knife.animfix`. Because the list is empty, a swapped knife whose animation
  data is not loaded renders its mesh but does not play its idle or deploy sequences.

The re-fire gates in section 6 depend on this fallback. The `animreset` experiment
(`mirv_filmmaker cosmetics animreset`) is default off and not part of the fix. Its instance offset
resolves to null on current builds, so it does not write.

Crash diagnosis uses the `crash.veh` vectored exception handler in `MvmDebugLog.cpp`. It is armed for a
few seconds after each swap with `MvmCrashWatch_Arm`. A fault is logged with its module, offset, access
type, and target address. A swap that completes with no `crash.veh client.dll` line has no native fault.

## External comparison sources

- Andromeda CS2 Base: full CS2 client inventory changer (knife, glove, agent, music, skins).
  Primary comparison for agents and the central AOB table.
- nerv: CS2 client project with `skin_changer` (including knife) and `glove_changer`.
  Corroborates the AOBs and provides a hand-built attribute-list comparison.

## 9. Demo-playback design rules

The skin-changer approach used by live-inventory tools does not carry over to demo playback. Those tools
start from the local player's inventory and copy a loadout item onto the local player's live weapon,
glove, or pawn. SOURCE:MVM starts from a recorded demo, where the inventory belongs to remote players and
playback, seeks, POV changes, weapon switches, round transitions, and tick advancement recreate entities.
The cosmetics system therefore keeps a per-demo-player override that re-discovers current entities and
reapplies to the right weapon, pawn, or viewmodel.



### Demo-playback assumptions

- The target is the spectated player, and third-person shots may need cosmetics on many players, including
  ones other than the current POV. Cosmetics are keyed by SteamID64.
- Demo players' inventory objects are not editable local loadouts, so the system does not rely on
  `CCSPlayerInventory::GetItemInLoadout` for remote players.
- Weapons may not exist until the relevant tick or deploy. The apply loop treats missing entities as normal
  and applies once they appear.
- Demo snapshots and entity recreation can revert econ item views, models, mesh groups, and body groups.
  Reapply is idempotent and lifecycle-aware.
- Each surface is refreshed separately: the world weapon, the first-person viewmodel or HUD weapon, the
  pawn's gloves, the pawn's agent model, and the HUD item caches.
- Gloves are pawn-level state (`m_EconGloves`, body groups, and arms), not weapon state.
- An agent is a pawn model path swap, so paint, fallback, and composite logic do not apply to it.
- Cosmetic profiles are runtime-only and clear when the demo path changes.


### Verification checklist

Check each case with `mvm_debug start`, `cosmetics status`, `cosmetics visualdiag`, and cropped screenshot
diffs where possible. Check both paused and playing demo states.

1. **Weapon skin:** apply a visible paint to a spectated primary or secondary weapon. The world and
   first-person viewmodel repaint, and the change holds through playback.
2. **Default knife paint:** keep the knife type default and apply paint. The log reports whether the knife
   has a paint target.
3. **Knife type swap:** default knife to another knife type with paint. Verify the first-person view,
   the third-person follow camera, weapon switch away and back, seek back and forward, and no crash.
   Compare the world model path and the viewmodel model path.
4. **Custom knife paint:** on a demo player already carrying a custom knife, change paint only. No
   subclass or model swap runs.
5. **Gloves:** default gloves to another glove type with paint. Verify first-person hands and third-person
   arms after the tick nudge, after a POV switch, after a seek, and after a team or round transition.
6. **Agent:** swap a CT or T agent model. Verify the third-person model and the `m_ModelName` readback.
7. **Per-player isolation:** apply different skins to two players, switch POV between them, and verify that
   owner XUID keeps each override on the right weapons and dropped guns.
8. **Late entity creation:** set cosmetics before the target weapon is deployed. The apply runs when the
   entity appears.
9. **Seek and recreate:** apply a skin, seek backward, seek forward, and switch POV. Overrides reapply after
   settle without touching half-rebuilt entities.
10. **Clear and reset:** run `cosmetics clear`, close and reload the demo, and verify no prior cosmetics
    reappear unless commands are sent again.












## 10. Glove HUD arms

The first-person arms entity is the complete arms viewmodel, not the glove model. Do **not** call
`SetModel(hudArms, gloveModel)`. `agents/models/shared/arms/glove_*/glove_*.vmdl` is only the glove
component, so replacing the arms entity with it removes the player's hands. Gloves are applied on the
pawn instead: the pawn's embedded glove econ item is written, `m_nEconGlovesChanged` and
`m_bNeedToReApplyGloves` are set, and the named `first_or_third_person` body group is applied. Rendered
frames then perform the engine-owned rebuild.

The HUD arms resolver accepts the live class name `cs2_hudmodel_arms` without an entity-index cap, because `m_hHudModelArms` can point to a high-index client entity. `UpdateBodyGroupChoice` is not
called on the current client, and `PostDataUpdate` is not called on the pawn or the HUD arms entity.

### Movie HUD mesh diagnostics

The Movie Director `MESH` section and its separator are emitted only while `MvmDebugLog_Active()` is true.
`mvm_debug start` shows the section; `mvm_debug stop` hides it. The `mkSep` Panorama helper returns the
created panel so the separator can be toggled.
