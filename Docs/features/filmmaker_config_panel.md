# Config panel

The lightweight **CONFIG** Panorama panel exposes general camera, effects, and ragdoll settings
without opening the full Camera Editor. Toggle it with:

```text
mirv_filmmaker config on|off|toggle
```

Config is a workspace, not a bare overlay. In a demo it scales the world into a preview rect, the same
way the Camera Editor does. Both use the same owner (`Panorama/WorkspacePreview.{h,cpp}`) and the same
`previewrect` attribute, which its JS publishes each frame. Config does not host a timeline or edit
camera paths.

A full-screen world behind the panel is a bug. The cause is always the same: a JS exception inside
`render()` aborts before `layoutDemoWorkspace()` publishes `previewrect`, so the scaler is never asked
for a rect. Watch for
`[confighud] chrome render failed:` in the console and the `confighud.previewrect` `mvm_debug`
channel. The same throw also kills `cfgApplyScroll()`, so "cards vanish while scrolling" and "no
preview" are one symptom. When a card changes, update its references in BOTH
`ConfigHudCameraTabJs.h` and the hydration block in `ConfigHudJs.h`. The header keeps mouse-ownership
and game-HUD controls available from every tab. The bottom actions open Render or Customize Player.

## Layout and files

The panel has four top-level tabs: **CAMERA**, **EFFECTS**, **ENVIRONMENT**, and **RAGDOLLS**.
Each inspector uses a grouped rail and one selected pane. The rail is 180px wide with 52px rows;
a row's title and summary wrap rather than being cut, and a card with a master switch carries it
on its row and in its header. The pane takes the rest of the column (400px at the default width).
A card is **one page with group headings as dividers**; sub-tabs stay only where one page would
not fit the pane (Depth of Field, Weather & Sky, Scene Objects). Every control is one row in the
same box: a 130px label column, the control, a 66px value column. Booleans are square switches in
that row, never chips. Nothing truncates: a segment whose widest caption, times its cell count,
does not fit beside the label drops under it at full width (`cfgCaptionsFit` in
`ConfigHudWidgetsJs.h`), option grids pick their column count from the pane width, both re-fit
when the inspector column is dragged (`cfgRelayout`), and labels wrap. When a tab's rail rows
outgrow a short window, the wheel scrolls the rail while the cursor is over it
(`$.ConfigHud.railWheel(n)` from the console), so every row stays selectable. A single colour field is a full-width bar of its colour, crossed by the custom mark;
on a palette that allows a custom colour, the custom chip is wider and carries the same mark.
The tab-row search indexes controls across all tabs. Rail selection persists independently per tab,
and hidden cards retain their state and render hooks. See the
[shared workspace contract](../panorama/panorama_ui_guide.md#shared-workspace-contract).

All paths below are under `AfxHookSource2/Filmmaker/`.

| File | Responsibility |
| --- | --- |
| `Panorama/ConfigHud.{h,cpp}` | Main-thread bridge and state JSON |
| `Panorama/ConfigHudJs.h` | Shell, chrome, tab strip, Render/Customize actions |
| `Panorama/ConfigHudWidgetsJs.h` | Shared controls and interaction painters |
| `Panorama/ConfigHudCameraTabJs.h` | CAMERA tab |
| `Panorama/ConfigHudEffectsTabJs.h` | EFFECTS tab and the VFX card |
| `Panorama/ConfigHudLightingTabJs.h` | EFFECTS lighting card |
| `Panorama/ConfigHudAtmosphereTabJs.h` | EFFECTS atmosphere card |
| `Panorama/ConfigHudWeatherSkyTabJs.h` | EFFECTS weather and sky card |
| `Panorama/ConfigHudEffectsResetJs.h` | EFFECTS reset footer |
| `Panorama/ConfigHudRagdollsTabJs.h` | RAGDOLLS tab |
| `Panorama/ConfigHudEnvironmentTabJs.h` | ENVIRONMENT: weather, props, extra lights and scene presets |
| `Config/FilmmakerSettingsStore.{h,cpp}` | Persistent feature and UI preferences |
| `Config/EffectsPersistence.{h,cpp}` | Carrying EFFECTS state across a demo change |

## Persistence

Preferences are written to `%APPDATA%\HLAE\filmmaker_settings.json` with a 500 ms debounce and a
shutdown flush. The store includes feature toggles/sliders, selected tabs, and disclosure state.
Opening Config does not reset or expand cards.

Particle category choices remain in `filmmaker_fx.json`. Camera targets, path keys, and
Follow/Attach transforms are shot state and do not belong in global Config preferences.

ENVIRONMENT uses explicit named presets rather than global Config autosave. See the
[Scene editor contract](environment-scene-editor.md) for its controls, reset scope and render limits.

## CAMERA tab

Resolution controls live in native CS2 Video settings, not Config or the launcher.
The camera-motion cards are **Smooth View**, **Handheld Camera**, **Weapon Recoil** and **Weapon Bob**;
lens controls and Viewmodel settings remain on this tab as well.
Each Camera effect card has its own **Reset** action. It resets only that card, including its
master toggle and card-specific companion controls (Smooth View's crosshair and fisheye, and
Weapon Recoil's scoped overrides). Reset is dimmed and cannot be clicked while the complete
card is already at its shipped defaults.

### Smooth View

Smooth View provides aim following, optional **Viewmodel Decoupling** (the weapon and true aim
lead ahead of the camera, which catches up smoothly), breathing, strafe tilt,
fall/jump effects, the fisheye lens toggle, and viewmodel follow controls. The **True Aim
Crosshair** shows the recorded aim direction while the camera trails it. Jump/fall pitch is
derived from the viewed pawn's own ground flag and vertical velocity but smoothed on the render
clock, so takeoff, falling velocity steps, and landing do not become camera-pitch steps.
Smooth View moves only the spectated player's own view. It stands down while one of our cameras
owns the view (a camera path, the Follow camera, a looked-through camera object, the free cam):
its sway on a camera riding a gun or a player moved the camera off what it rides, so the rider
seemed to jitter in frame.

**Strafe tilt leans INTO the strafe and runs on the pawn's real velocity.** Positive roll leans the
view right (measured in game: free camera at roll +10 rotates the world counterclockwise), and the
lean target has the same sign as velocity (`SmoothViewTiltMath.h`). Reverse Tilt leans away.
The camera-derived values remain only as the fallback for when no pawn is under the
camera (`SampleMotion`, using `CameraShake_ViewedPawnMotion`). Speed for the breathing amplitude
and the ground flag for jump/fall pitch use the same source, so climbing stairs does not read as a
jump.

**Adaptive Stabilization** runs the camera as a weighted operator: a critically damped follow that
trails the recorded aim, carries its momentum through a flick and settles without overshoot. The
`SmoothViewCinematicMath.h` header carries the measurements; the short version is what the two
sliders mean.

- **Aim Tightness** is the follow's time constant, and the whole slider is usable: 1 -> 600 ms
  (a heavy gimbal), 25 -> 178 ms, 50 -> 50 ms, 75 -> 14 ms, 100 -> locked to the aim. Steady-state
  camera/aim separation is simply `tightness time constant x turn rate`, bounded by Cone Size, at
  any frame rate. The default 50 saturates the 10 degree cone at 200 deg/s, so ordinary aim
  adjustments read proportionally on the weapon and a real flick reaches the full cone.
- **Stabilization Amount** is a straight wet/dry mix of that stabilized camera against the
  unstabilized one: 0% is the recorded aim exactly, 50% is exactly half the separation of 100%.

**Fisheye Lens** on the Effects page toggles the standalone barrel-distortion D3D pass. It is
its own module and command; Smooth View's Reset switches it off along with the crosshair.

Console: `mirv_filmmaker smoothview ...`, `mirv_filmmaker xhair ...` and
`mirv_filmmaker fisheye on|off|toggle|state`.

### Handheld and Recoil

Handheld provides deterministic procedural and baked camera motion, with viewmodel decoupling and
Centre Lock. Weapon Recoil kicks the gun itself.

The CAMERA tab has effect cards for **Smooth View**, **Handheld Camera**, **Weapon Recoil** and **Weapon Bob**.
Weapon Recoil's camera share is split three ways on its own tab.

Weapon Recoil's card is one page: the scope and style rows at the top, then two groups, then the
PIVOT drawer at the end (Smooth View, Handheld Camera and Weapon Bob are grouped the same way,
with their general switches under a first heading):

- **Weapon**: the gun's own kick: Amount, Punch, Rotation, Snappiness, Recovery.
- **Camera Shake**: how much of that kick the camera takes with it: **Rotation**, **Punch** and
  **Barrel Roll**.
  **These take movement away from the weapon. They add none.** The gun is placed at
  `eyeOrigin + kick` inside `CalcViewModelView`, and that eye origin does NOT contain our camera
  shake. It is bit-identical across a kick on a paused demo. The camera then
  moves separately by `share x kick`, so what reaches the screen is `(1 - share) x kick`: at 0 the
  camera holds still and the whole kick reads on the gun, at 100 gun and camera move together and
  the kick reads as the world moving instead. It is a crossfade at constant total recoil, and it
  is deliberate. Handheld Camera publishes an explicit viewmodel compensation (`handheldVmComp`)
  precisely so its drift does NOT do this, and Weapon Recoil's share does not.

- **Applies to** and **Style** sit at the top of the page, and **Test Shot** is the header action,
  so all three are visible whatever you are tuning. They are not a third topic. They say what the
  groups below are editing, and you fire a test while tuning any of them.

The scope picker is **All / Slot / Type**.
Picking Slot or Type preselects the category of the gun currently in shot, and the note under the
picker names that gun, plus, when the selected scope does not cover it, says so outright. An
override on a category nothing on screen belongs to behaves exactly like a broken one: the
sliders move, the card says the scope owns its values, and no shot ever uses them.

**Style** (**Style 1** default, **Style 2**) picks the reference channel table every per-weapon
tuning multiplies on top of, so switching rescales all of them rather than replacing any. Style 1 is
springy (heavier, punch-led, rides up and settles back), Style 2 is snappier (sharper, wider,
rollier). The names are deliberately numbers. The MW build years the two tables were derived from are
provenance for whoever retunes `WeaponRecoilStyles.h`, and in the card they read as a version picker
for something the tool does not ship. **Show Pivot Orb** under the Pivot slider draws the point the
weapon actually rotates around, in first person. It also flashes on its own for a couple of seconds
whenever Pivot changes, so dragging the slider shows you what you are dragging. The orb needs
Weapon Recoil enabled to draw anything.

Full behaviour and command grammar:
[filmmaker_camera_shake.md](filmmaker_camera_shake.md).

### Viewmodel and shot FOV

The **VIEWMODEL** card owns two different FOVs. **Field of view** is `mirv_fov`: the shot
itself, whole degrees only (5 to 170). **Weapon FOV** is the first-person gun projection and is
already stored with the rest of the viewmodel override.

Shot FOV is written to `%APPDATA%\\HLAE\\filmmaker_settings.json` as
`camFovOn` / `camFov` with the other camera preferences. Keep on carries both shot FOV and the
viewmodel override into the next demo; Keep off, and the CAMERA tab RESET, restore them to the
construction defaults (`mirv_fov default` and a released viewmodel).

## EFFECTS tab

Particle categories include impacts, tracers, muzzle FX, blood, HE/bomb, molotov, and map
ambience. The backend uses the runtime particle-create hook and category-specific precaching.
Fresh defaults are master Off and every category Off.

Console: `mirv_filmmaker fx ...`.

Full documentation:
[filmmaker_effects_modifiers.md](filmmaker_effects_modifiers.md).

### EFFECTS structure

Every card on the four sub-tabs collapses, and its header carries a live readout of its own state
(`TIME OF DAY  Dusk`, `CLOUDS  0.40x · 12 mats`, `GUNFIRE  2 of 3 on`). Set it from the card's
render hook with `body.__note(text)`; it is hidden while the card is open, because the card's own
controls already say it. A status line that
restates a control the operator is looking at is noise, so what remains says only what the
controls cannot: an unavailable engine hook, or a map with no cloud materials.

Consequences to preserve:

- **Card open/shut state persists generically.** `section()` registers any card with a `persistKey`
  in `cfgSections`; `render()` restores them all in one pass from the state JSON's `ui` object
  (`FilmmakerSettings_AppendUiJson`). A key absent from `ui` means the operator never toggled that
  card, so it keeps the `startOpen` it was built with. Defaults live only in the `section()` call.
- **Undo is not a primary action.** Reset/Auto/Hide are `actionBtn`s in a trailing `actionRowEnd`,
  sized to their words and right-aligned. `wideBtn` is reserved for a card's primary action.

- **Groups inside a card use `subHead`, never a nested card.** A caption trailed by a hairline
  (`SUN` → COLOUR / DIRECTION / DISC, `SKY ORIENTATION` → APPLIES TO). Pass `.style.marginTop='0px'`
  on the first one in a card.
- **The VFX mode controls share one fixed 224px column** (`FXSEG_W`) with cells splitting it
  evenly, joined into a single segmented control.
- **`SHADOWS` and `SKY ORIENTATION` are separate cards.** The skybox card's inner strip is CUSTOM | CS2 only.
- **ATMOSPHERE has one strip of sub-pages:** Smoke | Look | Volume | World tint, so the card fits
  the column without scrolling.
- **Cloud shape sits behind a `Shape…` disclosure.** Density is the primary slider; size, drift and
  brightness open on demand.

### Shared EFFECTS footer

Both controls sit below the sub-tab pages and are therefore present on VFX, LIGHTING, ATMOSPHERE
and WEATHER & SKY alike.

**Keep between demos** decides what happens to the EFFECTS tab when you leave one demo and load
another. It defaults to on.

- On: the settings are re-asserted onto the newly loaded level. Most EFFECTS modules keep their
  values in process memory, but what they wrote is level-owned: the sky material precache, the
  cloud material parameters and the sun billboard parameters are all rebuilt by a level load. So
  they are re-applied over three passes to survive a slow load.
- Off: every EFFECTS group is reset to the map default once the new level is up.

**The settle is counted in frames where the world is actually up, never from the level-init
detour**. `LevelInitPreEntity` fires *before* the
level's entities and materials exist, so a frame count from there guarantees nothing. Both
re-assert calls walk the engine's live material array and snapshot each material the first time
they see it. Running mid-load therefore records a half-loaded material as "the map's authored
value" and multiplies against it forever, besides walking an array the engine is still growing.
`WorldReady()` is the same predicate ParticleFx uses (a demo playing or paused, or a live map with
a real level name); the counter restarts if the world goes away again.

**Nothing is re-asserted that was not overridden.** `SkyboxLibrary_Current()` empty,
`CloudFx_HasOverride()` false and `SunFx_HasOverride()` false each skip their call. An
un-overridden module has nothing to carry across, so walking its materials is pure risk for no
effect. Every pass logs which of the three it applied to the `effects.keep` channel.

`mirv_filmmaker effects off` makes the module completely inert on a level change, and
`mirv_filmmaker effects status` reports what it thinks the world and the overrides are. Both exist
so this module can be ruled in or out of a crash in one launch.

Owned by `Config/EffectsPersistence.{h,cpp}`, armed from the `LevelInitPreEntity` detour in
`main.cpp` and performed from `Filmmaker::RunFrame`. The toggle is stored in
`filmmaker_settings.json` as `effectsKeepAcrossDemos`. Console:
`mirv_filmmaker effects keep on|off|toggle`, plus `mirv_filmmaker effects reapply` to re-assert
onto the current level by hand. Progress is logged to the `effects.keep` `mvm_debug` channel.

**RESET** is scoped to the sub-tab you are on and names it (`RESET WEATHER & SKY`). It is two-step:
the first click arms, the second commits, switching sub-tab disarms, and it auto-disarms after four
seconds. It never touches the CAMERA or RAGDOLLS tabs.

The command list lives twice, and the two copies must be kept in step: `FX_RESET` in
`Panorama/ConfigHudEffectsResetJs.h` (the button) and `kResetCommands` in
`Config/EffectsPersistence.cpp` (what a demo change replays when the carry-over is off). Anything
that lives only in JS, such as an open colour picker, sky brightness, or the hide-clouds latch, is
cleared by a per-sub-tab closure registered in `fxResetLocal`.

### Lighting

Lighting controls are reusable Config settings, not one-shot console state. The Sun card
persists its tint RGB, tint amount, uniform/filtered mode, sun yaw, and sun pitch; the Time of Day
exposure is stored alongside them. Values are written to `%APPDATA%\\HLAE\\filmmaker_settings.json`
with the same 500 ms debounce and shutdown flush as the rest of the Config panel, then hydrated
back into the sliders when Config is rebuilt or CS2 starts again.

The live scene-light direction still applies only while a map is loaded. A saved yaw/pitch is
re-applied as soon as the scene-light hook becomes available, so opening a demo does not require
re-entering the values.

## RAGDOLLS tab

The **Death Animations** card plays death animations 1:1, then hands the body to the ragdoll.
Its header switch turns the feature on. The **Animations** row picks MW2019, Classic CoD or
Extra (GMod). The **Nervous System** is its own card beside it, with its own header switch; it
needs Death Animations on, and its rail summary says so while they are off
([filmmaker_nervous_system.md](filmmaker_nervous_system.md)). **Running above** (default 130 u/s, CS2's walk speed) is the speed past which a dying
player only gets running clips. See [filmmaker_death_animations.md](filmmaker_death_animations.md).

The **Death Weapon** card follows the corpse model's `weapon_hand_R` attachment with the
victim's dropped world weapon. Enabling **Involuntary firing** also enables weapon hold; disabling
hold disables firing. These settings apply to new deaths:

- **Firing chance:** 0 to 100%, default 100% of armed deaths. Selection is deterministic across seeks.
- **Firing duration:** 1 to 5,000 ms, default 250 ms. Starts with a shot on the first resolved frame.
  Later shots use the gun's primary firing interval from the installed CS2 weapon data; snipers
  fire once per death. Shots are limited by rendered frames, never emitted as a catch-up burst.
- **Hold duration:** 1 to 10,000 ms, default 250 ms. Stops firing and releases the weapon from its
  last hand pose. Both timers use demo time and stop during pauses. Timing uses the sliders.

Release uses the weapon's existing native CS2 physics body. It starts at the hand's final pose
with measured linear and angular hand velocity, then CS2 simulates gravity, rotation, collision
and settling using that weapon's collision shape and filters. The scene pose follows the body
instead of the demo's recorded drop trajectory. Map-geometry preparation is not required.
The engine ABI is checked before use; if the body is unavailable or unsupported, the weapon
remains held and the card/debug log reports unavailable weapon physics.

Bone-attached cosmetics retain their original attachment-local transforms. CS2 evaluates charm,
StatTrak and bonemerge children; the hold code must not rewrite their offsets relative to the
weapon root or freeze them at the living pose captured during death.

Discharges emit a muzzle flash, casing and tracer from the dropped model's live muzzle and shell
attachments. Native particle attachment keeps the flash aligned as the corpse moves; equipped
attachment offsets are a fallback. This never creates bullets or changes demo events. Non-gun
items such as knives, grenades, C4 and the taser are ignored. Unknown firearm profiles do not
receive an invented firing rate and are logged as unresolved.

Physical arm recoil queues force through the resolved native ragdoll bone-to-body function
before client physics simulation. It targets the right lower arm and follows the weapon's
orientation. `ragdoll.weapon.arm` records `phase=physical` when that call executes, or reports
an unavailable body or fault. The control-pointer probe's `impulseApplied=0` describes
that read-only probe, not the separate physics call. Hand movement alone cannot distinguish
recoil from the corpse's existing motion.

Retention ends if a living player picks up that weapon in the recorded demo. The engine then
controls the gun for its new owner; the corpse feature must not pull that same entity between
two players. Backward seeks and round starts clear the retained poses.

The attachment updates both the interpolated scene-node source pose and its derived world
transform. CS2 can rebuild the latter after frame-stage callbacks, so writing only the final
transform causes the recorded dropped position and rotation to reappear during scene drawing.

For runtime verification, enable `mvm_debug start`, replay a death, then `mvm_debug stop`.
`Automation/tools/analyze_ragdoll_weapon_probe.py <log>` checks independent `ragdoll.weapon.draw`
samples against the intended held or falling prop pose, including quaternion sign equivalence.
Immediate `ragdoll.weapon.final` write/readback messages are diagnostic only, not evidence of a
stable draw.

```text
mirv_filmmaker ragdollweapon keep on|off|toggle
mirv_filmmaker ragdollweapon fire on|off|toggle
mirv_filmmaker ragdollweapon chance 100
mirv_filmmaker ragdollweapon firems 250
mirv_filmmaker ragdollweapon holdms 250
mirv_filmmaker ragdollweapon reset
mirv_filmmaker ragdollweapon state
```

Backend: `AfxHookSource2/Filmmaker/Movie/RagdollWeapon.cpp`, `RagdollWeaponPhysics.cpp` and
`RagdollWeaponRates.h`. Regenerate the firing table after weapon balance updates with
[`generate_ragdoll_weapon_rates.py`](../../Automation/tools/generate_ragdoll_weapon_rates.py)
using VRF-decompiled `scripts/weapons.vdata_c` and `scripts/items/items_game.txt`.
Switches and numeric settings persist in `filmmaker_settings.json` and participate in the RAGDOLLS
footer reset/carry-over behavior.

## Explanations are hover tooltips

Control explanations live on hover, not as printed subtext. Every slider, toggle, effect
category, and handheld preset row explains itself through a tooltip that appears only when the
pointer is over the control's **name label** (never the track, pill, or value). The tooltip is a
single custom fading panel in `ConfigHudWidgetsJs.h` (`attachTip`/`tipShow`/`tipHide`). It lives
in `ConfigHudOverlayRoot` (`cfgOverlay`), a full-screen child of the HUD context at `z-index: 280`,
together with the dropdown lists and menus. Panorama compares z-index between siblings only, so as
children of ConfigHudRoot (z 52) they drew behind the docked demo bar (z 58). `render()` shows and
hides the overlay with ConfigHudRoot. Do not add persistent hint lines under controls; dynamic status lines (e.g. "Effects
are still loading.") are the only standing text allowed.

## Motion

Nothing in the panel appears by hard cut. All entrances share one curve (`MO_ARRIVE`, a strong
exponential ease-out) defined in `ConfigHudWidgetsJs.h`:

- the panel itself slides in when Config opens (`moSlideIn`);
- the card a rail click selects rises 4px as it arrives (`moEnter` from `cfgSelectCard`), once per
  real selection, never per frame;
- a pressed button dips to 97% for the press flash (`makeInteractive`), and switch knobs slide;
- top-level tabs (CAMERA/EFFECTS/ENVIRONMENT/RAGDOLLS) and sub-tab pages fade and rise on a tab change (`moTabSwap`);
- collapsible card bodies fade and rise open, and reverse out before hiding, with the chevron
  rotating rather than swapping glyphs;
- tooltips fade in after a short hover-intent delay and fade back out.

Two rules govern all of it. **Entrances fire only on a real state change.** `applyTabs`,
`applySvTabs`, and `applyHhTabs` run every frame from the render hooks, so each
keeps a `…TabSeen` variable and animates only when the active key actually changed. Firing
unconditionally restarts the animation forever and the page never settles. **Deferred hides are
token-guarded.** A close schedules `visible = false` for after its fade, so a token counter
cancels stale schedules that would otherwise blank a re-opened card or leave a stale tooltip.
Card open/close animates on click only; the settings-hydration path passes `apply(false)` so
restoring five cards at once on panel open does not convulse.

## Panorama contracts

- `width: 100%` resolves against the parent's **border** box in this host, so a full-width child
  of a padded card overflows by the padding and drags its rows past the card edge. Use
  `width: fill-parent-flow(1.0)` for a card's body or wrapper. Check with
  `Automation/livejs/probe-overflow.js`, loaded via `mirv_filmmaker livejs load <abs path>`; it
  expands every card and reports any panel whose right edge passes its parent's.
- Drive diagnostic UI JS with `livejs load`, not `ui_eval`. The console tokenizer rewrites an
  inline script and injects spaces, including inside string literals.
- `$.ConfigHud.togglePass('env.sky')` invokes the actual header/rail pass-switch callback.
  Wait for the native state mirror before toggling again; normal confirmation dialogs still apply.
  `Automation/livejs/probe-preview-press.js` temporarily substitutes only preview input fields,
  logs the real backend selection/grab and visible hint, then releases and restores the render
  callback. It does not move the Windows cursor. Use it in an offline demo with Config open.
  `Automation/capture/capture-game-memory.ps1 -Out <file.json>` records per-process dedicated
  and shared GPU memory; unavailable readings are null rather than zero.
- **The panel chrome is pinned. Only the tab bodies scroll.** Title/Exit, the mouse + Game UI
  row and the tab strip are direct children of the inspector; `scrollView`
  (`height: fill-parent-flow(1.0)`, clip) holds `scrollWrap` with the three tab pages. Scrolling
  the chrome away would hide the tab strip and Exit button as soon as you moved down a page.
- **A page that already fits must not scroll at all, and the readable viewport is the viewport
  minus the fade.** `cfgApplyScroll` computes `usableH = viewH - FADE_H` and leaves `maxY` at 0
  whenever `contH <= usableH`. Only real overflow gets `contH - usableH + CFG_OVERSCROLL` of
  travel, so the last control can still be brought clear of the fade.
- **The wheel hides any open tooltip** (`api.wheel` calls `tipHide`). Scrolling moves content out
  from under a stationary cursor and Panorama fires no `onmouseout` for that, so the popup would
  otherwise hang over controls it has moved away from. Its label is `hittest = false` as well as
  the panel's. A Label is hit-testable by default, which would make the tooltip text a live click
  target over whatever it covered.
- **The TIMELINE dock under Config shares the wheel.** `MovieMode::OnMouseWheel` sends a plain
  notch over a panel to the hosting workspace while the dock is up, and `api.wheel` offers it to
  `$.GraphExp.wheel` first: over the dock it moves the dock's rows (Ctrl zooms, Shift pans),
  anywhere else it scrolls the column. Over the game it does nothing while the dock is up. The
  router used to swallow every notch while the dock was up, so Config could not scroll at all.
  A posted test wheel (`send-wheel.ps1 -Post`) does not move the cursor, and the dock reads the
  cursor from its own mouse stream, so only a real mouse proves the over-the-dock half.
- **The floating action row needs a shortened viewport, not padding.**
  RENDER / CUSTOMIZE PLAYER are parented to `root` at `z-index: 120`, so they float over the
  scroll column. `padding-bottom` on the inspector only extends the scrollable *range*; it never
  moves content up, so whenever a tab's content already fit the viewport (Effects measured 1151px
  in a 1200px viewport) its last row still rendered underneath the buttons. `syncInspectorHeight()`
  therefore sets a real pixel height of `rootHeight - ACTION_CLEAR`.
  `footerBg` (z 100) fills the freed strip so the game does not show through behind the buttons,
  and `bottomFade` (z 110) is the gradient cut-off just above them.
- `scrolloffset_y` is **read-only** here (assignment is silently ignored) and
  `RegisterEventHandler('OnMouseWheel', ...)` is not a valid event type. To verify scrolling,
  measure a child's `GetPositionWithinWindow()` before/after `ScrollToBottom()`.
- Synthetic mouse input (`SendInput`/`mouse_event` wheel) never reaches Panorama, because CS2 reads
  raw input. So UI wheel behaviour cannot be tested by simulating a wheel. Note also that Panorama's
  surface is not the Win32 client rect (measured 1600x1200 surface vs a 1280x960 client under
  125% display scaling); see `Automation/capture/send-wheel.ps1` for that coordinate trap.
- Panorama calls run on the main/UI thread.
- Programmatic Slider writes also raise `SliderValueChanged`; keep the value guards in
  `ConfigHudWidgetsJs.h` and the `quiet` command arguments.
- The HUD context has no mouse-move event; dragging controls must use native Slider panels.
- Do not assign Panorama style properties to an empty string. Use a real transparent value or
  release the inline property with `null` where supported.
- The native DropDown popup is unreliable in this host; cycling buttons are used where needed.
- Live `mvm_test` cursor mode must release both the CS2 game-mouse path and relative input mode.

## What the panel costs per frame

Measured at 2560x1440 on a paused demo frame, comparing the render pump rate with the
panel open and closed and verifying `mirv_filmmaker config state` at every sample:

| | frame time |
| --- | --- |
| Panel closed | 3.44 ms |
| Panel open | 7.15 ms |
| Panel open, `render()` stubbed out | 6.11 ms |

So the panel costs about **3.7 ms**, of which roughly **1 ms is the script** and **2.7 ms is
Panorama compositing the surface** plus the scaled-preview blit and the C++ frame work. Even
deleting `render()` entirely would leave most of it, so painting less is the lever. Trimming the
script alone will not do much. Closing the panel during playback is the only large win available today.

These are not the cost:

- **Collapsing every card changes nothing** (78 vs 79 fps). Collapsing hides panels but the render
  hooks still run, and the cost is not in what they write.
- **The per-frame value writes in `sliderRow`** are not the cost. Identical writes are skipped through a cache shared by drag and sync, so the cache cannot go stale.

`render()` runs every hook for every tab each frame, not just the visible one, so the sweep is
gated on the published `state` string actually changing (`ConfigHudJs.h`). Every hook is a pure
function of that state. None reads the clock, a frame counter or anything random, and the C++
side only republishes the attribute when it changes, so the comparison is exact. A 30-frame
heartbeat refreshes anyway as insurance. While a control is being dragged the state changes every
frame and the sweep runs on every frame.

**Measuring this yourself:** do not leave `mvm_debug` running. A per-frame probe that flushes each
line to disk dominates the frame and will make any reading meaningless. It can produce a bogus
59 fps reading. Compare render-pump counts from `mirv_filmmaker pumpstatus` on a
PAUSED frame, so every sample renders identical content, and check the panel's own state between
samples rather than trusting a toggle to have landed.
