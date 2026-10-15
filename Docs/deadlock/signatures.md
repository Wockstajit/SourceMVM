# Deadlock signatures and offsets

The values `AfxHookDeadlock` resolves at load depend on one game build. This page records how each one was derived. After a Deadlock update, re-derive a value from its recorded anchor instead of starting over.

Verify offline, without launching the game:

```batch
py -3 Automation/verify/verify-deadlock-signatures.py
```

Exit code 0 means every required entry resolved. The table itself lives in that script. This page explains it.

## Module layout

Deadlock keeps the engine modules where CS2 does. Only the client moves, because its mod directory is `citadel` instead of `csgo`.

| Module | Path under `<install>/game` |
| --- | --- |
| client | `citadel/bin/win64/client.dll` |
| host | `citadel/bin/win64/host.dll` |
| everything else | `bin/win64/<name>.dll` |

Module names match CS2's. The hook's suffix-matched `LoadLibrary` dispatcher needs one literal changed: `citadel/bin/win64/client.dll` replaces `csgo/bin/win64/client.dll`.

## What ports unchanged

These were verified against the installed build, so they need no Deadlock-specific work.

Every CreateInterface version string is byte-identical to CS2's: `Source2Client002`, `Source2EngineToClient001`, `ResourceSystem013`, `VFileSystem017`, `SchemaSystem_001`, `SceneSystem_002`, `RenderDevice003`, `PanoramaUIEngine001` and `NetworkClientService_001`. The interface-acquisition layer can use those names. Matching names do not prove binary compatibility. Method slots and argument layouts still need verification for each interface.

### Native console registration

Re-derived on 2026-10-12 (Deadlock build 6774). `VEngineCvar007` resolves to `CCvar`. Both installed games now have 46 entries. Deadlock had 48 before. Keeping the old mapping after the update sent registration to GetCmd. The adapter in [DeadlockIcvarSlots.h](../../AfxHookSource2/Deadlock/DeadlockIcvarSlots.h) uses these derived slots:

| Method | CS2 | Deadlock |
| --- | --- | --- |
| GetCvar | 41 | 41 |
| RegisterConCommand | 42 | 42 |
| UnregisterConCommand | 43 | 43 |
| GetCmd | 44 | 44 |

The registrar takes a 56-byte creation descriptor. MSVC passes its structure return through a hidden RDX buffer, so the descriptor goes in R8 and extra flags go in R9. [DeadlockConsoleAbi.h](../../AfxHookSource2/Deadlock/DeadlockConsoleAbi.h) encodes this contract, and [the ABI test](../../Automation/tests/deadlock-console-abi-tests.cpp) verifies the call shape. The current registrar at tier0 RVA `0x69ff0` keeps that layout. Live native-console checks enumerated 1417 engine commands, found all ten expected probe names, exercised both filmmaker aliases, and stopped `mvm_debug`. Those checks cover registration only. The remaining view, Panorama and demo signature drift, and the main pump, are still unresolved.
After a relevant game update, re-derive with [derive-deadlock-icvar-slots.py](../../Automation/verify/derive-deadlock-icvar-slots.py). Compare the installed registrar with the [Deadlock SDK](https://github.com/alliedmodders/hl2sdk/tree/deadlock), and verify native command execution. `mirv_filmmaker cvarprobe` and the `mvm_debug` registration handles provide runtime evidence. A matching command, cvar name, help text and flags prefix does not mean value storage matches. Deadlock has extra callback fields, so the adapter uses the engine setter for `sv_cheats`.

### Other shared anchors

The engine-generic RTTI class names survive, including `.?AVCViewRender@@`, `.?AVCFileSystem_Stdio@@`, `.?AVCMaterialSystem2@@` and `.?AVCRenderDeviceDx11@@`. The vtable-by-RTTI approach therefore works, and only the game-specific names change:

| CS2 | Deadlock |
| --- | --- |
| `CCSGO_Hud` | `CCitadel_Hud_Main` (Panorama panel class name `CitadelHud`) |
| `CCSGOInput` | `CCitadelInput` |
| `CRenderingPipelineCsgo` | `CRenderingPipelineCitadelPostHud` |
| no equivalent | `CCitadel_Hud_Replay`, `CCitadel_Hud_Replay_Scrubber` |

Two CS2 byte patterns match Deadlock unchanged: the `CUIEngine::makeSymbol` call site and the `CUIEngine` global store. Both are compiler-shape patterns in shared Panorama client code.

## Resolved signatures

The Panorama and HUD RVAs below were re-derived and tested on build 6774 (2026-10-12). Other rows keep the 2026-09-10 baseline and do not claim compatibility with build 6774. The verifier reports drift and missing required signatures separately.

| Name | Module | RVA | Source |
| --- | --- | --- | --- |
| `panorama_RunScript` | panorama | `0xb4450` | build 6774, tested |
| `client_hud_panel_global` | client | `0x1b195b6` | build 6774, tested |
| `client_uiengine_global` | client | `0x1ee8b30` | CS2 pattern, unchanged |
| `client_makeSymbol_call` | client | `0x1ee3ca8` | CS2 pattern, unchanged |
| `client_view_setup_prologue` | client | `0x5d7120` | build 6774, tested |

### `panorama_RunScript`

The JS injection primitive that the whole Panorama layer runs on: `CUIEngine::RunScript(engine, contextPanel, source, originFile, line)`.

Build 6774 references `CUIEngine::RunScript (compile+run)` at instruction RVA `0xb44ba`. Its `.pdata` entry begins at `0xb4450`. Chained unwind fragments continue past the first range, so the end of that range is not the end of the whole function. The prologue homes `r9`, `r8` and `rdx`. It then preserves source `r8` in `r12`, panel `rdx` in `rbx`, engine `rcx` in `r15` and origin `r9` in `rdi`. The signature includes the source-register move and matches exactly once in `panorama.dll`.

The five-argument call was verified on the restored main-thread pump. A `ui_eval` script printed its `Hud` context, reported its 1600x1200 dimensions and 14 direct children, and found the built workspace. It also worked after a failed old-replay load returned to the hideout, and an explicit workspace rebuild succeeded. This proves script execution and HUD access. It does not prove working replay playback or the remaining camera and schema integration.

### `client_hud_panel_global`

The HUD root panel global is a `CPanel2D**`. The `hud.xml` reference in build 6774 is the `lea` at `0x1b195bd`, inside the function that begins at `0x1b19310`. Seven bytes earlier, the new root is stored by `mov [rip+disp32], r14` at `0x1b195b6`, which targets global `0x3d00158`. Read the displacement at pattern +3, relative to the instruction end at +7. The following `mov rcx, [r14+8]` retrieves the underlying `CUIPanel`, as the runtime accessor does. The pattern matches exactly once in `client.dll`.

The layout path is `file://{resources}/layout/hud.xml`. CS2 uses `layout/hud/hud.xml`. Runtime script results confirmed a real `Hud` context, not just a non-null address.

### Build 6774 timing and demo accessors

The frame-stage time read at client `0x188665a` resolves `CGlobalVars**` at `0x3310df0`. It loads curtime from +0x30. The view matrix function reads framecount from +4 of the same global. The live interval at +0x54 measured 0.015625 seconds. In a new offline recording, pause held tick 79 and time 30.671680 across samples. Seeking to 250 produced 33.343555 (171 ticks, 2.671875 seconds). This is the engine clock, which is not necessarily the elapsed time since recording began.

Demo-player slots 2/3/11/12/14/17 did not move. Their member offsets grew by eight bytes: start tick +0x204, playing +0x1230, pause gate +0x228, seek target +0x208, paused +0x1231 and scale +0x214. Both offline tools now check the current accessor instructions. Runtime checks confirmed playing and paused states, tick, scale from 1.0 to 0.5, forward seek completion, and neutral time and tick after unload. A backward seek of the test recording failed inside the engine and returned to the hideout. Successful reverse playback is not claimed.

### `client_view_setup_prologue`

Build 6774 fills the view through `ClientModeCitadelNormal` slot 15, entry `0x5d7120`. The function takes `(mode, embeddedView)` in rcx/rdx and returns no consumed value. A home-slot spill alone is not evidence of extra parameters. Live read-only enumeration confirmed the mode's vtable at `0x26bf000` and that slot's exact entry and prologue.

The full chain runs: `CViewRender` vtable `0x26c1c08`, then slot 4 `0x1927580`, then slot 17 `0x192db80`, then the client-mode slot-15 call returning to `0x192dbb4`. Slot 17 passes `CViewRender + 0x10` as the embedded view, publishes the resulting pose to camera globals, and returns to slot 4, which builds the projection matrices. The default after phase applies our override after the provider fills the view and before those consumers run.

Runtime installation requires both class vtables, the expected setup entry and the caller contract. Each override also requires the view object's RTTI vtable, the exact return address and the initialization thread. A missing gate refuses writes. The offline verifier resolves and validates primary RTTI complete-object locators. A matching unrelated pointer table is not enough. Its synthetic regression tests cover a wrong class, slot, target, displacement, invalid locator and ambiguous table.

Offsets used by the hook are relative to `CViewRender`, not the embedded view:

| Field | CViewRender offset | Embedded-view offset |
| --- | --- | --- |
| width / height | `0x440` / `0x448` | `0x430` / `0x438` |
| FOV | `0x4a8` | `0x498` |
| origin | `0x4b0` | `0x4a0` |
| angles | `0x4c8` | `0x4b8` |

Live tests confirmed unclaimed zero writes, explicit position, angle and FOV changes, matching 90 and 45 degree screenshots, game camera restoration, and both body and write isolation switches. These checks cover the hook only. They do not cover every higher-level camera, renderer or replay feature.

## Replay filename accessor (build 6774)

The primary `CEngineClient` vtable at engine2 RVA `0x4e2790` has the replay filename getter at slot 43 (`0x73310`). It checks playback using player slot 11, gets the embedded `CDemoFile` through player slot 1 (`0x242c0`, player +0x20), then calls file slot 3 (`0x24180`, string +0x8). Engine slot 44 calls file slot 4 instead and returns the recorder's name. Both player references in the filename getter must resolve to the same global. The offline verifier checks the RTTI, the call sequence, the embedded-file displacement and the filename member. Six synthetic cases cover the valid chain and mismatched slots, globals, layout and class identity.

Read-only live inspection matched the filename and recorder fields. Live console-open and in-process replay-switch tests saved distinct projects, one with a filename containing spaces. Unload cleared the filename. `Engine_DemoFilePath` bounds and guards the returned string, converts UTF-8 strictly, and resolves existing files against `game/citadel`, `game`, then the working directory. It never uses a launch argument as a fallback.
## Remaining compatibility investigations

- Verify all CUIPanel/native UI interactions and clean render output on build 6774.
- Derive any additional scenesystem contracts needed by unverified features.
- Recheck player-aim release and all input ownership transitions without keyboard injection.
- Investigate old replay serializer failures and current-recording backward-seek failure.

## Re-deriving a broken signature

The steps match the CS2 side, and the verifier provides a tool for each one:

1. Anchor on a nearby string. Profiling scope names, assert text and RTTI names all work. `--find-string` locates it.
2. Find the code that references it RIP-relative, using `--xref-string`. This also reports the containing function, resolved through the `.pdata` `RUNTIME_FUNCTION` table instead of guessed.
3. Take the prologue with `--prologue <module> <rva>`, wildcarding the stack displacements.
4. Confirm with `--pattern`, checking that it matches once and in the module you expect.

`--dump-rtti <substring>` lists mangled class names when the anchor is a class rather than a string.

### `client_entity_list` and `client_entity_from_index`

The client entity table, and the index-to-pointer accessor beside it. Both are CS2's own patterns, unchanged. This is shared Source 2 entity-system code, not game logic. Both patterns are unique in Deadlock's `client.dll`. `DeadlockEntities.cpp` resolves them and reports them through the shared address table. A break then shows as one line in `mirv_deadlock health`, instead of a follow camera that silently never engages.

| Name | RVA | Pattern |
| --- | --- | --- |
| `client_entity_list` | `0x37d5a20` | the global loaded by the function that publishes `Entities/Client Entity Count`; read the displacement at pattern +18 |
| `client_entity_from_index` | `0x14a4dd0` | `4C 8D 49 10 81 FA FE 7F 00 00`: the `0x7ffe` bound check ahead of the chunk walk |

Everything read out of an entity comes from a schema lookup by name, never from an offset in this table. Measured on the 2026-09-11 build: `C_BaseEntity::m_pGameSceneNode` +816, `CGameSceneNode::m_vecAbsOrigin` +200, `m_angAbsRotation` +212, `C_BasePlayerPawn::m_vecViewOffset` +2184, `C_BaseEntity::m_iHealth` +852.

The hero pawn's entity class name is `player`. It is lowercase and comes from the entity's identity block. It is not `C_CitadelPlayerPawn`, the C++ class the schema declares the pawn to be made of. Matching the C++ name finds nothing while the entity table is healthy, and the fault looks exactly like an empty replay. The player's slot is `citadel_player_controller`. It is not a hero, and it has no body or position to frame.

### The schema class layout, measured on build 6774

`CSchemaClassInfo` has an extra eight-byte member at +0x18. Read size is at +0x20, field count at +0x24, and the field array at +0x30. Readiness also requires `C_BaseEntity::m_pGameSceneNode` and `CGameSceneNode::m_vecAbsOrigin` to resolve by name.

`mirv_deadlock schema raw <class>` dumps class bytes and compares candidate field arrays at +0x28 / +0x30 / +0x38. The build 6774 live samples give C_BaseEntity size 1520 with 81 fields, and CGameSceneNode size 304 with 34 fields. Their fields begin at +0x30. sceneNode resolves to 816 and absolute origin to 200. The full export contains 3106 classes and 10344 fields, all within declared size bounds. These numbers are evidence. They are not hardcoded entity offsets to copy. Testing with an invalid scope layout rejected lookups, and restoring the measured scope layout recovered them. Replay enumeration then read three heroes' positions, teams and health with zero reported entity faults.
