# Startup video (CS2 launch intro)

`source-mvm-intro.mp4` is the master for the SOURCE:MVM launch intro. It is the only
version-controlled file here. The encoded `.webm` files the game actually plays are a build
output and are regenerated from this master.

## How it reaches the game

CS2's launch intro is Panorama, not engine: `panorama/scripts/intromovie.vts` runs
`$("#IntroMoviePlayer").SetMovie("file://{resources}/videos/intro.webm")`.

The CS2 install is never modified. The encoded override goes here:

```
build/staging-release/fx/source_mvm_fx/ui_override/panorama/videos/intro.webm
```

`AfxHookSource2` (`main.cpp`) registers that `ui_override` subtree with `PATH_ADD_TO_HEAD`.
It outranks `csgo/`, so CS2's own player loads our file. There is no second panel or timing
race. Steam's "verify integrity" has nothing to revert, and a game update cannot clobber it.

### The trap, so nobody repeats it

The content mount one level up is registered with the SDK default, `PATH_ADD_TO_TAIL`, which is
literally *"Last path searched"*. An `intro.webm` placed there loses to `csgo/` every time,
regardless of what that mount's `gameinfo.gi` search order says. Two earlier attempts died on
this: a same-name override (silently ignored), then a uniquely-named file played by our own
Panorama panel (which raced Valve's intro and landed on top of it mid-playback). `-novid` does not help.
We confirmed it appears in the `cs2.exe` command line, and build 2000879 ignores it.

## It is applied automatically

`build.bat` step [8/8] runs `Automation/tools/set-startup-video.ps1 -Install` on every
build. The step is idempotent. It re-encodes only when this master is newer than the installed
override, so a normal build costs nothing, while a fresh clone or a wiped `build/` tree
re-encodes once.

## Changing it

```powershell
# swap in a new intro (also updates the committed master if you replace this file first)
powershell -File Automation\tools\set-startup-video.ps1 -Video "path\to\new-intro.mp4"

# what is active right now
powershell -File Automation\tools\set-startup-video.ps1 -Status

# back to Valve's stock intro
powershell -File Automation\tools\set-startup-video.ps1 -Restore

# export whatever is currently active as MP4
powershell -File Automation\tools\set-startup-video.ps1 -Extract "$env:USERPROFILE\Desktop\intro.mp4"
```

To change the intro permanently, replace `source-mvm-intro.mp4` in this folder and commit
it. The next build picks it up (its timestamp will be newer than the installed override).

## Encoding contract

One file, `intro.webm`: VP9, capped at 3840×2160 and never upscaled, 24 fps, `yuv420p`,
Opus stereo 48 kHz. Valve also ships a VP8 `intro720p.webm`, but their own script only loads
`intro.webm` (or `intro-perfectworld.webm` on the Perfect World launcher), so mirroring the
second variant gains nothing.

Our resolution deliberately differs from Valve's 4K master: ours is 1080p. Force-upscaling
would spend bitrate on pixels a scaler invented, and they would look no better.

`build.bat`'s staging `robocopy /MIR` excludes `ui_override\` so the startup-video
override survives a particle-pack restage. Several features share this mount, and
each owns only its own subtree. That exclusion only works with absolute paths (`build.bat` builds them from
`%~dp0`). A relative path silently fails to match, and `/MIR` deletes the tree. We verified this.
