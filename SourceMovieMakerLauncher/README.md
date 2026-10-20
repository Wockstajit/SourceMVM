# SourceMovieMaker launcher

The SOURCE:MVM launcher is a standalone Electron/React app. It is kept separate from the
inherited `HLAE/` WinForms application and from the Source 2 CMake build.

## Build without building CS2

From this directory:

```batch
build-app.bat
```

This builds the React renderer and the small .NET Framework injection host. It does not invoke
CMake, compile `AfxHookSource2`, compile shaders, or launch CS2.

To produce an installer and unpacked Windows app:

```batch
build-app.bat -Package
```

Packaging always overwrites the same output folder: `release\`.
The unpacked app is at `release\win-unpacked\SourceMovieMaker.exe`.

For live UI work with Vite HMR and Electron:

```batch
live-preview.bat
```

Leave that window open while you edit `src\App.tsx` or `src\styles.css`. The renderer reloads
without rebuilding the packaged release. Closing the live Electron window stops the preview server.

For a faster local package without the NSIS installer:

```batch
build-app.bat -Dir
```

### The complete installer

```batch
build-app.bat -Complete
```

Builds `release\SourceMovieMaker-Complete-Setup-<version>.exe`. The installer carries the launcher
and everything it would otherwise download: the hook runtime (from the staged
`build\source2-release`), the compiled FX pack (`FX\compiled\source_mvm_fx`) and the DLSS 5
payload (the repo's `dlss 5` folder: Streamline, both `nvngx_dlssnr` runtimes, the RenoDX add-on,
and ReShade from `dlss 5\reshade\` with its license). `scripts/build-bundle.ps1` stages them into
`bundle\` (git-ignored), and `electron-builder.bundled.cjs` packs that as `resources\bundle`.
Build the hook first. The script only reads built files.

The complete CS2 release also includes app-local Visual C++ runtime DLLs, the Electron runtime,
the injection host, demo helpers, and verified FFmpeg/FFprobe binaries with their upstream license
and source/build notice. It needs Windows 10/11 x64, Steam and Counter-Strike 2. Python, Node.js,
Visual Studio and a separately installed FFmpeg are not required on the recipient's PC. The host
targets .NET Framework 4.6.2, supplied by supported modern Windows installations.

Unfinished Deadlock files and DLLs, debug symbols, analysis databases and renamed backup hooks
are excluded from the complete payload.

A launcher with a bundle:

- copies the bundled hook and FX pack into its payload folder on first start and after every
  update (`installBundled`), and never downloads a payload, so an older online release cannot
  replace what it shipped with;
- installs DLSS 5 from the bundle, choosing the runtime from the GPU (`detectVariant`: RTX
  50-series and newer get NVIDIA's original runtime, RTX 20/30/40 the compatible one, anything
  else is refused with the reason);
- asks once on first launch, "Do you want DLSS 5 (ReShade) installed?"; yes installs it and turns
  on ADVANCED > DLSS 5, no is remembered (`dlssPromptAnswered`).

Do not create alternate `release-*` folders. The build script clears `release\` and removes any stray `release-*` leftovers before packaging.

## Runtime layout

The packaged app carries the Electron UI and the native injection host. Complete installers
copy the hook, injector, shader/snippet resources, FX pack and FFmpeg from their bundle into the
app's own userData. Thin installers download their runtime separately (see [Updates](#updates)):

```text
%APPDATA%\SourceMovieMaker\payload\runtime\x64\AfxHookSource2.dll
%APPDATA%\SourceMovieMaker\payload\runtime\x64\injector.exe
%APPDATA%\SourceMovieMaker\payload\fx\source_mvm_fx\
%APPDATA%\SourceMovieMaker\payload\runtime\ffmpeg\bin\ffmpeg.exe
<install>\resources\host\SourceMovieMakerHost.exe
```

The launcher uses the same runtime an end user gets. `runtimeCandidate()` in `electron/main.cjs`
uses an explicitly configured `runtimeRoot` (RUNTIME FOLDER) when one is set. Otherwise it uses the
downloaded payload, and it never searches a repository `build\` tree. The FX mount (`USRLOCALCSGO`)
follows the same rule: an explicit override, otherwise the payload's `source_mvm_fx`.

To run a local build from the launcher UI, set RUNTIME FOLDER (or HOOK DLL OVERRIDE) to it.
Automation (`Automation\launch\*`) passes explicit `--hook`/`--injector` paths and its own
`USRLOCALCSGO`, so it is unaffected.

## Launch failures

The launcher shows CS2 STARTED as soon as the host resumes the injected game; nothing waits on a
timer. The app passes `--watch-startup-ms 30000`, so the host keeps the game's process handle open
in the background. If CS2 exits within that window (including exit code zero), the status switches
to the error with the PID and exit code, and GO is available again. Automation and headless
launches do not pass the flag, so the host exits right after resuming the game. Surviving the
window does not prove the game reached the menu; later exits or hangs still need game/crash logs.

`CreateProcessW error 5` means Windows denied creating the game process, before hook injection.
The message includes Windows' error description and the executable path. Check the executable's
file permissions, Windows Security Protection history, and administrator compatibility settings.
Run Steam and SOURCE:MVM normally under the same signed-in Windows account. The error alone
cannot identify the permission or security rule responsible.

Every host attempt writes a timestamped `launch.log` in the launcher's data folder, including the
host arguments, resolved game/hook/injector paths, stdout, stderr and exit status. Failures lead
with stderr and show the log path. Advanced > Locations can reveal the launcher data folder.
Portable builds keep it under `SourceMovieMaker-data` beside the app. These logs stay local;
remove personal paths and custom launch-option values before sharing them.

Regression checks (no Steam or game launch required):

```powershell
node --test Automation/tests/launcher-launch-host-tests.mjs
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File Automation/tests/launcher-host-tests.ps1
```

## Saved data

The hook saves everything under `%APPDATA%\HLAE` (`GetHlaeRoamingAppDataFolderW`). Advanced
Settings > LOCATIONS lists each location with its size on disk and an open button:

| Location | Path under `%APPDATA%\HLAE` |
| --- | --- |
| Demo projects (camera paths, shots, repaired/keyframed demo copies) | `source_mvm\demos` |
| Effects settings | `filmmaker_fx.json` |
| Filmmaker settings | `filmmaker_settings.json` |
| Environment presets | `source_mvm\environment` |
| Map data cache (rebuilt automatically) | `source_mvm\environment\occlusion` |
| Relight lights | `source_mvm\relight` |
| Props | `source_mvm\props` |
| Skyboxes | `filmmaker\skyboxes` |
| Cosmetics profiles | `cosmetic_profiles.json` |
| Shader cache (rebuilt automatically) | `source_mvm\shader-cache` |
| Debug logs | `debuglogs` |

The list is `DATA_LOCATIONS` in `electron/main.cjs`; add a row there when the hook gains a new
save location. Launcher settings (`launcher.json`) and the downloaded payload live in the
launcher's own userData (`%APPDATA%\SourceMovieMaker`). Settings keys the launcher does not
define are dropped on load.

## Panorama assets

The renderer ships a small curated set of CS2 Panorama SVG and PNG assets for its controls and
surface texture, the native animated `linemap.webm` button texture, and the competitive
matchmaking "Let's roll!" sound (`public/sounds/mm_success_lets_roll.wav`). They are extracted
from the local CS2 installation into the ignored `reference/cs2 refs/assets` folder, then copied
into `public/` so the packaged renderer can load them without depending on a CS2 installation.

All of these files are local only. The Panorama icons and textures (`public/panorama/`, including the
`videos/` folder: `linemap.webm`, `gobutton.webm`, `speed_lines.webm`), the Stratum2 fonts
(`public/fonts/`, Valve's typeface) and the "Let's roll!" sound (`public/sounds/`) are Valve
material, so they are git-ignored and not part of the source tree. They stay in the maintainer's working copy and are bundled into released
installers from there. A fresh clone builds and runs without them: `styles.css` falls back to
Bahnschrift / Segoe UI, a missing sound is swallowed by `playLetsRoll`, and the Panorama icons and
the GO button's linemap texture stay blank. To get the real files locally, extract them from the CS2
install (`stratum2.uifont`, the matchmaking sound, `panorama/images/`, `panorama/videos/`) into
those folders.

### Native main-menu reference map

The GO comparison uses the actual CS2 resources:

- `panorama/styles/mainmenu_play.vcss_c` gives the action button a native-size cropped
  `videos/linemap.webm` background at `20% 60%`, `background-img-opacity: .5`, a 1px border,
  and `margin: 12px 0px`.
- `panorama/layout/mainmenu_play.vxml_c` adds
  `particles/ui/ui_mainmenu_playaction_active.vpcf`; the gold equivalent is
  `ui_mainmenu_playaction_active_gold.vpcf`. These are stored as decompiled references under
  `reference/cs2 refs/assets/particles/ui/`. The web renderer uses the cropped video itself,
  because Electron cannot render a Source 2 `ParticleScenePanel`.
- `panorama/layout/mainmenu.vxml_c` places the center labels `Inventory`, `Loadout`, `Play`,
  `Store`, and `News` in `#MainMenuNavBarCenterContainer`. The left icon controls use
  `home.vsvg`, `watch_tv.vsvg`, `overwatch.vsvg`, `settings.vsvg`, and `power.vsvg`.
  The extracted reference icon set is available in `public/panorama/icons/native-nav/`.
- The top-bar highlight is `particles/ui/ui_mainmenu_nav_play_mg.vpcf`, a 670px additive
  `ParticleScenePanel`. `particle_controls.js` moves its control points to the selected
  center label; it is not a video background.
- The main menu scene behind those tabs is a `MapVanityPreviewPanel` using
  `<setting>_vanity` (default `de_dust2_vanity`), not `linemap.webm`. Opening a tab dims that
  scene, hides the floating panels, and slides/fades the content panel in over 0.25 seconds.
- Native tab transition cues are `UIPanorama.tab_mainmenu_loadout`,
  `UIPanorama.tab_mainmenu_shop`, `tab_mainmenu_inventory`, `tab_mainmenu_news`,
  and `tab_mainmenu_play`; their extracted source sounds are kept under
  `reference/cs2 refs/assets/sounds/ui/panorama/`. Hover uses
  `UIPanorama.mainmenu_rollover`, and the GO action uses `mainmenu_press_GO`.

The launcher keeps a dark 10px gutter around the GO control. The gutter separates the gold border
from the action-panel wall and matches the native button's margin, so the button does not stretch
edge to edge.

## Launch mode: Normal / Dev

The session panel has a two-option mode switch, saved with the other settings (`devMode`).

- **Normal**: retail CS2, unchanged.
- **Dev**: appends `-tools -dx11`, the same mode as Steam's "Counter-Strike 2 Workshop Tools"
  entry. The tools and asset pipeline are available in the session.

The UI recommends neither mode. No badge, tip or tooltip pushes Dev. The caption under GO only
says what GO will launch. The 300px launch strip has no spare height for extra blocks, so anything
added below the session card pushes the utility rail (CHECK FOR UPDATES) out of the window. That is
why `.launch-field { min-height: 0 }` exists.

## Session card wordmark

The session card has the in-game workspace wordmark (`AfxHookSource2/Filmmaker/Panorama/WorkspaceShellJs.h`):
a short gold rule and a spaced `SOURCE // FILMMAKER SESSION` code line that ends with GAME READY.
Below it, SOURCE:MVM is dim gold with soft focus and glow, three motion-blur echoes, and an
out-of-focus ghost. The title runs the same streak-in, rest, whip-out loop as the rest of the card
(CSS keyframes, off under `prefers-reduced-motion`). Readable state never lives in the title. The
Advanced drawer shows the same contour map edge to edge.

`-dx11` is required. engine2 picks between `rendersystemdx11` / `rendersystemvulkan` /
`rendersystemempty`. Steam's own tools entry passes `-vulkan`, which would bypass **every** D3D11
hook this project owns, so Dev mode pins DX11. It mirrors `Automation/launch/launch-cs2-tools.ps1`,
the agent-facing entry point for the same mode. Change both when either changes.

### Workshop Tools detection

The tools are a free Steam DLC (appid 2279721) that installs into the same directory as the base
game, so there is no separate folder to look for. The only reliable local check is whether the
tool binaries are on disk:

```
game/bin/win64/tools/hammer.dll
game/bin/win64/csgocfg.exe
```

Both probes are re-run whenever the configured `cs2.exe` changes. When Dev is selected and they
are missing, the launcher shows an inline warning with a one-click **INSTALL** (which opens
`steam://install/2279721`) and disables GO.

The same check runs again inside `launchWithSettings`, so the headless/CLI path (`--dev` /
`--no-dev`) is covered too. That path never sees the warning. Without this check, dev mode fails
much later and with a far less clear error.

## Updates

The public website and download channel is [Wockstajit/SourceMVM][release-repo]. Application
source stays in the private development repository. The public repo holds the static website, and
installers and updater metadata belong in its GitHub Releases.
The first public complete release is version 0.1.7. Checksums accompany the installer.
The owner submits the build to VirusTotal. A report appears only when its hash matches the
published installer. A pending scan is never shown as completed.

Automatic updates are opt-in. The switch is **ADVANCED > UPDATES > Check for updates
automatically**, saved in `launcher.json` as `autoUpdate`. A fresh install, and an install whose
settings predate the switch, start with it off. Off, the launcher contacts nothing on its own:
startup only readies what is installed (the bundle a complete installer carries, the FFmpeg link)
and reports **UPDATES OFF**. The footer **UPDATES** button still checks once when pressed, which
is also how a thin installer downloads its runtime. Turning the switch on starts the usual check
right away (`settings:save` in `electron/main.cjs`).

With the switch on, every packaged startup checks the public channel in the main process. An
available launcher update downloads automatically through `electron-updater`, then installs and
restarts. **GO stays disabled until the restarted app is current and its runtime is ready.** The
`cs2:launch` IPC handler enforces the same gate. A failed download or install keeps GO blocked
and shows **RETRY UPDATES**. Installation waits for CS2 to close. It never force-closes the game.

GO works without internet. When the release list cannot be fetched (no connection, the update
server down), the launcher logs that once, skips the hotfix and payload checks too, reports
**OFFLINE / UPDATE CHECK SKIPPED** and launches the installed runtime. Steam still has to be open
and signed in; the launcher does not work around that. A release list that does arrive but is
malformed (a published release with no `latest.yml`) is still an error, so a broken publish is
noticed by every launcher that checks.

Complete installers carry the same GitHub update configuration as the ordinary installer.
They replace the whole app, then `installBundled` installs that app's matching hook and FX pack.
They never replace that bundle with an older separately published payload. Ordinary installers
check and install `payload-manifest.json` components and restore their FFmpeg link.
Unpackaged development preserves local build overrides. A portable exe cannot replace itself;
when a newer app is available it requires installing the setup exe before launch.

An empty channel is valid while preparing the first release. A complete app with its runtime
installed can become ready without a published update. A missing runtime or malformed/incomplete
published metadata remains an error. Checks/downloads share one operation, and renderer startup
reads a state snapshot so it cannot miss an update that finishes before the UI subscribes.
Pre-releases are accepted, but downgrades are disabled. Updates preserve launcher/user data.

### Publish only when the installer is ready

1. Increase `package.json` and `package-lock.json` to a new semantic version for each update.
2. Build the verified CS2 hook and stage its runtime, then run `build-app.bat -Complete`.
   Build/packaging commands use `--publish never`; building alone uploads nothing.
3. Inspect `release/latest.yml`. Its version, setup filename, size and SHA-512 must match the
   freshly built complete installer. Preserve the generated filename and blockmap. Do not rename
   only the exe, copy metadata from another build, or reuse a version for changed binaries.
4. Create a **draft** release in `Wockstajit/SourceMVM` tagged `v<version>`. Upload the complete
   setup exe, its `.blockmap`, and `latest.yml` to that draft. Include checksums and release notes.
5. Verify every uploaded asset and publish the complete draft. The website discovers the exe;
   installed launchers discover `latest.yml`. Public downloads need no GitHub sign-in or token.

The existing private launcher CI builds artifacts only; it cannot silently publish the smaller
launcher as a replacement for the complete setup. Publish a tested complete installer explicitly.
For optional separate runtime updates, `npm run build:payload` reads the staged CS2 build and
writes `payload-dist/`; it never compiles or launches CS2. Publish its validated manifest and
component assets together. Only `runtime` and `fx` are installable; destinations are fixed.

### Shareable offline bundle

To give someone one zip that needs no downloads, build the app and payload as above, then run
`Tools\make-friend-bundle.bat` from the repo root. The zip holds a portable exe with the runtime and
FX pre-seeded into `SourceMovieMaker-data\`, plus a plain-cmd starter. See that script's header for
the layout contract.

## Windows targets

`build-app.bat -Package` produces both:

- **`SourceMovieMaker-Setup-<v>.exe`**: NSIS installer. Self-updates via `electron-updater` once
  automatic updates are switched on.
- **`SourceMovieMaker-Portable-<v>.exe`**: single-file portable build. It keeps settings and the
  downloaded runtime in `SourceMovieMaker-data\` beside the exe (driven by
  `PORTABLE_EXECUTABLE_DIR`), so it runs from a USB stick. It does **not** self-update, because
  `electron-updater` cannot replace a running single-file exe. When a newer app is known, GO stays
  blocked until the setup exe is installed. When the app is current, runtime payload checks still work.

Only the installer appears in `latest.yml`; the portable build is a manual download.

## Advanced drawer

The drawer has five tabs: **Game**, **Runtime**, **Locations**, **DLSS 5** and **Updates** (the
automatic-updates switch, see [Updates](#updates)).

Every path setting shows what it *actually resolves to*, tagged `SET` (user override) or `AUTO`
(launcher's choice). It appears in red with `NOT FOUND` when the path does not exist. The AUTO
tag does not show *which DLL is about to be injected*. The resolution order has four
fallbacks, so the settings alone do not reveal the answer.

Resolution happens in the main process (`resolvedPaths()`), because it depends on what is on disk.

Every row has a go-to button that reveals the path in Explorer. **Locations** covers the paths
with no editable field: the injector, FX pack, downloaded runtime, and the launcher's data folder.
The UI has no other way to reach them. Revealing a path that does not exist yet opens the nearest
existing parent instead of failing.

The window grows to 600px tall while the drawer is open and returns to the launch strip's size on
close. The settings do not fit in a 300px window without scrolling. If you resize the
window yourself while the drawer is open, that size is kept.

> Testing the drawer through CDP: pass `--disable-backgrounding-occluded-windows` and
> `--disable-features=CalculateNativeWinOcclusion`. An unfocused Electron window freezes its
> animation timeline at `currentTime: 0`. The drawer then reads as permanently closed
> (`visibility: hidden`, `translateY(100%)`) even though the class is applied. Forced inline styles
> do not recompute either. The cause is throttling, and the drawer layout is fine.

## App icon

`build-resources\icon.png` is generated. To regenerate it:

```batch
npm run build:icon
```

`scripts\build-icon.cjs` renders the CS2 film-strip mark on the launcher's near-black plate with its
gold accent. It uses Electron as a headless rasteriser. Electron is already a devDependency, so no
ImageMagick or Inkscape is needed. electron-builder converts the PNG to the Windows `.ico` at
package time.

[release-repo]: https://github.com/Wockstajit/SourceMVM
