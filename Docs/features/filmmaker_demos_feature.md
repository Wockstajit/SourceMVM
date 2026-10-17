# Filmmaker "Demos" feature

The demo browser in AfxHookSource2 (CS2). It scans for demos, reads each demo's map, duration and
date, and reads the full scoreboard for downloaded matchmaking demos. It can play any demo in the
list. A native Panorama UI adds a film-icon button to the main-menu top navbar, which opens a
"Watch → Downloaded"-style page.

All code lives under `AfxHookSource2/Filmmaker/`, one component per file:

```
Filmmaker/
  Filmmaker.{h,cpp}          facade: init, per-frame pump, watch (playdemo)
  FilmmakerCommand.cpp       the mirv_filmmaker console command
  Demo/
    DemoEntry.h              data structs (demo + scoreboard player)
    DemoHeaderReader.{h,cpp} parse .dem header -> map + duration  (validated)
    DemoInfoReader.{h,cpp}   parse .dem.info -> scoreboard         (validated)
    DemoScanner.{h,cpp}      recursive *.dem discovery
    DemoLibrary.{h,cpp}      owns the list, runs the background scan, builds JSON
    DemoOrganizer.{h,cpp}    sort mode, user folders, last-played history
    DemoProjectPath.{h,cpp}  repaired-cache copies map back to the original .dem
    DemoProjectStore.{h,cpp} `%APPDATA%\HLAE\source_mvm\demos\` layout + project.mvm sections
    DemoProjectStoreMath.h   folder naming, index, generated-cache names
    DemoProjectFileMath.h    the project.mvm container format (sections + CRC-32)
  Config/
    DemoFolderStore.{h,cpp}  saved folder list (%APPDATA%\HLAE\filmmaker_demo_folders.txt)
    DemoFileStore.{h,cpp}    individually added demos (…\filmmaker_demo_files.txt)
    DemoOrganizerStore.{h,cpp} organiser state (…\filmmaker_demo_organizer.txt)
  Platform/
    ProtobufWire.{h,cpp}     minimal protobuf wire reader (no protobuf dependency)
    TextEncoding.{h,cpp}     UTF-8 <-> UTF-16
    JsonBuilder.{h,cpp}      tiny JSON writer (demo payload for the UI)
    FolderPicker.{h,cpp}     native IFileOpenDialog folder picker (worker thread)
    DemoFilePicker.{h,cpp}   native multi-select *.dem file picker (worker thread)
  Panorama/
    PanoramaBridge.{h,cpp}   engine access + RunScript (guarded)
    FilmmakerGuiJs.h         the embedded Panorama JS (the actual UI)
    DemoOrganizerJs.h        organiser rail JS fragment (toolbar, folders, drag)
    FilmmakerMenu.{h,cpp}    builds the UI, pushes data, polls the command queue
```

## What works (console-driven, validated)

Demo discovery, parsing and playback are complete and verified against real CS2 match files.
Run these in the HLAE console:

```
mirv_filmmaker scan          (re)scan the CS2 install + your folders
mirv_filmmaker list          list demos with index, map, length, scoreboard, file
mirv_filmmaker folders       show scanned folders
mirv_filmmaker addfolder     open a native folder picker; the folder is saved
mirv_filmmaker removefolder <i>
mirv_filmmaker watch <i>     play the demo at index i (playdemo)
mirv_filmmaker project [status]
                             show the source_mvm root and the playing demo's
                             project folder
```

- Demos are found by recursively scanning the auto-detected install root
  (`GetProcessFolderW()`) plus any saved folders, on a background thread. Leftover `repaired\`
  folders and source_mvm `repaired.dem` copies are excluded from discovery, so a repaired copy
  cannot appear as another demo.
- Map and duration come from the `.dem` header. The date comes from the file timestamp.
- The full per-player K/A/D and score scoreboard, plus team scores, come from Valve's matchmaking
  sidecar. The library reads it in place from `<name>.dem.info` beside the demo. Names show as
  `[unknown]`, as on CS2's own Downloaded page, because that file has no names.
- Saved folders persist across restarts. Missing or moved folders are skipped.
- The trash button and **Clear all** remove entries from the browser only. Demo files stay on
  disk, and adding their folder again restores them.
- Individual demos can be added without their neighbours (see the organiser below). Those paths
  persist in their own store and join the scan results.

## Demo organiser (list rail)

The left rail has a toolbar above the demo tiles. The tiles become collapsible rows that you can
drag and drop.

| Control | Where | Effect |
|---|---|---|
| Sort | rail toolbar, left | Recently played / Newest / Oldest / Map name / Demo name |
| `+` | rail toolbar, right | New folder / Add demo file… / Add demo folder… |
| Folder header | above its demos | Click collapses, right-click renames or deletes |
| Drag a tile | onto any header | Files it into that folder (`No folder` takes it back out) |
| Right-click a tile | anywhere | Same move actions, plus the last-played time |
| **FOLDER** | selected demo action row | Opens `%APPDATA%\HLAE\source_mvm\demos\cs2\<demo>\` (creates `project.mvm` if needed) |

The organiser is a separate module from `DemoLibrary`. The library owns what is on disk (scan
results, scoreboards, repair). The organiser owns how the user arranged that list. The library
behaves the same with the organiser inert, and the organiser never opens a `.dem`.

**Recently played** has two sources, so it stays correct however a demo was started:
`Filmmaker::PlayResolvedDemo` (the Watch button) and a once-a-second poll of `PlayingDemoPath()`
(CS2's own Your Matches page, or a console `playdemo`). A demo played through the automatic-repair
cache is folded back onto the original that the browser lists. Played demos carry a thin gold edge
on their tile, so the order never looks arbitrary.

The same folding sets the demo's logical **project path**. When the engine plays
`source_mvm/.../repaired.dem`, Source MVM validates that copy's `.src` source stamp. It then keys the
camera path, per-key shake and playback loop to the original `.dem`. SOURCE:MVM project files and
generated caches live under `%APPDATA%\HLAE\source_mvm\demos\cs2\<filename>/`:

| Project file | What it is |
| --- | --- |
| `project.mvm` | single container: the `project` info section (canonical demo path + game) plus one authored section per subsystem (`campath`, `camshake`, `loop`, `scenekeys`) |
| `scoreboard.fmjson` | parsed scoreboard / event cache |
| `demo.info` | recovery copy of Valve's `<name>.dem.info`, used only to restore a missing sidecar |
| `repaired.dem` + `.src` / `.clean` | repair cache and validity stamps |

Every section is keyed by the original demo, never by a copy made from it. Each subsystem reads
`PlayingDemoPath()`, which maps a validated `repaired.dem`, `keyframed.dem` or `keyframed-b.dem`
back through its folder's `project.mvm` (`PlanDemoCopyLookup` in `DemoProjectStoreMath.h`).
`Automation/tests/demo-project-keying-tests.mjs` fails any CS2 section that keys off the raw
playing file (`ResolvePlayingDemoPath()`).

`project.mvm` is a small binary container (format in `DemoProjectFileMath.h`). It starts with a
32-byte header: magic `SRCMVM\x1A\0`, version 1, a game flag in `flags` bits 0-3, and the section
count. A table of 32-byte entries follows. Each entry holds a 16-byte ASCII tag, an encoding id
(1 = UTF-8 JSON, the only value), a CRC-32, and the payload's offset and size. The JSON payloads are
packed after the table. Writes are atomic: a same-folder temp file, a flush, then a `MoveFileExW`
replace. A file that fails to decode is renamed to `project.mvm.corrupt-<utc>` rather than
overwritten. Each section payload is the JSON text its subsystem writes, so a subsystem
change never reshapes the container.

CS2 writes `<name>.dem.info` beside a newly downloaded demo. The library reads it there and never
removes or overwrites it. When a `demo.info` recovery copy exists in the project folder and the
native sidecar is missing, a scan restores a complete copy beside the demo, without replacing newer
CS2 metadata. The launcher also restores those files before a first-run settings reset. If recovery
fails, the saves are preserved.

Deadlock uses the same root under `demos/deadlock/` and the same `campath` / `camshake` / `loop`
section tags. The `project.mvm` header game flag tells them apart.

This also works when a validated repaired copy is launched from the console rather than the
Downloaded tab. A demo merely stored in a directory named `repaired` is not redirected without a
matching marker. `index.json` maps the canonical demo path to a human-readable folder. A second
`match.dem` from a different directory becomes `match.dem__2`. Renaming updates that index and the
folder name when the new name is free.

### The join key (change both sides together)

C++ persists demos under `NormalizeDemoKey(fullPath)`: lower-cased, `/` replaced with `\`, and no
trailing separator. The Panorama payload has no full path, so `orgKeyOf()` in
`DemoOrganizerJs.h` rebuilds the same string from `folder + '\' + fileName`. A mismatch raises no
error. Demos simply stop belonging to their folders. `Automation/tests/demo-organizer-tests.mjs`
covers this.

### Layout and rebuild rules

Tiles stay direct children of `FmMatchList`. Native `matchtile.vcss` styles them through that
parent relationship, so group headers sit in the same flow instead of tiles nesting in per-group
containers.

Collapse state is deliberately excluded from the render-plan signature. Collapsing only toggles
tile visibility. If it changed the signature, `reconcileList()` would rebuild the list and reset
scroll and selection on every expand. Sort changes, folder membership and renames do change the
signature, so they rebuild the list.

Tests: `node --test Automation/tests/demo-organizer-tests.mjs`

### Keyframed copies for scrubbing

Every demo plays from its keyframed copy once one exists. The copy is `keyframed.dem` in the project
folder, with a full snapshot every 32 ticks, so a backward seek restores in one frame. The first
Watch plays the normal file and builds the copy in the background. Selecting a demo starts the build
too. See [live demo scrubbing](filmmaker_demo_scrubbing.md#keyframed-copies).

### Automatic demo repair

Selecting an unchecked legacy demo runs `FilmmakerDemoRepair.exe -out` once in the background, before
Watch is pressed. That run already does a full parse, reports real failures with a non-zero exit
code, and writes a repaired copy only when one is needed.

The native Watch row shows an animated indeterminate progress bar while that work runs, plus a short
explanation and any failure. The helper does not stream byte or packet counts, so the UI shows no
percentage. Repair state invalidates the Panorama payload separately from scan changes, so the label
updates while the library is idle.

Compatibility is also classified before Watch is clicked. It uses the header protocol and build that
the fast scan already reads. Each selected demo reports one of these states:

- **Supported**: current protocol. Watch is available immediately.
- **Legacy demo**: accepted protocol, but the one-time compatibility scan is still required.
- **Validated repair**: a repaired copy with a matching source/helper marker is ready.
- **Unsupported demo**: the current client rejects the protocol. Watch is disabled, because message
  repair cannot rewrite a protocol version.

Legacy and unsupported rows carry Valve's native warning-triangle asset. Validated repairs carry a
green `R`. Repair failures and unsupported details belong to the demo that produced them, so
selecting another row replaces the message at once. A separate three-pixel gold rail marks only the
single most recently played visible demo. Full play history stays available through the Recently
played sort. The rail is not a repair indicator.

### Validation

The header and info parsers were validated by compiling them standalone and running them against
real files in `…\csgo\replays\`. Example (`match730_…140528`): map `de_ancient`, 41:02, team scores
13-11, 10 players, player 0 = `31 K / 5 A / 15 D`, matching the in-game scoreboard.

## Renaming a demo, and showing it in Explorer

Both actions work on the selected demo. Both leave the file where it is unless you ask otherwise.

### Rename (the title is the file name)

The big name above the scoreboard is the demo's file name, so editing it in place renames the file.
Click it and type, or right-click any demo in the list and choose **Rename demo** (which selects that
demo first). Enter commits, and Esc reverts.

`renamedemo <index> <new name>` takes the name last and unquoted, so it may contain spaces. This is
the same shape `org rename` uses. `Filmmaker::RenameDemoFile` sanitises the name. It drops the
characters Windows forbids and path separators, trims trailing spaces and dots, and forces a `.dem`
extension. It refuses an existing target, and it refuses a demo the engine has open, so playback
cannot have a demo renamed out from under it.

**A demo is several files, and the rename carries all of them:**

| Also moved | Why |
| --- | --- |
| `<name>.dem.info` | CS2's native matchmaking metadata; kept beside the demo |
| the organiser record | folder membership + last-played, keyed by path (`DemoOrganizer_NoteRenamed`) |
| the added-file / hidden entries | also keyed by path (`DemoLibrary::NoteRenamed`) |
| the source_mvm project folder | `project.mvm` (campath / camshake / loop / scenekeys) / scoreboard / repair / `demo.info` |

Every sibling whose name starts with the demo's file name moves with it and keeps its suffix. A sidecar
added later therefore travels too. Verified live by round-tripping a demo: 3 companion files moved
each way, and the library came back identical.

### Show in Explorer

This is the icon next to WATCH. **Do not shell out to `explorer.exe` for this.** Three approaches fail:

- `ShellExecuteW` returns success (>32) and opens **nothing**. Its dispatch depends on the calling
  thread's COM state, which inside an injected DLL is whatever the game leaves it as.
- `CreateProcessW` opens a window, but always a **new** one, and always behind the game.
- Raising it by window title is guesswork. Explorer titles a folder window `<folder> - File
  Explorer`, or the full path when the user has that setting on.

`ShowInExplorerAsync` runs on a detached worker. The worker starts its own COM apartment and calls
`SHOpenFolderAndSelectItems`, the call Explorer's own "show in folder" uses. That call reuses a window
already on the folder and selects the file instead of stacking duplicates. An Explorer window open on
a *different* folder is re-pointed first through `IShellWindows` / `Navigate2`, so it follows you to
the next demo instead of going stale. Activation needs both `AllowSetForegroundWindow(ASFW_ANY)`
(granted while CS2 owns the foreground, which it does when you click) and `AttachThreadInput`.
`SetForegroundWindow` alone is refused across processes.

## Panorama UI status

`FilmmakerGuiJs.h` authors the UI: the film navbar button, the demos page, scoreboard cards, and the
Watch, Add-folder and Clear-all controls, styled with Valve CSS classes. The C++ bridge
(`PanoramaBridge`) reuses the engine pointer, makeSymbol and get/setAttributeString already resolved
in `DeathMsg.cpp`. It adds the following:

- **`CUIEngine::RunScript`**: resolved directly from its prologue in `panorama.dll` via the pattern
  `48 89 5C 24 ? 4C 89 4C 24 ? 48 89 54 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 8D`
  (from current Osiris `PanoramaUiEnginePatternsWindows.h`; verified as a unique match in this build
  with `Misc/sigscan.py`). The match address is the function itself. It is called as
  `RunScript(uiEngine, contextPanel, source, "filmmaker", line)` with a non-zero `line` to skip script
  caching.
- **Context panel**: the in-game HUD root panel (`AfxHookSource2_GetPanoramaHudPanel`) is the build
  context. The UI auto-builds once a map or demo loads, because the HUD panel is null in the bare
  main menu. Once the page exists, it is parented under the main-menu content, and the bridge context
  is re-pinned to that page. Later updates do not depend on the transient HUD panel.

The JS walks to the top-level window and `FindChildTraverse`s for the main-menu navbar
(`MainMenuNavBarSettings`/…) to attach the film button. It works even when the script runs from the
HUD context.

**Threading (critical):** `RunScript` executes Panorama's V8 JavaScript, which must run on the game's
main/UI thread. Calling it from the DirectX Present (render) thread crashes with `v8::Context::Exit() -
Cannot exit non-entered context`. So all Panorama work runs from `Filmmaker::RunMainThreadFrame()`,
called out of `CS2_Client_FrameStageNotify` (`FRAME_RENDER_PASS`) in `main.cpp`. Only the thread-safe
backend pump (scan start, folder apply) runs on the render thread, through `Filmmaker::RunFrame()` in
the Present hook.

Console controls:

```
mirv_filmmaker ui_status     show engine / RunScript / context / built
mirv_filmmaker ui_rebuild    rebuild the UI next frame
mirv_filmmaker ui_context <hex>   (advanced) pin a specific context CUIPanel*
```

If the navbar anchor id changes in a CS2 update, the film button will not attach. Re-validate the
`RunScript` pattern with `Misc/sigscan.py`, then adjust the id list in `FilmmakerGuiJs.h`. This
remains offline demo use only.

## Build / test

1. `build.bat` (or `cmake --build --preset x64-release --target AfxHookSource2`).
2. Stage: `cmake --install build/x64-release --config Release --prefix build/staging-release`.
3. Launch CS2 through HLAE: open `build/staging-release/bin/HLAE.exe`, then choose Launch CS2.
4. In the CS2 console: `mirv_filmmaker scan`, then `mirv_filmmaker list`, then `mirv_filmmaker watch 0`.

### Backend scan verification

`mirv_filmmaker addfolder "<absolute folder path>"` adds a folder and requests a scan without opening
a picker. Without a path it keeps the normal picker behavior. Use `folders` to get its index, and
`removefolder <index>` to remove a temporary test folder when you finish.

Metadata preservation was verified through the real in-game scan. The test used copied demos and
synthetic Valve-format sidecars. The original and recovered sidecars kept their hashes through
repeated scans, both supplied scoreboards, and the copied demo loaded and played in CS2. These
fixtures do not establish that missing metadata caused the reported stuck loading screen.
