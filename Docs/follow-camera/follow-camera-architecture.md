# Follow / Attach camera architecture

## Components

| Layer | Where | Responsibility |
|---|---|---|
| State + logic | `FollowCamera.cpp` (the `FollowCamera` singleton) | Owns `FollowCameraState` and runs the per-frame solve. The class spans several translation units, and its other duties live in the files below. |
| Game host | `FollowCameraHost.h`, answered by `FollowCameraHostSource2.cpp` (CS2) and `AfxHookSource/Filmmaker/SourceFollowCamera.cpp` (CS:S) | Everything the follow camera asks the game: the demo clock and seeks, the editor's mouse, the other cameras it takes the view from, the target providers, the entity scan behind the candidate list, grenade sightings, mount points and the demo's recorded events. The files above and below it are shared and compile into both hooks. |
| Math | `FollowCameraMath.{h,cpp}` | Pure functions: look-at, smoothing, quat to angles, local-offset rotation. No engine deps (also compiled standalone by the test target). |
| Target providers | `FollowTargetProviders.{h,cpp}` (CS2; providers defined inline in the header) | Adapt a player / entity / attachment / held-weapon into a position (+ optional orientation) each frame. |
| Target discovery | `FollowTargetDiscovery.{h,cpp}` | Picking *what* to follow from the host's scan: `Candidates` (scoping and order), `Players`, `SelectNearest`, `SelectEntity`, `SelectHandle`, grenade tracking, the death retarget. |
| Demo event index | `FollowEventIndex.{h,cpp}` | `EnsureEventsLoaded` / `EventsSnapshot` / `SelectEvent`, the background load of the host's event scan, and the Preview-Tick seek. |
| State JSON | `FollowCameraStateJson.cpp` | `BuildStateJson`: the payload the Camera Editor's Follow inspector reads. |
| Commands | `FollowCommand.cpp` `Follow_RunCommand` | The `mirv_filmmaker follow ...` console surface (the automation API; routed from the `FilmmakerCommand.cpp` dispatcher). |
| UI | `CameraEditorFollowInspectorJs.h` (Panorama JS, `#include`d into `CameraEditorJs.h`'s script body) | The editor's Follow inspector. Reads the pushed state JSON, issues console commands back. |
| State bridge | `CameraEditorHud.cpp` | Embeds the follow state JSON into the editor's `state` attribute each frame and calls `render()`. |
| Pose application | `CameraBridge.h` | `CameraBridge_SetCameraPose` pushes an absolute pose; `CameraBridge_GetCurrentCamera` reads the free cam. |
| Demo pre-scan | `FilmmakerDemoInfoGo/main.go` (Go), then `DemoInfoHelper.{h,cpp}` (C++) | Writes and reads `scoreboard.fmjson` (v10) in the demo's source_mvm project folder, including the weapon/C4 event list. |

## Internal model

```cpp
enum class FollowTargetType { Player, Grenade, Weapon };  // Weapon = held/dropped weapon OR C4
enum class FollowMode       { LockOn, Attach };           // turret vs ride-along
enum class WeaponSource     { Auto, Held, Dropped };      // for Weapon type
```

Defined in [FollowCamera.h:20](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.h#L20) /
[:29](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.h#L29) /
[:36](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.h#L36).

`type` and `mode` are **orthogonal**. In the UI, the Lock-on/Attach toggle sets `mode` for any type, and
the Target-Type dropdown (its options depend on `mode`) sets `type`. Together they reach all six
`(type, mode)` combinations. The state struct ([FollowCameraState](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.h#L91))
carries `mode`, `rotationOffset`, `weaponSource` and `weaponPlayerIndex`, along with `offset`,
`fov`, smoothing and on-death fields. It reuses `attachmentName` as the bone or weapon attach point. The
configurable fields live in `FollowCameraSettings`, with a saved copy per mode. Switching modes saves and
restores only these fields. Target selection and camera placement stay shared. Reset affects the active
mode only.

## Provider interface

[`IFollowTargetProvider`](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.h#L120) has a transform
method that gives attach mode its orientation:

```cpp
virtual bool GetWorldTransform(FollowVec3& pos, FollowAngles& ang); // true = orientation available
```

| Provider | File | Notes |
|---|---|---|
| `PlayerTargetProvider` | [FollowTargetProviders.h:76](../../AfxHookSource2/Filmmaker/Movie/FollowTargetProviders.h#L76) | Player pawn; eye origin/angles give an orientation. |
| `EntityTargetProvider` | [FollowTargetProviders.h:114](../../AfxHookSource2/Filmmaker/Movie/FollowTargetProviders.h#L114) | Dropped weapon / grenade / C4 by handle; render angles when reachable. |
| `AttachmentTargetProvider` | [FollowTargetProviders.h:147](../../AfxHookSource2/Filmmaker/Movie/FollowTargetProviders.h#L147) (extends `EntityTargetProvider`) | `LookupAttachment(name)` + `GetAttachment(idx, origin, quat)`; quat to angles via `FollowQuatToAngles`. Attachment points only, with no bone-matrix work. |
| `HeldWeaponProvider` | [FollowTargetProviders.h:195](../../AfxHookSource2/Filmmaker/Movie/FollowTargetProviders.h#L195) | Resolves `pawn->GetActiveWeaponHandle()` each frame, so it tracks the *currently held* weapon across switches. |

[`MakeProvider`](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp#L447) asks the host for the provider
for `(type, mode, weaponSource)`. [`Candidates`](../../AfxHookSource2/Filmmaker/Movie/FollowTargetDiscovery.cpp#L73)
builds the selectable list (players, grenades, held and dropped weapons, and C4) from the host's scan.

## RunFrame: two paths

[`RunFrame`](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp#L491) is called from
`Filmmaker::RunMainThreadFrame`, and only while the camera editor is active.

- **Lock-on**: the turret solve (`FollowLookAt` and `FollowSmoothAngles`), run once per rendered frame
  at view setup ([`ViewSetupLockOnOverride`](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp#L789))
  against the interpolated target. The placed camera stays put, and `rotationOffset` is added as an aim
  trim after the look-at.
- **Attach**: `provider->GetWorldTransform()` gives `basePos,baseAng`. `finalPos = basePos + FollowRotateVector(offset, baseAng)`
  (local-space offset), and `finalAng = FollowComposeRotation(baseAng, rotationOffset)`. Smoothing and
  prediction stay at zero, and render-time sampling rebases the mount onto the interpolated body pose.

Capture stores `inverse(mount) * camera` as a rigid rotation. Render-time rebase composes that local
rotation with the fresh mount instead of adding Euler components. Position sliders use forward, right
and up. Rotation composition accounts for Source's negative Y right axis. Entity mounts read scene-node
world angles, not the direction of travel.

Attached head, chest and pelvis mounts ride stock `clip_limit`, `c4` and `grenade0` bone attachments while
alive and dead. Eyes is explicitly the render-eye mount. Hands use native attachments.

Missing body mounts hold the last camera pose. A requested weapon attachment survives target changes and
swaps. Native `muzzle`/`muzzle_flash` and `shell_eject`/`eject` aliases are retried. A drawn knife without
the requested point temporarily rides the grip. Dropped weapons without that point use their oriented
entity root. No fallback changes saved framing.

Death status is evaluated independently of provider validity. A dead pawn can still expose a moving
skeleton. For Lock-on and the remaining virtual mounts, `FollowPlayerPose` calibrates living virtual
mounts to stock model attachments (head `clip_limit`, upper spine `c4`, lower spine `grenade0`). The
local position and orientation basis then follow that attachment through death. Stale render-eye or
origin values are never used as corpse positions. Missing skeleton attachments use lost-target policy.

Death hold counts forward demo ticks once, independent of render frames and playback speed. The one-tick
correction CS2 can report on pause is ignored. Larger seeks reset the window. Retargeting runs after the
hold. Otherwise auto-disable stops the preview when it expires. With both options off, the camera keeps
following the corpse. `follow.sample` and `follow.deathhold` instrument the raw target, source, validity,
timer, and final stop decision.

Both paths push the final pose through `CameraBridge_SetCameraPose` and cache it for the `campose`
instrumentation readout.

`follow.mount.capture`, `follow.mount.render`, and `follow.mount.scene` record the capture frame, the
render camera in mount-local coordinates, and the resolved scene-node pointers and offsets. Facing and
up vectors must stay constant through animation and weapon-return cycles.

## Data flow

```
                 console command (human or netcon bot)
                              │
   mirv_filmmaker follow …    ▼
        ┌──────────────────────────────────┐
        │ FollowCommand.cpp  Follow_RunCommand() │  (routed from FilmmakerCommand.cpp)
        └──────────────────────────────────┘
                              │ setters / actions
                              ▼
        ┌──────────────────────────────────┐
        │ FollowCamera (singleton)          │
        │  RunFrame() ── pose ──► CameraBridge_SetCameraPose
        │  BuildStateJson()                 │
        └──────────────────────────────────┘
                              │ state JSON (every frame)
                              ▼
   CameraEditorHud::BuildStateJson() embeds it as  "follow": { … }
        (CameraEditorHud.cpp:315) ──► SetAttributeString(root,"state",…) (CameraEditorHud.cpp:461)
                              │
                              ▼
        ┌──────────────────────────────────┐
        │ CameraEditorJs render()           │  reads st.follow, updates controls
        │  buttons / sliders ──────────────────► issue mirv_filmmaker follow … (loop back)
        └──────────────────────────────────┘
```

The demo event list flows separately. `FollowCamera::EnsureEventsLoaded`
([FollowEventIndex.cpp](../../AfxHookSource2/Filmmaker/Movie/FollowEventIndex.cpp)) starts a joinable
worker that calls `FollowHost_ReadDemoEvents`, keyed on `FollowHost_DemoPath()`. In CS2 that is
`ReadDemoInfoViaHelper` (which reads and refreshes `scoreboard.fmjson`) on the engine's `GetDemoFilePath()`. The events are merged into the state JSON only for the Weapon type, nearest
the playhead first ([FollowCameraStateJson.cpp](../../AfxHookSource2/Filmmaker/Movie/FollowCameraStateJson.cpp)).

## File map

The class's method *declarations* live on `FollowCamera` in `FollowCamera.h`. The *definitions* are split
across the translation units listed below. Providers, candidate discovery, event indexing, and state-JSON
serialization each have their own translation unit (see the Components table above).

| File | Lines | What |
|---|---|---|
| [FollowCamera.h](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.h) | 21/30/37 enums (+ 43 `FollowTargetStatus`) · 84 `FollowEventRecord` · 128 `FollowCameraState` · 150 provider iface · 168 class | model, state, interface, instrumentation decls |
| [FollowCameraHost.h](../../AfxHookSource2/Filmmaker/Movie/FollowCameraHost.h) | whole file | what the follow camera asks the game |
| [FollowCameraHostSource2.cpp](../../AfxHookSource2/Filmmaker/Movie/FollowCameraHostSource2.cpp) | 125 `FollowHost_MakeProvider` · 151 `FollowHost_FindTargets` · 238 `FollowHost_AttachPoints` | CS2's answers |
| [FollowCamera.cpp](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp) | 386 `SetMode` · 404 `SetWeaponSource` · 410 `SetWeaponPlayer` · 430 `GoToTarget` · 447 `MakeProvider` · 491 `RunFrame` · 667 attach path · 789 lock-on solve · 903 `SetOffsetAxis` · 909 `SetRotationOffset` · 986 `PrintStatus` · 1002 `PrintCamPose` | placement, mode/type setters, the per-frame solve |
| [FollowTargetProviders.h](../../AfxHookSource2/Filmmaker/Movie/FollowTargetProviders.h) | 76 `PlayerTargetProvider` · 114 `EntityTargetProvider` · 147 `AttachmentTargetProvider` · 195 `HeldWeaponProvider` (all defined inline in the header) | CS2's target providers |
| [FollowTargetDiscovery.cpp](../../AfxHookSource2/Filmmaker/Movie/FollowTargetDiscovery.cpp) | 73 `Candidates` · 179 `Players` · 220 `SelectNearest` · 234 `SelectEntity` · 244 `SelectHandle` · 270 `TrackSelectedGrenade` · 312 `ResolvePendingGrenade` · 340 `TryDeathRetarget` | candidate scoping + selection |
| [FollowEventIndex.cpp](../../AfxHookSource2/Filmmaker/Movie/FollowEventIndex.cpp) | 13 `EnsureEventsLoaded` · 72 `SelectEvent` · 85 `PreviewTick` | demo event index |
| [FollowCameraStateJson.cpp](../../AfxHookSource2/Filmmaker/Movie/FollowCameraStateJson.cpp) | 51 `BuildStateJson` | state to JSON |
| [FollowCameraMath.h](../../AfxHookSource2/Filmmaker/Movie/FollowCameraMath.h) / [.cpp](../../AfxHookSource2/Filmmaker/Movie/FollowCameraMath.cpp) | `FollowQuatToAngles` (.cpp:47), `FollowRotateVector` (.cpp:62), `FollowInverseRotateVector` (.cpp:77) | math helpers |
| [FollowCommand.cpp](../../AfxHookSource2/Filmmaker/FollowCommand.cpp) | `Follow_RunCommand` (whole file; routed from `FilmmakerCommand.cpp`) | command surface |
| [CameraEditorFollowInspectorJs.h](../../AfxHookSource2/Filmmaker/Panorama/CameraEditorFollowInspectorJs.h) | n/a | Follow inspector UI: one card with a persistent mode selector and Target, Framing and Tracking pages. Lock-on aim controls are hidden in Attached. Both modes expose death/lost-target behavior. |
| [CameraEditorJs.h](../../AfxHookSource2/Filmmaker/Panorama/CameraEditorJs.h) | `render()` follow block ~1210–1343 | reads `st.follow`, drives the inspector built above |
| [CameraEditorHud.cpp](../../AfxHookSource2/Filmmaker/Panorama/CameraEditorHud.cpp) | `BuildStateJson` 205, embeds follow 315, push 461 | state bridge |
| [DemoEntry.h](../../AfxHookSource2/Filmmaker/Demo/DemoEntry.h) | `DemoEvent` 38 | event record |
| [DemoInfoHelper.cpp](../../AfxHookSource2/Filmmaker/Demo/DemoInfoHelper.cpp) | `kSchemaVersion=10` 24, parse `events` 246 | reader |
| [main.go](../../FilmmakerDemoInfoGo/main.go) | `schemaVersion=10` 36, `addEvent` 181, event handlers 230–309, emit 387 | writer |

> Raw-string note: the embedded Panorama JS in `CameraEditorJs.h` is split into adjacent `R"EDJS(…)EDJS"`
> literals because MSVC caps a single string literal at ~16 KB (error C2026). Keep each chunk under that
> cap. If a chunk grows too large, add another `)EDJS"` / `R"EDJS(` split at a line boundary.
