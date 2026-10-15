# SOURCE:MVM for Deadlock

The Deadlock port of the filmmaker is a separate hook DLL, `AfxHookDeadlock.dll`. It is injected into
`deadlock.exe` and draws its UI as native Panorama panels, as the CS2 build does.

The scope is the filmmaking core: the Camera Editor, camera path, timeline, graph, render output, and
a CONFIG workspace that ships empty. Cosmetics, econ skins, ragdolls, particle packs, the Play-menu
takeover and the demo browser are out of scope.

| Document | What it covers |
| --- | --- |
| [camera-editor-port-plan.md](camera-editor-port-plan.md) | The plan for the camera editor port: measured ground truth, workstreams, gates, and the debugging and automation parity checklist |
| [signatures.md](signatures.md) | Every version-coupled address, how it was derived, and what is still unresolved |
| [ui-style.md](ui-style.md) | Deadlock's design tokens, chrome conventions, and its native replay HUD |
| [reference-extraction.md](reference-extraction.md) | Rebuilding the local Panorama reference after a Deadlock update |
| [cameras.md](cameras.md) | The three camera systems, how they take turns, and how to verify them |
| [console-commands.md](console-commands.md) | Native F7 commands, shared names, camera IO and Deadlock alternatives |

## Build

The Deadlock target has its own CMake option and is off by default, so a normal CS2 build
does not build it.

```batch
cmake --preset x64-deadlock-release
```

```batch
cmake --build build/x64-deadlock-release --config Release --target AfxHookDeadlock --parallel
```

The injector and the .NET launch host come from the CS2 staging prefix. Both are game-agnostic:
`injector.exe` takes only a process id and a DLL path. Build the CS2 stage once (`build.bat -NoLaunch`)
to get them. `SourceMovieMakerLauncher\scripts\build-host.ps1` builds the host.

For the Deadlock native regression targets, run:

```powershell
ctest --test-dir build/x64-deadlock-release -C Release --output-on-failure -R '^Deadlock'
```

The shared CS2 tests belong to the CS2 build directory. An unfiltered Deadlock CTest run may
list shared executables that were not built there. The camera call-chain verification also has
an engine-free check: `py -3 Automation/tests/deadlock-view-chain-tests.py`.


## Launch

```batch
Automation\launch\deadlock.bat
```

Run it as a **background task**. The completion notification means the process is up, not that it
succeeded. Read the exit code.

The launcher requires the previous game to be closed. It does not kill a game or a process
holding the netcon port. It waits for the injection host, then checks the hook's reply. It does not
wait for the game to exit before reporting readiness. Host output is kept under
`Automation/runs/deadlock-launch/`. Pass `-mvmDebug` through `-CustomLaunchOptions` to record
initialization and console registration in the runtime log.

`-insecure` is mandatory: the DLL refuses to load without it. Never launch with `-vulkan`. Every
render hook here is a D3D11 hook, so Vulkan is not a supported mode, not even a reduced one. The hook
says so at load time instead of looking broken later.

Close Deadlock with a console `quit`. A `taskkill /F` breaks the Steam pipe and the next launch
comes up as a black window that loads no map while still reporting healthy.

### The launch host supports Deadlock

`SourceMovieMakerHost.exe` takes the Steam app id as a parameter, `--steam-appid`, defaulting to 730
(CS2), so existing CS2 callers are unaffected. The launcher passes 1422450 for Deadlock.

The launcher quotes every value it passes. `Start-Process -ArgumentList` joins its array with spaces
and adds no quotes, and the Steam, repository and Deadlock paths all contain spaces.

## Drive it over netcon

Deadlock gets the same command-and-control surface as CS2, on port **29020**. A CS2 session on
29010 and a Deadlock session can then run side by side.

```bash
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File Automation\netcon\deadlock-netcon.ps1 -CommandBatch "demo_pause;;mirv_deadlock health"
```

The channel is hosted **inside the DLL**, not by the engine. Deadlock's `engine2.dll` does carry the
netconsole server. The "Unable to open netconsole on port %d" and "must send PASS command" strings
are both there. A static scan cannot tell whether the client build exposes it through CS2's
`-netconport` flag, and the automation contract should not depend on the answer. Hosting it here also
removes the engine's 256-byte truncation on the `ui_eval` path. That truncation is why the CS2 UI is
driven through short registered helpers
instead of inline JavaScript. `mirv_deadlock netcon` reports the port, client count and drop count.

Output is queued and written from a separate thread, on purpose. The engine writes console output
synchronously, so a slow or stalled reader on the CS2 side makes the game hitch. Here, a reader that
stops reading loses lines instead, and the drop count shows it.

## The console surface

`mirv_filmmaker` is the canonical root in both games. The `mirv_deadlock` names below remain
compatible aliases. These subcommands are Deadlock's contract. They do not promise blanket CS2
compatibility.
The [console guide](console-commands.md) covers HLAE commands and native F7 testing.

| Command | What it does |
| --- | --- |
| `mirv_deadlock health` | Fast triage. Pump counts, every resolved address, engine state, tick rate, view hook, UI, netcon |
| `mirv_deadlock workspace off\|camera\|config\|render` | Which workspace the right-hand inspector column shows |
| `mirv_deadlock dock native\|camera\|graph` | Which editor is docked under the preview. Independent of the workspace |
| `mirv_deadlock dock rect <x> <y> <w> <h>` | The band the panel laid out. Reported by the panel; the graph's hit tests are solved against it |
| `mirv_deadlock dock nativeheight <h>` | Deadlock's replay bar's measured height. Only Panorama can measure it |
| `mirv_deadlock ui rebuild` | Rebuilds the Panorama panels in place, for iterating without a restart |
| `mirv_deadlock ui_eval "<js>"` | Runs Panorama JavaScript in the panel's context |
| `mirv_deadlock panorama` | The attribute-offset calibration state and its candidates |
| `mirv_deadlock view` | View hook: call count, write count, offsets, last observed values |
| `mirv_deadlock view phase before\|after\|both` | When the pose is written relative to the engine's own view setup |
| `mirv_deadlock view offsets ...` | Override the view struct field offsets at runtime |
| `mirv_deadlock engine slots` | The demo interface vtable slot table |
| `mirv_deadlock engine probe <first> <last> [demo]` | Call a range of slots and report what each returned |
| `mirv_deadlock engine tickinterval <s>` | Set the real seconds-per-tick |
| `mirv_deadlock freecam on\|off` | Free camera |
| `mirv_deadlock marker add\|delete\|clear\|goto <i>\|gotosel\|update` | Camera markers |
| `mirv_deadlock marker sel prev\|next\|<i>` | Step the selection, wrapping. The PATH card's stepper and the timeline both come here |
| `mirv_deadlock marker fov\|roll <v>` | The SELECTED marker's own values, keyed with it |
| `mirv_deadlock marker ease none\|in\|out\|inout` | Ease on the selected key. One value, three routes: the PATH card, the dock footer, the marker menu |
| `mirv_deadlock marker reposition` | Move the selected camera to the current view |
| `mirv_deadlock path play\|pause\|save\|load` | The shot |
| `mirv_deadlock path interp linear\|cubic` | The curve for the whole path. Same owner as the graph's Linear / Smooth pair |
| `mirv_deadlock path speedmode manual\|constant\|perseg` | How the segments are timed |
| `mirv_deadlock path draw\|drawmarkers 0\|1` | Draw the path and its numbered markers in the world |
| `mirv_deadlock path freeze 0\|1` | Hold the tick, so a camera can be placed without the action moving |
| `mirv_deadlock follow toggle\|place\|reposition\|clear` | The follow camera: one PLACED camera that watches a hero |
| `mirv_deadlock follow mode lockon\|attach` | Turn to face the hero, or ride a point on its model |
| `mirv_deadlock follow hero <index>` / `attach <point>` | What it watches and where it attaches |
| `mirv_deadlock follow yaw\|pitch\|dist\|height\|lateral\|fov <v>` | Framing |
| `mirv_deadlock follow look\|pos\|predict\|deadzone\|maxturn\|deathhold <v>` | Tracking |
| `mirv_deadlock follow releaseondeath\|holdlast\|retarget 0\|1` | What it does when the hero dies or respawns |
| `mirv_deadlock seek <tick>` | Seek the DEMO to an absolute tick. What the replay bar's kill markers use |
| `mirv_deadlock scrub <tick>` / `scrubend` | Drag the camera SHOT's playhead, then release and seek. A different playhead from `seek` |
| `mirv_deadlock scrubtick <n>` | Step the playhead by whole ticks |
| `mirv_deadlock speed up\|down\|<multiplier>` | Playback speed. A multiplier is taken exactly; up/down walk the presets |
| `mirv_deadlock loop on\|off\|toggle\|in\|out\|clear\|status` | The review loop, separate from the render range |
| `mirv_deadlock hud scaled\|hidden\|toggle` | The game's own HUD inside the preview frame |
| `mirv_deadlock preview [on\|off\|offsets <x> <y>]` | The scaled world preview |
| `mirv_deadlock schema [layout <a> <b>\|reload]` | Entity offsets, resolved by name from the live schema |
| `mirv_deadlock livejs load\|watch\|unwatch\|status <path>` | Hot-reload Panorama JavaScript from a file |
| `mirv_deadlock graph seed\|smooth\|linear\|undo\|redo` | The curve editor |
| `mirv_deadlock graph channel <0-6>` | Show or hide one channel: x, y, z, pitch, yaw, roll, fov |
| `mirv_deadlock graph solo <0-6>` | Show only that channel; soloing the last visible one restores them all |
| `mirv_deadlock graph drive on\|off` | Let the graph's curves drive the camera instead of the path |
| `mirv_deadlock render fps <n>` | Recording FPS (mux + host_framerate), 1 to 1000; presets and typed custom FPS |
| `mirv_deadlock render range <a> <b>` | Tick range; 0 means the shot's own range |
| `mirv_deadlock render dir <path>` | Output root; defaults to Videos/SourceMVM/Deadlock |
| `mirv_deadlock render range all\|shot\|segment\|loop\|custom` | What to render. One word is the MODE; two numbers is an explicit tick range |
| `mirv_deadlock render camsrc current\|free\|path\|follow` | Which camera drives the capture |
| `mirv_deadlock render hud none\|game\|full` | How much of the game's HUD is in the frame |
| `mirv_deadlock render format mp4\|avi\|gif\|png\|tiff\|tga\|bmp\|jpg` / `quality <0-51>` | Output encoding |
| `mirv_deadlock render names\|damage\|pauseondrop\|beep 0\|1` | Subjects in frame, and what happens during the capture |
| `mirv_deadlock render start\|stop` | Runs the capture |
| `mvm_debug start\|stop\|status` | The runtime debug log |

Value tiles in the Camera Editor and Render share a visible bar and handle. Drag anywhere on the
tile to adjust relative to its current value. **Shift** makes the drag five times finer. There are
no plus or minus step buttons. Scrolling only scrolls the inspector. Hidden tabs and game mouse mode
cannot edit values. Follow and attach expose their individual settings and reset controls. Config is an empty workspace shell with no feature controls. Deadlock has no
handheld camera shake.

Right-click a diamond on the **Camera** timeline for the same compact layout as CS2. It has a Keyframe
header and key chip, per-key Ease buttons, stacked Style/Intensity/Speed rows, an inline style list,
and Apply to All / Remove / Remove All. Intensity and speed use segmented meters with drag and Shift
fine control. Numeric step buttons are absent. Remove All uses CS2's in-place confirmation. These
settings do not appear in the Path card. Opening the menu does not seek or change selection, and its
edits target the clicked diamond. Switching away, or deleting or retiming that key, closes it.
`mirv_deadlock marker ease <none|in|out|inout> [index]` supports an explicit key for this menu.
Omitting the index keeps the existing selected-camera behavior.

## The graph editor

Same shot as the timeline, shown as seven keyframe curves: x, y, z, pitch, yaw, roll, fov. The
model is the shared `GraphExpModel`, with stable keyframe ids, Bezier handles and undo snapshots.

Two design points matter here.

The plot geometry lives in C++, not in the panel. Panorama gives an in-game HUD context no
mouse-move event, so a drag has to be driven from an OS-side cursor probe. The dragging code
therefore has to know where the plot rectangle is on screen. JavaScript could measure its own layout
and report it back, but that round trip is stale by definition. Instead, C++ owns the rectangle,
tells the panel where to draw, and converts cursor pixels to tick and value.

The curves are sampled by the same evaluator that moves the camera. C++ samples each visible
channel and sends a normalised polyline. The panel positions a pool of dot panels along it.
Re-implementing the Bezier solve in JavaScript would give two answers that agree only until they
diverge.

The playhead belongs to the shot, not to the graph. The timeline and the graph are two views of one
thing, so a scrub in either must be the same scrub. A click on empty plot scrubs. A release ends the
drag **and** seeks. Ending without the seek leaves the demo tick stale, and the timeline immediately
disagrees.

Driving the camera from the curves is off by default. The graph is an editing surface first. A panel
that silently takes the camera the moment it opens is worse than one that needs a button.

## Render

Beauty-pass capture writes MP4 (H.264), lossless AVI (HuffYUV), GIF, or PNG/TIFF/TGA/BMP/JPEG
sequences into numbered take folders. The Output tab has a native folder picker, presets and
custom FPS, resolution, CS2's MP4 quality presets and CRF, bitrate, CPU/NVIDIA GPU encoding, and
encode speed. Auto currently selects CPU. HDR EXR is unavailable because the capture source is
the display image, not a pre-tonemap HDR texture. Capture is video only. GIF is limited to 100 fps and stores timing in hundredths of a second.

FFmpeg is resolved through `SOURCEMVM_FFMPEG`, the launcher's `ffmpeg.ini`, bundled folders,
or PATH. Each take contains `encoder.log` for encoder failures. The default root is
`Videos/SourceMVM/Deadlock`; picking another folder persists that choice.

The job owns `host_framerate` and temporarily sets demo playback speed to 1. Recording FPS is both
the **mux** rate and the **capture** rate (`host_framerate`). The timeline speed dial is preview-only
and is not required for a take (see [`filmmaker_render_fps.md`](../features/filmmaker_render_fps.md)).
Output rates do not need to divide the replay tick rate. The previous frame clock, demo speed, pause
state, camera mode, loop toggle, workspace, and HUD preference are restored when the encoder
finishes. The live tick interval must be confirmed. Out is inclusive: 64 ticks at 60 fps contains
61 samples. Frame counts use the recording FPS rather than accumulating fractional tick steps.

Rendering hides the tool's controls and replay bar. No HUD additionally hides the game's HUD.
Write failures stop the job and appear in the render status; Pause on drop leaves playback
paused after a write failure. Sound when done plays after successful encoder completion.

## When something does not work

Run `mirv_deadlock health` first. It separates failures that look identical from the outside.

**Render pump 0.** The present hook never installed. Nothing else can work; check this before
anything else.

**Main pump 0.** The client interface detour did not take, so no UI will ever appear.

**No window at all, no dump, no log.** The hook killed the process before it drew a frame. Two
causes produce this symptom. It looks the same as a game that simply refuses to start:

- **Work done under the loader lock.** The module dispatcher runs inside `LoadLibraryExW`, so
  anything heavy there runs while the loader is held. The present hook creates a D3D11 device and
  registers a window class to read one vtable entry; doing that from the dispatcher took the process
  down every time. It is installed from the client's `Init` instead.
- **A printer that was never allocated.** `My_Console_Message` dereferences `g_ConsolePrinter`
  unconditionally, and the moment tier0 loads the hook points `advancedfx::Message` at it. A null
  printer turns the first status line into an access violation, again under the loader lock.

For this DLL, the general lesson is to keep the module dispatcher to storing handles and scanning
patterns. Anything that touches the graphics stack, the window or a socket goes behind the client's
`Init`.

### A netcon round trip waits for a game frame

A command sent over netcon is queued and executed by the **main-thread pump**, so the reply cannot
appear until the game reaches its next frame. During startup, a load or a seek can take comfortably
over a second.

Two consequences:

- The client's default read window is 3 seconds, not CS2's 1.5. A window shorter than the round
  trip reads as "the hook ignored me".
- The readiness probe holds **one** connection open and re-asks on it. It does not open a fresh
  socket per attempt.

### A stale instance holding the port

The launcher refuses to start while Deadlock is running. It then **waits up to 15 seconds for port
29020 to actually be free**, and refuses to launch if the port stays held.

That second step is not optional. Closing the process does not release the listener. When the new
instance fails to bind, the port stays open, and the hook still replies from the process that is on
its way out. Every reading taken through it belongs to the wrong game. The symptom is very
misleading. netcon connects, commands execute, and nothing in the output says it came from
somewhere else.

The socket is opened with `SO_EXCLUSIVEADDRUSE` rather than `SO_REUSEADDR` for the same reason. On
Windows, `SO_REUSEADDR` lets a second process bind an address that another is already listening on.
The OS then hands connections to one of them without saying which. That is the wrong behaviour for a
diagnostic console. Exclusive use makes the collision an error, and the error reports the problem.

### Bisecting a startup death

Each hook can be switched off at launch, one per run. When the process dies before drawing, this
isolates the fault in two launches instead of an afternoon of guessing:

| Flag | Turns off |
| --- | --- |
| `-mvmNoViewHook` | The camera body. The detour stays installed, so the main pump and netcon still work. |
| `-mvmNoViewDetour` | The camera detour. The independent main pump and netcon remain available. |
| `-mvmNoMainFrame` | The main-thread pump. Custom netcon listens but cannot process commands; use native VConsole. |
| `-mvmViewProbe` | Replaces camera installation with a read-only view probe and disables the main pump for that diagnostic launch. |
| `-mvmNoPresentHook` | The render pump. |
| `-mvmNoUi` | The Panorama panels. |
| `-mvmInert` | Everything. The DLL loads and returns from `DllMain` having done nothing at all. |
| `-mvmNoBootTrace` | The boot trace itself, which is otherwise on by default. |

The main-thread pump runs after stage 12 in CSource2Client's frame-stage dispatcher. Its
slot is checked against a unique binary signature, and the callback must run on the client
initialization thread. Camera installation is independent: a missing camera signature does not
prevent the custom console from processing commands. `-mvmNoMainFrame` isolates this pump;
`-mvmViewProbe` retains its read-only diagnostic behavior.

The boot trace at `%TEMP%\afxhookdeadlock_boot.log` is the other half. It writes through raw
kernel32 only, with no C runtime, no allocation and no tier0. That is the only thing safe to call
while the loader lock is held. It flushes every line as it is written, because the process may not
survive to the next one. It is always on, and it costs one file append per startup milestone.

### The camera hook only touches the main view

This is easy to get wrong and expensive to diagnose. The view-setup function is called for more than
one view per frame, on more than one kind of object. Writing origin, angles and field of view at
`+0x4a0` of whatever it is handed does **not** fault, because that memory is committed. The process
survives the write and dies later somewhere unrelated.

So every call is gated on two conditions, both derived rather than hardcoded:

- the object's vtable is the view class, found from the `.?AVCViewRender@@` RTTI name, and
- the return address is the one call site that sets up the main view, found as the direct `call` to
  the view setup inside the function that vtable slot 4 points at.

`verify-deadlock-signatures.py` checks that whole chain offline and reports it as
`vtable -> slot4 -> caller returns`. On the build this was written against it resolves to
`0x2349178 -> 0x16b6700 -> 0x16b6744`.

A third gate rejects views that are not yet filled in: a non-finite pose, a field of view outside
1 to 179 degrees, a zero viewport, or bit 1 of the flags byte set. `mirv_deadlock view` reports how
many calls each gate rejected, because "the camera does not move" and "the camera moved the wrong
view" are different problems with the same symptom.

### The main-thread pump

On build 6774, `CSource2Client` slot 36 dispatches frame stages, and the pump runs after stage 12 in
that dispatcher. The dispatcher is checked against a unique binary signature, and the callback must
run on the client initialization thread.

Only `Connect` (slot 0) and `Init` (slot 3) are hooked, and those come from `IAppSystem`, whose
layout has been stable across Source 2 for years.

A required address can read BROKEN. A Deadlock update moved it. Run the offline verifier, which
classifies every pattern without launching the game:

```batch
py -3 Automation/verify/verify-deadlock-signatures.py
```

[signatures.md](signatures.md) records the anchor each pattern hangs off, so a break means
re-deriving a pattern, not starting an investigation from scratch.

**View hook calls climbing, writes 0.** Nothing is claiming the camera. Expected until free cam or
a shot is running.

**View hook viewport reads as 0x0.** The field offsets are wrong, or the base register is not the
view struct. `mirv_deadlock view offsets` changes them without a rebuild.

**A guarded engine call faulted.** A vtable slot index is wrong. Every slot in this port started as
a hypothesis, which is why they are runtime data: `mirv_deadlock engine probe 35 55` against a
loaded demo finds the right one.

**Tick rate says ASSUMED.** It is CS2's 1/64 carried over, which is wrong for Deadlock. It paces the
render output, so measure it and set it before trusting a capture's frame timing.

Then instrument. A console reply only shows the instant it was typed; ordering, throttled per-frame
probes and "what did this look like two hundred frames ago" exist only in the log.

```batch
mvm_debug start
```

Reproduce, then read `%APPDATA%\HLAE\debuglogs\mvm_debug_*.log`. The hook logs under
`deadlock.view`, `deadlock.shot`, `deadlock.ui`, `deadlock.panorama` and `deadlock.engineprobe`.

## Tests

```batch
ctest --test-dir build/x64-deadlock-release -C Release --output-on-failure
```

```batch
node --test Automation/tests/deadlock-panorama-build-tests.mjs
```

The second one is worth knowing about. It extracts the raw JavaScript from the `*Js.h` fragments
and runs it against a throwaway Panorama stub. It builds the workspace and renders it three times
in every mode. This catches a failure that a parse check misses. A ReferenceError partway through a
panel's build leaves the panel registered on nothing and half-drawn. In game, that reads as "the UI
is just ugly" rather than as an error.

```batch
node --test Automation/tests/deadlock-mockup-tests.mjs
```

Guards the browser mockup below against drifting from the game. It covers the extraction, the
byte-exact round trip back into the headers, and the two places where it re-implements something C++
owns.

## The browser mockup

[`UiMockupDeadlock/`](../../UiMockupDeadlock/README.md) runs the **real workspace script** in a
browser, so the UI can be looked at, clicked through and edited without launching Deadlock. Double-
click `UiMockupDeadlock\deadlock-mockup.bat`.

It is not a drawing of the UI: `extract-deadlock-ui.mjs` pulls the twelve macros out of the C++ in
the order `DeadlockWorkspaces.cpp` pushes them, the browser executes them unmodified, and
`apply-deadlock-ui.mjs` writes an edit back into the macro it came from. The CS2 equivalent is
[`UiMockup/`](../../UiMockup/README.md) (`cs2-mockup.bat`); the two share no code and run on
different ports.

Two things it does that the stub test cannot. It stands up **Deadlock's own replay bar** from the
game's decompiled layout and stylesheets, because the Regular Timeline *is* `#hud_replay` hosted
rather than a lookalike, so the hosting can be verified without the game. And
`capture-views.mjs` reports **style writes per second in a static frame, which must be zero**: a
style write re-does a panel's geometry and a 3D transform allocates a composited layer against a
vertex-buffer list Panorama indexes with an unsigned short, so a panel restyled every frame ends in
`CVertexBufferDx11 ... EnsureCapacity allocation count overflow`. That counter found a live instance
of exactly that, where the HUD scaler was deriving each panel's transform from the panel's own
measured position, which the transform moves.

To diff it against a running game, dump the real geometry and compare:

```
mirv_deadlock livejs load <abs>/Automation/livejs/deadlock-dump-ui-geometry.js
```

```batch
node UiMockupDeadlock/compare-geometry.mjs <captured console output> --view camera-native
```

## What is verified and what is not

**Proven in game, read out of the boot trace:**

- the DLL loads and every required address resolves;
- the render pump runs;
- the view hook fires, and its main-view gate passes for the real view and rejects everything else;
- the main-thread pump runs from the signature-checked frame-stage dispatcher on build 6774;
- netcon binds, accepts connections, executes received commands, and delivers output back over the
  socket.

**Still open:** with every hook enabled the process survives startup and the first frames, then
exits cleanly some seconds later. With `-mvmNoViewHook -mvmNoPresentHook -mvmNoUi` it stays up
indefinitely, so it is one of the three, and the bisect flags exist to say which. Nothing has yet
been watched on screen, and no demo has been loaded.

**Not yet measured at all:** whether the camera visibly moves, whether the engine demo-interface
slot indices are right, the real tick rate, and whether the panels render as intended. The first
three are runtime-settable precisely because they are unverified. See
`mirv_deadlock view`, `engine probe` and `engine tickinterval`.
