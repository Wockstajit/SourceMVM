# Settings tab, languages and shortcuts

SOURCE:MVM has its own tab in CS2's settings menu (pause menu, Settings, SOURCE:MVM; the same tab
is in the main menu's settings). It holds:

- **Language.** Every language CS2 ships. `Automatic` follows the language CS2 is running in.
- **Window layout.** Docked or modular windows ([modular layout](modular-layout.md)).
- **Shortcuts.** The keys of the director's own shortcuts. Click a key, press the new one.

Deadlock's panels are not localized and its keys are fixed.

## How text gets translated

The English text written in a script is the key. Wrap the literal where it is written:

```js
lbl(row, T('Reset'));
status.text = T('Closest {0}', ratioName);
count.text = TN(keys.length, '{0} key', '{0} keys');
hint.text = T('Press {0} to place one', K('marker.place', 'K'));
```

- `T(text, ...)` returns the text in the active language, or the English when the table has no
  entry. `{0}`, `{1}` are filled after the lookup, so a translation can move them.
- `TN(count, one, other, ...)` picks the plural form for `count`. `{0}` is the count.
- `K(action, fallback)` returns the cap text of the key a shortcut is on now.
- One English word used in two senses gets a note after `##`: `T('Clear##weather')` beside
  `T('Clear')` the button. The note is part of the key and is never shown.
- All three come from
  [LocalizationJs.h](../../AfxHookSource2/Filmmaker/Panorama/LocalizationJs.h), which every script
  body includes right after its opening `(function () {`.

Rules for wrapping:

1. The first argument is a plain string literal. The catalog is read from the source, so a
   variable or a built-up string cannot be translated. Build sentences with placeholders
   (`T('Saved to {0}', path)`), never by joining translated pieces.
2. Translate at the point of display. A string that is also a panel id, a console argument, a
   settings key or something compared with `===` stays untranslated in the data, and `T()` goes
   where the label is set.
3. Leave these in English: the product name SOURCE:MVM, console commands and cvar names, file
   names and extensions, units and codec names (`px`, `fps`, `ProRes`), and text only a developer
   sees (debug overlays, `$.Msg` output).
4. A key that can be rebound is never written into a sentence. It is a placeholder filled with
   `K()`. Fixed keys (`ESC`, `Ctrl+Z`, the mouse) are written as they are.
5. Text that arrives from C++ as English and comes from a small known set is translated in the
   script that shows it, through a map of literals (`{ 'Manual': T('Manual'), ... }`). Free-form
   C++ status and error sentences are still shown in English.

A label in a fixed-width control (a button, a tab, a row name) must not wrap or push the layout
when its text gets longer. Give it `whiteSpace = 'nowrap'` and `textOverflow = 'shrink'` with a
bounded width, so Panorama scales the text down to fit. Only running text (a hint, a description)
wraps.

## How a language reaches the panels

[Localization.cpp](../../AfxHookSource2/Filmmaker/Localization/Localization.cpp) owns the active
language. The setting is the UI key `language` (`auto` or a CS2 language id). The tables are
compiled into the hook DLL as resources, so a hook-only build needs no extra files.

The menu and the in-game HUD are separate Panorama contexts with separate script globals. Each
gets the table pushed on its own, ahead of the panel drivers, and gets it again when a context
reload dropped it. A language change or a rebound shortcut bumps a UI epoch; every panel driver
compares it and rebuilds its script, which re-reads all text.

## Tables

One file per language in
[`AfxHookSource2/Filmmaker/Localization/lang/`](../../AfxHookSource2/Filmmaker/Localization/lang),
named after CS2's language id (`german.json`, `schinese.json`). A table maps each English key to
its translation. A plural key is `"<one> || <other>"` and its value is an array with one entry
per plural form of that language, in this order:

| Forms | Languages | Order |
| --- | --- | --- |
| 1 | Chinese (both), Japanese, Korean, Thai, Vietnamese, Indonesian | other |
| 2 | Most European languages. French and Brazilian Portuguese use the first form for 0 and 1 | one, other |
| 3 | Russian, Ukrainian, Polish | one, few, many |
| 3 | Czech, Romanian | one, few, other |

Translations use the terms CS2 uses in that language (Valve's `csgo_<language>.txt`) for anything
the game already names: settings, buttons, team and weapon words. Filmmaking terms follow what
video editors in that language use. Labels stay close to the English length so they fit.

## Shortcuts

The rebindable shortcuts are the single keys the director's key handler owns:
[KeybindsMath.h](../../AfxHookSource2/Filmmaker/Input/KeybindsMath.h) lists them. A pressed key is
translated to the key the handler has always tested, so the handler and its order of checks are
unchanged. A binding is stored as the UI key `bind.<action>`.

One key can hold two shortcuts. X is both X-Ray and roll right out of the box: the camera you are
in decides which one happens, and roll is offered the key first. Moving either one to another key
separates them. A third shortcut on the same key is refused.

Under the rebindable list the page shows the fixed controls, for reference: Scroll (camera mode),
Shift + Scroll (free camera speed), Ctrl + Scroll (field of view), W A S D, Esc, Delete and the
Ctrl shortcuts. Those cannot be changed. A shortcut cannot be moved onto Esc, Shift / Ctrl / Alt,
the mouse, W A S D, Delete / Backspace, Enter, Tab, or C and V.

## Tools and checks

```text
node Automation/i18n/catalog.mjs               # catalog size and coverage per language
node Automation/i18n/catalog.mjs --missing german
node Automation/i18n/catalog.mjs --prune       # drop table entries the scripts no longer use
node Automation/i18n/fit-audit.mjs german,french   # labels that are cut or wrap
node --test Automation/tests/localization-tests.mjs
```

The test fails when a `T()` call is not a literal, when a table misses a key or carries a stale
one, when a placeholder is lost, or when a plural entry has the wrong number of forms.

The fit audit opens the [browser mockup](../../UiMockup/README.md) views in English and in the
language and reports labels that overflow their box or take more lines. The mockup itself shows a
language with `?lang=<id>` in its address. Browser text metrics differ from the game's by a few
pixels, so confirm a fix with a game capture.

Console:

```text
mirv_filmmaker language                  # the setting, the language shown, the language CS2 runs in
mirv_filmmaker language <id|auto>
mirv_filmmaker keybind                   # every shortcut and its key
mirv_filmmaker keybind <action> <vk|default>
mirv_filmmaker keybind reset
```
