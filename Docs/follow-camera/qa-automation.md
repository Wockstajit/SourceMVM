# Follow / Attach camera QA automation

The Follow camera is **100% console-command driven**, so an automation bot can exercise and assert every
mode over the netconsole **without a physical mouse**. Drive it with `mirv_filmmaker follow …` and
`editor …`. Assert with `follow state64` and `follow campose`.

## Prerequisites

1. **Build**: run `build.bat` from the repo root and check for `BUILD OK`. Confirm that these exist:
   - `build\staging-release\bin\HLAE.exe` and `build\staging-release\bin\x64\AfxHookSource2.dll`
   - `build\staging-release\bin\x64\FilmmakerDemoInfo\FilmmakerDemoInfo.exe` (the Go demo-info helper;
     the `go build` step must succeed, not just warn)
2. **A demo** with players, grenades, weapon drops and a C4 (any competitive demo).
3. PowerShell 5+ (the harness scripts are cross-version).

## Launch CS2 with a netconsole (no GUI, no input takeover)

```powershell
powershell -ExecutionPolicy Bypass -File Automation\launch\launch-cs2-netcon.ps1 -Port 29010
```

This launches CS2 windowed at 1600x1200 with the hook injected and `-netconport 29010`. It then waits
for the window and port to respond. Counter-Strike 2 is located automatically from Steam's library list.
Pass `-Cs2Dir <install root>` only to override that (see [Machine paths](../../BUILDING.md#machine-paths)).
Then load and play or pause a demo (`playdemo <name>` over the netcon, or through the UI).

Send commands over the netcon with the shared helper, or with the verifier's `Send` and `Read-State`
pattern ([Automation/netcon/cs2-netcon.ps1](../../Automation/netcon/cs2-netcon.ps1),
[Automation/netcon/fm-eval.ps1](../../Automation/netcon/fm-eval.ps1)). `state64` output is base64 in
chunks (`index/total payload`). Decode it and run `ConvertFrom-Json`. See `Read-EncodedState` in
[verify-followcam.ps1](../../Automation/verify/verify-followcam.ps1).

## One-shot verifiers

```powershell
pwsh Automation\verify\verify-followcam.ps1 -Port 29010          # lifecycle, placement, mouse, preview, screenshots
pwsh Automation\verify\verify-grenade-tracking.ps1 -Port 29010   # discover grenade, seek before throw, prove reacquire
```

Artifacts (logs, screenshots, `run.json`) land in `Automation\runs\<name>\<timestamp>\`.

## Exercising the six modes (command, then assert)

Open the workspace first with `mirv_filmmaker editor on`. Read state with `mirv_filmmaker follow state64`,
which returns JSON `f`. After a preview frame, add `mirv_filmmaker follow campose` to assert that the
camera actually moved to the target.

### 1. Player lock-on

```
mirv_filmmaker follow mode lockon
mirv_filmmaker follow type player
mirv_filmmaker follow place
mirv_filmmaker follow nearest
mirv_filmmaker follow preview
```

Assert: `f.mode==0`, `f.type==0`, `f.hasCamera==true`, `f.targetIndex>=0`, `f.targetValid==true`,
`f.enabled==true`. `campose`: `pos` ≈ placed point, `target` ≈ the player.

### 2. Player bone attach

```
mirv_filmmaker follow type player
mirv_filmmaker follow mode attach
mirv_filmmaker follow nearest
mirv_filmmaker follow bone head
mirv_filmmaker follow offset 0 0 8
mirv_filmmaker follow fov 70
mirv_filmmaker follow preview
```

Assert: `f.mode==1`, `f.attachment=="head"`, `f.offset==[0,0,8]`, `f.fov==70`, `f.enabled==true`.
`campose`: `mode=attach`, and `pos` tracks the head. The camera survives the player's death while the body
persists when `autodead 0` is set. With `autodead 1`, it follows for `deathhold` playback seconds and then
stops. Check both modes, pause during the hold, and repeat at `demo_timescale 0.25`. `follow.sample` must
report a changing ragdoll target, and `follow.deathhold` must stop at the configured duration. Also switch
modes after editing offset, FOV and death settings to verify that each mode restores its own values.

With a demo already open through `live.bat`, the focused regression script
[verify-follow-death.ps1](../../Automation/verify/verify-follow-death.ps1) accepts a known death tick and
the victim's demo user ID. It checks both modes, independent settings and resets, continued corpse
tracking, pause, and timed holds at normal and quarter speed:

```powershell
Automation/verify/verify-follow-death.ps1 -DeathTick 14240 -UserId 4
```

Those example values identify user 4's death in the demo ending in `_129` (measured 2026-10-26: alive
at tick 14185, dead by 14262). The death must fall between 80 ticks before the given tick and 35 after
it. The script writes complete NetCon transcripts and `results.json` under
`build/verification/follow-death`, and it leaves playback paused.

For attachment drift, run [verify-follow-mount.ps1](../../Automation/verify/verify-follow-mount.ps1) on the
same demo. It replays head motion, both hands, a death, and weapon swaps. The weapon case needs a player
holding a gun who lives for 13 seconds from its tick; user 1 holds an M4A4 at 13900:

```powershell
Automation/verify/verify-follow-mount.ps1 -UserId 1 -MovementTick 13000 -DeathTick 14240 -WeaponSwitchTick 13900
python Automation/tools/analyze_follow_mount_probe.py <mvm_debug_log> --assert-stable
```

The [probe analyzer](../../Automation/tools/analyze_follow_mount_probe.py) groups render samples by mount
and entity handle, including returning weapons. It checks local camera position, forward and up drift
below 0.01 units/degrees, resolved orientation, and preserved mount names. Inspect the screenshots too,
because constant local math alone cannot prove that the chosen model point is correct. Check
`attachDebug.source`. Body mounts must report a bone source. Guns must resolve the requested native
attachment or its alias, and must not silently fall back to their root.

### 3. Grenade

```
mirv_filmmaker follow type grenade
mirv_filmmaker follow nearest          # or selecthandle <h> from f.candidates
mirv_filmmaker follow previewtick      # routes to the throw-tick seek + reacquire
```

Assert: `f.type==1`. After `previewtick`, `f.grenadePending==true`, then the demo seeks to about
throw−40 and the projectile is reacquired (`f.targetValid==true`). Toggle `follow mode attach` to ride it.

### 4. Weapon / C4 (event-driven)

```
mirv_filmmaker follow type weapon
mirv_filmmaker follow state64          # wait for f.eventStatus==2 (ready)
mirv_filmmaker follow eventselect <index from f.events[]>
mirv_filmmaker follow previewtick      # jumps to drop tick − 40, sets weaponSource=dropped
mirv_filmmaker follow nearest
mirv_filmmaker follow preview
```

Assert: `f.type==2`, `f.events[]` is non-empty once `f.eventStatus==2`, `f.selectedEvent==<index>`, and
after `previewtick`, `f.weaponSource==2` (dropped). For held tracking, run `follow weaponsource held`. Then
`f.weaponSource==1`, and the camera rides the player's active weapon through switches
(`f.weaponPlayerIndex>=0`).

### 5. Weapon attach point

```
mirv_filmmaker follow type weapon
mirv_filmmaker follow mode attach
mirv_filmmaker follow weaponsource held
mirv_filmmaker follow nearest
mirv_filmmaker follow bone muzzle
mirv_filmmaker follow preview
```

Assert: `f.mode==1`, `f.type==2`, `f.weaponSource==1`, and `f.attachment=="muzzle"`. The last check applies
only if the model exposes that point, so check the selected candidate's `attachments[]`. `campose` tracks
the gun as the player moves and shoots.

### 6. Lock-on and attach toggle (any type)

```
mirv_filmmaker follow mode attach
mirv_filmmaker follow mode lockon
```

Assert: `f.mode` flips. Placement (`hasCamera`) is required only in lock-on. Attach owns the view without a
placed camera.

## State fields to assert (no screenshots needed)

From `follow state64`: `enabled`, `hasCamera`, `mode`, `type`, `typeName`, `weaponSource`,
`weaponPlayerIndex`, `targetIndex`, `targetHandle`, `targetValid`, `targetName`, `targetDistance`,
`offset[3]`, `rotation[3]`, `fov`, `look`, `position`, `prediction`, `deadzone`, `maxTurn`, `autoDead`,
`switchWeapon`, `switchBomb`, `holdLast`, `attachment`, `grenadePending`, `grenadeThrowTick`,
`eventStatus`, `selectedEvent`, `events[]`, `candidates[]`.

From `follow campose`:
`[followcam][campose] enabled=… mode=lockon|attach type=… pos=(x y z) ang=(p y r) fov=… target=(x y z).`
Assert that `pos` and `ang` track `target`. In lock-on, `pos` stays static and `ang` follows. In attach,
`pos` rides the target plus or minus `offset`.

## Screenshot spot-checks (optional)

For visual confirmation, capture the primary monitor through the harness (engine and OS capture, no mouse):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Automation\capture\capture-main-monitor.ps1 -Out shot.png
```

`verify-followcam.ps1` already does this for the Path-camera and Follow-camera UIs. It uses
`mirv_filmmaker editor eval "$.CamEditor.setInspectorMode('follow')"` and `setAttachmentMenuOpen(true)` to
frame the panel first.
