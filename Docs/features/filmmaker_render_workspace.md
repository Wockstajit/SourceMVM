# Render workspace: range nudges, the done sound, previous renders, the queue and the kill feed

The parts of the Render workspace (the Camera Editor's Render tool) that sit outside the render
passes and the recorder. Passes are in [Render passes](filmmaker_render_passes.md), frame rates in
[Recording FPS](filmmaker_render_fps.md).

## Picture size and ReShade

A render is the size of the frame CS2 draws, which is the game window. The Quality setting changes
how many bits the encoder spends and never the size, so a 1280x720 window gives a 1280x720 file at
Highest. Measured 2026-11-08: 854x480, 1280x720 and 1920x1080 windows gave files of exactly those
sizes, with the workspace's preview shrunk on screen each time. PICTURE reads the real scene target
(`sceneW`, `sceneH`), turns amber below 1080p and offers **Set size** buttons for 1080p, 1440p and 4K
where they fit the monitor (`monitorW`, `monitorH`). They run `mirv_filmmaker window size`. While a
job runs the buttons are hidden, `window size` / `window aspect` refuse, and a border drag, snap,
maximise or restore is answered with the size the window already has, because a resize would
change the scene target under the recorder. If the
scene target is smaller than the window, the section says so and points at FidelityFX in CS2's video
settings. That branch has not fired in a test: the FidelityFX edit to `cs2_video.txt` did not reach
the session it was tried in.

The note under it says whether the take includes ReShade (`reshade`: 0 not running, 1 running without
the SOURCE:MVM add-on, 2 in the render). [Automation/dlss/README.md](../../Automation/dlss/README.md)
owns how that works.

## Range nudges

The RANGE & CAMERA tab shows the resolved span as IN, LAST FRAME and the length. IN and LAST FRAME
each have −1 and +1 buttons that move that end by one tick. A flag on the timeline covers dozens of
ticks per pixel on a long demo, so the last few ticks are set here. Each step sends the whole span
as `render range custom <in> <outExclusive>`, so it works from any range mode and switches the
range to Custom. A step stays inside the demo: IN never goes before its first tick and LAST FRAME
never past its last one (the published `demoStart` / `demoEnd`), so a button at a demo edge does
nothing. A last frame past the end would leave the job waiting on frames the demo's end hold never
plays. `render-range-nudge-tests.mjs` runs the buttons' function against the bounds.

## The sound when a render ends

A short chime plays when a job ends: two rising notes when it finished, two falling notes when it
failed. Cancelling a job is the user's own click, so it stays silent.

- The header's SOUND chip, next to Back, says `SOUND: ON` or `SOUND: OFF` and switches it.
- OUTPUT > WHEN THE RENDER ENDS has the same switch, a Volume value (0–100%, default 60) and
  Play the sound. Releasing the volume drag plays the chime at the new level.
- The chime is synthesised in memory and played through its own XAudio2 voice
  (`Filmmaker/Render/RenderDoneSound.{h,cpp}`). It needs no sound file, is heard whatever CS2's own
  volume or mute is, and never touches the game's audio. XAudio2 is only started the first time a
  chime plays. A chime started while one is still sounding cuts the first one off: its voice is
  destroyed, which waits until XAudio2 has let go of the buffer, before the buffer is rewritten.
- The switch and volume persist in `filmmaker_settings.json` as `renderDoneSound` and
  `renderDoneSoundVolume` (`Config/FilmmakerSettingsStore.cpp`).

```
mirv_filmmaker render sound                       on/off state and volume
mirv_filmmaker render sound on|off|toggle
mirv_filmmaker render sound volume <0..100>
mirv_filmmaker render sound test                  play it now, whatever the switch says
```

## When a render cannot be written

Every format but TGA, BMP and EXR is encoded by FFmpeg (`ffmpeg\ffmpeg.ini`'s `[Ffmpeg] Path`,
else the bundled `ffmpeg\bin\ffmpeg.exe`). The encoder probe checks that this `ffmpeg.exe` exists
and answers `-version` before it tests NVENC. A job in an FFmpeg format waits for that probe
("checking FFmpeg") and refuses to start when FFmpeg is missing or broken, with one sentence that
names the path. A job after a failed probe probes again, so restoring `ffmpeg.exe` needs no restart.

While a job records, the first stream that cannot open its output or write a frame ends the job at
once. The failure text gives that stream's problem and the last lines FFmpeg wrote to its error
output, for example the encoder it could not open. The check after the last frame stays, so a late
failure still refuses the manifest. The rules are in `Render/RenderWriterFailureMath.h`, tested by
`render-writer-failure-math-tests.cpp`.

Before this, a job with no working FFmpeg rendered the whole take and then failed with only a count
("capture writer reported N output failure(s); refusing a completed manifest"), and the cause
reached only the console (viewer report, 2026-11-18).

## Previous renders

The JOB column lists the latest renders of the demo that is playing, newest first (three rows, up
to six in `render takes`). Each row shows when it was written, its status, its tick range, frame
rate, format and passes. **Use** starts the next job from that render's settings, to render a shot
again with one change or to add a pass for exactly the same In and Out.

The column shows only rows that fit whole above START RENDER. When they do not all fit (at 480p
with the TIMELINE dock up, one does), the heading says which are showing, such as `1-2 OF 3`, and
its ◀ ▶ buttons page through the rest, so every listed take stays usable. With no room for even one row, the heading still says
how many there are. The count comes from the room the rest of the summary leaves, not from the rows
on screen, because Panorama lays a change out a frame or two late and a self-measuring list
flickered.

- `Render/RenderTakeHistory.{h,cpp}` scans the output folder's take folders for `manifest.json`
  (written by `RenderManifest.cpp`, at most 512 KB each) on a worker thread. It scans when the
  Render workspace opens, after every job, and when the output folder changes. Only the scan runs
  on the worker; the list it returns is read on the main thread.
- A take belongs to the playing demo when its manifest's `demo` resolves to the same project path
  (`ResolveDemoProjectPath`, `CanonicalDemoPath`), so a take rendered from the demo's keyframed copy
  still lists under the original.
- `Render/RenderTakeImportMath.h` maps a manifest to settings and to the `mirv_filmmaker render ...`
  commands a person would type, so an import can never set a combination the workspace could not.
  Their order matters: the camera goes first, because picking the Camera Path source replaces a
  non-path range with the path's span. Then extension, `range custom`, fps, format, `pass clear`, one
  `pass add` per stream, then HUD, camera data, blood, key colour and depth range. Encoder settings
  are not in the manifest, so an import keeps the current ones. `RenderTakeImportMathTests` covers
  the mapping.
- An import is refused while a job runs.

```
mirv_filmmaker render takes [refresh]    this demo's earlier takes, newest first
mirv_filmmaker render import <n>         start from take n's settings
```

The workspace state JSON carries `sound` (`on`, `volume`) and `takes` (the listed rows) for the
panel (`CameraEditorRenderJs.h`).

## The After Effects camera

With the camera sidecar on (Passes tab, **After Effects camera**, on by default) a finished take also
holds `camera_aftereffects.jsx`. File > Scripts > Run Script File in After Effects makes a comp the
render's size, frame rate and length, with the take's camera keyed on every frame. The rendered plates
go in at frame 0, and 3D layers then sit in the shot with no 3D camera track. It works for every camera
source: a POV, a camera path, a dolly zoom or a follow camera.

`Render/CameraExportMath.h` builds it from the camera of every captured frame (the same rows as
`frames.csv`). Source points go to AE as (x, -z, y), one Source unit per pixel times **Scale**. CS2's
4:3 field of view is widened to the frame's aspect and turned into AE's Zoom, (width / 2) / tan(hfov / 2).
AE's Orientation locks at a Y of 90 degrees, which a panning camera passes all the time, so the comp
has a rig: a null carries the position and the heading as its Y Rotation, unwrapped so a full turn keys
a full turn, and the camera, parented to it at [0, 0, 0], carries only the pitch and roll that remain.
**Start at the origin** puts the first frame's camera at [0, 0, 0], so a new 3D layer lands in front of
it; off keeps map coordinates, the same for every take of the demo. The manifest records the file, the
scale and the origin choice. `camera-export-math-tests.cpp` projects world points through both cameras
and checks they land on the same pixel.

```
mirv_filmmaker render pass cameradata 0|1     the sidecar and camera_aftereffects.jsx
mirv_filmmaker render pass aescale <0.01..100>
mirv_filmmaker render pass aeorigin 0|1
```

## The render queue

The JOB column's **+ QUEUE** copies every setting on screen into a queued shot: the In / Out as ticks,
the camera source and path extension, the output and the passes. A POV shot also keeps the watched
player (by SteamID), and a Camera Path shot keeps the path's keys, so a POV and a dolly zoom of the same
moment can wait side by side. The QUEUE list shows each shot and its state. **Render all** runs them in
turn through ordinary jobs, each into its own take folder. A POV shot first seeks to its start and
watches its player there; a player who cannot be watched skips that shot. Cancelling a job stops the
queue, **Stop** ends it after the current shot, and a closed workspace ends it. The settings and the
camera path on screen before the queue are put back when it ends, however it ends
(`Render/RenderQueue.cpp`, `mvm_debug` `render.queue`).

```
mirv_filmmaker render queue                   list + help
mirv_filmmaker render queue add | remove <n> | clear
mirv_filmmaker render queue start | stop
```

## The kill feed

CS2 draws the kill feed (its death notices). The Passes tab's **KILL FEED** section shapes it for a
movie, live and in renders (`Movie/KillFeed.cpp`, applied to CS2's notices in `CameraTimelineJs.h`):

- **A name per player.** Each player of the demo has a field: type a name and press Enter, and every
  notice with them shows it. × puts the real name back. Quotes, semicolons and angle brackets are left
  out of a typed name.
- **Only the watched player.** Keeps only the notices the watched player is in, as attacker, assister
  or victim, for a POV edit.
- **Kill feed** (under GAME HUD). With No HUD, the clean plate keeps the feed. In-game and Full HUD
  always carry it. A No HUD plate is normally taken before the UI draws, so with the feed kept the
  Beauty · Everything plate is taken after it instead, like In-game, with every other HUD panel hidden
  (`HudPlate` in `RenderTypes.h`). It reuses CS2's own `cl_drawhud_force_deathnotices` path, and that
  cvar's -1 (forced off) still wins. EXR output refuses the kill feed, because the HDR frames are tied
  to the before-UI capture.

CS2 keeps a pool of notice panels and shows and hides them itself, so a notice left out by the filter
is folded to no height and no opacity rather than hidden. A label cannot carry its real name between
frames (Panorama hands back a new JS object for a panel on every lookup), so the script remembers every
name it has written and the player it stands for.

```
mirv_filmmaker killfeed                       players and their shown names
mirv_filmmaker killfeed name <n> <new name> | name <n> | name clear
mirv_filmmaker killfeed only all|watched|toggle
mirv_filmmaker render pass killfeed 0|1
```

The names belong to the demo that is playing and are dropped when another one loads.

## Verification

- `ctest -R RenderTakeImportMathTests` for the manifest mapping, `ctest -R RenderWriterFailureMathTests`
  for the failure text.
- In the mockup's Render tab, the SOUND chip, the WHEN THE RENDER ENDS section and the PREVIOUS
  RENDERS rows follow the fixture state (`UiMockup/shim/state.js`, `commands.js`).
- In game: finish a short job and hear the rising chime, fail or cancel one and check the falling
  chime or silence, then Use the new row and check that range, camera and passes match it.
- In game: rename the staged `ffmpeg` folder and start an MP4 render. It must stop at "checking
  FFmpeg" with the missing path. Put it back and the next render must complete.
