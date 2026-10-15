# Filmmaker: Movie Director controls reference

**The controls below work only while a GOTV or POV demo is playing.** In the main menu and live matches, the director intercepts nothing, so your normal mouse and keys work as usual.

## Help and status HUD

A small panel on the middle-right of the screen shows the live camera state and this control list.

| Action | Control |
| --- | --- |
| Show / hide the HUD | **F8** |
| Show / hide the HUD (console) | `mirv_filmmaker hud [on\|off\|toggle]` |
| Bind it to your own key | `bind "h" "mirv_filmmaker hud toggle"` |

The HUD is display-only. It never blocks clicks, even where it is drawn.

## Camera modes (mouse wheel)

Scrolling cycles through three camera modes and wraps around at the ends:

```
  Scroll Up ▶   First person → Third person → Free cam → (back to First) ...
  ◀ Scroll Down First person → Free cam → Third person → (back to First) ...
```

| Action | Control | Under the hood |
| --- | --- | --- |
| Cycle mode up | **Scroll Up** | first-person / native third-person orbit / free-cam |
| Cycle mode down | **Scroll Down** | same, reversed |

## Playback (in a demo)

| Action | Control | Under the hood |
| --- | --- | --- |
| Pause / resume | **Space** | `demo_pause` / `demo_resume` (reads live pause state) |
| Skip back 5s | **Left arrow**, or the demo bar's **-5s** | `mirv_filmmaker skip back` (current tick − 5s) |
| Skip forward 5s | **Right arrow**, or the demo bar's **+5s** | `mirv_filmmaker skip forward` (current tick + 5s) |

**Ctrl** is reserved for Movie Director shortcuts during demos. Its press and release do not reach CS2's overview-map bind, so tapping Ctrl does not open the map while you watch a demo. Ctrl+Z, Ctrl+A, and Ctrl+Shift+Z still work in the editor. Live matches keep normal Ctrl input.

The demo bar shows inline speed buttons (0.1x to 4x) in place of the native timescale dropdown. To show the native dropdown instead, toggle the buttons off: `mirv_filmmaker speedbar [on|off|toggle]`.

> First person = `spec_mode 2` (in-eye). Third person = the HLAE-owned orbit camera (`ThirdPersonCamera.cpp`, solved in the view-setup hook). CS2's native `thirdperson` and chase cvars are not used, because they fight the demo view pass and jitter. Free cam = HLAE free camera. `spec_mode 4` is CS2's native roaming. Numbers are centralised in `MovieMode.cpp`. Confirm in-game and adjust if needed.

**Third person.** The mouse orbits the camera. The pointer is hidden, as it is while flying the free cam. Both re-centre the cursor every frame, so a visible arrow would jitter in the middle of the shot (G brings it back). **Left and right click** switch to the next and previous player, and the camera stays in third person behind them. The orbit follows whichever player the engine's own spectator camera is watching, read from that camera before any override. In first person, that camera sits in the player's eye. In the chase mode that third person sets, it aims at them. GOTV demos report no observer target at all (observer services read mode 0 / target −1), so this is the only signal that follows a switch.

## Free-cam controls (only when in Free cam mode)

These are HLAE's `mirv_input` camera keys. The director only turns the camera on when you scroll into Free cam. They do nothing in first or third person.

| Action | Key(s) |
| --- | --- |
| Move forward / back | **W / S**  (or Numpad 8 / 2) |
| Strafe left / right | **A / D**  (or Numpad 4 / 6) |
| Move up / down | **R / F**  (or Numpad 9 / 3) |
| Roll left / right | **X / Z**  (or Numpad . / 0) |
| Pitch (look up / down) | **Arrow Up / Down** |
| Yaw (look left / right) | **Arrow Left / Right** |
| FOV in / out | **Numpad 1 / 7**  (or PageDown / PageUp) |
| Reset view + speed | **Home**  (or Numpad 5) |
| Look around | **Mouse**: always-look (point the camera, and it stays). Sensitivity is **independent of cam speed**. |
| Slow / fine movement (hold) | **Shift**: applies a 25% slow factor to all free-cam movement + rotation while held |
| Cam speed up / down | **Shift + Scroll** (0.5x per notch; below 0.5x each notch halves, down to 0.125x) |
| FOV in / out (scroll) | **Ctrl + Scroll** (1 degree per notch) |
| Exit camera control | **Esc** |

> **Shift-scroll changes third-person distance** and free-cam speed. Shift-hold slow movement only affects the free camera. While Config or the Camera Editor is open, each speed or FOV notch shows a short toast under the top scoreboard with the new value.

## Spectator (first / third person)

| Action | Control | Notes |
| --- | --- | --- |
| Next player | **Left click** | CS2's own spectator bind, **first person only**. In third person, clicks are consumed so orbiting cannot switch players |
| Previous player | **Right click** | CS2's own spectator bind, first person only (see above) |
| Orbit current player | **Mouse move in Third person** | HLAE-owned orbit camera locked to the spectated player's eye; works identically while the demo is paused. |
| Third-person distance | **Shift + Scroll** | Clamped to 30 to 200 units. |
| Toggle X-ray | **X** | `spec_show_xray`, **first and third person only**. In free cam, X is camera roll instead. |

Third-person commands:

```text
mirv_filmmaker thirdperson on|off|toggle|state
mirv_filmmaker thirdperson preset front|back|left|right
mirv_filmmaker thirdperson angles <yaw> <pitch>
mirv_filmmaker thirdperson distance <30-200>
mirv_filmmaker thirdperson sens <deg per pixel, 0.005-0.5>   (default 0.05)
```

Presets and the orbit yaw are relative to the spectated player's facing: `back = 0` (camera stays behind them as they turn), `front = 180`, `left = -90`, `right = 90`. The orbit yaw is eased (~0.12 s), so player flicks swing the camera instead of snapping it. HLAE solves the camera from the pawn's render-time eye pose. No engine `cam_*` cvars are touched.

While the camera is detached (third person or free cam), the first-person viewmodel is hidden (`r_drawviewmodel false`). Its `_fp/_fps` particle systems are blocked to `dev/empty`. They anchor to the camera and would otherwise render as giant floating arms and mid-air muzzle flashes. The spectator is also held in chase mode (`spec_mode 3`, re-asserted every 2 s) while detached. In-eye spawns only the `_fp` variants for the spectated player, so without chase there would be no weapon effects on their gun at all. This is what makes the Modern/Povarehok swaps show in third person. Everything restores automatically when the camera returns to first person.

World collision (pull-in at walls) is not implemented yet. The DLL has no engine trace hooked. Adding one (Source 2 `GameTraceManager` AOB) is the known follow-up.

## Native CS2 demo playback UI

These buttons belong to CS2 itself, not this mod. Open them with the console:

```
demoui
```

The director does not intercept clicks, so this panel (and any other in-game UI) is fully clickable. Bind it for convenience, e.g. `bind "p" "demoui"`.

## Keeping the desktop cursor free

CS2 normally switches to relative mouse mode whenever its window becomes active, including after launch or Alt+Tab. For UI-first filming sessions, add `-afxFreeMouse` to the CS2 launch options. This starts in click-to-capture mode. The desktop cursor stays visible after launch and Alt+Tab, and CS2 takes it only after you click the game window. Losing focus re-arms the free cursor.

The mode can also be changed at runtime:

```text
mirv_filmmaker mouse free
mirv_filmmaker mouse click
mirv_filmmaker mouse game
mirv_filmmaker mouse toggle
mirv_filmmaker mouse state
```

`free` keeps the cursor free until `game` is selected. `click` arms the click-to-capture mode without restarting CS2. `game` restores normal CS2 mouse capture and mouse-look. The launch flag is opt-in, so ordinary gameplay launches keep their normal input behavior.

## Console commands

| Command | Purpose |
| --- | --- |
| `mirv_filmmaker hud [on\|off\|toggle]` | Show/hide the help+status HUD |
| `mirv_filmmaker thirdperson ...` | Third-person orbit camera controls and presets |
| `mirv_filmmaker speedbar [on\|off\|toggle]` | Inline demo-bar speed buttons (off = native dropdown) |
| `mirv_filmmaker mouse free\|click\|game\|toggle\|state` | Control free or click-to-capture mouse modes |
| `mirv_filmmaker hud_eval <panorama js>` | Run Panorama JS in the HUD context (debug) |
