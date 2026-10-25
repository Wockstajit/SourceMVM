# Render workspace: range nudges, the done sound and previous renders

Three parts of the Render workspace (the Camera Editor's Render tool) that sit outside the render
passes and the recorder. Passes are in [Render passes](filmmaker_render_passes.md), frame rates in
[Recording FPS](filmmaker_render_fps.md).

## Range nudges

The RANGE & CAMERA tab shows the resolved span as IN, LAST FRAME and the length. IN and LAST FRAME
each have −1 and +1 buttons that move that end by one tick. A flag on the timeline covers dozens of
ticks per pixel on a long demo, so the last few ticks are set here. Each step sends the whole span
as `render range custom <in> <outExclusive>`, so it works from any range mode and switches the
range to Custom.

## The sound when a render ends

A short chime plays when a job ends: two rising notes when it finished, two falling notes when it
failed. Cancelling a job is the user's own click, so it stays silent.

- The header's SOUND chip, next to Back, says `SOUND: ON` or `SOUND: OFF` and switches it.
- OUTPUT > WHEN THE RENDER ENDS has the same switch, a Volume value (0–100%, default 60) and
  Play the sound. Releasing the volume drag plays the chime at the new level.
- The chime is synthesised in memory and played through its own XAudio2 voice
  (`Filmmaker/Render/RenderDoneSound.{h,cpp}`). It needs no sound file, is heard whatever CS2's own
  volume or mute is, and never touches the game's audio. XAudio2 is only started the first time a
  chime plays.
- The switch and volume persist in `filmmaker_settings.json` as `renderDoneSound` and
  `renderDoneSoundVolume` (`Config/FilmmakerSettingsStore.cpp`).

```
mirv_filmmaker render sound                       on/off state and volume
mirv_filmmaker render sound on|off|toggle
mirv_filmmaker render sound volume <0..100>
mirv_filmmaker render sound test                  play it now, whatever the switch says
```

## Previous renders

The JOB column lists the latest renders of the demo that is playing, newest first (three rows, up
to six in `render takes`). Each row shows when it was written, its status, its tick range, frame
rate, format and passes. **Use** starts the next job from that render's settings, to render a shot
again with one change or to add a pass for exactly the same In and Out.

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

## Verification

- `ctest -R RenderTakeImportMathTests` for the manifest mapping.
- In the mockup's Render tab, the SOUND chip, the WHEN THE RENDER ENDS section and the PREVIOUS
  RENDERS rows follow the fixture state (`UiMockup/shim/state.js`, `commands.js`).
- In game: finish a short job and hear the rising chime, fail or cancel one and check the falling
  chime or silence, then Use the new row and check that range, camera and passes match it.
