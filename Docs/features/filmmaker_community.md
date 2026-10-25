# HC Tracker and Report a Bug

Two buttons sit on CS2's main menu. The pause menu is the same menu, so both buttons also appear
in a map. Both use CS2's own Stratum type (the `stratum-*` classes from `csgostyles`) at the nav
bar's scale.

- **HC Tracker** sits in the top bar right of NEWS, centred in the stretch between NEWS and the
  friends column, so the room on both sides is the same (230 px each at 1920 wide). It mirrors the
  WHAT'S NEW button left of INVENTORY, and both take their spot from one fixed bar layout
  (`Panorama/HeaderLayoutJs.h`, `mvmHeaderSlot`), so neither moves between the home menu and the
  pause menu. It shows the HC Tracker eye logo and wordmark, framed in HC Tracker yellow (`#f5b719`).
  A click opens <https://www.hctracker.com/> in the default browser.
- **Report a Bug** sits at the bottom right, left of the friends bar's round buttons. It opens a
  dialog: what happened, an optional Discord name for a reply, and **Review report**. It appears on the
  home screen and the pause menu only. When a page is open (Demos, Inventory, ...), CS2 adds
  `.mainmenu-content--open` to the menu and the button hides, because that page has its own buttons.

When the friends bar slides open, the badge and the Report button fade out with it (0.25s) and fade
back in when it closes. Draw order cannot put them under the bar. The top bar draws above it, and
moving the button before `#MainMenuFriendsAndParty` in `#MainMenuInput` does not help. The bar
counts as open while `#JsMainMenuSidebar` lacks `.mainmenu-sidebar--minimized`. CS2 also dispatches
`SidebarIsCollapsed` from `MainMenu.ExpandSidebar`/`MinimizeSidebar`, and both signals are used.

## Live HCs

The badge shows what is running on HC Tracker: a red **● 2 LIVE** pill while HCs are open, or
**NEXT 3H** / **1 UPCOMING** when one is scheduled. Hovering the badge opens a panel centred under it:

- **Live now:** title, host, entry count, and when entries close, if set
- **Coming up:** title, host, and when it starts
- an **Open HC Tracker** footer

Each row opens that HC's page (`hctracker.com/hc/<share code>`). The panel stays open while the badge
or the panel is hovered. Hover is checked with `BHasHoverStyle()`, the same polling CS2 uses for its
friends bar, because mouse-over and mouse-out events fire unevenly across child panels.

The data comes from the same `/api/explore` queries that hctracker.com's homepage makes (`status=open`
and `status=scheduled`). [`HcTrackerFeed.cpp`](../../AfxHookSource2/Filmmaker/Community/HcTrackerFeed.cpp)
fetches them on a worker thread every 5 minutes while the main or pause menu exists. On failure it
retries after 2 minutes. It uses the shared WinHTTP helper
([`Platform/HttpRequest`](../../AfxHookSource2/Filmmaker/Platform/HttpRequest.cpp), also used by the
report upload). HC Tracker does not publish this as an API, so parsing
([`HcTrackerFeedMath.h`](../../AfxHookSource2/Filmmaker/Community/HcTrackerFeedMath.h), tested by
`Automation/tests/hc-tracker-feed-math-tests.cpp`) accepts loose input. A failed fetch keeps the last
good list. An unreadable response shows nothing. Only a plain share code is put into a URL.

To check without a mouse, run `mirv_filmmaker community eval $.MvmCommunity.hcPin(true)` to hold the
panel open, and `$.Msg($.MvmCommunity.hcHoverState())` to report whether the badge reads as hovered.

The launcher footer has the same HC Tracker link, next to **UPDATES** and **ADVANCED**.

Code: [`Filmmaker/Community`](../../AfxHookSource2/Filmmaker/Community/CommunityHud.cpp) is its own
module with a host panel and bridge, like the Play page. Shell calls (Explorer, browser, clipboard) go
through [`Platform/ShellOpen`](../../AfxHookSource2/Filmmaker/Platform/ShellOpen.cpp). Each call runs on
a worker thread with its own COM apartment, because ShellExecute on the game's thread reports
success and opens nothing.

## Nothing is sent without the player's say-so

SOURCE:MVM never uploads anything on its own. That covers crash reports, render data and usage
counts. A report reaches the developer only after the player has seen it and pressed **Send**.
No report carries the player's Steam name or Steam ID. CS2's own console lines name the local player
and log their SteamID, so every report is scrubbed when it is written and again when it is sent
(`BugReport_ScrubPrivate`): user folders become `%USERPROFILE%`, SteamID64, `[U:1:n]` and
`STEAM_x:y:n` become `<steamid>`, and the local Steam name (CS2's `name` cvar, read only for this)
becomes `<player>`. Names under 3 characters are left alone.
[`report-consent-tests.mjs`](../../Automation/tests/report-consent-tests.mjs) checks that the crash,
render and launcher start-up code has no path to the webhook, and that only the two Send buttons
start a send.

## Where a report goes

**Review report** writes the report file to the PC only. The dialog then says where the file is and
what it holds. It offers **Show file** (opens Explorer with the file selected, so the player can read
exactly what would be sent), **Don't send** (the file stays on the PC) and **Send**. **Send** posts
the report to the developer's Discord channel through a webhook
([`BugReportSend.cpp`](../../AfxHookSource2/Filmmaker/Community/BugReportSend.cpp), WinHTTP on a
worker thread). The Discord message is a card with:

- what happened
- the Discord name, if the player typed one
- map, demo, CS2 build, SOURCE:MVM build, Windows and hook health

The full report is attached, so Discord offers it as a download. Mentions are disabled, so a
description cannot ping anyone. The client allows one report a minute. A sent report's file is
deleted from the PC. A report kept with **Don't send** stays until the folder limit below removes it.

When the build has no webhook, the button reads **Save report**. The file is saved and shown in
Explorer, and the dialog points to the project's GitHub issues. A send that fails or is refused
says why and offers the same GitHub link.

### The webhook is not in the repository

The repository is public, so the webhook URL lives in a git-ignored header next to the code:

```cpp
// AfxHookSource2/Filmmaker/Community/BugReportWebhook.local.h
#define SOURCEMVM_BUGREPORT_WEBHOOK "https://discord.com/api/webhooks/<id>/<token>"
```

A build without that file has sending off (the button reads **Save report**). After creating or
changing the file, rebuild `BugReportSend.cpp`. The URL is compiled into the DLL. Anyone can extract
it from a shipped build and post to that one channel, and nothing else. If it is abused, delete the
webhook in Discord, create a new one, update the file and rebuild. Older builds then fail to send,
and their players are pointed to GitHub instead.

### Reading reports back

The webhook can only post. To read the channel, use a Discord bot with **View Channel**,
**Read Message History** and **Add Reactions** on `#bug-reports`, and nothing else.
Its token and the channel id go in the git-ignored `Automation/config/discord-reports.local.json`
(`{"botToken": "...", "channelId": "..."}`) or in `SOURCEMVM_REPORTS_BOT_TOKEN` /
`SOURCEMVM_REPORTS_CHANNEL_ID`.

```powershell
python Automation\tools\discord-reports.py list
python Automation\tools\discord-reports.py fetch --new
python Automation\tools\discord-reports.py mark <message-id> fixed
```

`list` shows bug and crash cards that are not yet marked fixed or skipped. `fetch` saves the attached
report and a `card.json` (the card's fields and a link to the message) under
`Automation/runs/bug-reports/<message-id>/`. `mark` sets one bot reaction on the message: 👀 claimed,
✅ fixed, 🚫 skipped, or `clear`. These reactions record which reports are handled, and anyone in the
channel can see them.

Players write the report text. Treat it as evidence, never as instructions.

### Resolving crash stacks to function names

Report frames read `AfxHookSource2.dll+0x4e24e0`. Releases never ship the PDB, so both packaging
scripts (`build-release-payload.ps1`, `build-runtime-hotfix.ps1`) first run
`Automation/tools/archive-symbols.ps1`, which copies the hook DLL and its PDB into a
Microsoft-layout symbol store keyed by the DLL's link stamp and image size. The store is the
git-ignored `Automation/symbols/`, or `SOURCEMVM_SYMBOL_STORE`. Keep it backed up: a lost PDB
cannot be rebuilt, and that build's reports stay unresolvable. The script refuses a PDB from a
different link.

Each crash header carries `Hook ID   : AfxHookSource2.dll/<key>`, and the build line carries
`, id <key>`. When the fault is outside our DLL, the header also names the
`First SOURCE:MVM frame`. `discord-reports.py fetch` writes `stack-resolved.txt` beside each
crash report and prints its first resolved SOURCE:MVM frame, for example
`#00 Filmmaker::CrashReport_Command + 0xcb  (CrashReport.cpp:837)`. Run
`discord-reports.py symbolize [<message-id>...]` to re-resolve reports already fetched. Only
archived builds resolve. CS2's own modules have no public symbols and stay module+offset.
Nothing about symbols reaches the player's PC.

### Diagnosing a render stall

The heartbeat includes `render phase`, its wall-clock `age`, `frameAge` since the last render
pump boundary, and cumulative render/main `pumps`. Frame percentiles and average hook costs cover
completed frames only, so they can stay unchanged during a stall. An age that keeps rising while the
render pump stays still helps tell a stopped render thread from slow playback.
The phases are Environment, ReShade, native Present, pre-UI and hook frame callbacks.
`engine-frame` is a broad boundary. It does not name a faulting function or prove a GPU driver problem.

`Automation/livejs/probe-loading-panels.js` takes a read-only snapshot of loading-related Panorama
panel IDs, ancestor visibility and layout sizes. Run it in the `menu` and `hud` contexts after a demo
switch, paired with a window capture. The snapshot reports traversal truncation. A truncated snapshot
or a panel's visibility flag cannot certify the screen on its own.

To capture a stalled process without sending input or terminating it, run the local helper as the
same Windows user who launched CS2:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Automation/tools/capture-cs2-hang.ps1 -Out Automation/runs/loading-stall.dmp -WaitForStallSeconds 90 -MapName de_nuke
python Automation/tools/read-hang-contexts.py Automation/runs/loading-stall.dmp
```

The watcher checks the current session's heartbeat for a render frame age of at least two
seconds. `-MapName` is optional. Use it to exclude stalls during a return to the menu. Capture
can briefly pause the process. A qualifying heartbeat may have recovered by the time of capture,
so a bounded watch that captures nothing does not prove the problem is absent. Without
`-WaitForStallSeconds`, the default captures immediately for a healthy comparison. Existing output
files are never overwritten. The helper saves thread contexts, stacks and module information, but
no full process-memory dump, and it uploads nothing. Keep the dump and the heartbeat in ignored
`Automation/runs/` storage, because stack memory can contain private data.

The Python reader lists AMD64 thread instruction pointers as module offsets. It does not unwind
stacks or identify the cause. A thread waiting in `ntdll` alone is not a diagnosis. For stack
analysis, use a debugger and the exact DLL/PDB from the captured build. Never map an older report's
offset against a newer DLL.

### Download counts

`python Automation\tools\discord-reports.py stats` prints installer downloads per release, from
GitHub's public download counts. The launcher itself reports nothing: there is no install or launch
counter.

### Save reset

On its first start, a launcher whose `SAVE_EPOCH`
([`save-reset.cjs`](../../SourceMovieMakerLauncher/electron/save-reset.cjs)) is newer than
`%APPDATA%\HLAE\source_mvm.save-epoch` deletes SOURCE:MVM's saved state once: `source_mvm\`
(props, Environment scenes and presets, demo projects, caches) and the `filmmaker_*`,
`cosmetic_profiles.json` and `easter_eggs.json` settings. `bugreports\`, `debuglogs\` and `osu\`
are kept.
## The report

The report is a single `.txt` file (not a zip) under 2 MB. It is written to
`%APPDATA%\HLAE\bugreports\SourceMVM-bug-<date>_<time>.txt` and holds:

- what the user wrote (control characters removed, at most 4000 bytes)
- the optional Discord name the user typed
- the SOURCE:MVM hook build time, CS2's `steam.inf` build lines, Windows version, map and demo
- render and main pump counts, schema offsets, particle hook state, whether `mvm_debug` is recording
- **recent history from the always-on recorder** (below)
- the last 700 KB of the newest `mvm_debug` log, when there is one
- the last 200 KB of CS2's `console.log` when CS2 was started with `-condebug`
- the names, ages and sizes of the three newest crash dumps. The dumps themselves are not
  attached because they are too big.

### Always-on recorder

Logging does not need to be switched on before a bug happens. From the first main-menu frame, the
hook keeps CS2's console listener registered for the whole session (`MvmConsoleCapture_KeepAlive`;
`mvm_debug stop` does not unregister it). Every console line, engine and SOURCE:MVM alike, goes
through `MvmDebugLog_ConsoleLine` into an in-memory ring of the newest ~768 KB, whether or not an
`mvm_debug` file is open. Each line is timestamped and tagged, and lines over 1000 bytes are cut.
A report includes the whole ring.

Text rules are in [`BugReportMath.h`](../../AfxHookSource2/Filmmaker/Community/BugReportMath.h),
tested by `Automation/tests/bug-report-math-tests.cpp`:

- tail trimming on whole lines and characters, and description cleaning
- the URI decoding of dialog text
- the recorder ring
- the multipart upload body
- the webhook URL check

### Debug log memory

While `mvm_debug` is active, repeat compression keeps at most 4096 distinct events and 4 MiB of
string payload. Before that window fills, pending repeat summaries are written and the cache is
cleared. A later occurrence starts a new summary. Oversized messages are written directly without
being kept, and `mvm_debug stop` releases the cache. This caps diagnostic memory during long
playback with changing frame values. A `debuglog.retention` line records each window flush. The engine-free cache is in
[`DebugLogDedupMath.h`](../../AfxHookSource2/Filmmaker/Debug/DebugLogDedupMath.h), covered by
`Automation/tests/debug-log-dedup-math-tests.cpp`.

## Crash reports

When CS2 crashes with SOURCE:MVM loaded, a crash report is written to the PC. It is never sent at
crash time. On the next launch the main menu shows **CS2 crashed last time** with the crash summary,
the file name and what the report holds, and offers **Show file**, **Don't send** and **Send**.
**Send** posts the newest reports (at most three) as red **Crash report** cards, each with its
minidump, and then deletes all of their files. **Don't send** renames them `.declined` and deletes
their dump; the report text stays on the PC and is never offered again.

**The minidump.** At crash time the worker also writes `<base>.dmp` through CS2's own `dbghelp`
(`MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules`, typically under 1 MB).
It holds every thread's stack and registers, the module list and the exception, but no heap. The
prompt says so before the player chooses. The dump is attached only when it is under 7 MB, which keeps
the post within Discord's limit. `discord-reports.py fetch` resolves the dump's exception address against the archived
symbols into `stack-resolved.txt`
(`Minidump: exception 0xC0000005 ... AfxHookSource2.dll+0x4e9557  Filmmaker::CrashReport_Command + 0x1c7  (CrashReport.cpp:939)`).
For every thread, open the `.dmp` in Visual Studio or WinDbg with `Automation/symbols` on the
symbol path.

**Folder size.** Each session start keeps the newest 10 reports and at most 200 MB of them
(`kMaxKeptReports`, `kMaxKeptReportBytes` in `CrashReportMath.h`); every file of an older report
is deleted, bug and crash alike. Reports still waiting for the player's decision are always kept.
`mirv_filmmaker community crash prune` runs the same pass on demand. Code:
[`CrashReport.cpp`](../../AfxHookSource2/Filmmaker/Community/CrashReport.cpp); rules in
[`CrashReportMath.h`](../../AfxHookSource2/Filmmaker/Community/CrashReportMath.h), tested by
`Automation/tests/crash-report-math-tests.cpp`.

**What is caught at crash time.** CS2 catches most of its own crashes. Its engine handler writes
`game\bin\win64\cs2_<time>_accessviolation.mdmp` (or `_error.mdmp` for a fatal error such as out of
memory) and terminates the process. No unhandled-exception filter runs, and Windows logs nothing.
The primary signal is therefore a detour on `MiniDumpWriteDump` in the `dbghelp.dll` that CS2 ships
in `bin\win64`. The detour is installed when the session starts, because it needs `LoadLibrary`
and so cannot go in `DllMain`. CS2's dump is written first, untouched. The report is then built from
the exception pointers CS2 passes, plus CS2's own dump comment. The fatal-error text becomes the
headline, and the demo/tick and memory lines up to the ConVar list go into the crash section.
`CrashReport_InstallHandlers` covers what CS2 does not catch:

- unhandled exceptions (access violations and the rest), through a top-level filter. A detour on
  `SetUnhandledExceptionFilter` keeps it on top when CS2 installs its own later; CS2's filter is
  chained, so its own `.mdmp` still gets written.
- `abort()` (SIGABRT): `std::terminate`, pure virtual calls and CRT aborts. Windows logs these as
  `0xC0000409` in `ucrtbase.dll`, and no exception filter sees them. The hook links the shared UCRT,
  so the handler covers CS2's own aborts.
- CRT invalid parameters, chained to the previous handler and then the CRT's own fast-fail, so
  behaviour is unchanged.

The handler writes the crash header first, without allocating. It holds the summary, the exception
code and access, the faulting `module+offset`, the thread (main or not), uptime, map, demo, and up to
48 stack frames as `module+offset`. Then a worker thread writes the full report: the crash section
first, then the usual build and system lines and recorder history. The handler waits at most 25 s,
then lets the crash continue.

**What is offered on the next launch.** At startup, a worker thread writes out what an earlier
session left behind. The newest three are offered on the prompt. Older ones are renamed `.unsent`,
which keeps them without offering them. Reports that carry a Steam ID are never
offered.

- a crash whose send failed or never finished: the crash-time report as written
- an **unexpected exit**: the previous session left `session.lock` behind (no clean exit, no caught
  crash). That covers fast-fails Windows handles itself (heap corruption, stack cookies), a process
  killed from Task Manager or a tool, a force-closed hang, and power loss. Its history is the
  previous `session.log`. When CS2 wrote its own `.mdmp` in `game\bin\win64` within two minutes of
  that log's last line, the summary says CS2 crashed and names the dump: that crash bypassed our
  handler, and the dump holds the faulting module.
- a **slow session the player quit**: frames were slow (a heartbeat p95 of 250 ms or worse over at
  least 600 frames) and the game was then closed normally. The frame window starts when the hook
  loads, so the first heartbeats hold only CS2's startup load; those never count. The lock is kept with `exit=clean` so the report still arrives,
  marked as a quit after a slowdown rather than a crash.

A clean exit is client shutdown (`Source2Shutdown`), `DLL_PROCESS_DETACH`, or the process
terminating itself. Each of these deletes the lock unless the session was slow. CS2 often ends the
process after shutdown without a DLL detach, so the client-shutdown hook is what keeps normal quits
from being reported as crashes.

Every report also lists **Injected** third-party graphics code found in `cs2.exe`: ReShade and its
add-ons, D3D/DXGI wrapper DLLs outside System32, and the RivaTuner, Discord, OBS, NVIDIA, Overwolf
and Steam overlays. The scan runs at session start and every ~10 s after. A dead session's list comes
from its lock.

### Heartbeat

Every 5 s the recorder gets one `health` line: the map, frame p50/p95/max, the hook's own CPU cost
per frame, the game's CPU %, GPU % (busiest GPU engine, as Task Manager shows it), RAM (working set
and private), VRAM in use against the OS budget, system CPU % and free RAM, what taking the sample
cost, shader creation since start (`shaders N engine X ms hook Y ms (max Z)`: CS2's own pixel and
vertex shader creation and what our detours added on its loader threads, summed over threads), and
the relight/GI state. A `machine` line records the CPU model, RAM, GPU and VRAM budget
once. The system figures are sampled on their own thread
([`SystemUsage.cpp`](../../AfxHookSource2/Filmmaker/Platform/SystemUsage.cpp)), never on the game's
threads; the heartbeat only formats the newest sample. About 70 KB per hour.

**Files** in `%APPDATA%\HLAE\bugreports\`:

| File | Meaning |
| --- | --- |
| `session.lock` | this session is running: pid, start time, last map, demo, injected modules, worst slow p95; `exit=clean` after a quit from a slow session |
| `session.log` | every recorder line, written straight to disk (capped at 32 MB) |
| `SourceMVM-crash-<time>.pending` / `SourceMVM-exit-<time>.pending` | waiting for the player's decision; holds the crash header |
| `<base>.txt` | the report |
| `<base>.dmp` | the crash's minidump; sent with the report, deleted after Send or Don't send |
| `<base>.declined` / `<base>.unsent` | kept on the PC, never offered again |
| `<base>.session.log` | the dead session's log, until its report is written |

Nothing can be sent without a webhook in the build (`BugReportWebhook.local.h`). Crash sends skip the
dialog's one-per-minute limit, and one Send posts at most three reports. A session start deletes any
`crashreports.off` marker.

## Logo

The eye is HC Tracker's own logo, taken from the PNG embedded in the site's header SVG. It is compiled
into our FX pack as a Panorama texture:

- source: [`FX/ui/panorama/images/sourcemvm/`](../../FX/ui/panorama/images/sourcemvm/hctracker_eye_png.vtex)
  (the 512px PNG and its `.vtex`, RGBA8888 so it stays sharp)
- compiled: `FX/compiled/source_mvm_fx/panorama/images/sourcemvm/hctracker_eye_png.vtex_c` (in the
  local FX pack, not the repository)
- loaded as `s2r://panorama/images/sourcemvm/hctracker_eye_png.vtex`

To rebuild it, use CS2's `resourcecompiler.exe` (Workshop Tools) on the `.vtex` with
`-game build/fx/fx-source1import/source2/game/source_mvm_fx`, the root
`FX/tools/build-render-materials.ps1` sets up. Then copy the `_png.vtex_c` into `FX/compiled`. A
build whose FX pack lacks it falls back to the web copy committed at
[`Assets/hctracker/hctracker-eye.png`](../../Assets/hctracker/hctracker-eye.png), loaded from GitHub
`main`. That PNG must stay a regular git file, not LFS, or GitHub serves a pointer. Failing both, the
badge shows the wordmark alone. The launcher bundles its own copy
(`SourceMovieMakerLauncher/public/hctracker/`).

## Console

- `mirv_filmmaker community status`: whether the buttons are injected, and the last report
- `mirv_filmmaker community report <what happened>`: write a report on this PC without the dialog
  (it is not sent)
- `mirv_filmmaker community crash status`: handlers, whether sending is possible, reports awaiting
  the player's decision
- `mirv_filmmaker community crash test`: run the whole crash path without crashing; the report is
  marked TEST and offered on the main menu like a real one
- `mirv_filmmaker community crash prune`: apply the folder limit now
- `mirv_filmmaker community crash now`: crash CS2 on purpose (an access violation) to verify it end to end
- `mirv_filmmaker community rebuild`: re-inject the main-menu script
- `mirv_filmmaker community eval <js>`: run Panorama JS in the main-menu context. Try
  `$.Msg($.MvmCommunity.report())`, `$.Msg($.MvmCommunity.dialogStep())` or
  `$.Msg($.MvmCommunity.crashPromptState())`. The console splits on `;` and pads quoted strings,
  so buttons are pressed by number: `$.MvmCommunity.reportPress(1)` (0 show file, 1 don't send,
  2 send) and `$.MvmCommunity.crashPress(1)` (0 show file, 1 don't send, 2 send, 3 ok).
