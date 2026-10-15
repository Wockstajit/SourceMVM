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

The director uses CS2's own three demo cameras. Scrolling steps through them and wraps around at the ends, the same way CS2's camera button on the demo bar does:

```
  Scroll Up ▶   First person → Chase cam → Free cam → (back to First) ...
  ◀ Scroll Down First person → Free cam → Chase cam → (back to First) ...
```

| Action | Control | Under the hood |
| --- | --- | --- |
| Next camera | **Scroll Up** | `spec_mode` (CS2's own step) |
| Previous camera | **Scroll Down** | `spec_mode` twice |
| Look around (Chase, Free cam) | **Hold a mouse button and drag** | CS2's own click-and-drag look. Let go and the cursor is yours again |

Scrolling always steps the camera over the game, in every mode. Inside the Camera Editor or Config, a notch over a panel scrolls that panel instead; a notch during a look-around drag always belongs to the camera. The director takes the wheel away from CS2 during demos, so CS2's own wheel binds (which step the spectated player and drop free cam to first person) never fire.

The HUD's mode name follows the camera CS2 is actually showing, including changes made with CS2's own camera button or player stepping.

### Free-cam controls (CS2's free cam)

| Action | Control | Under the hood |
| --- | --- | --- |
| Move | **W / A / S / D** while looking | CS2's own free cam |
| Fly speed up / down | **Shift + Scroll** | `sv_noclipspeed`, the same speed ladder as before (0.5x per notch) |
| FOV in / out | **Ctrl + Scroll** | `mirv_fov`, 1 degree per notch. `mirv_fov default` goes back to the game's FOV |
| Roll | **Hold X / Z** | Added on top of CS2's free cam. Cleared when the camera changes |

Shift and Ctrl + Scroll also work in the middle of a look-around drag. While Config or the Camera Editor is open, each speed or FOV notch shows a short toast over the preview with the new value.

## Playback (in a demo)

| Action | Control | Under the hood |
| --- | --- | --- |
| Pause / resume | **Space** | `demo_pause` / `demo_resume` (reads live pause state) |
| Skip back 5s | **Left arrow**, or the demo bar's **-5s** | `mirv_filmmaker skip back` (current tick − 5s) |
| Skip forward 5s | **Right arrow**, or the demo bar's **+5s** | `mirv_filmmaker skip forward` (current tick + 5s) |

**Ctrl** is reserved for Movie Director shortcuts during demos, and CS2's overview map is kept hidden, so tapping Ctrl never opens it while you watch a demo. Ctrl+Z, Ctrl+A, and Ctrl+Shift+Z still work in the editor. Live matches keep normal Ctrl input.

The demo bar shows inline speed buttons (0.1x to 4x) in place of the native timescale dropdown. To show the native dropdown instead, toggle the buttons off: `mirv_filmmaker speedbar [on|off|toggle]`.

## Spectator (first person / chase)

| Action | Control | Notes |
| --- | --- | --- |
| Next player | **Left click** | CS2's own spectator bind |
| Previous player | **Right click** | CS2's own spectator bind |
| Toggle X-ray | **X** | `spec_show_xray`, first person and chase cam. In free cam, X / Z roll the camera instead |

## The mouse in the Camera Editor and Config

There is no mouse toggle. Where you click decides it:

- Click the game **preview**: the mouse belongs to the camera (CS2's look-around drag, or the editor's own camera).
- Click any **panel**: the mouse belongs to the UI.
- Popups that sit over the preview (Editor Settings, a keyframe's shake menu) keep their clicks.

Outside a workspace there is nothing to switch: CS2 gives the cursor back by itself after a look-around drag, so the demo bar is always clickable.

## Camera Editor paths

The Camera Editor's keyframe cameras still use SOURCE:MVM's own camera, because a path has to set every frame's pose. On the camera timeline, **click a diamond** to go to that keyframe's tick and camera, and **right-click** it for its shake menu. Both work on every inspector tab. While a path keyframe camera owns the view, its own keys keep working: W/A/S/D/R/F to move, Shift + Scroll for speed, Ctrl + Scroll for FOV, X / Z to roll, Shift held for slow movement.

When our own camera is detached from the spectated eye, the first-person viewmodel can be hidden with `mirv_filmmaker fphide on` (default off). Its `_fp/_fps` particle systems anchor to the camera and would otherwise float in mid-air.

The spectator overview map (CS2's **[CTRL] Close / [Z] / [X] / [SPACE] Clear Drawing** panel) never shows during a demo.

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
| `mirv_filmmaker speedbar [on\|off\|toggle]` | Inline demo-bar speed buttons (off = native dropdown) |
| `mirv_filmmaker mouse free\|click\|game\|toggle\|state` | Control free or click-to-capture mouse modes |
| `mirv_filmmaker hud_eval <panorama js>` | Run Panorama JS in the HUD context (debug) |
