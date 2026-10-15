# Live UI editing and game-window screenshots

How to change the Filmmaker Panorama UI **live in the running game** (no rebuild), read panel state, and capture clean screenshots of just the game window.

This is the fast inner loop: tweak a value live, take a screenshot, and iterate. Once a change looks right, bake it into [`AfxHookSource2/Filmmaker/Panorama/FilmmakerGuiJs.h`](../../AfxHookSource2/Filmmaker/Panorama/FilmmakerGuiJs.h) and rebuild so it ships.

## 1. What this is

The Filmmaker UI is Panorama (CS2's HTML/CSS-like UI). The whole UI is injected as one big JavaScript string (`kFilmmakerGuiJs`) baked into the DLL. Changing it normally requires a rebuild, which is slow.

Instead, while CS2 is running you can run arbitrary Panorama JS against the live UI over the netcon console:

```
mirv_filmmaker ui_eval "<panorama js>"
```

This runs the JS in the UI context immediately. You can move panels, change styles, add or remove classes, and read layout sizes. The result appears on screen at once. Nothing persists across a relaunch, so treat it as a scratchpad.

## 2. Prerequisites

1. CS2 running with netcon on port `29010`. The easiest options:
   ```
   Automation\launch\live.bat                 (relaunches CS2 + opens the dashboard)
   Automation\launch\launch-cs2-netcon.ps1    (just CS2 + netcon, no dashboard)
   ```
2. A demo or map loaded far enough that the Watch UI exists. `mirv_filmmaker ui` opens the demos page.

## 3. Sending commands

Any TCP client to `127.0.0.1:29010` works. The repo helper:

```powershell
pwsh Automation\netcon\cs2-netcon.ps1 -Commands "mirv_filmmaker ui", "mirv_filmmaker ui_eval ""$.Msg('hi\n');"""
```

For an interactive loop, use the small inline socket pattern from the `_diag-*` scripts. Connect once, then send many commands. Minimal version:

```powershell
$c = New-Object System.Net.Sockets.TcpClient; $c.Connect('127.0.0.1',29010)
$s = $c.GetStream(); $enc = [Text.Encoding]::ASCII
function Send($cmd){ $b=$enc.GetBytes($cmd+"`n"); $s.Write($b,0,$b.Length); $s.Flush(); Start-Sleep -Milliseconds 800
  $buf=New-Object byte[] 8192; $o=''; while($s.DataAvailable){ $n=$s.Read($buf,0,$buf.Length); $o+=$enc.GetString($buf,0,$n)}; $o }
Send 'mirv_filmmaker ui_eval "$.Msg(`'pong\n`')"'
```

`$.Msg('...\n')` prints back over netcon, so it is how you return values to your script. Search the output for a tag such as `[VERIFY] ...`.

## 4. The 256-byte limit

The netcon console truncates any single command at 256 bytes. Measured: a 251-character command runs, while a 271-character command is cut off mid-string and throws a JS SyntaxError. After the `mirv_filmmaker ui_eval "` prefix and the closing quote, about 230 characters of JS are usable. Longer payloads silently do nothing.

There are two ways to stay under the limit:

- Keep each `ui_eval` short. Use one probe or one tweak per command, and split the work across several commands.
- Put heavy logic in the injected script and call a short entry point. `RunScript` (how the GUI JS is injected) has no length limit, so the real work lives in `FilmmakerGuiJs.h` behind short `$.Filmmaker.*` calls. These calls already exist (see the "automation hooks" block):

  | Call | What it does |
  |------|--------------|
  | `$.Filmmaker.gotoTab('downloaded'\|'yourmatches'\|'tournaments')` | Real sub-tab switch. It toggles `WatchMenu--Hide` and runs the Downloaded takeover. It is needed because the native `mainmenu_watch.NavigateToTab` is a module function outside our JS scope. A synthetic `Activated` on the tab button only highlights the tab. |
  | `$.Filmmaker.report()` | Prints `[VERIFY] dlHide=.. ymHide=.. tiles=.. styledTile=.. scoreboard=.. playerRows=..` |
  | `$.Filmmaker.tagFirst(n)` / `.stab()` | Tag or select for list-stability checks; prints `[STAB] ...` |
  | `$.Filmmaker.btnReport()` | `[BTN] ref=.. add=.. sameRow=..` for the navbar buttons |
  | `$.Filmmaker.render()` / `.show()` | Re-render, or open the demos page |

For a new long probe, add a method to `FilmmakerGuiJs.h` and rebuild once, instead of working around the 256-byte limit each time.

## 5. Reading and changing panels

The UI context panel sits in the HUD/menu tree, not inside the Watch page. Walk up to the root first, then use `FindChildTraverse` to go down:

```js
var r=$.GetContextPanel(); while(r.GetParent()) r=r.GetParent();
var w=r.FindChildTraverse('JsWatchContent');   // the Watch page
```

Useful APIs:

- `panel.FindChildTraverse('id')`: find a descendant by id.
- `panel.BHasClass('x')`, `AddClass`, `RemoveClass`, `SetHasClass('x', cond)`.
- `panel.style.height='40px'` sets a style. See the gotchas below.
- `panel.actuallayoutwidth` / `actuallayoutheight`: the laid-out size in px. Use these to check whether something is 0px or off-screen.
- `panel.visible`, `panel.checked` (RadioButton), `panel.GetChildCount()`, `GetChild(i)`.

Example: read the navbar button row height live:

```
mirv_filmmaker ui_eval "var r=$.GetContextPanel();while(r.GetParent())r=r.GetParent();var b=r.FindChildTraverse('FmNavBtns');$.Msg('[h] '+b.actuallayoutheight+'\n');"
```

Example: nudge a style live, then take a screenshot and decide what to change:

```
mirv_filmmaker ui_eval "var r=$.GetContextPanel();while(r.GetParent())r=r.GetParent();r.FindChildTraverse('FmRefreshBtn').style.marginLeft='5px';"
```

## 6. The workflow: tweak, then bake

1. Open the UI: `mirv_filmmaker ui`.
2. Probe or change live with short `ui_eval` commands until it looks right.
3. Take a screenshot to confirm (section 7).
4. Bake the final values into `FilmmakerGuiJs.h` (the same JS, now in the injected source).
5. Run `build.bat`, then relaunch CS2 (`live.bat`) to load the new DLL.
6. Verify on the fresh DLL.

Live edits never persist. Step 4 is what makes the change real.

## 7. Screenshots: capture just the game window

A full-monitor grab shrinks the game into a small part of a large image, so UI detail (tile colors, icons, text) is unreadable. Use the window capture instead:

```powershell
# Just the game's render area (no title bar / borders / desktop) -- best for reading UI:
pwsh Automation\capture\capture-game-window.ps1 -Out shot.png

# The whole OS window incl. title bar:
pwsh Automation\capture\capture-game-window.ps1 -Out shot.png -Mode window
```

- `-Mode client` (default) crops to CS2's client area at full resolution (for example 1600x1200), which is tight and crisp.
- It restores the window automatically if it is minimized, and it is DPI-aware.
- The fallback target is the `cs2` process. Override it with `-Process <name>`.

Use [`capture-main-monitor.ps1`](../capture/capture-main-monitor.ps1) only when you also need the desktop, for example the netcon console window beside the game. For a look at the UI, always prefer `capture-game-window.ps1`.

## 8. Gotchas

- Use single quotes only inside the JS. The payload is wrapped in double quotes for the console, so a `"` inside ends the argument early and truncates the script. Use `'...'` everywhere in the JS.
- Never assign `style.x = ''`. An empty style string throws and aborts the script. To clear a color, use `rgba(0,0,0,0)`.
- `height: 100%` can collapse to 0. If a parent sizes to fit its children, a child that asks for `100%` of it resolves to 0, because the two depend on each other. Use an explicit px height. This was the cause of the "navbar buttons missing" bug.
- Win/loss tile color depends on the logged-in account. A tile is colored only when your currently logged-in Steam account is a player in that demo. Demos you are not in (for example, ones recorded on a different account) stay grey. This is expected.
- The `$` context is not the Watch page. Always walk to the root before `FindChildTraverse`.

## 9. Related files

- [`cs2-netcon.ps1`](../netcon/cs2-netcon.ps1): send commands and print the output.
- [`cs2-live.ps1`](../launch/cs2-live.ps1) / [`live.bat`](../launch/live.bat): browser dashboard and launcher.
- [`capture-game-window.ps1`](../capture/capture-game-window.ps1): game-window screenshot (this guide, section 7).
- [`verify-watch-ui.ps1`](../verify/verify-watch-ui.ps1): a full example that drives `$.Filmmaker.*`, asserts, and screenshots each step.
- [`FilmmakerGuiJs.h`](../../AfxHookSource2/Filmmaker/Panorama/FilmmakerGuiJs.h): where live edits get baked in.
