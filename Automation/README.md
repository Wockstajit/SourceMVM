# Automation

Lighting verification uses the [relighting contract](../Docs/effects/whole-map-relighting.md)
and [saved regression fixtures](fixtures/relight-regressions/README.md). Capture fixed-camera
before/after frames and record event/atlas diagnostics for dry fire and overlapping infernos.
NetCon connectivity does not prove CS2 accepts attack input: confirm actual shots and foreground
focus before treating a capture as evidence. A run with no shots cannot validate muzzle lighting.

This folder contains the supported launch, control, capture, and verification tools for SOURCE:MVM. Generated screenshots and logs are disposable and do not belong beside the scripts.

The scripts here find Counter-Strike 2 themselves, through Steam's registered install path and
`steamapps\libraryfolders.vdf`, so none of them needs `-Cs2Dir`. Pass `-Cs2Dir` (or set
`SOURCEMVM_CS2_DIR`) only to override that. New scripts should call the shared resolver,
`lib\Resolve-Cs2Dir.ps1` or `lib\steam_paths.py`, instead of adding another hardcoded path.
See [Machine paths](../BUILDING.md#machine-paths).

Game launchers must run as the same Windows user as Steam. In Codex, launch CS2 with an approved execution outside the sandbox, because the sandbox account cannot use your Steam SDK session. The netcon launcher rejects that account before it creates a game process. It closes an existing game with console `quit` and stops if startup exits early.

The native injection host starts the game without inherited parent handles. If the launcher captures
output, it must still return while CS2 keeps running. Otherwise an inherited pipe can block the
demo-opening step until the game exits. After changing the host's process setup, rebuild it with
`SourceMovieMakerLauncher/scripts/build-host.ps1`.

The netcon launcher writes `readiness.json` to its run folder. It records elapsed readiness time,
process responsiveness, window handles, and the staged hook path. Timeout errors link to that file.
The default readiness deadline is still 75 seconds.

While it waits for the window and hook, [Cs2StartupStatus.ps1](lib/Cs2StartupStatus.ps1) reads
visible CS2-owned standard error dialogs. If one shows an explicit fatal error, the launcher reports
it immediately. A Steam SDK initialization error keeps its original message instead of becoming a
generic readiness timeout. The probe uses bounded, read-only text queries. Unreadable or unsupported
dialogs fall back to the normal readiness checks. Steam must have an active signed-in account. When
the probe finds a failure, it leaves the failed game and its dialog open and does not retry the launch.

## Root entry points

[launch.bat](../launch.bat) opens the GUI. [live.bat](../live.bat) runs
`launch/launch-cs2-netcon.ps1` directly, and [demo.bat](../demo.bat) delegates to `launch/demo.bat`.
The direct and demo wrappers forward arguments and exit codes. NetCon defaults to 29010.
`readiness.json` and demo `session.json` keep their existing contracts. These entry points involve
no DLSS files, driver checks, or DLSS installation. The GUI also defaults to NetCon 29010 unless
custom launch options name a different port.

### The newest hook wins in live.bat

Both `live.bat` files (the root one and `launch/live.bat`) pass `-NewestHook`. The launch then looks
at every checkout of this repository: the main checkout and each worktree on a branch (a detached
worktree is a release or bisect tree and is skipped). It finds the one whose hook build is newest,
meaning its staged `AfxHookSource2.dll` or a newer build that launch would stage. When that is
another checkout, the same launcher runs from there, with that checkout's own stage, shaders and
resources, and prints which branch it picked. So a hook built in a worktree is what the user's
usual `live.bat` starts. A checkout needs a staged hook, `injector.exe` and the native host to
count. Ties stay in the checkout `live.bat` ran from.

Only `live.bat` passes the switch. `launch-cs2-netcon.ps1` and `launch-demo.ps1` called directly,
as agents do, launch the checkout they run in, because a verification has to run the code it just
built. The pick is `Find-NewerCs2HookCheckout` and the hand-off `Invoke-NewestHookCheckout`
(`lib/AutomationCommon.ps1`), tested in `tests/cs2-hook-staging-tests.ps1`.

## Open a demo (recommended for Codex and Claude)

Use the non-interactive demo launcher:

```powershell
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File Automation\launch\launch-demo.ps1 -Demo "replays\match.dem"
```

The `-Demo` value may be:

- an absolute `.dem` path;
- a path relative to the repository or CS2's `game\csgo` directory; or
- a replay name such as `replays\match` (the `.dem` suffix is optional).

If `-Demo` is omitted, the newest `.dem` in `game\csgo\replays` is opened. `Automation\launch\demo.bat` is a short wrapper around the same command.

The launcher:

1. resolves and validates the demo before touching CS2;
2. starts the staged HLAE/SOURCE:MVM build with netcon enabled;
3. sends `playdemo`;
4. runs `mirv_filmmaker health`;
5. polls `mirv_filmmaker demoprobe` until the requested demo is actually playing; and
6. prints the run directory and writes `session.json`.

`session.json` is the launcher's state contract. Its `state` is `launching`, `connecting`, `loading-demo`, `ready`, or `failed`. It also records the resolved demo path, netcon endpoint, hook health output, last demo probe, and any error. A running `cs2.exe` does not mean success. Wait for `state: "ready"` or a zero launcher exit code.

By default the launcher restarts CS2, because the staged hook must be injected into a fresh process. Before it promotes a newer build, it waits for the old process and its injected DLL mapping to release. Agents can therefore rebuild and relaunch without copying the hook by hand. Use `-NoLaunch` only when attaching to an existing SOURCE:MVM session that already exposes the selected netcon port.

CS2 test launches default to a small 16:9 window at 854x480 (480p) on the left-hand monitor
(Windows `DISPLAY3`). The window moves there automatically after CS2 exposes its window handle.
Demo, practice, offline, Workshop Tools, RenderDoc and optional DLSS test launches use these
defaults. Pass `-Width`, `-Height` or `-Monitor` only when a specific check needs an override.
These development defaults do not change the shipped launcher.

## Control the open demo

Send commands without taking keyboard or mouse focus:

```powershell
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File Automation\netcon\cs2-netcon.ps1 -CommandBatch "demo_pause;;mirv_filmmaker health"
```

Use `-Commands "one command"` for a single command. Use `-CommandBatch` with `;;` between commands
when launching through `powershell.exe -File`, because that host does not reliably bind
comma-separated `-Commands` values as an array. In an existing PowerShell session, direct script
calls can still pass a real array: `& Automation\netcon\cs2-netcon.ps1 -Commands @('demo_pause', 'mirv_filmmaker health')`.

Per-frame runtime diagnosis still belongs in `mvm_debug`. Netcon is the command channel and does not keep a runtime log.

## Other launchers

- `launch/launch-cs2-netcon.ps1`: launch CS2 + hook + netcon, without loading a demo.
  Every launch carries the private local assets too. `FX\compiled`, `FX\death-animations` and a
  stage's `bin\ffmpeg` setup are git-ignored, so a git worktree has none of them; the launcher takes
  them from the main checkout (`Get-PrivateAssetRoots` in `lib/AutomationCommon.ps1`). It seeds the
  worktree's own FX stage from the main checkout's stage (cfg excluded), copies the ragdoll library
  next to the hook, and borrows a working FFmpeg setup when the stage has none.
- `launch/launch-cs2-test.ps1` / `launch/test.bat`: launch the offline `mvm_test` bot-practice harness.
- `launch/cs2-live.ps1`: optional browser dashboard for an already-running netcon session.
- `netcon/cs2-demo.ps1`: bounded, verified demo control for a running game. `-Demo` loads a demo
  (it re-sends `playdemo` until the map starts loading, then waits up to 150 s). `-Tick N` seeks and
  stays PAUSED there (`demo_gototick N 0 1`; a bare `demo_pause` is a toggle). `-Position x,y,z -Angles p,y,r`
  sets the free cam in the Movie Director's own pitch/yaw/roll order. `-Status` prints one line.
  `-PlayTo N` plays into a target, including effects a seek cannot reconstruct, and pauses within
  eight ticks. It picks full, quarter or crawl speed from the initial distance, then slows further
  as it approaches. A nearby target starts crawling before the first resume. Playback speed returns
  to 1 on completion or timeout.
  The script reads every step back from the game (`mirv_filmmaker demoprobe` now reports `tick=` and
  `paused=`) and exits non-zero on the first unconfirmed step. Prefer it over hand-rolled health loops.
- `launch/live.bat`: interactive human launcher. When given a demo path, it loads the demo whether or not the optional dashboard is selected. It launches the newest hook build of any checkout ([The newest hook wins in live.bat](#the-newest-hook-wins-in-livebat)).

Agents do not need the dashboard. It holds a long-lived netcon connection, so keep it opt-in. CS2 writes netcon output synchronously, and a slow reader can make the game hitch.

## Folder map

- `capture/`: screenshot and input helpers. Prefer `capture-game-window.ps1` for UI detail;
  `capture-game-burst.ps1` for a one-frame glitch (N consecutive PNGs, and it can fire the console
  command that triggers the glitch from inside the capture loop); `capture-game-motion.ps1` for a
  video. `send-wheel.ps1` / `send-click.ps1` deliver real input to the window.
- `config/`: configs copied into CS2 for automation runs.
- `docs/`: automation hygiene and live UI editing notes.
- `launch/`: supported CS2, demo, test, and dashboard entry points.
- `lib/`: shared helpers. `AutomationCommon.ps1` (run folders + metadata) and
  `Resolve-Cs2Dir.ps1` for PowerShell, `steam_paths.py` for Python.
- `netcon/`: low-level command helpers.
- `tests/`: C++ automation tests plus launcher integration tests.
- `tools/`: reusable generators and maintenance scripts. Run
  [sync-merged-branches.ps1](tools/sync-merged-branches.ps1) after every PR merge. It retires the
  merged local branches and worktrees and fast-forwards local `main`.
- `verify/`: live and static verification harnesses.
- `runs/`: ignored, timestamped run artifacts and session files.
- `output/`: ignored, temporary verifier output.

Verify a completed render take (including a minimum 10-second duration, stream/frame-map counts,
frame-name alignment, and resolution) with:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Automation\verify\verify-render-streams.ps1 -TakeDir "C:\path\to\mvm_render_take"
```

## Continuous Relight motion verification

Use [verify-relight-camera-slide.ps1](verify/verify-relight-camera-slide.ps1) with an already
running offline CS2 session. Supply a JSON route with `start` and `finish` arrays, each containing
`[x, y, z, pitch, yaw, roll]`, plus optional `fov`. The script interpolates the live free camera
over eight seconds in each direction using one persistent netcon connection. It records every
native frame and saves at least 24 lossless full-size PNGs per direction, pose samples, and a
capture manifest. It does not create a camera path. Use a viewport of at least 1500 by 1000.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Automation/verify/verify-relight-camera-slide.ps1 -RouteFile Automation/output/route.json -OutputDir Automation/output/relight-camera-slide
```

BMP disk recording affects frame rate, so take performance measurements in a separate run without
recording. For temporal stability, compare moving surfaces and background detail across the sequence.
Settled endpoint images are not enough. The script restores the finish pose and camera sensitivity.

## Archive completed audit log copies

[archive-mvm-debug-log.py](tools/archive-mvm-debug-log.py) losslessly compresses one explicit
`mvm_debug_YYYYMMDD_HHMMSS.log` copy under this repository's `Automation/runs`. It requires a
matching native start header and a terminal stopped footer with consistent event counters.
Active or incomplete logs, paths outside that root, reparse paths and existing archives are rejected.
The production AppData logger and its retention policy are unaffected.

```powershell
py -3 Automation/tools/archive-mvm-debug-log.py "<completed-log-path>" --remove-original
```

Omit `--remove-original` to retain the raw copy. Before optional removal, the helper verifies the
entire decompressed SHA-256 and byte count, confirms the source is unchanged, and protects the
source and published archive against concurrent writes or replacement. JSON reports original
and archive byte counts, SHA-256, saved logical bytes and confirmed removal. An existing archive
is never overwritten; do not rerun with removal after first creating an archive in keep mode.

The resulting `.log.gz` keeps every log byte. Plain-text analyzers need extraction first.
Restore the original `.log` filename so evidence references and line numbers remain usable.
This Python snippet creates a new file and refuses to overwrite an existing one:

```python
import gzip
import shutil

with gzip.open("mvm_debug_20261012_092013.log.gz", "rb") as source:
    with open("mvm_debug_20261012_092013.log", "xb") as restored:
        shutil.copyfileobj(source, restored)
```

## Hygiene

For output-device enumeration and actual audio recordings, use the optional
[AudioProbe](tools/AudioProbe/README.md). It can select the game's active audio endpoint by PID,
records bounded loopback WAVs with packet timing and level measurements, and never opens a
microphone. Endpoint recordings include other applications routed to that device; use baseline
and source-waveform comparisons to attribute sound to a tested feature.

For read-only Easter egg enumeration, select `mirv_filmmaker livejs context menu` or `hud`,
then run `mirv_filmmaker livejs load <absolute-path-to-automation/verify/inspect-easter-eggs.js>`
through NetCon. Each `[audit.eggs]` line contains JSON describing the context, overlay count,
state, ancestry, layout or hint boxes and label text/opacity/transforms. Inspect both contexts around level transitions; retained HUD pointers
can have zero layout at the menu. Enumeration is bounded and reports truncation. Combine it with
`mvm_debug` and window captures for runtime evidence; it does not activate effects or send input.

Keep scripts, docs, and configs in their named source folders. Never put them under `runs/` or `output/`. Delete stale runs, screenshots, logs, compiled verifier binaries, and `__pycache__` directories after they stop being useful. See [`docs/AUTOMATION_HYGIENE.md`](docs/AUTOMATION_HYGIENE.md).

Successful map preparation retains the published cache and small run logs, then removes
exported world nodes and decoded textures from that run. Failed runs retain their inputs for
diagnosis. Keep bounded before/after captures and compact difference reports when trimming
old verification output.

Check documentation links, portable references, schema-version agreement, and tracked-output
hygiene with:

```powershell
node --test Automation/tests/documentation-integrity-tests.mjs
```

The `[InputSystem] Processing SDL events took Xms` message comes from CS2 itself, not from the hook. `-console` only makes it visible. If it correlates with stutter, lower the mouse polling rate, avoid free-cam cursor warping while testing, and use the Release staged hook.

For live Panorama iteration without restarting CS2, `tools/extract_panorama_raw_js.py` expands the
adjacent C++ raw-string fragments (including their local `#include`s) into one `.js` file. Put the
generated file under `Automation/output/`, then run
`mirv_filmmaker livejs load <absolute-path>` over NetCon. This is development-only; committed UI
still comes from the compiled header.
