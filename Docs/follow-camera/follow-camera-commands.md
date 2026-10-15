# Follow / Attach camera command reference

Every Follow camera control is a console command, so this page is the **automation surface**. A human
keybind, the Panorama UI and a netconsole bot all drive the feature through these commands. Dispatch
lives in [`Follow_RunCommand`](../../AfxHookSource2/Filmmaker/FollowCommand.cpp), which the
`mirv_filmmaker` dispatcher in `FilmmakerCommand.cpp` routes to.

Prefix everything with `mirv_filmmaker`. Run `mirv_filmmaker follow` with no arguments for the in-game
help text.

## Editor (workspace) commands

The Follow inspector only exists inside the camera editor workspace.

| Command | Effect |
|---|---|
| `mirv_filmmaker editor on` / `off` / `toggle` | Open or close the camera editor workspace (preview, inspector and bottom editor). |
| `mirv_filmmaker editor scale on` / `off` / `toggle` | True scaled-preview viewport (whole frame shrunk, not cropped). |
| `mirv_filmmaker editor curveeditor native` / `graph` / `timeline` / `camera` / `toggle` | Choose the bottom editor. |
| `mirv_filmmaker editor state64` | Base64 JSON dump of editor state (`[cameraeditor][state64]`). |
| `mirv_filmmaker editor eval <panorama js>` | Run JS in the editor context (optional UI poke). |
| `mirv_filmmaker camtl cursor on` / `off` / `toggle` | UI mouse or game mouse (freecam). Forced on while the editor is open. |

## Lifecycle and placement

| Command | Effect |
|---|---|
| `follow place` | Place (or **Replace**) the lock-on camera at the current free-cam pose. |
| `follow reposition` | Enter reposition mode (move in freecam, left-click to set). In attach mode, the left click instead captures the freecam pose as an attach-relative offset, rotation and FOV (`attachcapture`). |
| `follow repositionplace` / `repositioncancel` | Commit or cancel a reposition. |
| `follow attachcapture` | Capture the current freecam pose as the attach offset, rotation and FOV, relative to the target's current transform. |
| `follow clear` | Remove the placed camera. |
| `follow preview` (alias `play`) | **Live Preview**: make the follow camera the active view. |
| `follow stop` | Stop Live Preview and release the view. |
| `follow goto` | Move the freecam to the currently selected target. |
| `follow status` | Human-readable one-line status (`[followcam] …`). |
| `follow state` | Raw state JSON (`[followcam][state]`). |
| `follow state64` | Base64 state JSON (`[followcam][state64]`), sent in chunks. Use it for assertions. |

> Lock-on requires a placed camera. **Attach mode needs no placement**: it rides the target directly, and
> `OwnsView()` returns true in attach mode even without a placed camera.

## Type, mode and source (the six type × mode combinations)

| Command | Effect |
|---|---|
| `follow type player` | Target a player. |
| `follow type grenade` | Target a thrown grenade. |
| `follow type weapon` (aliases `droppedweapon`, `bomb`, `c4`) | Target a held or dropped weapon, or the C4. |
| `follow mode lockon` / `attach` | Turret or ride-along. This is orthogonal to type. |
| `follow weaponsource auto` / `held` / `dropped` | For Weapon targets: auto-pick, the player's active weapon, or the ground entity. |
| `follow weaponplayer <pawnIndex>` | For Weapon targets: scope the candidate and held-weapon list to one player's pawn index. |
| `follow bone <name>` / `follow attachment <name>` | Attach point name (e.g. `head`, `pelvis`, a weapon muzzle point). Only points the model actually exposes resolve. |

The Camera Mode toggle issues `follow mode lockon` or `attach` directly. The Type dropdown next to it
issues `follow type <value>`. Its option list depends on the current mode (see
[follow-camera-ui.md](follow-camera-ui.md)):

| Mode | Type dropdown option | Command issued |
|---|---|---|
| Lock-on | Player | `follow type player` |
| Lock-on | Grenade | `follow type grenade` |
| Lock-on | Weapon/C4 | `follow type weapon` |
| Attach | Grenade | `follow type grenade` |
| Attach | Player Bone | `follow type player` |
| Attach | Weapon Attach | `follow type weapon` |

## Target selection

| Command | Effect |
|---|---|
| `follow nearest` (alias `newest`) | Select the nearest or newest candidate of the current type. |
| `follow select <entityIndex>` | Select a candidate by entity index. |
| `follow selecthandle <handle>` | Select by entity handle (survives index reuse). |
| `follow trackgrenade` | Arm the grenade throw-tick seek and reacquire flow. |

## Events (weapon, C4, grenade) and preview tick

| Command | Effect |
|---|---|
| `follow eventselect <index>` | Select a recorded drop or pickup event (index from the state JSON `events[]`). |
| `follow previewtick` | Jump the demo to about 40 ticks before the selected event (a grenade routes to the throw-tick flow), then arm acquisition. A drop sets `weaponSource=dropped`. |

## Framing (offset, rotation and FOV)

| Command | Range | Effect |
|---|---|---|
| `follow offset <x> <y> <z>` | — | Set the full offset (local-space in attach). |
| `follow offsetx` / `offsety` / `offsetz <v>` | ±256 (UI) | Per-axis offset (slider granularity). |
| `follow rotation <pitch> <yaw> <roll>` | — | Set the full rotation trim. |
| `follow rotpitch` / `rotyaw` / `rotroll <v>` | ±180 (UI) | Per-axis rotation. Attach: orientation trim. Lock-on: aim trim. |
| `follow fov <v>` | 1–170 (UI) | Camera FOV. |
| `follow reset framing` (alias `resetframing`) | — | Reset offset, rotation and FOV to defaults for the current mode. |

## Advanced tracking

| Command | Clamp | Effect |
|---|---|---|
| `follow look <v>` | 0–5 | Look or aim smoothing half-time (s). |
| `follow position <v>` | 0–5 | Position smoothing half-time (s). |
| `follow prediction <v>` | 0–2 | Lead the target by velocity × this. |
| `follow deadzone <v>` | 0–45 | Aim deadzone (degrees). |
| `follow maxturn <v>` | 0–4000 | Max turn rate (°/s). |
| `follow autodead <0\|1>` | — | 0 keeps following the ragdoll. 1 stops after Death hold. Applies to both modes. |
| `follow deathhold <seconds>` | ≥0 | Playback seconds following the ragdoll before stopping or retargeting. Zero acts immediately. Pause does not consume the hold. |
| `follow switchweapon <0\|1>` | — | After Death hold, switch to the dropped weapon. |
| `follow switchbomb <0\|1>` | — | After Death hold, switch to the dropped bomb. |
| `follow hold <0\|1>` | — | Hold the last known position when the target is lost. |
| `follow reset tracking` (alias `resettracking`) | — | Reset tracking and death/lost-target settings for the active mode only, including a three-second timed hold. Attached hides the aim sliders and keeps them at zero. |

Framing and tracking values are remembered separately for Lock-on and Attached during the session.
## Instrumentation

| Command | Output |
|---|---|
| `follow debug <0\|1>` | Per-frame `[followcam][tick]` telemetry on or off. |
| `follow targetprobe` | Log the selected pawn's attachment availability and resolved positions. |
| `follow campose` | `[followcam][campose] enabled=<0\|1> mode=<lockon\|attach> type=<…> pos=(x y z) ang=(p y r) fov=<f> target=(x y z).`: the **computed** camera pose and resolved target world position, for asserting that the camera is tracking or attached. |
| `follow status` | `[followcam] enabled=… hasCamera=… type=… entity=… fov=… offset=(…) look=… pos=… …` |
