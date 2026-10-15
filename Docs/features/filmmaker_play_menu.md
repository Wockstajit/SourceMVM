# Play menu: Practice + Workshop Maps

SOURCE:MVM edits CS2's native main-menu **Play** page in place. It does two things, both with pure Panorama edits on native panels:

1. **Strip.** The page keeps only the two tabs an offline movie/demo tool uses: *Practice* and *Workshop Maps*. Official Matchmaking and the community-server browser are hidden.
2. **Workshop sidebar.** Sorting and keyword filtering for the workshop map list, placed next to CS2's own "Filter maps" box.

Matchmaking still works at the engine level. Only the tabs are hidden.

## Where the code lives

| Piece | File |
|---|---|
| Per-frame pump, JS injection, payload push, command drain | `AfxHookSource2/Filmmaker/PlayMenu/PlayMenuHud.cpp` |
| Console surface (`mirv_filmmaker play ...`) | `AfxHookSource2/Filmmaker/PlayMenu/PlayMenuCommand.cpp` |
| JS fragment 1: `$.MvmPlay`, host panel, tick, tab strip | `AfxHookSource2/Filmmaker/PlayMenu/PlayMenuStripJs.h` |
| JS fragment 2: workshop sidebar, sorting, filtering | `AfxHookSource2/Filmmaker/PlayMenu/WorkshopFilterJs.h` |
| JS fragment 3: keyword editor | `AfxHookSource2/Filmmaker/PlayMenu/WorkshopKeywordEditorJs.h` |
| Workshop metadata + play history | `AfxHookSource2/Filmmaker/Workshop/WorkshopOrganizer.cpp` |
| Persistence | `AfxHookSource2/Filmmaker/Config/WorkshopOrganizerStore.cpp` |
| Tests | `Automation/tests/workshop-organizer-tests.mjs` |

The play menu is separate from the Watch/Demos tab (`AfxHookSource2/Filmmaker/Panorama/FilmmakerMenu.cpp`). It has a different page and lifetime, its own host panel, and its own Panorama bridge. Neither knows the other exists.

## Native panels it touches

From CS2's `layout/mainmenu_play.xml` and `scripts/mainmenu_play.js`:

| Panel | Role | What we do |
|---|---|---|
| `PlayTypeTopNav` | the top navbar | cached anchor for the strip |
| `Play-official` | Official Matchmaking tab | hidden |
| `Play-listen` | Practice with Bots tab | relabelled to "Practice" |
| `PlayWorkshop` | Workshop Maps tab | untouched |
| `PlayCommunity` | community-server browser icon | hidden |
| `WorkshopSearchBar` | the native "Filter maps" box | our sidebar is inserted under it |
| `gameModeButtonContainer_workshop` | lazily built map tile container | sorted + filtered |

Each workshop tile carries `mapname` (`@workshop/<id>/<map>`), `addon` (`<id>`), `data-workshop-modes` and `data-tooltip`, plus a `MapGroupName` label. Those five are the only input to classification.

## Rules this feature follows

**Never replace a native handler.** The tabs keep CS2's own `onactivate`, so `PlayMenu`'s internal bookkeeping (`m_serverSetting`, the map-group containers) stays correct. When CS2 restores a saved server setting of *Official* and lands the page on a tab the strip has hidden, the strip dispatches `Activated` on the Practice tab. That is the same event a click sends.

> **`$.DispatchEvent('Activated', panel, 'mouse')`: the third argument is required.** Without the source string, the event never reaches the panel's `onactivate`. Setting `checked` alone is not a substitute. CS2's own `_UpdateServerSettingBtns` sets `checked` without firing the handler, which leaves `PlayMenu`'s `m_serverSetting` pointing at the hidden tab. The result looks right and is internally wrong. Native reference: CS2's `scripts/mainmenu.js:999`.

The search box follows the same approach. The sidebar adds a `TextEntryChanged` handler with `$.RegisterEventHandler`. It does not take over `ontextentrychange`, which would break the native filter and its clear button.

**One writer for `panel.visible`.** Native `_UpdateWorkshopMapFilter()` filters by writing `visible` on each tile. The sidebar also writes it, so its predicate includes a replica of the native search test instead of layering on top of whatever native last wrote. With two independent writers, the result depends on who ran last.

**A tick, not a one-shot.** `mainmenu_play.xml` is created and destroyed as the user navigates, and CS2 restores the saved server setting every time. The injected script re-applies every 0.5s. It caches `PlayTypeTopNav` and re-traverses the panel tree only when that cached panel stops being valid.

**Re-injecting must not leave a second copy behind.** Repeated `mirv_filmmaker play rebuild` runs can stack copies of every injected panel. It has two independent causes. Fixing either one alone still leaves duplicates:

1. *Panels in native containers survive.* Deleting our own `MvmPlayMenuHost` does not reach a panel parented into a native CS2 container. The fresh fragment, whose reference to it is `null`, builds another.
2. *`$.Schedule` loops outlive the script that started them.* The previous round's `tick()` keeps running with its own `openBtn` / `bar` references, so whatever the sweep deletes, it rebuilds.

The strip handles both:

- **Sweeps every `Mvm*` panel in the tree** before creating anything. At that point nothing of ours exists yet, so every match is a leftover. Every panel a fragment creates must carry an `Mvm` id. A panel created with `''` is invisible to the sweep and becomes the next duplicate.
- **Every tick loop stops when its own host dies.** `if (!host.IsValid()) return;` has no reschedule. The sweep deletes the old host, so that check is what tells a superseded round to stop. A generation counter would not work, because each injection gets a fresh context and nowhere to keep one. A registry of live panel references on `$` reads back empty, so it cannot identify the current round.

`$.MvmPlay.dupes()` prints every `Mvm*` panel in the tree with a count. One of each is correct. It is the only way to see this problem, because a second sidebar just looks like a layout bug.

**No ICU.** Panorama's V8 ships without ICU data. `localeCompare`, `toLocale*` and `Intl` throw `RangeError: Internal error. Icu error.` at runtime and take the panel down. Sorting uses `<` / `>` (codepoint order). Case-folding uses `toLowerCase`, which is not a locale API. The test suite has a static ban on the locale APIs.

## Sorting and filtering

**Sort**: `name`, `installed`, `played`. Name is the workshop title. The other two come from C++, keyed by the tile's `addon` attribute:

- *Recently installed* is the newest mtime seen in `<steam library>\steamapps\workshop\content\730\<id>`. The code finds that folder by walking up from the process folder until a path component is literally named `steamapps`, so it survives Valve moving the executable. It is read fresh on every scan and never persisted, so a Steam update is picked up automatically.
- *Recently played* is our own history, stamped when the engine loads a map whose name matches a `.vpk` inside a subscribed item. It is sampled once a second from `GetLevelNameShort()`, alongside the demo browser's equivalent poll.

Reordering uses `MoveChildBefore`, the same API `friendslist.js` and `context_menu_lobbies.js` use. It runs only when the sort mode or the tile count changes.

**Hide training maps**: one toggle over one keyword list. A map counts as a training map when its **map name or workshop title** contains one of the keywords.

Keyword matching follows two rules, and the test suite covers both:

- **Keywords match the map name and workshop title only.** A workshop description is prose about a map, so a word in it says nothing reliable about what the map is. The search box still searches modes and the description. That is native behaviour, and the user typed those words on purpose.
- **Both the map name and the title are checked.** A map name can be opaque, and the title can give the match away, as with *Config Generator*.

The keyword list is a substring match. That rules out some obvious-looking words: a bare `train` hides `de_train`, so the list spells out `trainer` / `training`. A test covers that specific word.

The defaults live in `kDefaultWords` in `WorkshopOrganizerStore.cpp`. The sidebar is data-driven from the payload and hard-codes no keyword, so the editor can replace the whole list.

## Game modes on workshop maps

Native hides the game-mode row on the Workshop tab and disables every radio in it:

```js
element.enabled = element.enabled && !isWorkshop && ...   // mainmenu_play.js
```

As a result, a workshop map launches in whatever mode was last selected. For most people that is Wingman, a 2-player mode with no room to add bots. Since bots are the point of this build, the sidebar's tick hook puts the row back (visible and enabled) whenever the Workshop tab is up. The buttons keep their native handlers, so picking Casual or Deathmatch goes through CS2's own bookkeeping.

The row is re-asserted every tick, not once, because native re-runs its disabling pass on every tab switch and would otherwise take the row away again.

## Persistence

`%APPDATA%\HLAE\filmmaker_workshop_organizer.txt` is a line-based UTF-8 text file. It uses the same style as the demo organiser's store:

```
v 2
sort installed
hidetraining 0
words aim,training,trainer,surf,config,generator
p 1786000000 3070394076
```

Unknown lines are ignored, so a file with unrecognised lines still loads rather than failing.

## Bridge contract

A hidden host panel `#MvmPlayMenuHost` carries two attributes:

- **C++ → JS**: `ws` (the JSON payload), then `$.MvmPlay.renderWs()`.
- **JS → C++**: `cmd`, a newline-separated queue drained every frame.

Commands: `ws sort <name|installed|played>`, `ws hide <0|1>`, `ws words <comma,separated>`, `ws reset`, `ws rescan`.

The test suite checks that every verb the JS emits is handled in C++, that the sort ids match the store's whitelist, and that the attribute and panel names agree on both sides. The compiler cannot check any of these.

## Console surface

```
mirv_filmmaker play status
mirv_filmmaker play rebuild
mirv_filmmaker play rescan
mirv_filmmaker play ws sort installed
mirv_filmmaker play eval $.MvmPlay.gotoPlay()
mirv_filmmaker play eval $.MvmPlay.gotoWorkshop()
mirv_filmmaker play eval $.Msg($.MvmPlay.report())
mirv_filmmaker play eval $.Msg($.MvmPlay.wsReport())
mirv_filmmaker play eval $.Msg($.MvmPlay.wsHidden())
mirv_filmmaker play eval $.Msg($.MvmPlay.wsOrder(5))
```

Two netcon constraints shape those helpers. `ui_eval` truncates at **256 bytes**, so checks call a short helper instead of inlining JS. CS2's console tokenizer also **rewrites quoted strings inside an eval**: `'Activated'` arrives as `' Activated '`, and `DispatchEvent` rejects it. So anything a check needs to navigate is wrapped in a helper (`gotoPlay`, `gotoWorkshop`, `gotoPractice`), and the eval carries no string literals at all.

Run `wsHidden()` when classification looks wrong. It prints the training list and the kept list side by side. That shows an over-broad keyword (an ordinary map in the hidden list) or a missing one (a config map in the kept list).

## Testing

```
node --test Automation/tests/workshop-organizer-tests.mjs
```

The behaviour tests evaluate the shipping filter fragment against stub Panorama panels. Classification, the category/hide-training interaction, search composition and sort order are all tested against the real code, not a reimplementation.
