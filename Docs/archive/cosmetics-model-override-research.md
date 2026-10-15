# Cosmetics model-override research (knife/agent model swap)

> **Status.** Knife-type, glove, and agent model swaps run client-side via `C_BaseModelEntity::SetModel`.
> See `../cosmetics/cosmetics-cs2-methodology-notes.md` for the working recipes and `../cosmetics/cosmetics-overview.md`
> for the architecture. The implementation lives in `AfxHookSource2/Filmmaker/Cosmetics/CosmeticKnifeSwap.cpp`,
> `CosmeticGloveSwap.cpp`, and `CosmeticModelSwap.cpp`. The `CModelState`/schema layout research and
> the nSkinz/Source-1 comparison below are current reference material. Sections 4 through 6 record the
> server-side reference implementations and the open questions that the client-side path still has to answer.

This document covers the model-swap research behind the gloves and agent apply path in
`AfxHookSource2/Filmmaker/Cosmetics/CosmeticOverrideSystem.cpp`.

## 1. The paint and defIndex writes, and why they do not change the model

`Filmmaker::CosmeticOverrideSystem::RunFrame` (`AfxHookSource2/Filmmaker/Cosmetics/CosmeticOverrideSystem.cpp:283-385`)
walks every `C_EconEntity`-derived weapon on each main-thread frame. It resolves the original owner's XUID
(`m_OriginalOwnerXuidLow/High`, `AfxHookSource2/SchemaSystem.h:94-95`), looks up a `CosmeticProfile` by
SteamID64, and writes the following through `ApplyCosmeticWrite` (`CosmeticOverrideSystem.cpp:57-116`):

- `C_EconItemView::m_iItemIDHigh = -1`: the client stops trusting the networked econ item and uses the
  entity's own fallback fields. nSkinz and Osiris used the same trick in CS:GO.
- `C_EconEntity::m_nFallbackPaintKit / m_flFallbackWear / m_nFallbackSeed / m_nFallbackStatTrak`
- optionally `C_EconItemView::m_iItemDefinitionIndex` ("best-effort knife DEF swap", `knifeDefOverride`,
  `CosmeticOverrideSystem.cpp:90,99`)
- clears `m_bInitialized`/`m_bInitializedTags` to force the client to recompute the composited material

This pipeline only rewrites the econ item view that feeds material compositing. It never touches the
entity's `CModelState`/`m_hModel` resource handle. Compositing changes the skin or pattern texture on whatever
`.vmdl` is already loaded. It cannot turn a default knife into a Butterfly Knife, because those are two
different `.vmdl` resources with two different skeletons.

### Does writing `m_iItemDefinitionIndex` alone already get us the right model? No.

`knifeDefOverride` in `ApplyCosmeticWrite` only patches the econ item's definition index. That index drives
HUD icons, inspect-panel data, and which paint-kit table the composite step looks up. It does not touch any
model-resource field. The defIndex and the model are two independent properties of the entity. The model is
resolved once at entity creation (server-side, from `GiveNamedItem`/loadout resolution; see §3). The client
does not re-derive it from `m_iItemDefinitionIndex` afterward. nSkinz confirms this: even after
`definition_index = new_def`, it must separately call a model-index setter (`item->SetModelIndex(...)`),
see §2. Paint-only is confirmed insufficient. A true model swap is a separate, additional step, performed
by the client-side model-swap path (see §4).

## 2. nSkinz (CS:GO / Source 1): how it actually swaps the knife model

Source: `namazso/nSkinz`, `src/Hooks/PostDataUpdate.cpp`
(https://github.com/namazso/nSkinz/blob/master/src/Hooks/PostDataUpdate.cpp), MIT licensed, hooked from the
advancedfx fork (https://github.com/advancedfx/nSkinz).

Knife/weapon model swap (`apply_config_on_attributable_item`, lines 49-111):

```cpp
if (config->definition_override_index && config->definition_override_index != definition_index) {
    if (const auto replacement_item = game_data::get_weapon_info(config->definition_override_index)) {
        definition_index = short(config->definition_override_index);
        // Set the weapon model index -- required for paint kits to work on replacement items
        // after the 29/11/2016 update.
        item->SetModelIndex(g_model_info->GetModelIndex(replacement_item->model));
        item->GetClientNetworkable()->PreDataUpdate(0);
        ...
    }
}
```

Viewmodel + world-model sync (`post_data_update_start`, lines 246-269):

```cpp
    const auto view_model = get_entity_from_handle<sdk::C_BaseViewModel>(local->GetViewModel());
const auto view_model_weapon = get_entity_from_handle<...>(view_model->GetWeapon());
const auto override_info = game_data::get_weapon_info(view_model_weapon->GetItemDefinitionIndex());
const auto override_model_index = g_model_info->GetModelIndex(override_info->model);
view_model->GetModelIndex() = override_model_index;          // first-person viewmodel
const auto world_model = get_entity_from_handle<...>(view_model_weapon->GetWeaponWorldModel());
world_model->GetModelIndex() = override_model_index + 1;     // third-person world model
```

Key mechanism: `IVModelInfoClient::GetModelIndex(path)` (`VModelInfoClient004` engine interface, declared
in `src/SDK/interfaces.hpp`, used via `extern sdk::IVModelInfoClient* g_model_info;`,
https://github.com/namazso/nSkinz/blob/master/src/SDK/Sequence.cpp#L8-L20) is a Source-1 engine function.
It resolves or precaches a `.mdl` path string to an integer model index. The index is assigned directly to
an `int m_nModelIndex`-style field on the entity. On `item`, `view_model`, and `world_model`, `GetModelIndex()`
is a field accessor that returns a reference into the entity.
`PreDataUpdate(0)` makes the client networkable re-run its data-update path, so dependent state (for example
attachment-driven viewmodel pieces) re-resolves against the new model.

Animation-graph risk, handled by nSkinz: a knife swap changes the model's `.mdl`. Its embedded sequence
table does not share the original knife's layout. nSkinz runs a parallel hook (`src/Hooks/Sequence.cpp`,
https://github.com/namazso/nSkinz/blob/master/src/Hooks/Sequence.cpp). It intercepts the viewmodel's
animation-sequence RecvProxy and remaps sequence numbers per target model (`get_new_animation`,
lines 38-191). For example, for `models/weapons/v_knife_butterfly.mdl`, the draw, idle, and lookat sequence
indices differ from the default knife's. nSkinz hard-codes per-model remap tables by hashed model path. The
comment at lines 34-37 reads: *"This only fixes if the original knife was a default knife... The best would be
having a function that converts original knife's sequence into some generic enum... I won't write that."*
The reference implementation therefore treats this as a known-incomplete workaround.

Source 1 and CS:GO details that differ in Source 2 and CS2:
- `IVModelInfoClient`/`VModelInfoClient004` is a Source-1 engine interface. CS2 does not expose model
  resolution this way. Model loading in Source 2 goes through the resource system (`CStrongHandle<InfoForResourceTypeCModel>`),
  not an integer "model index" looked up in a global table.
- `int m_nModelIndex` as a flat field does not exist on CS2 schema classes (see §3). Source 2 entities
  hold a `CStrongHandle` resource reference instead.
- `CBaseWeaponWorldModel` (a separate Source-1 GOTV/CS:GO world-model entity class with its own model
  index) has no confirmed Source-2 equivalent in the schema dump checked (§3). In CS2 the weapon entity
  appears to be both the "owner" and the rendered world-model object. No distinct world-model entity class
  appears in the generated client.hpp schema. See `C_CSWeaponBase` at client.hpp:2779.
- The sequence-remap hack is tied to legacy `.mdl` sequence tables. Source 2's AnimGraph-driven model state
  (`CModelState`, `CBodyComponentBaseAnimGraph`) is a structurally different animation system. It uses graph
  nodes and decision trees, not a flat numbered sequence array, so this remap technique does not translate.

## 3. CS2 (Source 2) schema: current field names

Source: generated schema dump `NotOfficer/cs2-sdk`, `client.hpp`
(https://raw.githubusercontent.com/NotOfficer/cs2-sdk/master/client.hpp), a source2gen-style dump from the same
tool family as the local `AfxHookSource2/SchemaSystem.cpp` resolver. The local repo has no
`CBaseModelEntity`, `CModelState`, or `CSkeletonInstance` definitions under `Deps/release/prop/cs2/`
(confirmed with `Glob` over `Deps/release/prop/cs2/**/*.h`). No header the DLL compiles against declares
these fields. They exist only at runtime in the CS2 schema system. Resolve them the same way
`AfxHookSource2/SchemaSystem.cpp` resolves `C_EconEntity` and `C_EconItemView` fields, via
`getOffsetsFromSchemaSystem` walking `client.dll`'s declared classes.

The model-resolution chain in current CS2 comes from the cs2-sdk dump. Offsets are build-specific. Re-resolve
them live through the schema system, the same way `g_clientDllOffsets.C_EconEntity.*` is resolved today. Do
not hardcode the hex offsets quoted here:

```
C_BaseEntity                              (client.hpp:589)
  CBodyComponent* m_CBodyComponent;        // 0x38 — NOT a value member, a pointer

CBodyComponentSkeletonInstance            (client.hpp:5624, : CBodyComponent)
  CSkeletonInstance m_skeletonInstance;    // embedded value member
    (network sub-paths, per MNetworkVarNames on the embedded member)
    -> m_modelState           (CModelState, embedded inside CSkeletonInstance)
    -> m_bIsAnimationEnabled
    -> m_materialGroup
    -> m_nHitboxSet

CSkeletonInstance : CGameSceneNode        (client.hpp:4570)
  CModelState m_modelState;                // embedded value member
    -> m_hModel      (offset +0x210 from CSkeletonInstance base in this dump)
    -> m_ModelName   (offset +0x218 from CSkeletonInstance base in this dump)

CModelState                                (client.hpp:1946, standalone layout)
  CStrongHandle<InfoForResourceTypeCModel> m_hModel;  // 0xa0 (MNetworkChangeCallback "skeletonModelChanged")
  CUtlSymbolLarge m_ModelName;                         // 0xa8 (MNetworkDisable)
```

Important nuances:
- `m_CBodyComponent` is declared as a pointer (`CBodyComponent*`) on `C_BaseEntity`. The runtime object
  behind it (for a model entity) is a `CBodyComponentSkeletonInstance` or `CBodyComponentBaseAnimGraph`. Resolve
  through the pointer. Do not assume an embedded struct. The concrete subtype varies by entity type: weapons and
  players use the `BaseAnimGraph` variant, and static props use `CBodyComponentBaseModelEntity`.
- `m_hModel` carries `MNetworkChangeCallback "skeletonModelChanged"`. The schema system annotates this field so
  that a server-side write fires a network change callback, which drives the actual resource (re)load on the
  client. The annotation says nothing about what happens when a client manually overwrites the resolved
  client-side value. The callback machinery is not guaranteed to run. A `CStrongHandle` is a refcounted resource
  reference (`InfoForResourceTypeCModel`), not a plain index. Overwriting it with raw
  memory writes risks an invalid or dangling refcounted handle (use-after-free or resource-system corruption)
  instead of a clean swap. The resource system tracks load and unload through that handle's own refcounting,
  and a raw write bypasses it entirely.
- `m_ModelName` is `MNetworkDisable`, so it is never networked. It is a local-only cache. The client does not
  use it to re-resolve the model on a plain write. Writing it alone almost certainly does nothing visually,
  because nothing re-reads it to trigger a reload.
- No flat `int m_nModelIndex` exists anywhere in the dump. CS2 uses per-entity `CStrongHandle` resource references
  instead of the CS:GO/Source-1 "global model index table".
- `C_ViewmodelWeapon::m_worldModel` (`client.hpp:12434-12438`) is a raw `char*` at offset `0xed0`. It is not
  marked `MNetworkEnable`, and it is a bare pointer rather than a `CUtlSymbolLarge` or `CStrongHandle`. It looks
  like an internal cached display-name pointer, so it is not safe to repoint.
- `C_BaseViewModel` (`client.hpp:4023-4056`) has `m_hWeapon` (`CHandle<C_BasePlayerWeapon>`) and
  `m_hWeaponModel` (`CHandle<C_ViewmodelWeapon>`). The viewmodel entity references a weapon entity and a
  separate viewmodel-weapon entity. Neither field is a model resource itself. The model lives on whichever
  entity the handle resolves to, through that entity's own `CBodyComponent` chain above.
- `C_CSWeaponBase` (`client.hpp:2779`) carries `m_seqIdle`, `m_seqFirePrimary`, `m_seqFireSecondary`, and
  `m_thirdPersonFireSequences[]` (`HSequence` typed, `client.hpp:2793-2802`). CS2 still has a sequence concept
  per weapon, but it runs through the modern `HSequence`/AnimGraph system, not the legacy flat int array nSkinz
  patches in Sequence.cpp. This is the strongest evidence that a knife-model swap on Source 2 carries the same
  class of animation risk nSkinz hit on Source 1: mismatched draw, idle, and attack sequences between source and
  target knife models. The exact remediation is unconfirmed. No public CS2-specific reference implementation has
  been identified (see §4/§5).
- `C_CSPlayerPawn`/`C_CSPlayerPawnBase` (`client.hpp:5893-7300+`) expose no agent or player-model field. There is
  no `m_AgentSkin` and no model-path string. The only econ-shaped field on the pawn is `C_EconItemView m_EconGloves`
  (`client.hpp:7286`), which matches the local `g_clientDllOffsets.C_CSPlayerPawn.m_EconGloves` already resolved
  in `AfxHookSource2/SchemaSystem.h:130-132`. The server resolves the agent or player body model at pawn spawn
  from the player's loadout. It is not exposed as a "current model" field, so the client changes the model through
  `C_BaseModelEntity::SetModel` (see §4) rather than a field write.

`C_BaseModelEntity` itself (`client.hpp:1070-1110`) puts the render-relevant fields behind `m_CRenderComponent`.
That is a different pointer from `m_CBodyComponent`. It handles render state such as `m_clrRender` and
`m_nRenderMode`, not the model resource. `m_CHitboxComponent` sits alongside it. The model resource is reached
only through `m_CBodyComponent`, then the skeleton instance, then `m_modelState`, as charted above.

## 4. `SetModel`: the server-side references and the client-side path

Source: `Salvatore-Als/cs2-signature-list`, `CBaseModelEntity_SetModel.md`
(https://github.com/Salvatore-Als/cs2-signature-list/blob/main/CBaseModelEntity_SetModel.md). The signature
entry is `void CBaseModelEntity::SetModel(CBaseModelEntity* pthis, const char* pszModel)`, DLL: server.

The two public implementations below perform their model swaps server-side:

1. `yuzhouUvU/cs2_weapons_skin` (`Skin.cpp`, https://github.com/yuzhouUvU/cs2_weapons_skin/blob/main/Skin.cpp,
   a Metamod:Source server plugin) does not call `SetModel` directly, even for a knife swap. Its knife-swap path
   (`CON_COMMAND_F(skin, ...)`, `Skin.cpp:407-411`) is:
   ```cpp
   pWeaponServices->RemoveWeapon(pPlayerWeapon);
   FnEntityRemove(g_pGameEntitySystem, pPlayerWeapon, nullptr, -1);
   FnGiveNamedItem(pPlayerPawn->m_pItemServices(), weapon_name->second.c_str(), nullptr, nullptr, nullptr, nullptr);
   ```
   It destroys the weapon entity and recreates it from scratch through the item-give system. That system
   resolves the correct model, animgraph, and sequences as part of normal weapon spawning. It does not mutate
   a model field on a live entity. For the default-knife-to-skinned-knife case, it drives the engine's own
   subclass-change console command instead:
   ```cpp
   sprintf(buf, "i_subclass_change %d %d", knife_idx->second, index);
   engine->ServerCommand(buf);
   ```
   This implementation also defers to an internal engine command, the same mechanism CS2's own loadout system
   uses for "preferred knife by side". It does not touch model fields by hand. Both paths are
   server-authoritative. `RemoveWeapon`, `FnEntityRemove`, `FnGiveNamedItem`, `i_subclass_change`, and
   `ServerCommand` exist and are valid only in a live game-server context (a Metamod hook into `server.dll` with
   a real `IVEngineServer2`). They do not work in offline demo playback, where there is no
   server simulating the world. The client replays networked snapshots and deltas that were already
   baked when the demo was recorded.

2. `samyycX/CS2-PlayerModelChanger` (https://github.com/samyycX/CS2-PlayerModelChanger) is a
   CounterStrikeSharp addon (C#, server-side managed plugin framework) for player and agent model changes.
   CounterStrikeSharp's managed `SetModel`-equivalent API is a thin wrapper around the native `server.dll`
   function in the signature list above. The agent-model reference implementation listed here is
   server-side.

The server-side entry points (`SetModel` in `server.dll`, `GiveNamedItem`, and `i_subclass_change`) are
reachable only in a live game-server context, so they do not run during offline demo playback. This DLL
is limited to `client.dll` (`AfxHookSource2/` is the entire scope), and the `CLAUDE.md` constraint limits
the work to offline demo and movie use. The model swap therefore runs client-side. The client build
resolves `C_BaseModelEntity::SetModel`, and `CosmeticModelSwap.cpp` calls it on the weapon, viewmodel,
and pawn entities. See `../cosmetics/cosmetics-cs2-methodology-notes.md` for the recipe.
The paint-kit override works because `C_EconItemView`/`C_EconEntity` fallback compositing is designed as a
client-side rendering fallback for cases like missing or stale networked econ data.

## 5. Achievable now vs. uncertain

### Achievable now (low risk, consistent with existing architecture)
- The paint, skin, and defIndex numeric override (`Cosmetics_RunFrame`) is a client-only, schema-field-write
  path. It does not change the model, which is handled by the model-swap path in §4.
- UI and storage: gloves and agent profile fields are entered, saved, and displayed. This is implemented
  (`CosmeticDebug.cpp:141-149`, `CosmeticProfile.cpp:15,121,179`).
- Diagnostics: extending `Cosmetics_DebugWeapon`-style introspection (`MirvCosmetics.cpp:149-168`) to dump the
  resolved `CBodyComponent`, then `CSkeletonInstance`, then `m_modelState.m_ModelName` chain is low-risk. It is
  read-only. It confirms offset resolution and the current model path live without writing anything. It is a
  contained next step for instrumentation.

### Uncertain, needs in-game testing before any write
- Whether a raw client-side overwrite of `CModelState::m_hModel`/`m_ModelName` does anything at all. Because
  `m_hModel` is a refcounted `CStrongHandle`, a naive overwrite has three possible outcomes: (a) no visible
  change, because nothing triggers a load from the new value; (b) a crash or resource-system corruption, from
  bypassing the handle's own ref-counting; or (c) the best case, where the render path re-derives the mesh from
  `m_hModel` every frame and a correctly acquired strong handle to the target `.vmdl` renders. Outcome (c)
  requires calling the Source 2 resource system to acquire a new strong handle for the desired `.vmdl`, probably
  through the resource manager interface rather than a raw memory write. No public reference for doing this from
  client.dll has been identified.
- Animation and skeleton compatibility. Section 3's `HSequence`/AnimGraph evidence suggests a knife model swap
  still needs a CS2 equivalent of nSkinz's sequence remap. In Source 2, a compatible AnimGraph or skeleton
  replacement model is more likely. No CS2-specific reference for this has been identified. This is
  the largest unknown. Naive attempts most likely fail with visible breakage: a T-pose, snapped bones, or a swap
  that is silently ignored. The agent (player pawn) case carries a worse risk. A player model swap also changes
  hitboxes (`CSkeletonInstance::m_nHitboxSet`, `client.hpp:4602`), jiggle bones, gameplay-relevant attachment
  points (weapon-hand attachment, viewmodel parenting), and ragdoll setup. All of these are baked into the specific
  skeleton or animgraph asset, and they are not interchangeable across arbitrary `.vmdl` files the way a paint kit
  composite is. This tool records movies rather than running live gameplay, so hitboxes and gameplay do not matter.
  A skeleton or animgraph mismatch still causes visual T-posing or limb snapping.
- Whether the demo-playback client owns enough of the resource pipeline to load an arbitrary `.vmdl` that the demo
  does not reference. Live gameplay precaches models server-side at map load. During demo playback, the resources
  the client prepared for rendering may be limited to what the recorded demo referenced. Test this in game before
  any implementation attempt: resolve or precache an unrelated `.vmdl` while a demo plays. Neither outcome is
  confirmed.
- No CS2-specific public reference for a true client-only model swap has been identified, after searching for
  "spectator/demo/clientside model change" techniques (see §4). This area appears undocumented.

## 6. Minimal next-step implementation plan

### Milestone A: "knife defIndex + model override" (read-only instrumentation first)
1. Extend the non-fatal cosmetics-offset block in `AfxHookSource2/SchemaSystem.h`/`.cpp`
   (`initCosmeticsOffsets`, `SchemaSystem.cpp:141-167`). Resolve these fields read-only and best-effort,
   mirroring the `g_cosmeticsOffsetsOk` non-fatal pattern:
   - `C_BaseEntity::m_CBodyComponent`: already implicitly available. It uses the same `m_pGameSceneNode` pattern
     resolved at `SchemaSystem.h:26`/`SchemaSystem.cpp:105`, on a different pointer field of the same class.
   - `CBodyComponentSkeletonInstance::m_skeletonInstance`: embedded. Its offset is needed to add to the
     `m_CBodyComponent` pointer's target.
   - `CSkeletonInstance::m_modelState`: embedded within the skeleton instance.
   - `CModelState::m_hModel` and `CModelState::m_ModelName` offsets.
2. Add a read-only diagnostic, `Cosmetics_DebugModel(int pawnOrWeaponEntityIndex, ...)`, modeled on
   `Cosmetics_DebugWeapon` in `MirvCosmetics.cpp:149-168`. It walks this chain and prints the live `m_ModelName`
   (a `CUtlSymbolLarge`, safe to read as a string) for a target entity. Wire it into `mirv_filmmaker cosmetics debug`
   next to the existing weapon debug output (`CosmeticDebug.cpp`/`Cosmetics_PrintSpectatedDebug`).
3. With that diagnostic running, confirm in game: (a) the offsets resolve on the current CS2 build; (b) the
   printed model path matches the equipped knife visually; (c) the model path string for a default knife versus a
   real Butterfly Knife, on a teammate or bot if a test demo can arrange one. This gives the exact target string,
   for example something under `models/weapons/v_knife_butterfly*.vmdl`, analogous to the CS:GO path nSkinz used.
   Confirm CS2's actual path. Do not assume the CS:GO path is unchanged.
4. After step 3 works read-only, run a viewmodel-only, single-player-pawn, easily reverted experiment. Acquire
   and assign a `CStrongHandle` for a known-good alternate `.vmdl`. Start with something structurally similar, such
   as two default knives from the same skeleton family. Do not start with a default-to-Butterfly swap. Observe
   whether it renders, T-poses, or crashes. This experiment decides whether Milestone A is viable before any
   further design work.
5. Do not add `m_iItemDefinitionIndex` writes for model-swap purposes. `ApplyCosmeticWrite` already writes it for
   cosmetic and icon purposes only. Leave it that way until step 4 produces a working mechanism. Section 1
   confirmed that the defIndex write alone does not change the model.

### Milestone B: "agent model override"
Milestone B depends on Milestone A succeeding for *any* model swap, knife or otherwise. Agent swap is the harder
case, since it needs the full skeleton, hitbox, and attachment set (§5):
1. Repeat the read-only diagnostic from Milestone A, steps 2-3, on a `C_CSPlayerPawn`'s body component chain. The
   chain is `m_CBodyComponent`, then the skeleton instance, then `m_modelState`. Pawns and weapons both derive from
   `C_BaseModelEntity`, so the chain is identical and only the starting entity differs. Confirm that the live agent
   model path matches the catalog values in `AfxHookSource2/Filmmaker/Data/cosmetics.json` (for example
   `agents/models/ctm_fbi/ctm_fbi.vmdl`).
2. If Milestone A's swap mechanism works for weapons, try the same mechanism on a pawn. Use two agent models known
   to share a compatible skeleton. Valve's CT and T agent models are generally built on a small number of shared
   base skeletons. Try this before any arbitrary cross-skeleton swap.
3. Gloves and agent application in `CosmeticOverrideSystem::RunFrame` apply `prof->agent` and `prof->gloves` through
   the model-swap path. Any further change to that block uses the same XUID-keyed, SEH-guarded,
   non-fatal-on-missing-offset pattern the file already uses for weapons.

### Open questions
The paint, skin, and defIndex overrides are implemented. The client-side model swap is implemented for knives,
gloves, and agents through `CosmeticModelSwap.cpp`. Knife type swaps are opt-in
(`mirv_filmmaker cosmetics modelswap knife 1`). Two concrete unknowns remain for this client-only,
offline-demo-playback architecture: resource-handle acquisition, and animation and skeleton compatibility.

## Sources

- Local: `AfxHookSource2/SchemaSystem.h`, `AfxHookSource2/SchemaSystem.cpp`,
  `AfxHookSource2/ClientEntitySystem.h`, `AfxHookSource2/ViewModel.cpp`,
  `AfxHookSource2/Filmmaker/Movie/MirvCosmetics.{h,cpp}`,
  `AfxHookSource2/Filmmaker/Cosmetics/CosmeticOverrideSystem.{h,cpp}`,
  `AfxHookSource2/Filmmaker/Cosmetics/CosmeticProfile.{h,cpp}`,
  `AfxHookSource2/Filmmaker/Cosmetics/CosmeticCatalog.{h,cpp}`,
  `AfxHookSource2/Filmmaker/Cosmetics/CosmeticDebug.cpp`,
  `AfxHookSource2/Filmmaker/Cosmetics/CosmeticCommands.cpp`,
  `AfxHookSource2/Filmmaker/Data/cosmetics.json`
- nSkinz (advancedfx fork, MIT): https://github.com/advancedfx/nskinz ;
  upstream source used for code excerpts: https://github.com/namazso/nSkinz/blob/master/src/Hooks/PostDataUpdate.cpp ,
  https://github.com/namazso/nSkinz/blob/master/src/Hooks/Sequence.cpp ,
  https://github.com/namazso/nSkinz/blob/master/src/SDK/interfaces.hpp
- CS2 generated schema dump: https://raw.githubusercontent.com/NotOfficer/cs2-sdk/master/client.hpp
  (source2gen-style dump; see also https://github.com/NotOfficer/cs2-sdk)
- CS2 offsets project (cross-reference, not separately quoted above):
  https://github.com/sezzyaep/CS2-OFFSETS
- `CBaseModelEntity::SetModel` signature (server.dll):
  https://github.com/Salvatore-Als/cs2-signature-list/blob/main/CBaseModelEntity_SetModel.md
- Server-side knife/skin plugin (Metamod:Source, GPL):
  https://github.com/yuzhouUvU/cs2_weapons_skin/blob/main/Skin.cpp
- Server-side player-model plugin (CounterStrikeSharp, GPL-3.0):
  https://github.com/samyycX/CS2-PlayerModelChanger
- CS2 entity list reference: https://cs2.poggu.me/dumped-data/entity-list/

## Reference comparison

The practical guidance lives in `Docs/cosmetics/cosmetics-cs2-methodology-notes.md`, especially section 9.
Writing `m_iItemDefinitionIndex` alone does not change the rendered model. CS2 model swaps run from
`client.dll`. All three local references use or corroborate `C_BaseModelEntity::SetModel(const char*)`, `CGameSceneNode::SetMeshGroupMask`,
`C_CSWeaponBase::UpdateSubclass`, and `CGameSceneNode::PostDataUpdate` from `client.dll`.

For demo playback, the remaining model-swap risk is lifecycle timing, not whether the function exists. Seek, POV,
and deploy can recreate entities and viewmodels while the demo packet stream rebuilds them. The correct shape is a
conservative reapply. Target entities by owner SteamID. For first-person viewmodel writes, operate only on the
current active weapon. Before knife type swaps, wait for a post-seek stability window. Keep a user-visible kill
switch for knife model swaps.

The deeper `real-time-internal-overlay-research-main` reference adds one escalation path, for when conservative reapply
still shows a default knife for a frame. That repo hooks `C_BaseModelEntity::SetModel` in `src/game/game_hooks.cpp`.
Its `src/game/skins/skin_changer.cpp::OnSetModel` rewrites the incoming viewmodel knife model argument before the
original engine call runs. SOURCE:MVM keeps the current after-the-fact reassert path first. If demo seeking, POV
switches, or deploy animations redraw the default model before our frame loop can repair it, test a guarded
demo-only `SetModel` hook. The reference supports this as a fix.
