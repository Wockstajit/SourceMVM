# Follow / Attach camera changelog

What the rebuild added and changed, with file and line references for review.

## Follow / Attach camera rebuild

### Internal model and math

- New enums `FollowMode { LockOn, Attach }` and `WeaponSource { Auto, Held, Dropped }`, orthogonal to
  `FollowTargetType { Player, Grenade, Weapon }`
  ([FollowCamera.h:17](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.h#L17), :26, :33).
- `FollowCameraState` gained `mode`, `rotationOffset`, `weaponSource` and `weaponPlayerIndex`. It reuses
  `attachmentName` as the attach point
  ([FollowCamera.h:77](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.h#L77)).
- `IFollowTargetProvider::GetWorldTransform(pos, ang)` added so attach mode gets orientation
  ([FollowCamera.h:114](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.h#L114)).
- New math helpers `FollowQuatToAngles` and `FollowRotateVector`
  ([FollowCameraMath.cpp:39](../../AfxHookSource2/Filmmaker/Movie/FollowCameraMath.cpp#L39), :54).

### Providers and solve

- `AttachmentTargetProvider` uses `GetAttachment(idx, origin, quat)` to get angles
  ([FollowCamera.cpp](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp)).
- New `HeldWeaponProvider` re-resolves `GetActiveWeaponHandle()` each frame, so it tracks the currently
  held weapon across switches ([FollowCamera.cpp](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp)).
- `MakeProvider` selects by `(type, mode, weaponSource)`
  ([FollowCamera.cpp](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp)). `Candidates` merges held
  and dropped weapons and C4 ([FollowCamera.cpp](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp)).
- `RunFrame` is split into a lock-on path (turret and aim trim) and an attach path (local-offset ride)
  ([FollowCamera.cpp](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp), attach :987, lock-on trim :1074).

### Commands ([FollowCommand.cpp Follow_RunCommand](../../AfxHookSource2/Filmmaker/FollowCommand.cpp))

- Added `follow mode`, `weaponsource`, `rotation` / `rotpitch|rotyaw|rotroll`, `offsetx|offsety|offsetz`,
  `fov`, `bone`, `eventselect`, `previewtick`, and the `campose` instrumentation dump. All prior commands
  and `state` / `state64` are retained.

### UI ([CameraEditorJs.h](../../AfxHookSource2/Filmmaker/Panorama/CameraEditorJs.h), follow section 259–487)

- Rebuilt into grouped blocks: Mode toggle, 5 Target-Type presets, Placement, Target dropdown, Weapon
  source, Attach point, Events and Preview Tick, Framing sliders (offset, rotation and FOV), and a
  collapsible Advanced Tracking block.
- `render()` reads `mode`, `rotation`, `fov`, `weaponSource` and `events`, and shows or hides blocks per
  type and mode ([CameraEditorJs.h:711](../../AfxHookSource2/Filmmaker/Panorama/CameraEditorJs.h#L711)).
- `BuildStateJson` extended with `mode`, `weaponSource`, `rotation`, `fov`, `eventStatus`,
  `selectedEvent`, and an `events[]` array
  ([FollowCameraStateJson.cpp](../../AfxHookSource2/Filmmaker/Movie/FollowCameraStateJson.cpp)).

### Demo event pre-scan (`.fmjson` events, v6)

- Go writer emits a top-level `events[]` (weapon and C4 drop and pickup ticks). `schemaVersion = 6`
  ([main.go:35](../../FilmmakerDemoInfoGo/main.go#L35), `addEvent` :182, handlers :231-307).
- C++ `DemoEvent` struct and `events` parse. `kSchemaVersion = 6`
  ([DemoEntry.h:38](../../AfxHookSource2/Filmmaker/Demo/DemoEntry.h#L38),
  [DemoInfoHelper.cpp:20](../../AfxHookSource2/Filmmaker/Demo/DemoInfoHelper.cpp#L20), parse :229).
- Lazy background load keyed on `GetDemoFilePath()`
  ([FollowEventIndex.cpp](../../AfxHookSource2/Filmmaker/Movie/FollowEventIndex.cpp)).

### Instrumentation and automation

- `follow campose` prints the computed pose and the resolved target world position
  ([FollowCamera.cpp](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp)).
- `FollowCameraMathTests` CTest target ([Source2Tests.cmake](../../CMake/Source2Tests.cmake),
  driver [Automation/tests/follow-camera-math-tests.cpp](../../Automation/tests/follow-camera-math-tests.cpp)).
- Netcon verifiers under `Automation/verify/` (`verify-followcam.ps1`, `verify-grenade-tracking.ps1`, …).

## Build-completion fixes (this pass)

This issue blocked a clean `build.bat`. It is fixed, and the build passes.

1. **MSVC C2026 "string too big"**: the embedded Panorama JS raw-string literal in `CameraEditorJs.h`
   exceeded MSVC's ~16 KB single-literal cap after the follow-section rebuild. The oversized chunk was
   split into two adjacent `R"EDJS(…)EDJS"` literals at a line boundary (CameraEditorJs.h ~807). All
   chunks are now under 16 KB.

**Verification:** `build.bat` reports `BUILD OK` (HLAE x64 DLL and `FilmmakerDemoInfo` Go helper built).
`FollowCameraMathTests` reports "all checks passed".
