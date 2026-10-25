# Customize Player

The Camera Editor and Config panel share a **Customize Player** modal for changing any player's
appearance during offline CS2 demo playback. It edits agent, primary/secondary weapon finishes,
knife type/finish, gloves, and per-finish wear.

This page covers the UI contract. The native mechanisms and reverse-engineering details are
in [cosmetics-overview.md](../cosmetics/cosmetics-overview.md) and
[cosmetics-cs2-methodology-notes.md](../cosmetics/cosmetics-cs2-methodology-notes.md).

## Current status

Implemented:

- generated, localized catalogs for weapon finishes, knife finishes, gloves, and agents;
- one target for both panels: the player CS2's own demo bar is spectating (first or third
  person), or any player, alive or dead, picked from the menu on the name in the title bar;
- current-loadout detection for that player, and the last loadout a dead player had alive;
- a window that can be dragged anywhere and shrunk (full, compact, collapsed), and keeps its place
  and size for the session;
- SteamID64-keyed profiles that survive pawn recreation, round changes, seeks, and observer
  switches;
- weapon paint, knife type/paint, glove, and agent application;
- native 3D preview with a 2D fallback. It works **while the demo is paused**: it composites,
  idles, and draws the character, including a close and reopen done entirely while paused
  (see [Paused preview](#paused-preview) below);
- staged Apply/Cancel behaviour plus Reset to Default and Clear All;
- search, rarity colours, thumbnails, and picked-up-weapon ownership warnings;
- per-finish wear sliders with legal float ranges and an optional cap override.

Not implemented in the modal:

- seed selection (commands currently send `seed 0`);
- StatTrak and nametag controls.

The modal is development-facing. Re-verify it after each CS2 update. The model, econ,
Panorama, and animation integration depend on engine details that change between versions.

## User flow

1. Open Camera Editor or Config. In Config the modal opens over the panel; Config stays open
   behind it.
2. Select **Customize Player**. It edits the player you are watching, in first or third person.
   In free cam, or to pick someone else, open the menu on the name in the title bar. With nobody
   to edit yet, that menu opens by itself.
3. Picking a player switches the view to them in first person. A dead player stays in the menu,
   dimmed, and picking them puts the free cam at the eye of their body.
4. Choose an agent, finish, knife, or gloves. Changes are staged in the preview.
5. Select **Apply** to write touched slots to the demo.

A click anywhere inside the window, or on the empty screen around it, never closes it. ✕, Cancel
and Escape do (Escape closes the player menu first).

Primary and secondary sections appear only for weapon slots the player currently owns. Knife,
gloves, and agent choices remain available. If the held weapon was picked up from another player,
the modal identifies the original owner and routes that weapon finish to the profile that actually
owns the econ entity.

**Randomize agents** applies immediately to every spawned T/CT player, including bots.
It uses the full agent catalog and chooses a model matching each player's team; the selected
player's team filter does not restrict the pool. **Apply agent to all players** also includes
bots and applies only to the chosen model's team. Bot agent choices are runtime-only. They are keyed by
the full controller handle (including its serial) and cleared when the demo session ends.
They never share a SteamID-zero profile or change bot weapon or glove cosmetics. Dead pawns keep
the model they had while alive; new choices apply when the pawn is alive again.

## Who is being customized

`Cosmetics/CustomizeTarget.{h,cpp}` answers that once per frame for both panels, and
`CustomizeTarget_StateJson()` hands the same answer to the Camera Editor's state and to Config's
**Customize Player** button (`customizeReady`). The rules are engine-free in
`Cosmetics/CustomizeTargetMath.h` and tested by `Automation/tests/customize-target-math-tests.cpp`.

- **Who is watched comes from CS2's demo bar.** `CameraTimelineJs.h` reads
  `GetDemoControllerState()` and `GetDemoPlayers()` from the `HudDemoController` panel every frame
  and publishes them (`specstate`, `demoplayers`). In a GOTV demo the local observer reads mode 0
  and no target, and matching the camera against a pawn's eye only works in first person. That is
  why Customize used to grey out or guess the nearest player in third person.
- **The roster is every T and CT controller**, from the demo bar, with a scan of the controllers as
  the fallback. A dead player's controller points at an observer pawn, so the body is found
  through `CCSPlayerController::m_hPlayerPawn`, or a pawn whose `m_hOriginalController` names
  them. Observer pawns are never a body.
- **The order is: pick, then watched, then the last target.** A pick holds while the view settles
  on that player (up to 120 frames), and is dropped when the view later moves to someone else
  (`spec_next`, a click on CS2's own bar). Free cam watches nobody, so the last target stays.
  Without a demo bar, the eye match is the fallback, and only onto a living roster player: on its own
  it lands on bodies and observer pawns.
- **Picking switches the view.** A living player gets CS2's own first-person spectate
  (`SpectatePlayer`, run by the demo bar's script). CS2 will not spectate the dead, so a dead
  player gets free cam at their body's render eye for a few frames.
- **Dead players keep their last loadout.** While a player is alive with health and holds a gun,
  their loadout is kept by SteamID. After death the modal shows that copy (`lastSeen`) instead of
  the weapons on the ground; only the agent and gloves are read off the body when there is no copy.
  A seek back drops the copies, since they came from later lives. After any seek nothing is
  saved for 32 frames, while the pawns and their weapons arrive. The gun rule exists because the
  demo's first frames give pawns health but only a default knife; a seek past those players' deaths
  made that knife-only read their last loadout (`customize.seek` logs each jump).
- `customize.target` (on change) and `customize.pick` log every decision to `mvm_debug`.

## The window

`Panorama/CameraEditorCustomizeWindowJs.h` owns size, position and dragging.

| Size | Width | What shows |
| --- | --- | --- |
| Full | 1240 × 720 | Slot list, 3D preview, picker |
| Compact | 800 × 720 | Slot list and picker. The game behind the window is the preview |
| Collapsed | 800 × 56 | The title bar alone, with its buttons |

- The size buttons sit in the header. Only full size keeps the 3D preview; the other sizes
  destroy it, and returning to full builds it again (`custShowsPreview()`).
- Drag the window by its title bar (the grip dots or the title), never by the player button.
  `custClampPos()` keeps the 56px title bar on screen; the body may hang below the bottom edge.
- Position and size last the session: the script publishes them as `custlayout`,
  `CameraEditorHud` keeps them (`m_customizeLayout`), and every rebuild and both panels read them
  back from the state (`customizeLayout`).
- The player menu hangs under the title. One column per team, full names, T in gold and CT in
  blue, dead players at 45% opacity with a `Dead` tag on the title when one is picked.
- At full size the menu drops over the 3D preview, a native composition-layer panel that took the
  menu's clicks whatever its z-index. The preview steps aside while the menu is open
  (`positionPreview3d`) and is re-settled when it closes (`custSetPlayerMenu`).

## Switching players without a hitch

A pick reseeds the whole modal. Three things keep that from stalling a frame:

- The slot lists are decorated with one read of the selected option (`wearSuffix()`), not one
  per option. Per option it re-filtered the catalog: 77-120 ms for some knife lists.
- A picker pane builds its first 9 tiles at once and the rest 6 per frame (`pumpRows`).
- The 3D preview follows on the next frame (`custPreviewDue`), so CS2's own work for the new
  preview scene does not land on the frame that rebuilt the lists.

Measured on Inferno, paused, Customize at full size over Config, with `mirv_filmmaker health
resetTiming` before each burst of 8-10 picks: the worst frame went from 154-340 ms to 44-59 ms,
about one frame over 33 ms per pick. Normal frames there were 15-18 ms. The same picks with the
window closed stayed under 31 ms. `[mvmpreview] reseedCost` (only while `mvm_debug` records)
splits each reseed into its parts.

## Wear control

`wearSliderBlock()` in
`AfxHookSource2/Filmmaker/Panorama/CameraEditorCustomizeJs.h` owns the float UI.

- The generated catalog carries `wearMin` and `wearMax` from the paint kit's
  `wear_remap_min/max`.
- The native Slider always reports 0..1; `wearFracToValue()` and `wearValueToFrac()` map that
  fraction into the finish's legal float range.
- `wearTiersForSlot()` clips Factory New, Minimal Wear, Field-Tested, Well-Worn, and
  Battle-Scarred bands to the reachable range. Empty tiers are omitted and narrow tiers keep the
  colour band even when their abbreviation does not fit.
- **CAP ON** enforces the finish's legal range. **CAP OFF** exposes the full 0..1 range.
- The cap flag and float are stored per slot and selected finish in `custItemWear`, so switching
  away and back restores the staged value.
- The bar is driven by the editor's OS-cursor pipe (`attachCursorDrag` in `CameraEditorWidgetsJs.h`),
  like the inspector sliders: a click jumps to the point under the pointer and a drag maps the
  pointer across the rail. The native Slider is only value storage and the fallback drag when the
  pipe is unavailable. While Customize is open, only its own bars (`__modal`) can claim a press,
  so a click on a wear bar cannot also move an inspector slider hidden under the modal.
- Default paint-kit-less items hide the wear block and send `wear 0`, preventing a previous
  finish's float from leaking into the default item.

The mapping and interaction invariants are exercised by
`Automation/tests/panorama-interaction-layout-tests.mjs`.

## Catalog generation

Source data:

- `AfxHookSource2/Filmmaker/Data/cosmetics.json`
- `AfxHookSource2/Filmmaker/Panorama/CameraEditorCosmeticsCatalog.inc`

Regenerate from current CS2 game data:

```batch
python Automation/tools/generate_cosmetics_catalog.py
```

Run the generator's check mode and the CS2 asset verifier after a game update:

```batch
python Automation/tools/generate_cosmetics_catalog.py --check
python Automation/verify/verify-cs2-assets.py
```

Do not hand-edit the generated catalog include.

## Architecture

All paths below are under `AfxHookSource2/Filmmaker/`.

| Surface | Owner |
| --- | --- |
| Modal, slot list, picker panes, staged state, wear UI | `Panorama/CameraEditorCustomizeJs.h` |
| 3D preview wiring and fallback | `Panorama/CameraEditorCustomizePreviewJs.h` |
| Player menu, target chrome, the per-frame pass both panels run (`customizeFrame()`) | `Panorama/CameraEditorCustomizePlayersJs.h` |
| Window sizes, drag, session layout | `Panorama/CameraEditorCustomizeWindowJs.h` |
| Modal input/state bridge; the modal over Config (`RunCustomizeOverConfig`) | `Panorama/CameraEditorHud.{h,cpp}` |
| Config's button and `config customize` | `Panorama/ConfigHud.cpp`, `ConfigHudJs.h` |
| Demo bar read and native spectate | `Panorama/CameraTimelineJs.h`, `CameraTimelineHud.{h,cpp}` |
| Target, roster, picks, last-alive loadouts | `Cosmetics/CustomizeTarget.{h,cpp}`, rules in `CustomizeTargetMath.h` |
| Loadout reads | `Cosmetics/CosmeticUiQueries.{h,cpp}` |
| Console grammar | `Cosmetics/CosmeticCommands.cpp` |
| Profile/application system | `Cosmetics/CosmeticOverrideSystem.{h,cpp}`, `CosmeticProfile.{h,cpp}` |
| Knife/glove/agent model work | `Cosmetics/CosmeticKnifeSwap.cpp`, `CosmeticGloveSwap.cpp`, `CosmeticModelSwap.{h,cpp}` |
| Paused preview: scene-render gate | `Panorama/PreviewRenderProbe.{h,cpp}` (`mvm_previewrender`) |
| Paused preview: animgraph tick | `Panorama/PreviewAnimTick.{h,cpp}` (`mvm_previewtick`) |
| Paused preview: pawn visibility | `Panorama/PreviewRenderFlag.{h,cpp}` (`mvm_previewvis`) |
| Preview instrumentation | `Panorama/PreviewSceneProbe.{h,cpp}` (`mvm_previewprobe`), `PreviewAnimClock.{h,cpp}` (disproven clock harness) |

Panorama runs on the main/UI thread. While the modal is open, the native input path captures raw
keyboard characters and wheel input, queues them, and dispatches them to the Panorama helpers from
`CameraEditorHud`. They must never reach demo controls.

## Layout (design 7b, "always-open picker")

At full size the modal is a **1240 × 720** card with a 56px header and three body columns: slot
list, preview, and an always-open picker. Its only popup is the player menu, and it has no footer.

| Band | Contents |
| --- | --- |
| Header (56px, `#0f131a`) | Grip dots, `CUSTOMIZE PLAYER`, the player button (the name in team colour and ▾, or `Choose a player`), a `Dead` tag, the pickup banner, then `Reset to default`, `Cancel`, `Apply`, the size buttons and ✕. The actions are 34px tall and built into `custHeadActions`. |
| Slot list (262px) | Five 76px rows (Agent, Primary, Secondary, Melee / knife, Gloves), each with a 3px rarity bar, an 84×56 thumbnail, the caption, the short name in rarity colour and a wear line (`Field-Tested · 0.182`, `Stock`, `CT agent`) |
| Preview (440px) | The `MapPlayerPreviewPanel` over a radial `#1c2534 → #0c1016` ground, with a `HOLDING · PRIMARY` chip in the corner |
| Picker (the rest) | The selected slot's pane: crumb, search, rarity chips, a 3-up tile grid, and the wear block pinned to the foot |

`itemDrop()` keeps its name and contract (`update` / `setPickup` / `setSearch` / `open`), which staging,
pickup routing and every command rely on. The slot row is the selector, and a **pane** in the picker
column holds the options.

- **One pane per slot, one showing.** `selectSlot()` shows the selected slot's pane, paints its row
  gold (`#1a2230`, 1px `#f0b323`), and for Primary, Secondary and Melee it calls
  `setActiveCategory()` so the preview holds that slot. Gloves and Agent do not change what the
  model holds. A pane that is hidden when its option set changes is marked stale and rebuilt when
  it is next selected, so a populate never lays out five grids. A slot that disappears (no primary)
  hands the selection on via `ensureActiveSlotVisible()`, and a loadout reseed under the open modal
  reselects the held slot.
- **Tiles.** `CUST_TILE_W` × 118px, 8px gap, 3 per row. Panorama has no grid, so `tileShell()`
  builds each row as its own `flow-children: right` line of three, and the last tile of a line drops
  its right margin. At 1080p the tiles are 162px (`3 × 162 + 2 × 8 = 502` inside the 504px picker
  column). Below that, Panorama snaps every width and gap to whole device pixels, and the card's 1px
  border stays a whole device pixel. At 854 × 480 the grid measures 223 device px, but the tiles
  need 72 + 4 + 72 + 4 + 72 = 224, so in a single `right-wrap` row the third tile wrapped. So
  `sizePane()` measures the grid and gives the tiles `custTileFitW()`: the widest whole-device-pixel
  tile that fits three to a line, capped at `CUST_TILE_W`. That is 162px at 1080p and above, and never
  more than one device pixel narrower anywhere. A tile shows the artwork, the
  **short** name (no `M4A1-S |` prefix and no `· Minimal Wear` suffix, since the crumb and the wear block say
  those), a ✔ when equipped, and a 3px rarity bar. Stock rows read grey with dimmed art. `tileShell()`
  owns the tile's fill and border through one painter, so a mouseout cannot revert the selected look.
- **Grouped slots** (agents, knives, gloves) show the stock picks first, then one **model tile** per
  family reading `N finishes ›` (`N variants ›` for agents). Clicking it sets `rec.navGroup`; the
  crumb becomes `ALL KNIVES › KARAMBIT`, and clicking the root goes back. The family holding the
  current pick is outlined and its count line turns gold.
- **Search** is a plain panel drawing `[query][caret][placeholder]`: this HUD context never gives a
  TextEntry keyboard focus, so characters arrive through `custChars()` (the C++ WM_CHAR route) and
  always land in the **selected** pane. Escape closes the player menu, then backs out of a model,
  then closes the modal. The
  caret blinks from the per-frame pump (`custCaretBlink`), time-based.
- **Scrolling.** The grid sits in a clipped viewport and is translated by `sizePane()`; the wheel is
  consumed in C++ and routed to `custWheel()`, which scrolls the selected pane. `fadeTop`/`fadeBot`
  show only in a direction that still has tiles, and need the viewport's `flow-children: none`.
- **Agent actions.** `Randomize agents` and `Apply agent to all players` sit on the right of the
  Agent pane's crumb row.
- **The HOLDING chip** is a root-level panel one z-step above the 3D preview (which is itself
  root-level and laid over the stage), and `positionPreview3d()` moves it with the preview rect.

The card is drawn at its design size: `CUST_SCALE` and `CUST_FONT_BOOST` are both 1, so the px in the
source are the px on screen, and the design's 10px captions are the floor. Both stay live-tunable
over netcon (`customizeScale(pct)`, `customizeFont(pct)` + `ui_rebuild`), and `customizeOpacity(pct)`
retunes the surface while keeping its blue-grey `rgba(11,14,20,…)` base.

The reset confirm and the 3D preview are root-level panels and do **not** inherit the card's
transform; `applyCustScale()` scales the confirm, and `positionPreview3d()` multiplies the stage's
measured size by `custScale()`. `GetPositionWithinWindow()` is transform-aware;
`actuallayoutwidth`/`actuallayoutheight` are not. That asymmetry is why positions need no correction
and sizes do.

## The wear control

One direct float slider per skinnable slot, pinned to the foot of its pane (design 7b:
10px 12px padding on `#141a23`, a 24px head over a 24px bar, a 3px white handle). Layout: a
non-flowing head row (`WEAR` left, the float readout **centred**, the cap toggle right) over a rail
carrying the rarity-tinted fill, a **tick at each internal tier boundary**, a tag per reachable tier,
and the handle.

- The head **must not** be a flow row. In one, the readout's position is whatever the `WEAR` label and
  the cap button leave over (off-centre), and it shifts every time the cap button changes size. For
  the same reason the readout gets no `width`/`height`: a Label sized to its parent anchors to the
  parent's top-left and the alignment properties stop applying.
- The **boundary ticks** mark where one tier ends and the next begins. They are pooled at
  `tiers − 1`; tick *k* sits at tier *k+1*'s low edge.
- **Tags and ticks are placed in px** once the rail has a width. A pane is built hidden, so the first
  `sync()` sees a 0-wide rail; `relayout()` re-runs the layout from the per-frame pump when the real
  width appears. (A percent x in `position` is not portable either. The browser mockup resolves it
  against the tag, not the rail.)
- **What animates, and what deliberately does not.** The handle's *position* snaps and its width and
  glow ease, because `SliderValueChanged` fires continuously through a drag and an animated position
  lags behind the cursor. The fill transitions its colour only: an animated width rubber-bands.
  `react()` owns everything that depends on hover/drag, and the drag is *inferred* from the value
  stream (`goLive()`, decaying on a 0.25s timer).
- The **cap toggle rests bigger while uncapped** and overshoots on each toggle, scaled from its right
  edge so the growth can never reach the centred readout. The resting weight is real type and
  padding (`capRest()`), not a permanent transform.
- **Every scheduled settle is sequence-guarded** (`capPopSeq`, `tierPopSeq`, `liveSeq`), so a second
  trigger inside the settle window cannot let the first `$.Schedule` collapse the newer animation.
- The float readout draws in **`S.fontMonodigit`** so its centred, constantly rewritten digits do not
  jitter under the cursor.
- The drag path (`setWear(..., light)`) touches only the slot's wear, its wear control and the slot
  row's wear line; the full populate is debounced to after the drag settles (`wearSettleSoon`).

## Type

Named steps live in the `TYPE` scale in `CameraEditorCustomizeJs.h`; the 7b layout writes its own
design sizes inline (10px captions, 12-13px names). Glyphs (`›`, `✔`, `✕`) are icons sized by their box.

Copy is stored in **natural case** and uppercased with `text-transform`. `custTitleName` and
`custPickupLbl` carry player names taken from the demo, and JS `toUpperCase()` is
locale-insensitive and length-changing (German `ß` becomes `SS`). On labels ellipsised at a `maxWidth`,
uppercasing could itself push a name into truncation. `normalizeTeamName` still uses `toUpperCase()`
because that is data normalisation, not display. APPLY is a plain "Apply"; when a picked-up weapon
routes to its original owner, the pickup banner beside the title is what says so.

A truncated tile name carries a hover tooltip (`showTip`, clip-gated on the tile's own name label), so
the hidden tail (`Doppler Phase 2` vs `Phase 4`) stays reachable without a tip on names that fit.

## The 3D preview is a fixed view

There is deliberately **no user zoom, pan, or rotate** on the character. It always sits at the baked
`PREVIEW_FIT_*` framing.

- `previewUpdateDrag()` is gated behind `PREVIEW_INTERACTIVE = false`. The tracking code is still
  there because `previewSave()` / `previewFit()` use the same constants for tuning over netcon;
  `previewGeom().pan` should read `0,0` in normal use.
- `previewDragCatcher` (the transparent panel over the preview) **stays**. The native
  `MapPlayerPreviewPanel` has `mouse_rotate` and `panzoom_enabled` set. They are part of its
  compositing path, so the catcher is what stops the panel's own drag-rotate from spinning the model.
- Re-framing is a code/tuning change, not a user gesture: `previewFit('{"scale":N,"x":N,"y":N}')`.

## Pickers: rarity filters and model drill-down

Each slot's picker gets whichever of the two navigation aids helps that slot. Never
both, because they solve different problems:

| Slot | Aid | Why |
| --- | --- | --- |
| Primary, Secondary | **Rarity chips** under the search box | firearm finishes span every tier, so filtering to Covert/Classified is the fast way through the list |
| Agent, Knife, Gloves | **Model drill-down** (Bayonet / Karambit / … , Bloodhound / Sport / …) | every knife and glove finish is Covert, so a rarity filter there is seven dead buttons; the useful axis is *which knife* |

Chips are spelled out (Consumer … Contraband), toggle independently and OR together. Only tiers the
carried weapon actually has get a chip, and the bar is omitted below two live tiers. Tier-0 stock rows
ignore the filter so the slot's reset stays reachable. A grouped slot opens on its model list.

## Rarity is read from the loot lists, not the paint kit

`paint_kits_rarity` is **not** the rarity CS2 displays for a weapon finish, so the generator does not
trust it. Trusting it would show two visible bugs:

* **Knives would come out smeared across every tier.** A paint kit like Case Hardened is Mil-Spec on a
  rifle, but ★ Karambit | Case Hardened is Covert. The *item* rarity wins. `FIXED_ITEM_RARITY` in
  the generator pins knives and gloves to Covert.
* **Whole weapons would lose their Covert finish.** Verified in the live data: `cu_usp_kill_confirmed`
  and `cu_ak47_asiimov` are both tagged `legendary` (Classified) in `paint_kits_rarity`, yet both
  are Covert in game. The authoritative source is the rarity-suffixed client loot list the
  `[kit]weapon` combo ships in (`*_ancient` = Covert). `Catalog._load_combo_rarity()` indexes those;
  the paint kit tag is only the fallback for items in no case.

Contraband is tier 7 and CS2 spells it `immortal` in `paint_kits_rarity`; the M4A4 | Howl is the
only one, and it reaches tier 7 through that fallback because it ships in no rarity-suffixed list.
`RARITY` in `CameraEditorCustomizeJs.h` and `RARITY_MAP` in the generator must stay in sync.

`Automation/tests/panorama-interaction-layout-tests.mjs` asserts all of this against the *generated*
catalog, so a regeneration that reintroduces either bug fails there rather than in-game.

## Paused preview

Dressing a shot happens while the demo is paused, so the preview has to work then. It does, and
nothing in this path resumes or re-pauses playback. Three **independent** engine gates had to be
opened. Each has its own module and failure signature. After a CS2 update, the symptom shows which
one broke:

| Symptom while paused | Gate | Module / command |
| --- | --- | --- |
| Preview box is **black** | The panel never submits its 3D view (Valve throttles the scene render to every third *demo* frame) | `Panorama/PreviewRenderProbe.{h,cpp}`, `mvm_previewrender` |
| Character is a **still frame**; item picks leave the gun floating in the old stance | The animgraph delta is an integer **tick** count, and a paused demo pins the tick to 0 | `Panorama/PreviewAnimTick.{h,cpp}`, `mvm_previewtick` |
| **Only the held weapon draws**: body missing, "a gun floating in a black box" | The pawn never interpolated, and the renderer skips un-interpolated entities | `Panorama/PreviewRenderFlag.{h,cpp}`, `mvm_previewvis` |

The third gate is the subtle one. `C_BaseEntity::m_bHasSuccessfullyInterpolated` only becomes true
once the clock advances, so a preview pawn **spawned during a pause** never sets it and its model is
skipped. The held weapon is a separate entity and still draws. That is why the failure shows up
as a problem with *reopening* the modal. Opening the modal while the demo plays lets the pawn interpolate
immediately, and it keeps drawing after you pause. A close and reopen during the pause spawns a
replacement that starts from zero.

The fix uses the engine's own escape hatch for exactly this case,
`C_BaseEntity::m_bRenderEvenWhenNotSuccessfullyInterpolated`, applied to `*previewplayer*` pawns
while the modal is open and released on close. No interpolation state is forged, and playback, the
demo tick, and every non-preview entity are untouched. Both schema fields resolve **non-fatally**:
if Valve renames one, `mvm_previewvis status` reports `available=0` and the preview skips the
write instead of writing through a guessed offset.

Full root-cause analysis, the measurements, and the differential method that found it:
[panorama-player-preview-summary.md](../panorama/panorama-player-preview-summary.md).

## Console surface

The modal emits the same commands available for manual testing:

```text
mirv_filmmaker config customize
mirv_filmmaker cosmetics pick <controller index|clear>
mirv_filmmaker cosmetics roster
mirv_filmmaker cosmetics observerprobe
mirv_filmmaker cosmetics target current
mirv_filmmaker cosmetics player <steamId|current> weapon <def> paint <id> wear <float> seed <id> stattrak <id>
mirv_filmmaker cosmetics player <steamId|current> knife <def> paint <id> wear <float> seed <id>
mirv_filmmaker cosmetics player <steamId|current> gloves <def> paint <id> wear <float> seed <id>
mirv_filmmaker cosmetics player <steamId|current> agent <modelPath|default>
mirv_filmmaker cosmetics clearPlayer <steamId|current>
mirv_filmmaker cosmetics status
```

`config customize` opens the modal over Config. `cosmetics pick` makes a player the target and
switches the view to them, as the menu does; `clear` goes back to the watched player. `roster`
prints the players with their controller, body, alive state and team, and `observerprobe` prints
what the demo bar, the local observer and every pawn report, for diagnosing the target.

Run `mirv_filmmaker cosmetics` for diagnostic and recovery subcommands. Use `mvm_debug` for
per-frame investigation; see the "Diagnose runtime behaviour" section of the repo's `CLAUDE.md`.

## Verification

Source-level:

```batch
node --test Automation/tests/panorama-interaction-layout-tests.mjs
node --test Automation/tests/customize-player-picker-tests.mjs
cmake --build --preset x64-release --parallel --target AfxHookSource2
ctest --test-dir build/x64-source2-release -C Release -R CustomizeTargetMathTests
```

Live verification should cover:

- current target and current loadout detection, the same in Config and the Camera Editor, in
  first person, third person and free cam;
- picking a living and a dead player from the menu: the view goes to first person on them, and a
  dead player shows the last loadout they had alive;
- dragging the window, the three sizes, and the layout surviving close, reopen and a switch
  between Config and the editor;
- a burst of picks with `mirv_filmmaker health resetTiming` before it and `mirv_filmmaker health`
  after it, against the numbers in [Switching players without a hitch](#switching-players-without-a-hitch);
- Apply versus Cancel;
- legal-range and CAP OFF wear values;
- default-item wear reset;
- picked-up weapon routing;
- weapon switch, seek, death, round transition, and spectator switch;
- knife/glove/agent precaching with no first-use hitch;
- preview behaviour while playing and paused. The paused case has its own acceptance test.
  Opening the modal during playback lets the pawn interpolate first, which hides the paused failure. Run this acceptance test:
  open the modal, let the demo play, confirm the character renders, **pause**, close the modal, reopen it
  **still paused**, and confirm the character renders with no resume/re-pause anywhere. Then repeat
  the close/reopen cycle a few times and change an agent while still paused. Each rebuild spawns a
  fresh pawn that has to pass the same gate. `mvm_previewvis scan` shows every preview pawn's
  `hasInterpolated` / `renderEvenWhenNot` state if the picture disagrees with expectations.
