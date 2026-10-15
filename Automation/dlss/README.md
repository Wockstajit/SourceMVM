# DLSS 5 neural rendering for CS2: external setup

## In the launcher (the supported route)

The launcher's **ADVANCED > DLSS 5** tab installs and switches DLSS 5
([`dlss-tool.cjs`](../../SourceMovieMakerLauncher/electron/dlss-tool.cjs)). INSTALL asks for the
local `dlss 5` folder and checks it against [payload-versions.json](payload-versions.json). A copy is
bundled in `SourceMovieMakerLauncher/electron/dlss/`. If the folder has no ReShade with add-on
support, INSTALL asks for one: `ReShade64.dll`, or the official `ReShade_Setup_*_Addon.exe`, which is
a zip that the DLL is read out of. Nothing is downloaded.

Never place ReShade beside Steam's `cs2.exe`. A proxy `d3d11.dll` there loads in every CS2
session, online matches included, which is the ban risk. The launcher instead keeps a private copy of
`game/bin` in its data folder (`%APPDATA%\SourceMovieMaker\dlss5-cs2`). Every other `game/` folder in
that copy is a junction back into Steam, so the copy costs ~1.2 GB. The launcher installs ReShade and
the DLSS payload there, and starts that `cs2.exe` only while **Use DLSS 5** is on. Every DLSS launch
first re-syncs changed binaries from Steam, so the copy keeps up with CS2 updates.

The tab and the status line warn when ReShade or DLSS files are found beside Steam's `cs2.exe`. MOVE
OUT puts them in a timestamped backup and never deletes them. UNINSTALL removes the private copy. It
unlinks the junctions first, so Steam's assets are never touched.

The scripts below are the automation route to the same isolation (repo-local
`build/dlss5-cs2`); the launcher does not use them.

## SOURCE:MVM-only installation

Normal startup uses [launch.bat](../../launch.bat), [live.bat](../../live.bat), or
[demo.bat](../../demo.bat). None requires DLSS files, checks the NVIDIA driver, or installs DLSS.
GUI installation is future work. The feature and manual setup scripts are optional.

For an explicit DLSS experiment, close CS2 and double-click:

```text
install-dlss5.bat
```

The installer finds CS2 through Steam, asks whether the GPU is an RTX 20/30/40 model, validates
the selected local payload, and prepares the private executable tree automatically. It never
downloads packages. Re-running it compares SHA-256 hashes and skips DLSS files that are already
current.

If ReShade is installed beside Steam's `cs2.exe`, the installer recognizes the ReShade binary from
its Windows version metadata. It moves the proxy DLL to a recoverable
`build/dlss5-steam-backups/<timestamp>` folder and installs ReShade in the private tree. That stops
ReShade from loading when CS2 starts normally through Steam. Unrecognized graphics proxy DLLs are
never moved automatically.

After installation, launch the isolated configuration explicitly with:

```powershell
powershell -ExecutionPolicy Bypass -File Automation/dlss/launch-isolated.ps1 -EnableDlss
```

Only `-EnableDlss` selects the private tree and checks its driver/setup requirements.
Without that switch, the compatibility script delegates to the normal NetCon launcher.
Launching never downloads, prepares, or installs DLSS. The optional payload targets a compatible
NVIDIA GPU and driver 616.56 or newer; successful startup does not prove neural evaluation.
`-Cs2Dir` overrides Steam discovery; `-Monitor 0` selects the primary display.

The private tree in `build/dlss5-cs2` copies `game/bin`; game assets remain junctions into Steam.
Steam identity and settings remain shared. Graphics loaders are installed only in the private tree.
Close CS2 before first setup or package updates. To refresh private binaries after a Steam update,
run [prepare-isolated.ps1](prepare-isolated.ps1). [install-isolated.ps1](install-isolated.ps1)
preserves custom overlay settings and rejects conflicting payload versions.

The package manifest and update procedure live in [portable setup](../setup/README.md).

The installer reads the DLSS and Streamline payload only from the gitignored `dlss 5/` directory.
It has no network bootstrap. The local payload is:

- [RenoDX downloads channel](https://discord.com/channels/1408098019194310818/1545049227321810974):
  ShortFuse's build `2026-10-04T00:41:17` (add-on version 0.2026.1003.2333),
  `renodx-dlss.addon64` (3,013,120 bytes), pinned in [payload-versions.json](payload-versions.json).
- `RTX 40, 30, 20 compatible/nvngx_dlssnr.dll` is the supplied patched runtime. Its
  Authenticode status is `HashMismatch`, which is expected for a modified NVIDIA DLL; the
  exact SHA-256 is pinned in [payload-versions.json](payload-versions.json). Hardware
  compatibility is the supplier's claim and needs testing on each GPU generation.
- `NVIDIA original/nvngx_dlssnr.dll` preserves the valid NVIDIA-signed runtime for RTX 50-series
  use or rollback. `streamline/nvngx_dlssnr.dll` is the active compatibility copy used by the
  shared installer.
- [Official ReShade 6.8.0 with add-on support](https://reshade.me/downloads/ReShade_Setup_6.8.0_Addon.exe),
  extracting `ReShade64.dll` as the private `d3d11.dll`.

For games without native DLSS the release instructions require the Present hook. In this
build the Hook Method row is `Off, Render, Upscaled, Present`, stored as
`DirectNeuralRenderingHookStage`, and Present is 3. [ReShade.isolated.ini](ReShade.isolated.ini) starts at 0 (Off). The launcher resets both
ReShade configurations to Off before each DLSS launch.
Players can select Present in ReShade to enable neural rendering for that session.
The launcher removes the `DirectNeuralRenderingHookPoint` and `DirectNeuralRenderingRequireDlss` keys
from `RENODX-DLSS`. "Use DLSS Upscaling Resources" stays on its default,
Optional, which falls back to dummy inputs. Only the neural runtime is required; no generic
`renodx-dlss5.addon64`, ASI loader, or automatic DLSS installation is used.

The private folder records payload versions and SHA-256 hashes in `dlss5-files.json`.
Installation verifies the selected runtime against its recorded hash. The original variant
also requires a valid NVIDIA signature. Re-running installation preserves replaced private-tree
files under `build/dlss5-cs2/backups` before updating them.
After a Steam update, rerun preparation to refresh the private game binaries.

Verified through the launcher's DLSS path (private `cs2.exe`,
RTX 5070, driver 616.56, NVIDIA original runtime). The add-on's status line in the ReShade overlay
reads **`Present: Active (dummy inputs)`** with Hook Method on Present, and the frame rate drops
while the pass runs. This build logs only failures (`NR skipped evaluation`) and has no success
counter, so check the overlay status line instead. With the hook at `Off`, the same line reads
`Off: Disabled`. Explicit display suffixes are available with `powershell -File Automation/dlss/launch-isolated.ps1 -EnableDlss -Monitor 12 -Width 1920 -Height 1080`.
The hook preserves a separate texture-creation original for each device vtable,
fixing invalid texture creation when ReShade proxies and native devices coexist.
The installer also includes the SOURCE:MVM ReShade overlay-state addon for input gating.

Unresolved: repeated SDL resolution changes can hang during ReShade runtime creation.
A hang dump shows the thread polling a D3D11 timestamp query inside ReShade. It does not establish
which component stalled the query. Standard launches pass the resize sequence.
A visible A/B comparison and export through the render pipeline have not been verified.
Addon loading and window or netcon readiness do not prove that neural rendering works.

The compatibility entry point
`install-dlss.ps1` redirects to the private installer and cannot place files beside Steam's
normal `cs2.exe`.

This is entirely outside the mod. `AfxHookSource2` contains no DLSS code. Neural rendering runs
through ReShade plus a RenoDX add-on, which process the presented frame without the game's help.
Nothing here requires a mod rebuild, and removing all of this leaves SOURCE:MVM untouched.

Commands using `pwsh Automation/dlss/install-dlss.ps1` are forwarded
to [install-isolated.ps1](install-isolated.ps1). Shared-tree uninstall and the unsupported generic
add-on mode fail with a clear error rather than modifying Steam's CS2 directory.

## How it works

CS2 has no native DLSS. The engine ships a complete DLSS integration in `rendersystemdx11.dll`
(`CDxDlssUpscaler`, the full `NVSDK_NGX_D3D11_*` surface), but CS2's game code never selects it.
A vtable-swap watch across a full level load measured **zero** calls to `CreateUpscaler` or
`IsUpscalerSupported`. The client uses its own unrelated spatial `CFSRUpscaleRenderer` instead.

That one fact determines the whole configuration:

- Add-on `renodx-dlss.addon64` ("RenoDX DLSS"). Its Hook Point selector includes *On Present*,
  which processes the presented backbuffer on its own private D3D12 device. The add-on's help text
  says: *"On Present supports D3D9, D3D11, and D3D12 presentation. D3D9 and D3D11 use a same-adapter,
  device-only D3D12 endpoint."* That matches CS2's case.
- `DirectNeuralRenderingHookStage=3` (Present). The 2026-10-04 build's Hook Method row is
  `Off, Render, Upscaled, Present`. 0 is Off, the add-on's default, which disables it.
  Present with dummy temporal inputs is the only stage that runs in a game that never calls
  native DLSS. The launcher forces this value in both `ReShade.ini` and `ReShade2.ini` on every
  install and every DLSS launch, and keeps the rest of what the overlay saved.
- `renodx-dlss5.addon64` ("RenoDX DLSS5 Generic") cannot work alone here. It depends entirely
  on an existing game DLSS call.
Changing the hook point **requires the add-on in `ADDON.LoadFromDllMain` and a game restart**. The
installer writes that line with an absolute path.

## Layout: flat, next to cs2.exe

Everything goes directly in `game/bin/win64`, no subfolder. The add-on resolves
`DLSSPath=nvngx_dlss.dll` and `StreamlinePath=sl.interposer.dll` as **bare relative names**, which
only resolve from the exe directory, and ReShade only scans `bin\win64` for add-ons. A `dlss5\`
subfolder produced `NVNGX DLSS not found`, and the add-on never loaded at all. The upstream
installer (`yumlevi/renodx-dlss-installer`) does the same thing: "DLLs copied next to the game
executable".

Payload source is `dlss 5/` at the repo root (gitignored): `streamline/` holds the active runtime,
`addons/` holds add-on builds, and the named runtime folders preserve the compatible and original
variants. The selected runtime is installed as `nvngx_dlssnr.dll`, because the loader requires
that filename.

## Two swapchains and two ini files

CS2 creates two swapchains on the same window at startup: the first succeeds (flip model), the
second is refused with `0x80070005` because the HWND already has one, and CS2 falls back to a legacy
BLT swapchain that succeeds. ReShade attaches a runtime to each and names the second config
`ReShade2.ini`.

A runtime whose ini lacks `[DEPTH]` renders the depth-reading Lumenite effects with no depth buffer.
A runtime whose ini lacks the add-on's section has nowhere to save settings. Together these make the
add-on sliders dead, which reads as *"ReShade stopped working"*. The runtime that owns the overlay is
not predictable, so the installer writes the same complete config to both ini files.

Both swapchains are created at the same size, so this is not a resolution
mismatch. Aligning `cs2_video.txt` to the launch size does not prevent it. Do not retry it: it also hides the Apply button in the in-game video settings.

ReShade also has an "Effect Runtime Sync" add-on with a *Synchronize effect runtimes* option, which
is worth trying as a cleaner fix for the preset half of this problem.

## Verifying

Read `game/bin/win64/ReShade.log` after a launch. The line that matters is:

```
[RenoDX] DLSS-NR direct: EvaluateFeature succeeded: evaluation=1205 options_revision=95 result=0x1
```

`evaluation=` climbing per frame means neural rendering is running. `options_revision=` climbing
means setting changes are reaching it. Together they are the quickest check that the sliders are live.

On RTX 20/30/40, explicitly select the compatibility variant and confirm SHA-256
`E67DEE209320CDAFE0E93E45675D7AA34323A53ACC57A72B2E40A181581C989A`. Its modified signature
cannot be used as an integrity signal. The original signed variant remains
`E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E`.

Two warnings are benign and always appear. They are emitted at DllMain, before anything has loaded
those modules, so judge runtime success from the later evaluation counter:

```
WARN | NVNGX DLSS not found: nvngx_dlss.dll
WARN | Streamline interposer not found: sl.interposer.dll
```

Judge success by the
`DLSS-NR direct: EvaluateFeature succeeded` counter, never by the absence of warnings.

`ERROR vtable::Hook(Failed to find NVSDK_NGX_D3D12_EvaluateFeature_C)` is benign. The add-on
hooks `EvaluateFeature` successfully, and only the `_C` variant is absent. It appeared in every
session, including ones where NR demonstrably ran.

A log line is not a picture. The add-on reporting successful evaluations shows that the network ran,
not that the result reached the screen. Only a desktop-level A/B on a **paused** demo shows that,
and it needs an on-vs-on control to establish a noise floor. Internal dumps can look correct while the on-screen difference sits at 0.002 against a 0.014 noise floor, which means nothing.

## Requirements

The supplied patched runtime claims RTX 20/30/40 compatibility. The original signed runtime is
preserved for RTX 50-series. Both still require a compatible NVIDIA driver and ReShade 6.8+ with
add-on support installed as `d3d11.dll`; exact minimum driver/GPU behavior remains to be tested.

## Anti-cheat

Offline demo/movie work only, with `-insecure`. Never take ReShade add-ons or NGX hooks onto a
VAC-protected server.
