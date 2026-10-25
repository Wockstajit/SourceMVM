# Version and What's new

SOURCE:MVM shows which version is installed, and after an update it shows what changed, once, in
CS2's own news popup.

## What the player sees

- The main menu's top bar has a **WHAT'S NEW** button left of INVENTORY, built like the HC TRACKER
  badge, with the version beside it (`v0.3.1 hotfix 1`, or `dev build`). A gold dot marks notes
  not opened yet. It opens the popup, and it is there in the pause menu too.
- The Movie Making button's tooltip also names the version.
- The first time the main menu is up after an update, the popup opens by itself. It leads with the
  releases since the version last seen. Older ones fold under SHOW EARLIER VERSIONS. Once it has
  been shown it stays shut until the next update.
- The popup's link button opens the full changelog on GitHub in the default browser.

## Where the pieces live

| Piece | Files |
| --- | --- |
| The changelog | [`CHANGELOG.md`](../../CHANGELOG.md) at the repository root, installed as `bin/resources/CHANGELOG.md` (`AfxHookSource2/CMakeLists.txt`) |
| Parsing and the "what's new since" pick | `Filmmaker/Community/ChangelogMath.h`, tested by `ChangelogMathTests` (`Automation/tests/changelog-math-tests.cpp`) |
| Installed version, payload, seen key | `Filmmaker/Community/ToolVersion.{h,cpp}` |
| Push and commands | `Filmmaker/Panorama/FilmmakerMenu.cpp` (`PushWhatsNew`, `whatsnew seen`, `whatsnew changelog`) |
| Header button and popup | `Filmmaker/Panorama/WhatsNewJs.h`, spliced into `FilmmakerGuiJs.h` |
| The top bar's fixed layout (both header buttons) | `Filmmaker/Panorama/HeaderLayoutJs.h`, spliced into `FilmmakerGuiJs.h` and `Community/CommunityJs.h` |
| Mockup stand-ins for the native popup and the bar's ids | `UiMockup/shim/native-menu.js` (`ShowCustomLayoutPopupParameters`, `MainMenuNavBarInventory`, `#MainMenuInput`) |

## The header button

It is a Button at the end of `#MainMenuInput`, like the HC TRACKER badge (`Community/CommunityJs.h`),
not a child of the top bar: a later drag layer covers the bar and would take its clicks. That is
also what keeps it in the pause menu. During demo playback CS2 collapses the bar's whole centre
group (INVENTORY to NEWS carry `.DisableDuringDemoPlayback`), so a button inside it would vanish
exactly when paused.

Its spot is fixed and the same in the home menu and the pause menu, from the bar layout it shares
with the HC TRACKER badge (`Panorama/HeaderLayoutJs.h`, `mvmHeaderSlot`), which is centred the same
way right of NEWS (230 px each side at 1920 wide). It is centred in the gap between
where the bar's left icons end on the home screen and where INVENTORY's word starts, so the space on
both sides is even. Both ends are fixed layout, measured at 1080p on 2026-10-24: the icons end 175 px
from the left (pause hides the quit icon but leaves home, Movie Making and settings in place), and
INVENTORY starts 250 px left of the bar's centre. At 1920 wide the button spans 341 to 545, with
166 px on each side. Nothing it reads changes when CS2 pauses: INVENTORY is collapsed then and keeps
a stale layout, so it is not measured. On a narrow screen (4:3) the version text is dropped before
the gaps would close up, and it comes back when the window is widened again.

All hook paths are under `AfxHookSource2/`.

## The version

`ToolVersion_Runtime()` reads `runtime.version` from the launcher's `installed.json`, one folder
above the runtime the hook loaded from (`<payload>/installed.json` next to `<payload>/runtime/`).
A hook staged by hand has no such file and calls itself a dev build. A dev build never opens the
popup by itself, and opened by hand it shows Unreleased and the newest release. Bug reports read
the same version (`BugReport.cpp`).

## The changelog format

`ChangelogMath.h` reads only this shape, so keep it when editing `CHANGELOG.md`:

- `## Unreleased` first, for work on `main` that no release carries yet. It may name the version it
  is meant for, `## Unreleased (0.4.0)`: any heading that starts with "Unreleased" counts, and the
  popup shows the whole heading.
- `## <version> - <YYYY-MM-DD>` for a release, and `## <version> Hotfix <n> - <YYYY-MM-DD>` for a
  runtime hotfix. Newest first.
- `### <group>` headings inside a release. The popup prints them as `[ GROUP ]`, the way CS2's
  release notes head their sections.
- One `- ` bullet per change. An indented line continues the bullet above it. Bold, backticks and
  links are reduced to plain text (a link keeps its text).
- A `##` heading without a version (and the intro above the first release) is not shown.

`ChangelogMathTests` also parses the shipped `CHANGELOG.md` and fails if a release has no bullets,
has no date, or is out of order.

## Which sections open the popup

`ChangelogWhatsNew(current, seen)`:

- Updated from a known version: every release newer than it, up to the installed one.
- No version seen yet (the first start with this popup): the installed release and its hotfixes.
- A dev build: Unreleased and the newest release.

`ToolVersion_WhatsNewJson()` sends those as `top` sections plus up to eight sections in total, and
sets `open` when the installed version differs from the UI setting `whatsnew.seen`. When the popup
is shown, the page sends `whatsnew seen` and the hook stores the installed version there.

## The native popup

The popup is CS2's `file://{resources}/layout/popups/popup_news.xml`, the window CS2 opens for a
new blog post (`new_news_entry_check.js`), opened with `UiToolkitAPI.ShowCustomLayoutPopupParameters`
and the `date`, `title` and `link` parameters it expects. It brings the native frame, dim backdrop,
title and date, link button, Close button, Escape and the open and close animation. Its body is an
HTML view that loads `link`. The page passes `about:blank`, removes `#BlogHTML` and `#spinner`, and
draws the changelog into `#HTMLContainer` as Panorama panels, so nothing is fetched. The link
button's handler is replaced with `whatsnew changelog`.

Like CS2's news popup, it opens by itself only on the main menu: not in the pause menu
(`MainMenuRootPanel--PauseMenuMode`) and not while a map is loaded (`GameStateAPI.GetMapBSPName()`
reads `<empty>` at the main menu). If the hook builds the page in a map, it waits for
`CSGOShowMainMenu`. CS2 may show its own news popup at the same time; the two stack.

## Console

```
mirv_filmmaker whatsnew          print the version and open the popup
mirv_filmmaker whatsnew reset    forget the version last seen first, as after an update
mirv_filmmaker ui_eval $.Filmmaker.whatsNewReport()
```

The report prints one line:
`[WHATSNEW] open=1 filled=1 rows=24 top=2 earlier=3 auto=1 btn=1 x=341 w=204 gaps=166,166 inv=690 dot=0 label=0.3.1_hotfix_1`.
`btn` is the header button, `x` and `w` its left edge and width in layout pixels, `gaps` the room on
either side as placed, and `inv` where CS2 last laid out INVENTORY's button. `x` must read the same
in the home menu and the pause menu.

## Releasing

Before a release, rename `## Unreleased` to the new version heading with its date. The next change
after the release starts a new `## Unreleased` above it; a section with no entries is refused by
`ChangelogMathTests`, so none is added empty. A runtime hotfix gets its own `## <version> Hotfix <n>` heading and
ships `resources\CHANGELOG.md` next to the DLL. See
[Publishing releases](../setup/publishing-releases.md).

## Verification

- `ChangelogMathTests` (ctest) covers the parser, the pick and the shipped file.
- Review the header button and popup content in the mockup's Demos tab: the button sits left of
  INVENTORY, the stand-in popup opens by itself, the earlier-versions toggle expands, and Close
  works. The mockup's bar spaces its tabs differently, so check the exact placement in game.
- In game, on the main menu and in the pause menu during a demo (`gameui_activate`), the button sits
  at the same `x` with even gaps, and the popup opens over both.
- In game, with a test `installed.json` in the stage, the popup opens on the main menu, is drawn
  inside CS2's news frame, and `whatsnew.seen` holds the installed version afterwards.
