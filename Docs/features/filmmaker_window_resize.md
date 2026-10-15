# Free window resizing + live aspect-ratio readout

## Resize dispatch

Windows resize messages queue the latest SDL commit. The main-thread frame pump applies it only
after message dispatch and the modal drag loop finish. It skips the commit if the swapchain already
matches. Startup size notifications set the initial dimensions without a second resize. The
post-drag snap guard disarms before restoring a maximized window, because restore sends another
resize message synchronously. `window.resize` logs queue depth and commit duration with `mvm_debug`
active. A queued request does not prove success. The `FOLLOWED` entry verifies the swapchain size.

Every modal exit arms dismissal, including cancelling a resize or returning to the initial size.
The native timer and the main-frame deadline both enforce expiry, and losing focus hides the card.
Mouse-button releases are recorded before foreign-UI filtering. Focus loss clears held buttons, and
cursor-unclip requests always reach Windows.

## What it does

Drag the CS2 window border to any size. While you drag, a card in the middle of the window shows the
pixel size and the closest named aspect ratio. The card updates during the drag and fades out shortly
after you let go.

**Hold Shift while dragging to snap.** With Shift held, the window snaps to a true size for its ratio.
The shape locks to the nearest named ratio. The size then lands either on a standard resolution of
that ratio (the card names it, `SNAP 1080p`) or on the nearest whole multiple of the ratio (1504×846,
never 1500×844). Without Shift, the card shows `SHIFT to snap`.

The card also rides along when you drag the window by its **title bar**, so the numbers stay in the
middle of the window instead of being left behind on the desktop. Moving never *summons* the card.
Only a resize does.

```
mirv_filmmaker window status              - SDL binding, window size, aspect, engine agreement
mirv_filmmaker window resizable on|off    - default ON, windowed only
mirv_filmmaker window size <w> <h>        - exact client size
mirv_filmmaker window aspect <r>          - keep the width, set the height (2.39, 16:9, 2.35:1, ...)
mirv_filmmaker window ratios              - list every ratio the readout can name
mirv_filmmaker window snap both|ratio|resolution|off - what holding SHIFT snaps to
mirv_filmmaker window overlay on|off|test - the readout card
mirv_filmmaker window commit sdl|auto|videomode|off - how the finished size reaches the engine
mirv_filmmaker window probe 0|1           - per-message logging into the mvm_debug log
```

## Files

| File | Role |
|---|---|
| `AfxHookSource2/Filmmaker/Window/AspectRatioMath.h` | Header-only ratio table + closest-match maths. No Windows, no engine. |
| `AfxHookSource2/Filmmaker/Window/EngineVideoMode.{h,cpp}` | Engine-sanctioned resolution change (the settings-menu path). |
| `AfxHookSource2/Filmmaker/Window/Sdl3Window.{h,cpp}` | By-name binding to the SDL3 CS2 already loads. |
| `AfxHookSource2/Filmmaker/Window/WindowSnapMath.h` | Header-only Shift-snap maths + the standard-resolution table. |
| `AfxHookSource2/Filmmaker/Window/WindowSizeOverlay.{h,cpp}` | The layered Win32 readout card. Knows nothing about CS2. |
| `AfxHookSource2/Filmmaker/Window/WindowResize.{h,cpp}` | The feature: message tap, style application, follow-up check, console surface. |
| `Automation/tests/aspect-ratio-math-tests.cpp` | `AspectRatioMathTests`: table integrity and match behaviour. |
| `Automation/tests/window-snap-math-tests.cpp` | `WindowSnapMathTests`: snap behaviour and standard-resolution table stability. |

Two existing files hook into this feature. `WindowInputHooks.cpp` forwards every window message,
and `Filmmaker.cpp` calls the main-thread pump.

## The aspect table

One entry per **distinct** ratio, so "closest" is never ambiguous. Matching happens in log space, so
being 5% too wide and 5% too narrow count as the same distance. A raw ratio difference would bias
every match toward the wide end of the table.

Three naming decisions look like bugs unless you know them:

- **1.33:1 is not the Academy ratio.** 1.33:1 is silent/full-frame and is numerically identical to
  4:3, so it lives inside that entry's name. The Academy ratio is **1.375:1** and has its own row.
- **Monitors sold as "21:9" are not 21/9.** 2560×1080 is 2.370:1 and 3440×1440 is 2.389:1, so a
  real ultrawide window reports as **2.39:1 anamorphic scope**. That is the correct label for a movie
  tool, and the entry's name says so. The literal 21:9 (2.3333) row matches a window dragged to
  exactly that shape.
- **"1.66:1" is the rounded trade name for 5:3.** The table stores the exact ratio, so a window at
  exactly 1.660:1 reports as *close to* 1.66:1 and never as exact.

The readout has three shapes, from the strongest match to the weakest:

```
16:9 · Widescreen HD                 exact table hit
7:5 · closest 1.43:1 (-2.1%)         exact simple ratio that has no name
≈ 16:9 · Widescreen HD (+0.4%)       nothing exact
```

## Why the readout is a Win32 window and not a Panorama panel

Dragging a window border puts Windows into a **modal resize loop** inside `DefWindowProc`. The
game's frame loop is parked inside `DispatchMessage` for the whole drag. Nothing renders, Panorama
never runs, and any in-game HUD is frozen on the last frame drawn before the drag started. A
readout that changes as you drag is precisely the thing an in-game panel cannot do.

A separate layered window owned by the same thread still receives and paints its messages from
that modal loop, so it updates live. `WindowSizeOverlay_Show` repaints synchronously
(`RDW_UPDATENOW`) for the same reason: there is no pump of ours to return to.

The auto-hide uses `SetTimer` **on the overlay window** rather than the main pump, so the card
still disappears if the drag ends in a state where the game loop has not resumed.

### Keeping the card centred on the window

The card is positioned from an explicit screen-space centre that the caller passes in. It ignores the
anchor window's live rect. During a drag the window has not moved or resized yet: `WM_SIZING`
carries the rect Windows is *about* to apply, and dragging a left or top edge moves the origin as
well as the size. Centering on the live rect leaves the card a step behind the pointer and visibly
off-centre.

The centre therefore comes from the proposed rect, converted to client space with the live frame
insets (`GetFrameInsets`, measured per side from `GetWindowRect` / `ClientToScreen`, which also
handles the case where the two axes have different border thicknesses).

`WM_MOVING` / `WM_MOVE` do the same for a title-bar drag, through `WindowSizeOverlay_MoveTo`. That
function repositions **only a card that is already visible** and leaves its text and pending hide
timer alone. That split is deliberate. A card left from the resize you just finished would stay on the desktop
while the window slides out from under it. A move does not change the size, so the live client size is used. Only
the origin comes from the proposed rect.

`WM_SIZE` inside the modal loop re-shows the card on the same rect, and it has to carry the note
(`SNAP 1080p` / `SHIFT to snap`) that the `WM_SIZING` immediately before it produced. Hence
`s_lastNote`. Without it the note blinks out and back on every step of a drag.

### Shift to snap

Constraining a resize on Windows means **editing the `RECT` that `WM_SIZING` points at, in place**.
This module's message tap is otherwise observe-only, so that rect is the single thing it writes. The
message still goes on to SDL and `DefWindowProc` afterwards, so SDL's minimum-size clamp and its
own bookkeeping still apply on top.

The size is written back with the edges the user is **not** dragging pinned
(`ApplyClientSizeToWindowRect`). Dragging the left edge therefore grows leftwards instead of sliding the
whole window right. The dragged axis leads and the other is derived, so the edge under the cursor
never fights the mouse: top/bottom edges keep the height, everything else keeps the width.

Snap order and thresholds live in `WindowSnapMath.h`. There are three passes:

1. **Ratio**: the shape becomes exactly the nearest named ratio.
2. **Resolution**: a standard size of that **same** named ratio, if one is close. Same-ratio-only is
   essential. A 2.39:1 drag must never be pulled onto a 16:9 monitor resolution just because the
   pixel counts happen to be close. The pull scales with the size being dragged (`32px + width/24`,
   so 112px at 1080p and 192px at 4K).
3. **Grid**: otherwise, the nearest **whole multiple of the ratio**. `AspectRatio_ExactStep` finds the
   smallest even integer pair that reproduces the ratio exactly (16:9 gives 32×18, 4:3 gives 8×6, and
   1.85:1 gives 74×40, because 1.85 is exactly 37/20). The dragged axis quantises onto that pair.
   Ratios with no small whole pair (1.43:1 and 2.39:1 are 143/100 and 239/100) quantise the dragged
   axis to a multiple of 16 and derive the other axis. That is as close as integers get.

Pass 3 gives the truest resolution for a ratio when no named size is close.
1500×844 is 1.7773 and 1504×846 is exactly 1.7778. Only one of those is a size you would want on a
render.

The derived axis rounds to the **nearest** even number. For example, 3008 at 2.39:1 gives 1258.58.
1258 is 0.58px from the ratio and 1260 is 1.42px away, so the derived height is 1258.

## Why SDL and not `SetWindowLongPtr` / `SetWindowPos`

`Docs/features/filmmaker_render_passes.md` already records the rejected lever: a raw `SetWindowPos`
"succeeds, then CS2/SDL restores the configured size". That result shows that **SDL owns the window**
and overwrites bookkeeping it did not issue itself. It does not show that the window cannot be
resized.

CS2 ships `SDL3.dll` with a normal export table, and `tier0.dll` is the module that creates and
drives the window (it imports `SDL_CreateWindowWithProperties`, `SDL_SetWindowSize`,
`SDL_SetWindowBordered`, `SDL_SetWindowPosition`, `SDL_SyncWindow`). `inputsystem.dll` carries
`SDL_EVENT_WINDOW_RESIZED` and `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` by name, so the engine at
least *knows about* the resize events SDL emits.

Going through `SDL_SetWindowResizable` / `SDL_SetWindowSize` therefore does two things a raw Win32
call cannot. It updates SDL's own record of the window, and it makes SDL emit those events.

**Every function used resolves by export name, so none of this is version-coupled.** There is no
pattern to re-find on CS2 update day. That is unusual for this project, and it is the main reason this
route was chosen.

## The engine follows an SDL-issued resize

Measured over netcon on CS2 build 2000879:

```
mirv_filmmaker window commit sdl
mirv_filmmaker window size 1504 846
  -> Successfully created dx11 swap chain [FlipDiscard | AllowTear]
  -> Scratch RT Allocations: RT 1504x846 ... 65 RTs, Total = 255027656 Bytes
  -> window status: client size 1504x846, swapchain 1504x846 (matches the window)
```

This is a complete resolution change: new swapchain, every scene render target rebuilt at the new
size, including the half- and quarter-res chains (752×423, 376×211). No video-mode change, no
settings menu. `SDL_SetWindowSize` + `SDL_SyncWindow` is the whole lever.

### The two traps this hides

**`SDL_SetWindowSize` to a size SDL already holds does nothing.** No swapchain, no rebuild, no event.
This was measured by asking for 1280×720 twice in a row. The first call produced the full rebuild
dump, and the second produced silence. This is the normal case after a drag. Windows has already
applied the size, and SDL updated its own record from `WM_SIZE`. Re-asserting the size you ended on
is therefore a no-op, and the engine keeps rendering into stale targets. **Those stale targets are
the black patches in the frame.** `ReassertThroughSdl` steps 2px off the target first whenever SDL
already holds it, so SDL has a change to act on.

**The video-mode commit can undo the resize.** See the next section.

### Why a resize can look ignored

A separate fault produces the same symptom. Polling `RenderResolution_Sample()` every frame after
a resize calls `GetBuffer` and holds a backbuffer reference, and `ResizeBuffers` fails outright
while one is outstanding. A failed swapchain resize produces black patches that look exactly like
those from an engine that did not follow.

`Docs/features/filmmaker_render_passes.md`, "Size ownership", says CS2 sizes its scene render targets
from the video mode at startup. That sets their *initial* size. It is not the only way they can be
resized, and the measurement above is the counter-example. The contract's four rejected
levers (client-rect spoofing, raw `SetWindowPos`, enlarging the swapchain, scaling engine2 render-
window metadata) all remain rejected. None of them is `SDL_SetWindowSize`.

### The video-mode route: measured broken

`commit videomode` wrote `setting.defaultres` / `setting.defaultresheight` with
`GameInterfaceAPI.SetSettingString` and then dispatched `CSGOApplyVideoSettings`, the same event
CS2's own settings code uses (`scripts/settingsmenu_shared.js`). It does not
work, and CS2 says so on the console:

```
Failed to find ui preference 'setting.defaultres'
Failed to find ui preference 'setting.defaultresheight'
```

`setting.defaultres` is a real key, but it lives in the **video config**
(`<Steam>/userdata/<id>/730/local/cfg/cs2_video.txt`), and `SetSettingString` writes the separate
**ui preference** store. It is not a convar either: `find defaultres`, `find aspectratiomode` and
`find videomode` all return `no results`. The native resolution control is a
`CSGOSettingsEnumDropDown` with no `convar` and no setting key (`layout/settings/settings_video.xml`).
Its value lives in native pending state that Panorama cannot address. It could not offer an
arbitrary size like 1504×846 anyway. It only lists real display modes.

**Dispatching the event after a failed write is harmful.** `CSGOApplyVideoSettings` applies the
settings the engine already has. That rebuilds every render target at the *stored* resolution and
drags the window back to it. Symptom: you drag the window, let go, and it snaps back to where it
started. 
The dispatch is **guarded**. The values are read back in the same script, and the event only
fires if they actually took. On this build they never do. `videomode` is not the
default, warns when selected, and falls through to the SDL re-assert via the verify ladder. Should a
future CS2 build expose the key, it starts working on its own.

CS2 has **no** `mat_setvideomode`. That command belongs to Source 1, and the string does not exist in
any CS2 binary.

`commit sdl` is the default. `auto` waits to see whether the engine followed before re-asserting.
`videomode` and `off` are diagnostics. Note that the engine's own resize produces another `WM_SIZE`,
so committed sizes and a cooldown window are tracked. Without that, the commit and the resulting
message drive each other in a loop.

### Verifying it

```
mvm_debug start
mirv_filmmaker window probe 1
(drag the window)
mirv_filmmaker window status
```

`window.resize` carries `FOLLOWED swapchain=WxH …` or `NOT FOLLOWED …`, and `window.videomode`
records each apply. The check reads the swapchain **description** only, never `GetBuffer`. The next
section explains why that distinction matters.

### The polling hazard

Polling `RenderResolution_Sample()` every frame for two seconds after each resize is a hazard. That
function calls `IDXGISwapChain::GetBuffer`, which **takes a reference on the backbuffer**, and
`ResizeBuffers` fails outright while any backbuffer reference is outstanding. Polling it from the
main thread while the engine resizes its swapchain can make the engine's own resize fail, which
adds a second source of black frames.

`RenderResolution_SwapChainSize` is the polling read. It reads the description only and takes no
reference. The verify loop uses it, every tenth frame. `RenderResolution_Sample` keeps its GetBuffer
call and is documented as one-shot only.

### If the video-mode commit turns out not to work

The unexplored lever is **tier0's own window layer**. It is recorded here so it is not lost. These are
real named exports in `tier0.dll` (verified with `dumpbin /exports`):

| Export | Signature | How it was established |
|---|---|---|
| `Plat_OsSpecificHandleToPlatWindow` | `void* (void* osHandle)` | Disassembly: tests `rcx`, walks a list, returns a pointer. |
| `Plat_GetWindowClientSize` | `void (void* platWindow, int* outW, int* outH)` | Disassembly: null-window path writes `0` through `rdx` and `r8`. |
| `Plat_SetWindowSize` | `void (void* platWindow, int w, int h)` (**assumed**) | Export is a dispatch thunk through a function pointer, so the argument list cannot be read from its bytes. |
| `Plat_ReconfigureWindow` | unknown, 4+ arguments | Large stack frame with four spilled integer arguments. Named the way the video-settings apply path would be. |

`Plat_SetWindowSize` is very likely just a wrapper around `SDL_SetWindowSize` and so adds nothing.
**`Plat_ReconfigureWindow` is the interesting one.** Its signature is not known, and it was
deliberately **not** called blind.

Do **not** reach for the rejected levers. The render contract lists four measured failures
(client-rect spoofing, raw `SetWindowPos`, enlarging the swapchain, scaling engine2 render-window
metadata), and none became a working resolution change.

## Move and resize detection

**The card appears only for a resize.** `WM_ENTERSIZEMOVE` fires for a title-bar *move* as well as
an edge *resize*, so entering the modal loop shows nothing. Only `WM_SIZING` (which is never sent for a move) or a `WM_SIZE` that
actually changes the size promotes the session to a resize. A drag that returns the window to its
starting size is also treated as "nothing happened".

**The window style is re-applied for the whole session.** The style is checked every 500 ms, never
during a modal loop, and re-applied however many times it takes.
The count appears in `window status`. This matters more with the video-mode commit, because the
engine's own mode change is itself likely to reset the style.

## Fullscreen: never size an SDL window that is fullscreen

Measured 2026-10-13 (CS2 with the RenoDX DLSS add-on loaded): `SDL_SetWindowSize` +
`SDL_SyncWindow` on a **fullscreen** window hung the game ("Not Responding", no dump). Stacks of
both threads showed the main thread blocked inside that SDL call, having pumped the engine's
resize handler into `rendersystemdx11`, then `dxgi`, and the render thread inside
`IDXGISwapChain::SetFullscreenState` waiting on a window message. The same Custom Resolution Apply
also silently did nothing while fullscreen, because a fullscreen window has no size for SDL to set.

- `ReassertThroughSdl` refuses to touch SDL while `SDL_WINDOW_FULLSCREEN` is set. The engine's own
  fullscreen transition arrives as an "external" resize, so this is the one place that covers it.
- `window size` from fullscreen first asks the engine to go Windowed the way Settings > Video does
  (`EngineVideoMode_LeaveFullscreen`: select `displaymode0` in the native `DisplayModeEnum`,
  dispatch the `CSGODisplayModeSelectionChanged` and `CSGOApplyVideoSettings` **events**), waits for
  SDL to drop the fullscreen flag, lets the engine settle for 30 frames, then applies the size. It
  gives up with a message after 8 s (the Video page was never opened, so the control is missing).
- **The native handlers are Panorama events, not JS functions.** `CSGOResolutionSelectionChanged`,
  `CSGOAspectRatioSelectionChanged` and `CSGODisplayModeSelectionChanged` are `undefined` as globals
  in this context. Calling them directly throws a ReferenceError that a `try/catch` hides. Use
  `$.DispatchEvent('<name>')`. `VideoSettingsResolutionJs.h` replaces the dropdown's
  `oninputsubmit`, so it has to re-issue the native event itself.

The RenoDX DLSS add-on is **not** a cause of plain resizes. With it loaded, five `window size`
commands and a mouse drag in windowed mode all completed with the engine following (ReShade logs
`Destroyed runtime`, then `Recreated runtime`, each time).
## Interaction with the render pipeline

This module never resizes a render target and never spoofs a client rect. It asks the engine to
change video mode, which is a normal resolution change from the render pipeline's point of view.
The render contract's rule stands: a capture is whatever the engine renders, and the Render
workspace's "Game res" picks the new size up like any other resolution change.

## Known gaps

- **Not persisted.** `resizable`, `overlay`, `commit` and `probe` reset to their defaults each
  launch. `FilmmakerSettingsStore` is where they would go.
- **Config has exact size controls.** The Camera tab's Window Size card mirrors the live client
  width/height, accepts typed values down to 640x360, and applies them through the same
  `mirv_filmmaker window size` path documented above.
- **Config has a draft aspect lock.** Enabling Lock Aspect captures the current Width:Height ratio.
  Committing either field derives the other on the nearest even pixel grid before Apply. This
  lock applies only to typed Config edits. Free edge-dragging is not locked. Shift-drag still snaps to the nearest
  named ratio, and a future edge-drag latch could build on `SDL_SetWindowAspectRatio`.
- **The overlay is a topmost layered window.** It should sit correctly over a windowed game. It
  will not appear over exclusive fullscreen, which is fine because resizing is windowed-only.
