# Follow / Attach camera

The **Follow / Attach camera** is a tracking and ride-along tool in the CS2 filmmaker (the HLAE fork
`AfxHookSource2`). You pick a target in a demo, jump to the right tick, preview the shot live, fine-tune
the offset, rotation and FOV, and play the demo while the camera stays locked to the target or rides
rigidly with it.

Each target uses one of two camera models:

- **Lock-on**: a camera is *placed* at a fixed point and only rotates to keep the target framed
  (a turret). `rotation` acts as an aim trim on top of the look-at solve.
- **Attach**: the camera position **and** orientation are derived from the target every frame. It
  uses a local-space `offset`, a `rotation` trim and an `fov`, with a rigid mount and render-time
  sampling. Framing and tracking settings are remembered per mode.

Both modes follow the moving ragdoll after death. Choose Keep following or Timed hold. The hold counts
playback seconds and pauses with the demo. See the [UI guide](follow-camera-ui.md).

A Target-Type dropdown and a free **Lock-on / Attach** toggle together reach all six `(type, mode)`
combinations: **Player**, **Grenade**, **Weapon / C4** (lock-on), and **Grenade**, **Player Bone**,
**Weapon Attach** (attach). So any target can ride along.

The whole feature is **100% console-command driven**. The Panorama UI issues only the same
`mirv_filmmaker follow ...` commands that a person or an automation bot can send over the net console.
That makes it verifiable without a physical mouse (see the QA runbook).

## Where to start

| You want to… | Read |
|---|---|
| Understand the components and data flow | [follow-camera-architecture.md](follow-camera-architecture.md) |
| Drive the feature from the console (the automation surface) | [follow-camera-commands.md](follow-camera-commands.md) |
| Find a control in the editor UI, or the state it reads | [follow-camera-ui.md](follow-camera-ui.md) |
| Understand the `.fmjson` v10 weapon/C4 event pre-scan | [demo-event-prescan.md](demo-event-prescan.md) |
| Run the automated verification (no mouse) | [qa-automation.md](qa-automation.md) |

## Build

From the repo root:

```bat
build.bat
```

`build.bat` builds the win32 and x64 HLAE staging release to `build\staging-release\bin\HLAE.exe`. It also
builds the `FilmmakerDemoInfo` helper (Go, using demoinfocs-golang, which pre-scans `.dem` files into
scoreboard and event JSON) next to `AfxHookSource2.dll`. CMake
also builds a standalone math test target, `FollowCameraMathTests` (driver:
[Automation/tests/follow-camera-math-tests.cpp](../../Automation/tests/follow-camera-math-tests.cpp)),
and registers it with CTest.

> **Schema sync:** the demo pre-scan format is at **v10**. The Go writer (`FilmmakerDemoInfoGo/main.go`)
> and the C++ reader (`DemoInfoHelper`) must agree on the version. If you change one, change the other
> and bump both. See [demo-event-prescan.md](demo-event-prescan.md).
