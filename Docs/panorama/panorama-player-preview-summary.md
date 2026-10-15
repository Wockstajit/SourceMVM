# Panorama Player Preview

Scope: offline CS2 demo / movie-making UI only.

> The paused preview composites, animates and draws. The maintained UI-side contract is in [customize-feature.md](../features/customize-feature.md#paused-preview).

## Useful findings

### 1. `MapPlayerPreviewPanel` is the right native Panorama panel

The native player-preview route is:

- Create a `MapPlayerPreviewPanel`.
- Use `map: "ui/buy_menu"`.
- Use a loadout/vanity camera such as `cam_loadoutmenu_ct` or `cam_vanityloadout`.
- Set a valid AG2 model path such as `agents/models/tm_professional/tm_professional_varf.vmdl`.
- Enable `player`, `mouse_rotate`, `sync_spawn_addons`, `transparent-background`, and `pin-fov`.
- Pair `require-composition-layer` with `composition-layer-texture-name`.

The Customize modal's 3D-preview fragment is `AfxHookSource2/Filmmaker/Panorama/CameraEditorCustomizePreviewJs.h`, split out of `CameraEditorCustomizeJs.h`.

### 2. Current preview implementation

The customize modal creates a native `MapPlayerPreviewPanel` in:

`AfxHookSource2/Filmmaker/Panorama/CameraEditorCustomizePreviewJs.h` (preview wiring; modal in `CameraEditorCustomizeJs.h`)

Key pieces:

- `createPreview3d()` sets `require-composition-layer` and `composition-layer-texture-name`.
- It uses `map: ui/buy_menu`, `cam_vanityloadout` / `cam_loadoutmenu_ct`, `player`, `sync_spawn_addons`, and `csm_split_plane0_distance_override`.
- It sets `hide_while_waiting_for_composite_materials: false`, which keeps the preview panel from being hidden while its composite materials load.
- `applyPreview()` calls `SetPlayerCharacterItemID`, `SetPlayerModel`, `EquipPlayerWithItem`, and `SetReadyForDisplay`.

The modal's code already covers this native path.

### 3. Lifecycle and timing

If the preview is black or missing, investigate these first:

- The modal must be visible and non-zero-size before creating the preview panel.
- The panel needs a unique composition-layer texture name.
- The preview may need a few frames of re-application while paused.
- Rebuild the panel when the selected model/item set changes.
- Delete the parent/container to remove preview panels cleanly.

The code has delayed creation and `previewPokeFrames` re-application, so verification should focus on whether both fire in the failing state.

### 4. Texture capture is only needed for an external/ImGui render path

The UC render-system SRV capture references are useful only if we want to copy the Panorama preview texture into another renderer, such as ImGui or a custom DX overlay.

For SOURCE:MVM's native Panorama UI, the cleaner path is to let the `MapPlayerPreviewPanel` render directly inside the filmmaker modal. Avoid adding a render-system SRV hook unless there is a strong reason to display that texture outside Panorama.

### 5. `GetHudElement` is lower priority

External HUD-element lookup, which finds HUD panels and runs Panorama JS in their context, is not needed for player preview. SOURCE:MVM already has `PanoramaBridge`, `AfxHookSource2_GetPanoramaMainMenuPanel()`, `AfxHookSource2_GetPanoramaHudPanel()`, and `RunScript` wiring.

It is only needed if the existing HUD/main-menu panel signatures break.

### 6. VPK Panorama editing is not useful for this feature

Editing compiled CS2 Panorama resources is not needed for this feature. SOURCE:MVM injects Panorama JavaScript at runtime through `RunScript`, so recompiling VPK Panorama files is unnecessary for this UI.

## Verifying the native preview path

Do not start with render-system hooking. Live-verify the existing native preview path first:

1. Open the filmmaker camera editor customize modal in a demo.
2. Confirm `createPreview3d()` succeeds and `preview3d.IsValid()` returns true.
3. Confirm the panel has non-zero layout size when created.
4. Confirm `SetReadyForDisplay` is called after creation and after model/item changes.
5. If it is still black while paused, check the three gates in the next section.
6. If the panel is visible but the character is wrong/washed out, verify the selected agent has an econ item id and that `SetPlayerCharacterItemID` runs before `SetPlayerModel`.

## The paused preview: three gates

The paused preview renders only when three independent gates are open. Each one alone causes a visible failure, and each was verified by turning it off in a live session.

| gate | symptom when it alone is missing | fix |
| --- | --- | --- |
| the panel never submits its 3D view | preview box is **black** | `Panorama/PreviewRenderProbe.cpp` scene-gate bypass |
| the animgraph gets a zero tick delta | character is a **still frame**, item picks leave the gun floating in its stance | `Panorama/PreviewAnimTick.cpp` tick-delta grant |
| the pawn never interpolates | **only the held weapon draws**, no body | `Panorama/PreviewRenderFlag.cpp` |

The sections below cover each gate, the pawn identification, and the commands.


## Identifying the preview pawn

The Customize preview is a native `MapPlayerPreviewPanel` created with `map: 'ui/buy_menu'`, `camera: 'cam_loadoutmenu_ct'`, `animgraphcharactermode: 'buy-menu'`, `player: true`, and `sync_spawn_addons: true`, plus the composition-layer attributes. Its character is driven by an animgraph on entities that simulate on the client.

The preview pawn resolves through the ordinary client entity-list accessor. It has class `csgo_previewplayer` and clientclass `C_CSPlayerPawn`, and it appears at client-only indices such as 16389, 16395, and 16897 (`agents/models/ctm_sas/ctm_sas.vmdl`, matching the modal's model).

* `g_GetEntityFromIndex` is not bounded by a networked ceiling. The clamp is caller-side, and two same-named helpers differ:
  * `CosmeticEntFromIndex` (`CosmeticEntityAccess.cpp:16`) and `CosmeticDebugRead::EntFromIndex` (`CosmeticDebug.cpp:41`) clamp to `GetHighestEntityIndex()`, so they do not see client-only entities.
  * `modelswap::EntFromIndex` (`CosmeticModelSwap.cpp:25`) guards only `index >= 0x8000` and resolves the **full range**. Use this pattern.
* `GetHighestEntityIndex()` is a hardcoded `return 2048` stub (`ClientEntitySystem.cpp:451`, with a TODO). It is not an engine bound. Anything clamped to it sees only indices 0..2048.
The five `csgo_previewplayer` pawns exist even with the modal closed, and the same pawns appear before and after it opens. The panel reuses a pooled pawn rather than spawning a fresh one, so do not assume the highest new index is the preview character. Identify the live pawn by matching the model the JS reports (`[mvmpreview] sceneChanged ... model=...`) and by which pawn actually moves.

### The probe: `mvm_previewprobe` (read-only, `Filmmaker/Panorama/PreviewSceneProbe.{h,cpp}`)

Standalone module, no memory writes, no detours. Own per-frame pump
(`PreviewSceneProbe_RunFrame`, disarmed by default).

```
mvm_debug start
mirv_filmmaker editor customize      # open the modal so the scene exists
mvm_previewprobe scan                # list preview pawns: index / class / model
mvm_previewprobe watch auto [everyN] # or `watch <index>`; samples into mvm_debug
```

Each throttled sample logs `preview.watch idx frame tick paused attach ok apos arot origin`. The
animation-advance proxy is the first attachment that resolves (`eyes`, `facemask`, `head`, ...): an
idle/breathing animgraph moves those bones while the entity's **root origin stays constant**, so
"apos changes / origin constant" means animating and "apos constant" means frozen. It logs via
`MvmDebugLog_LinefAlways` deliberately, since a frozen animgraph emits a byte-identical payload every
sample, and the deduplicating logger would collapse exactly the evidence being collected.

### Animation advance

The preview animgraph advances with the demo tick, not with render frames, and the advance is selective per pawn.

* While the demo is paused, the live preview pawn's attachment positions are bit-identical across samples, even across many render frames, because the demo tick is pinned. When the tick advances, the positions move again. The oscillating x and steadily rising z are the idle breathing loop.
* While the demo plays, the live preview pawn moves continuously.
* A pooled `csgo_previewplayer` pawn that nothing animates does not move, even while the demo plays. The client already distinguishes which preview pawns advance.

The native buy menu and the main-menu loadout declare the same panel attributes as `createPreview3d`, including `map="ui/buy_menu"`, `animgraphcharactermode="buy-menu"`, `sync_spawn_addons`, and `pin-fov`. They animate because a live simulation clock runs, so panel attributes do not control the freeze. The fix operates on the per-pawn advance path.

### Diagnostic clock: `PreviewAnimClock`

`PreviewAnimClock` writes the client `curtime` and `frametime` globals from the main-thread pumps (`stage main|late|both`). It is off by default and is kept only as a diagnostic harness. The client re-applies its own curtime from the demo clock after both hook points, so these writes never reach the animation path.

### Preview AG2 objects

The Customize pawn resolves to this preview-only animation chain:

* `previewPawn + 0x34F0` -> `CCS2UIPawnGraphController`
* `previewPawn->m_CBodyComponent` -> `CBodyComponentBaseAnimGraph`
* current body component `+ 0x960` -> `CNmGraphInstance`

RTTI confirms both concrete controller and body types. The graph-controller layout agrees with the schema (`m_nAnimationSeed`, character and weapon mode and state inputs).

Graph memory is not written directly. The graph instance owns task and pose state, so it is advanced through the engine's own tick (see the tick-delta grant below).

### Tick delta: the AG2 delta is an integer tick count

The AG2 `EvaluateAnimList` job in `client.dll` (`CAnimGraphGameSystem`) computes each entity's animation delta as `currentTick - lastUpdateTick`, scaled by 1/64. An entity whose delta is `<= 0` is skipped before any evaluation. A paused demo pins the tick, so every entity reports delta 0. No wall-clock value is consulted on this path, which is why `PreviewAnimClock` cannot drive the animation.

### Tick-delta grant (`PreviewAnimTick`)

`Filmmaker/Panorama/PreviewAnimTick.{h,cpp}` is a standalone module with its own command (`mvm_previewtick`). It is armed automatically while the Customize modal is open, and its detours are installed lazily on first arm:

* Resolves `CAnimGraphGameSystem::AnimTickUpdate` durably: profiling literal
  `"AnimGraph Client Tick"` -> its single rip-relative xref -> enclosing function from the PE
  exception directory (`.pdata`, following `UNW_FLAG_CHAININFO`). No byte pattern is used.
* Update-list layout: `count @ +0x00`, `entries @ +0x10`, stride `0x60`,
  entity `CHandle` at entry `+0x00`, state byte at `+0x30`. Decoded entries resolve to real
  entities (`csgo_previewplayer`, `weapon_*`), so `layoutValidated=1` gates any filtering.
* The engine runs this tick **every rendered frame, including while the demo is paused**, and the preview pawn is in its list with state byte `0x02`. The freeze comes only from the zero delta.
* A positive delta is granted to the preview pawn's animation object only, during the engine's own tick. Evaluate, commit, bone setup, and render therefore run in the engine's native per-frame order. The scope comes from a discovery pass: a list-filtered forced tick records which animation objects belong to preview pawns, and grants apply only to that set.
* Granting stops when playback resumes or the modal closes. The learned set is cleared, because the panel and its pawn are rebuilt between opens.

Out-of-band `AnimTickUpdate` calls are not used, because the pose they compute is discarded later in the frame.

### Composition layer



With the demo paused, the panel's composition layer is not produced. The preview box shows only the black modal behind it, and the character returns as soon as playback resumes. `transparent-background` is set, so the black is the modal showing through an unproduced layer. Pixel comparisons of the preview box are identical from frame to frame while paused, including with a forced tick. Re-asserting `SetReadyForDisplay` does not make the layer render. The tick-delta grant is required but not sufficient, because the scene's composition-layer render does not run while the demo is paused. The `PreviewRenderProbe` scene-gate bypass covers this gate.

### Interpolation gate

`C_BaseEntity::m_bHasSuccessfullyInterpolated` controls whether the client draws an entity. The client draws an entity only after it has interpolated at least once, and interpolation needs the clock to advance. A paused demo pins the clock, so a preview pawn spawned during a pause never sets this flag, and the renderer skips its model. The held weapon is a separate entity and still draws, which is why the modal can show a gun with no body.

**The fix uses the engine's own escape hatch**, `C_BaseEntity::m_bRenderEvenWhenNotSuccessfullyInterpolated`. `Panorama/PreviewRenderFlag.cpp` sets it on the preview pawns while the modal is open and releases it on close. Nothing forges an interpolation state, and nothing touches playback, the demo tick, or any non-preview entity.

### Diagnosing this kind of bug

Use `mvm_previewprobe snap a|b|c|report`. It takes a three-point differential of the same pawn: A while broken, B a few seconds later while still broken, and C after a brief play at `demo_timescale 0.1` (about 7 ticks) has restored it. The report lists offsets where `A != C` **and** `A == B`, so values that the running animation churns on its own are excluded. Poking the reported dwords back one at a time isolates the cause. The schema dump at `%TEMP%\hlae_schema_econ.txt` maps each offset to its `C_BaseEntity` field name.

Some reported differences are effects rather than causes. `BODY+0x4D0..4D8` is recomputed every frame and cannot be written, and the pointer at `ENT+0xF68` appears once the entity is drawn. Only latched values are worth poking.

### Commands

* `mvm_previewrender`: the scene-gate bypass for the Customize preview, implemented in `Panorama/PreviewRenderProbe.{h,cpp}`.
* `mvm_previewvis auto|on|off|scan|status`: the interpolation fix. `auto` follows the Customize modal. `scan` shows each preview pawn's `hasInterpolated` and `renderEvenWhenNot` values. `off` restores the engine default, where a pawn spawned during a pause is not drawn, and is the quickest way to reconfirm this diagnosis after a CS2 update.
* `mvm_previewprobe rs|read|poke|hold|snap`: the instruments above. `poke` and `hold` are raw dword writes for bisecting this kind of problem.
* Both schema fields resolve **non-fatally**. If a game update renames them, `mvm_previewvis status` reports `available=0`, and the preview falls back to the engine default instead of the tool writing into an entity. See `CLAUDE.md`.
