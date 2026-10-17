# CLAUDE.md

This file gives repository guidance to AI assistants working on SOURCE:MVM.

## What this repo is

SOURCE:MVM is a Windows-only, offline demo and movie-making toolkit forked from HLAE and advancedfx.
CS2 runs `AfxHookSource2.dll` injected into `cs2.exe`. Deadlock runs `AfxHookDeadlock.dll` injected
into `deadlock.exe`. Both draw native Panorama UI inside the running game. The Electron launcher
(`SourceMovieMakerLauncher/`) starts CS2. Deadlock has its own automation launcher.

- Active work targets CS2 and Deadlock. CS2 features live in `AfxHookSource2/Filmmaker/`. Deadlock
  integration lives in `AfxHookSource2/Deadlock/`, and its features live in
  `AfxHookSource2/Filmmaker/Deadlock/`. Pick the build, commands, and runtime for the game you are
  changing.
- `AfxHookGoldSrc/`, `AfxHookSource/` (Source 1), `HLAE/`, and `Installer/` are inherited upstream
  code, kept for compatibility. Leave them alone unless the task explicitly requires a change.
- This is for offline demo and movie work only. Never wire the DLL up to VAC-protected or online
  servers.

## Working scope

Read this file once and reuse it for the whole task unless it changes. Follow a linked document only
when it applies to the work. `Docs/README.md` points to the maintained feature contracts. Use a skill
when the request or workflow calls for it, and load supporting references as you need them. You do
not have to read every doc or skill.

Code shows current behavior. The user's request and the maintained contracts show intended behavior.
When they disagree, follow the intent, then update the code and the docs. Carry authorized work
through implementation and verification. Ask only for information or approval that the governing
rules actually require, and reuse approval the user already gave. Prepare a reviewable result before
you ask for a final sign-off.

## Build

```batch
build.bat
```

The full CS2 path runs through `build-source2.bat`. It asks whether to rebuild particle assets
(default: No), closes running `cs2.exe` and `hlae.exe` so the staged files unlock, configures
`CMake/Source2Build.cmake` into `build/source2-release`, builds the Go demo helpers, stages FX, and
launches CS2. Pass `-NoLaunch` to skip the launch.

For fast CS2 hook-only iteration:

```batch
cmake --preset x64-source2-release
```

```batch
cmake --build --preset x64-source2-release --parallel --target AfxHookSource2
```

```batch
cmake --install build/x64-source2-release --config Release --prefix build/source2-release --component x64-runtime
```

```batch
cmake --install build/x64-source2-release --config Release --prefix build/source2-release --component x64-source2
```

Both CS2 install steps are required, because the launcher stages from `build/source2-release`.
Presets: `x64-source2-{release,debug}` (`AFX_SOURCE2_ONLY=ON`, the default stack),
`x64-{release,debug}` (legacy Source 1 and Source 2), and `win32-{release,debug}` (legacy HLAE
GUI and GoldSrc).

For Deadlock hook-only iteration:

```batch
cmake --preset x64-deadlock-release
cmake --build build/x64-deadlock-release --config Release --target AfxHookDeadlock --parallel
```

The Deadlock launcher injects the DLL directly from
`build/x64-deadlock-release/AfxHookSource2/Deadlock/Release/`. A Deadlock-only hook change does not
need a CS2 install step. Injector and host setup is in [Docs/deadlock/README.md](Docs/deadlock/README.md).

To build only the launcher, without touching the CS2 build, run `SourceMovieMakerLauncher\build-app.bat`
(add `-Package` for the installer). The legacy full HLAE package is built with `build-hlae.bat`.

See [BUILDING.md](BUILDING.md) for prerequisites and the `SOURCEMVM_*` environment overrides.

## Test

Run the checks that cover the behavior you changed. The native, JS, and Python suites run
independently, and no single command runs all of them. Rerun checks after relevant changes or
failures. Widen the scope when shared behavior changes.

```batch
ctest --test-dir build/x64-source2-release -C Release --output-on-failure
```

Native tests are registered in `CMake/Source2Tests.cmake`, which `AfxHookSource2/CMakeLists.txt`
includes. The sources live in `Automation/tests/`: pure math and format checks, plus isolated native
and D3D11 fixtures. To run one suite, use `ctest ... -R FollowCameraMathTests`, or run the built
executable directly.

```batch
node --test Automation/tests/documentation-integrity-tests.mjs
```

JS tests run per file: `node --test Automation/tests/<name>-tests.mjs`. Most are static regression
checks that read the C++ and JS sources, so they run without CS2. Python FX tests run as
`py -3 Automation/tests/<name>-tests.py`.

`documentation-integrity-tests.mjs` checks every repo-relative Markdown link and rejects private
absolute Windows paths in maintained docs. Run it once after the last batch of Markdown edits.

## Testable logic lives in `*Math.h`

Camera, interpolation, geometry, and other nontrivial engine-free calculations belong in a
header-only `*Math.h`. Add meaningful behavior tests in `Automation/tests/*-math-tests.cpp` and an
`add_test` entry. Extend the math unit that already owns the area, and keep engine plumbing in the
`.cpp`. Trivial arithmetic needs no separate module and no tests that restate the formula. Hook
integration still needs live verification.

## Threading model

For CS2, `AfxHookSource2/Filmmaker/Filmmaker.h` documents three per-frame entry points:

| Entry point | Thread | Called from | Allowed to do |
| --- | --- | --- | --- |
| `Filmmaker::RunFrame()` | **render** | `Before_Present` in `RenderSystemDX11Hooks.cpp` | thread-safe backend work only |
| `Filmmaker::RunMainThreadFrame()` | **main/UI** | `new_CS2_Client_FrameStageNotify` in `main.cpp` | Panorama `RunScript`, panel IO |
| `Filmmaker::RunMainThreadLateFrame()` | **main/UI** | same hook, *after* the engine handler | mesh-group re-assert only |

Deadlock has its own pumps in `AfxHookSource2/Deadlock/DeadlockFrame.cpp`. Panorama work belongs in
`Frame_MainThread()`. Do not assume the CS2 entry points also drive Deadlock.

- Never call Panorama `RunScript` from the render thread. V8 runs on the main thread.
- The late-frame hook exists because the client reapplies each weapon's recorded mesh-group mask
  during its own `FRAME_RENDER_PASS`. Writes made earlier in the frame get overwritten.
- `mirv_filmmaker health` reports pump counts. If the render count is 0, check hook installation and
  whether the render pump runs before you blame the feature logic.

## Panorama UI is JavaScript embedded in C++ headers

Each in-game panel is a `*Hud.cpp` driver plus one or more `*Js.h` headers. Each `*Js.h` body is a raw
string literal (`R"CFJS( ... )CFJS"`) of Panorama JS, sent through `PanoramaBridge::RunScript`. Large
panels split into several `Js.h` fragments, for example `ConfigHud.cpp` with `ConfigHudJs.h`,
`ConfigHudCameraTabJs.h`, and `ConfigHudEffectsTabJs.h`, so each file stays readable.

- The JS stays ES5-style and registers itself on `$.<Name>`, so C++ can call `$.CamEditor.render()`
  and similar functions.
- These headers hold user-visible text. For mojibake, check the source encoding and confirm the
  running DLL includes the latest headers. A stale `.obj` is one possible cause.
- The console and netcon `ui_eval` path truncates at 256 bytes. Drive the UI through short registered
  helpers instead of long inline JS.

For CS2 and Deadlock UI changes, follow the [mockup-first workflow](Docs/panorama/mockup-first-workflow.md).
Edit the shared Panorama scripts, review them in the browser mockup, then build the same changes into
the game and compare real screenshots. Do not put a requested in-game fix only into the mockup's HTML
or CSS. The preview launchers pick up direct header edits automatically. Intentional generated-JS
drafts must be applied back to the headers, and stale generated JS must never overwrite newer headers.
Browser checks cover UI logic and layout. Fonts, game-world scaling, and renderer behavior need live
game captures. See [CS2](UiMockup/README.md) and [Deadlock](UiMockupDeadlock/README.md).

## Console command surface

Console commands also drive automation. Use the surface for the game you are targeting:

- `mirv_filmmaker <subcommand>` is the main surface in both games. CS2 uses `FilmmakerCommand.cpp`,
  and Deadlock uses `DeadlockCommands.cpp`. `mirv_filmmaker health` is the quick triage command.
  Subcommands differ by game, so check the target game's help.
- `mirv_deadlock <subcommand>` is a compatibility alias for Deadlock's `mirv_filmmaker` dispatcher.
- `mvm_debug start|stop|status` controls the runtime debug log (see below).
- `mvm_test` is CS2's offline bot-practice harness for live-match style testing.
- `mirv_*` commands: CS2 uses `MirvCommands.cpp` and `RenderCommands.cpp`. Deadlock registers its
  ported subset through `DeadlockMirvCommands.cpp` and `DeadlockCameraCommands.cpp`. See
  [Deadlock console commands](Docs/deadlock/console-commands.md) for supported names and alternatives.

## Diagnose runtime behaviour with `mvm_debug`

A netcon reply only shows what happened at the moment you ran the command. Throttled per-frame
probes, event ordering, and "what happened 200 frames ago" exist only in the log file.

1. Start with the existing health output and `mvm_debug` probes. Reproduce the problem and read the
   log. Where evidence is missing, add `MvmDebugLog_Linef` instrumentation. The API is in
   `AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h`, and CS2 and Deadlock share it across the whole
   filmmaker surface.
2. Log the inputs, the intermediate state, and the final written value. For engine-memory writes, log
   the resolved offset and target pointer. A stale schema offset looks exactly like broken math.
3. Gate logging on `MvmDebugLog_Active()` and throttle it. The logger flushes after every line.
4. Run `mvm_debug start`, reproduce the problem, and read `%APPDATA%\HLAE\debuglogs\mvm_debug_*.log`
   yourself.

`mirv_filmmaker ...`, `mirv_deadlock ...`, and `mvm_debug` run over the game's netcon and return
replies. Recheck after a game update that affects them. If a command goes silent, use the console.

This rule covers every Deadlock bug fix. Start `mvm_debug` before you reproduce the problem, collect
the Deadlock runtime log, and base the diagnosis on that evidence. A source-only theory, a clean
build, a screenshot, or one netcon reply does not prove a Deadlock runtime fix.

## Launch and drive CS2 from automation

Launch as the signed-in Windows user, outside the Codex sandbox. The sandbox accounts
(`codexsandboxoffline` and `codexsandboxonline`) cannot reach the user's Steam SDK session. Use an
approved `require_escalated` launch. Builds, tests, and netcon can stay sandboxed. Never force-kill
CS2. Close it with the console `quit` command, and stop if the launch fails.

`Automation/` owns every supported launch and control path. The scripts find the CS2 install
themselves from Steam's `libraryfolders.vdf`. Call `Automation/lib/Resolve-Cs2Dir.ps1` or
`Automation/lib/steam_paths.py` instead of hardcoding a path.

```powershell
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File Automation\launch\launch-demo.ps1 -Demo "replays\match.dem"
```

The launcher blocks until the hook answers, then writes `session.json` (`state` reaches `ready` or
`failed`). `Automation\launch\{demo,test,tools,live}.bat` wrap the PowerShell launchers.
`Automation\launch\wait-for-cs2-ready.ps1` checks readiness for a session you did not start.

Routine CS2 tests use the launchers' small 854x480 window (16:9, 480p) and move it to the user's left
monitor, Windows DISPLAY3. Keep these defaults so the game uses less of the PC. This is development
automation, not a shipped launcher setting.

To send commands without stealing focus:

```powershell
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File Automation\netcon\cs2-netcon.ps1 -CommandBatch "demo_pause;;mirv_filmmaker health"
```

Use `-CommandBatch` with `;;` separators when calling from `powershell.exe -File`. Comma-separated
`-Commands` does not bind as an array through that host. Netcon defaults to port 29010. Keep the
`live.bat` dashboard opt-in. CS2 writes netcon output synchronously, so a slow reader makes the game
hitch.

Run long builds and launches as background tasks, using the tool's completion or session mechanism.
Collect the exit code and output, diagnose any failure, then continue with the requested runtime
verification. A build or launch notification alone does not finish a bug fix. Use bounded waits,
avoid busy polling, and keep the user informed.

See [Automation/README.md](Automation/README.md) for the full launcher and netcon contract.

## Launch and drive Deadlock

```powershell
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File Automation\launch\launch-deadlock.ps1
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File Automation\netcon\deadlock-netcon.ps1 -Port 29020 -CommandBatch "mvm_debug start;;mirv_deadlock health"
```

The launcher finds the game through `Automation/lib/Resolve-DeadlockDir.ps1`. Use Deadlock's port
29020, which is separate from CS2's 29010. Launch offline with `-insecure` and D3D11. The port does
not support Vulkan. Close Deadlock with the console `quit` command. Never use `taskkill /F`, because
it can break the Steam pipe and leave the next launch with a black screen. See
[Docs/deadlock/README.md](Docs/deadlock/README.md) as needed.

## Version-coupled engine values

`addresses.cpp`, `addresses.h`, `SchemaSystem.cpp`, and the pattern scans across `AfxHookSource2/` are
tied to specific game builds. One update can break several features at once. Start with the affected
game's health command before you assume a logic bug. Deadlock's resolved values are documented in
[Docs/deadlock/signatures.md](Docs/deadlock/signatures.md).

## FX pipeline

`FX/` converts donor-game particle assets into CS2 `.vpcf`, `.vmat`, and `.vtex` files. Each pack has
one script under `FX/tools/`. [FX/README.md](FX/README.md) describes the current pipeline, and
`Docs/porting/` has the notes for each pack. The donor trees (`FX/sources/`), the compiled pack
(`FX/compiled/`), and the ragdoll library `FX/death-animations/ragdoll_animations.mvmanim` belong to
other games. They are git-ignored, never committed, kept as a private local copy, and still shipped in
releases. A rebuild needs Workshop Tools, because retail CS2 no longer ships `resourcecompiler.exe`,
and it republishes into the local `FX/compiled/`.

- Never guess particle names, material paths, texture paths, or asset references. Check them against
  the real files, and diff the original against the converted output.
- Precaching is a hard rule. Anything that touches particles, swapped models, ragdolls, or materials
  must precache the affected assets at demo or level load, or when the feature is enabled. Use a safe
  empty or dev fallback when an asset is missing. A toggle that hitches in game is a precache bug, not
  a working feature.

## Map data

Map lighting, shadows, and surface features read a per-map cache (authored lights and environment,
collision, shadow casters). The hook extracts that cache natively on the first map load, with no VRF
or .NET. [Docs/effects/map-data-extraction.md](Docs/effects/map-data-extraction.md) owns the contract.
The code lives in `AfxHookSource2/Filmmaker/MapData/`. It is engine-free and also builds as
`MapDataTool.exe`.

- If you change the extractor's output, bump `kExtractorVersion` (`MapData/MapExtract.h`) so caches
  rebuild. Run a parity check with `Automation/verify/compare-map-data.py` against an earlier
  extraction (Inferno at minimum). Do not loosen its tolerances.

## Repository rules

- Extend the module that owns the behavior. Give unrelated capabilities separate entry points, and
  avoid dependencies on unrelated workflows. Reuse genuinely shared helpers with explicit inputs. Do
  not duplicate code just to keep files separate.
- `.gitattributes` sets `* -text`. Files keep whatever line endings they were committed with, and the
  tree mixes CRLF and LF. A blanket `sed -i` over a glob silently turns CRLF files into whole-file
  diffs. Edit files one at a time, and check `git diff --stat` before you commit.
- Large binary assets a clone needs go through Git LFS. Add a `.gitattributes` rule before you commit
  a new large binary type. Generated output stays in the ignored `build/` or `Automation/runs/`
  folders.
- Every doc lives in exactly one `Docs/` subfolder. Only the index sits loose in `Docs/`. Chat
  transcripts, machine paths, and generated output do not belong in the docs tree.
- [AGENTS.md](AGENTS.md) points to this file. Keep it a pointer so the two cannot drift apart.
- Replace, do not migrate. The project is unfinished. When a path, cache, format, command, or UI is
  superseded, delete the old one and keep only the new one. Do not add readers, fallbacks, or "still
  accept the old location" shims. Wipe leftover on-disk data for the old location when that is safe.
  Compatibility applies only to shipped engine or demo formats that users cannot regenerate, such as
  CS2 demo repair. It does not apply to our own unfinished caches and settings.

## GitHub workflow and AI-assisted contributions

This section is mandatory. Before you change branches, create commits, issues, or PRs, or prepare a
release, read the canonical policy in [Docs/setup/git-workflow.md](Docs/setup/git-workflow.md). Where
[the finish workflow](Docs/setup/finishing-work.md) suggests direct pushes to `main`, that policy takes
precedence.

- `main` is the default and only primary integration branch. Do not rename it, and do not create a
  persistent `active` or `develop` branch.
- Do substantive work on a named, short-lived branch: `feat/`, `fix/`, `perf/`, `refactor/`, `docs/`,
  `chore/`, `security/`, or `upstream/`. Use `release/<major.minor>` only for an intentionally supported
  release line. Do not name branches after release tags.
- Pass `--repo Wockstajit/SourceMVM` on every `gh` command, and always on commands that write
  (issues, PRs, comments, labels, releases). The clone has an `upstream` remote for
  advancedfx/advancedfx, and without a default `gh` can pick it. Never create issues, PRs, comments,
  or labels in advancedfx/advancedfx.
- Open a pull request into `main` for routine work, or into the relevant release branch for a supported
  hotfix. Use draft PRs while work is incomplete. Do not merge or publish without the required authorization.
- Respect the CI gate (`ci-gate`), code review, scoped changes, tests, privacy and licensing rules, issue
  links, and safe staging. Never push directly to a protected branch, and never force-push one.
- AI-assisted contributions are welcome, not prohibited. Disclose material AI assistance. A responsible
  contributor must understand, review, and test every change. Never invent builds, tests, review approvals,
  in-game verification, benchmarks, or deployment results.
- Report build, automated-test, real-game, and deployed status separately. A passing compile does not show
  that a CS2 or Deadlock feature works in game.
- Never commit secrets, Discord webhooks, personal machine paths, proprietary third-party code,
  game-derived assets without redistribution rights, raw private reports, logs, or generated binaries.
  `.gitignore` does not clean history.
- Merged code is not released automatically. Version tags are immutable. Preserve the existing `v0.1.x`
  releases and the runtime-hotfix serial updater protocol, and test them explicitly before you change them.
- After a merge, delete the short-lived branch when that is safe. Do not delete permanent branches or
  historical version tags. Do not rebase or rewrite published history as routine housekeeping.
- GitHub deletes only the remote branch. Right after a merge, run
  `Automation\tools\sync-merged-branches.ps1` from the main checkout. It removes the merged local
  branch and its worktree, moves their git-ignored notes and captures to
  `Automation\runs\worktree-leftovers\`, and fast-forwards local `main`. Never run
  `git worktree remove --force` yourself: on Windows it follows junctions (`reference\`, FX
  `game\` links into the CS2 install) and empties their targets.
- Branch from `main`, not from another open PR. A squash merge leaves a stacked branch carrying the
  base PR's old commits. If you must stack, update the next branch after each merge as
  [Stacked pull requests](Docs/setup/git-workflow.md#stacked-pull-requests) describes.

## How we finish work

Follow [the finish workflow](Docs/setup/finishing-work.md) for tracking issues, coloured GitHub labels,
commit messages, and PRs. Route routine work through a branch and a PR into `main`, as the GitHub
workflow section above describes. A direct push to `main` needs an explicit request from the repository owner, and the
tracking issue should record that exception. Keep one issue and one commit per finished request,
unless the user asks for separately pushed fixes. Stage only this task's changes. Source belongs in
`Wockstajit/SourceMVM`, and `main` is the default branch. Verify remote commits, and report issue state
accurately: pushes to a feature branch do not close issues. Labels describe the work and its evidence.
Release tags are reserved for requested releases.

## Publishing to the public repository

`Wockstajit/SourceMVM` is the only repository and it is public: source, website and release downloads.
Publish only when the owner asks, and only by [publishing releases](Docs/setup/publishing-releases.md):
a reviewed `main` commit, noreply author and committer, no co-author or tool trailers, and only the new
`v<version>` tag. The history of the earlier private repository is not part of this one. Never fetch or
push it here, never push `--all`, `--mirror` or `--tags`, and never force-push. The launcher version, the
tag and the installer name are the same number, and the new release must carry `latest.yml` or installed
launchers stop at an error.

## Completion and reporting

For runtime fixes, build the affected DLL, reproduce the original steps with that build in the target
game, and inspect the `mvm_debug` evidence. For UI or rendering fixes, also capture and inspect the actual
game window across the affected tabs and across open, switch, and exit transitions. When freezing or jitter
is involved, check live playback across several frames. One screenshot cannot show motion.

Deadlock's `Automation/verify/verify-deadlock-screenshot.ps1` pairs captures with commands and health output.
When another window covers the game, use a window-targeted capture path and inspect the result. Black, stale,
or obscured captures do not verify the UI. If the capture falls back to desktop pixels, it cannot show what an
occluded game looks like. Keep fixing failures while progress is possible.

After a change, report what changed, what verification you actually performed, and any material gaps or risks.
If live verification is blocked, state the concrete blocker and leave the runtime outcome marked unverified.
Documentation-only changes need documentation checks, but they do not require launching either game.
