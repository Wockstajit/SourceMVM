# Panorama UI architecture guide (CS2 filmmaker)

How the filmmaker's Panorama UI works. The shape implemented in this repo comes
first, followed by the design research (based on the
public structure found in Osiris and AdvancedFX/HLAE Panorama work) that the
implementation follows. Offline/demo filmmaking only, not for live gameplay.

## As implemented in this repo (`AfxHookSource2/Filmmaker/Panorama/`)

Every concept below has shipped. The concrete shape:

- `PanoramaBridge.cpp`: the small engine wrapper (`CUIEngine::RunScript`, attribute
  get/set, context-panel pinning). Engine access is resolved by byte-pattern scanning in
  `DeathMsg.cpp` (`getPanoramaAddrs*`), the single fragile file a CS2 update can break.
- One panel per file pair: each UI surface is a C++ bridge (`*Hud.cpp` / `FilmmakerMenu.cpp`)
  plus its embedded Panorama JS in an adjacent `*Js.h` header: `FilmmakerGuiJs.h` (the
  main-menu Demos page), `MovieHudJs.h`, `MarkerHudJs.h`, `CameraTimelineJs.h`,
  `CameraEditorJs.h` (+ per-section fragment headers `CameraEditor*Js.h`), `ConfigHudJs.h`,
  `GraphEditorJs.h`, `DemoBarButtonsJs.h`. The bridge builds the panel once, pushes a state
  JSON attribute per frame, and reads a command attribute back (JS emits
  `mirv_filmmaker ...` console commands or attribute writes).
- `PanoramaFindPanel.{h,cpp}`: the shared `Filmmaker::FindChildById` panel lookup
  (bounded recursive child search by id over the live engine panel tree). New bridges
  include this header instead of redefining it.
- Threading rule: all Panorama work runs on the game's main thread via
  `Filmmaker::RunMainThreadFrame`. Calling `RunScript` from the render thread crashes V8
  (`v8::Context::Exit() - Cannot exit non-entered context`).
- Constraints learned the hard way: no mouse-move event in HUD JS (build dragging from `Slider`
  panels); never set `style = ''` (clear colors with `rgba(0,0,0,0)`); MSVC's ~16 KB
  string-literal cap forces `)FMJS" R"FMJS(`-style splits in the `*Js.h` headers; netcon
  `ui_eval` truncates at 256 bytes, so drive the UI through predefined `$.Filmmaker.*`
  helpers.
## Shared workspace contract

- Config, Camera Editor and Render share the chrome, header chips and timeline dock builders;
  workspace accents must not create separate implementations of those controls. The dock is
  bottom-anchored; its design heights (ribbon included) are 126 for the native bar and 476 for the
  TIMELINE dock (Docs/features/camera-editor-feature.md, "The TIMELINE dock").
- Workspace tabs are text on a rule, not filled pill buttons. Action rows use named, bordered
  groups; selections and supported options stay visible rather than hiding behind dead dropdowns.
- Config and Camera Editor inspectors use a rail and one selected pane. Scroll vertically within
  the pane; never scroll the whole workspace sideways or move pinned headers and footers.
- The rail is 180px wide with 52px rows whose title and summary wrap (`UI.rail` in
  `PanoramaUiMetricsJs.h`). A card is one page with group headings (`card.__group`); sub-tabs stay
  only where a page would not fit the pane. Every control is one row in the same box (label 130,
  control, value 66). Booleans are square switches in that row, never chips.
- Nothing truncates. A segment or option grid whose captions do not fit beside the label drops
  under it at full width; buttons size to their caption; labels wrap. Fit beats column alignment.
- A single colour field is a full-width bar of that colour with the diagonal custom mark; a
  palette's custom chip is wider and carries the same mark. Icons come from one outline set
  (Lucide, compiled into the FX pack as `panorama/images/sourcemvm/ol_*`), never drawn from panels.
- Mouse-wheel input scrolls only. Value drags are relative to the value at press, never jump to
  the click, and retain their finer Shift gain. Every control exposes its tooltip over the control.
- Card actions, Reset and master switches remain available in the pane header. Selecting a rail
  item does not reset or recreate its controls; hydration continues for hidden cards.
- Edit the embedded source headers and verify through the
  [mockup-first workflow](mockup-first-workflow.md). Exported design handoffs are not runtime inputs
  or a second source of truth.

The sections below are the approach guide.

## Goal

Build UI that looks and behaves like native CS2 UI by using CS2's Panorama system instead of drawing a separate ImGui/DirectX overlay.

The target is:

```text
CS2 Panorama XML/CSS/JS style UI
Native-looking panels
Native controls such as buttons, sliders, dropdowns, text entries, preview panels
UI attached to existing CS2 menu or HUD roots
Commands sent back to your C++ filmmaking code
```

The target is not:

```text
Dear ImGui overlay
OBS/browser overlay
External window
Generic DirectX overlay
Online cheat menu
VAC-safe live-server tool
```

## High-level architecture

The Panorama path works like this:

```text
AfxHookSource2 / filmmaker DLL
  -> find or receive access to CS2's Panorama UI engine
  -> find an existing root panel, usually main menu, settings tab, or HUD root
  -> run custom Panorama JavaScript inside that panel context
  -> JavaScript creates real Panorama panels with $.CreatePanel
  -> panels reuse Valve's CSS classes and built-in panel types
  -> UI writes commands into a panel attribute
  -> C++ reads the command attribute each frame
  -> C++ applies the filmmaker feature
```

You create panels through CS2's own UI engine. Nothing is drawn manually.

## Core pieces you need

### 1. Panorama UI engine wrapper

You need a small C++ wrapper around CS2's Panorama UI engine. The wrapper should expose only the few operations needed for UI creation and cleanup.

Conceptually:

```cpp
class PanoramaUiEngineBridge {
public:
    void runScript(CUIPanel* contextPanel, const char* scriptSource);
    CUIPanel* getPanelFromHandle(PanelHandle handle);
    PanelHandle getPanelHandle(CUIPanel* panel);
    CPanoramaSymbol makeSymbol(const char* name);
    void deletePanelByHandle(PanelHandle handle);
};
```

Start small. The first version only needs:

```text
runScript
getPanelFromHandle
getPanelHandle
deletePanelByHandle
makeSymbol
```

Osiris does this through `PanoramaUiEngine.h`, where it wraps `runScript`, `getPanelPointer`, `makeSymbol`, and panel deletion.

#### Verified CUIEngine interface (current Osiris master)

The exact engine entry points matter when you wire
the bridge. The declarations below are copied verbatim from the current Osiris source
(`Source/CS2/Panorama/CUIEngine.h`):

```cpp
struct CUIEngine {
    using getPanelPointer      = CUIPanel* (*)(CUIEngine* thisptr, const PanelHandle* handle);
    using runScript            = void (*)(CUIEngine* thisptr, CUIPanel* contextPanel,
                                          const char* scriptSource,
                                          const char* originFile, std::uint64_t line);
    using makeSymbol           = CPanoramaSymbol (*)(CUIEngine* thisptr, int type, const char* text);
    using onDeletePanel        = void (*)(CUIEngine* thisptr, CPanel2D* panel);
    using RegisterEventHandler = void (*)(CUIEngine* thisptr, CPanoramaSymbol eventName,
                                          CUIPanel* panel, CUtlAbstractDelegate* handler);
};
```

These details matter:

- `runScript` takes two extra args beyond `(contextPanel, scriptSource)`: an `originFile`
  string and a `line` number. These are only used for Panorama's error/console attribution, so you
  can pass a constant like `"filmmaker"` and `0`. You still must pass them or the call signature is
  wrong and the stack will be corrupted.
- `makeSymbol` takes an `int type` first argument (not just the name). Pass the symbol type the
  engine expects; the existing wrapper in this fork already resolves `makeSymbol`
  (`DeathMsg.cpp:1346-1351`), so reuse it instead of re-deriving the type.
- Cleanup goes through `onDeletePanel(engine, CPanel2D*)`, not an ad-hoc free. This is the
  correct, engine-sanctioned deletion path; use it when tearing down the filmmaker panels on reload.
- Panel event callbacks (slider changed, text submit, dropdown changed) are attached with
  `RegisterEventHandler(engine, eventSymbol, panel, delegate)`. Osiris' handler shapes live in
  `Source/EntryPoints/GuiEntryPoints.h` (e.g. `hueSliderValueChanged`, `hueSliderTextEntrySubmit`,
  `dropDownSelectionChanged`). For the filmmaker tool, you can skip C++ delegates and use
  Panorama's `onactivate`/`onvaluechanged` attributes in JS that write to the command-queue
  attribute (see "Command bridge" below). This is simpler and holds up better across updates.

#### The CUIPanel wrapper

`CUIPanel` (`Source/CS2/Panorama/CUIPanel.h`) is the engine-side panel object. It wraps a
`CPanel2D* clientPanel` and exposes the handful of operations you need:

```text
setParent(CUIPanel* parent)
setVisible(bool)
getAttributeString(symbol, defaultValue) -> const char*
setAttributeString(symbol, value)
```

It also carries an `EPanelFlag` enum (visibility / "is from layout file" bits) and the panel id
(`m_pchID`). This fork already resolves `getAttributeString`/`setAttributeString`
(`DeathMsg.cpp:1318-1343`), which is the whole C++→JS / JS→C++ bridge you need.

#### How to resolve these at runtime

Modern Osiris does not hardcode vtable indices for these. It locates each function as a raw
function pointer via pattern search and caches it (its `HookContext` pattern-result system).
This fork already uses that style in
`DeathMsg.cpp:getPanoramaAddrsFromClient()`: signature-scan `client.dll`, read a relative offset,
store the resolved pointer/offset. Add `runScript` (and, if needed, `getPanelPointer` /
`onDeletePanel`) the same way, keeping all of the fragile reverse-engineered scanning in that one
place so a CS2 update only ever requires fixing signatures in a single file.

### 2. Existing root panel lookup

You need a known CS2 panel to attach your UI into. Good targets:

```text
Main menu panel
Settings panel
HUD root panel
Demo playback HUD panel
```

For a first prototype, the main menu/settings route is easier than HUD injection because the settings menu already has useful styles and controls loaded.

The Osiris approach does this:

```text
1. Get main menu panel.
2. Force-load the CS2 settings tab.
3. Find the settings panel named JsSettings.
4. Run custom JS inside that panel.
5. Move the new custom panel under the main menu content container.
6. Add a navbar button that opens the custom tab.
```

The key idea is to reuse CS2's existing panel hierarchy instead of creating a totally separate root.

### 3. Embedded Panorama JavaScript

Your UI should be authored as Panorama JavaScript. You can embed it as a C++ string or load it from a resource file in development.

A simple embedded script would look like this:

```js
(function () {
  var root = $.CreatePanel('Panel', $.GetContextPanel(), 'FilmmakerMenuTab', {
    class: 'mainmenu-content__container',
    useglobalcontext: 'true'
  });

  root.visible = false;
  root.SetReadyForDisplay(false);

  var navbar = $.CreatePanel('Panel', root, 'FilmmakerNavbar', {
    class: 'content-navbar__tabs content-navbar__tabs--noflow'
  });

  var center = $.CreatePanel('Panel', navbar, 'FilmmakerNavbarCenter', {
    class: 'content-navbar__tabs__center-container'
  });

  var cameraButton = $.CreatePanel('RadioButton', center, 'camera_button', {
    group: 'FilmmakerNavBar',
    class: 'content-navbar__tabs__btn',
    onactivate: '$.Filmmaker.navigateToTab("camera");'
  });

  $.CreatePanel('Label', cameraButton, '', { text: 'Camera' });

  var content = $.CreatePanel('Panel', root, 'FilmmakerContent', {
    class: 'full-width full-height'
  });

  var cameraTab = $.CreatePanel('Panel', content, 'camera', {
    class: 'SettingsMenuTab'
  });

  var cameraTabContent = $.CreatePanel('Panel', cameraTab, '', {
    class: 'SettingsMenuTabContent vscroll'
  });

  $.Filmmaker = {
    rootPanel: root,

    addCommand: function (command, value) {
      var oldCommands = root.GetAttributeString('cmd', '');
      root.SetAttributeString('cmd', oldCommands + command + ' ' + value + '\n');
    },

    navigateToTab: function (tabId) {
      cameraTab.visible = false;
      cameraTab.SetReadyForDisplay(false);

      var panel = root.FindChildInLayoutFile(tabId);
      panel.visible = true;
      panel.SetReadyForDisplay(true);
    }
  };
})();
```

That gives you a real Panorama panel, not a rendered overlay.

## What a per-frame panel costs, and the one rule that keeps it cheap

Every open panel is pumped once per main-thread frame: the C++ side writes this frame's state onto
the root panel as an attribute, then calls `render()` through `RunScript`. The attribute half is
nearly free. The `RunScript` call is the expensive part, for one specific reason.

**`CUIEngine::RunScript` caches a compiled script against its origin file, and consults that cache
only when the origin line is 0.** `PanoramaBridge::RunScript` bumps the line on every call, which it
has to: its source changes from call to call. But the frame loop's scripts do not change. They are
one constant line per panel, `$.CamTimeline && $.CamTimeline.render();` and its siblings. Each
one pays a full V8 compile, every frame, for the identical eleven words.

Measured in a Mirage replay at 1600x1200 (`mvm_debug perf on`, averages over 120-frame windows):

| | frame | `camtl` pump | `cameditor` pump |
| --- | --- | --- | --- |
| no workspace open | 6.5 ms (153 fps) | 1.00 ms (0.87 of it the `RunScript`) | -- |
| Camera Editor open | 8.4 ms (118 fps) | 1.47 ms | 1.45 ms |

That is ~3 ms of fixed main-thread cost, which is a 23% frame-rate loss at 150 fps and closer to
half at 300.

A script whose text is a compile-time constant goes through `RunScriptCached(source,
originFile)`, which passes line 0 and a per-call-site origin. Both of the following are requirements:

- the origin must be unique per script text (`"mvm/camtl.render"`, `"mvm/cameditor.render"`, ...).
  Line 0 with a shared origin is worse than no cache at all: every call site collides on
  `"filmmaker":0`, the cache hands back some other site's compiled script, and the workspace panels
  silently stop building;
- the source passed under an origin must never vary. Pass a different string under the same origin
  and Panorama will keep running the first one forever.

Anything generated (state pushes, `eval`, built fragments) keeps using plain `RunScript`.

`mvm_debug perf on` arms the probe (`Panorama/PanoramaPerfProbe.h`) and `mvm_debug perf` prints the
last window; `mvm_debug start` arms it automatically and writes every window to the log as
`perf.ui`. Each slot reports `average/worst-frame` in ms, so a pump that is cheap on average and
occasionally spikes is distinguishable from one that is uniformly expensive.

## Attaching a tab to the main menu

For a native-looking main menu tab, use the same pattern as Osiris:

```js
(function () {
  var mainMenuContent = $('#JsMainMenuContent');
  var root = $('#FilmmakerMenuTab');

  root.SetParent(mainMenuContent);

  var navParent = $.GetContextPanel()
    .FindChildTraverse('MainMenuNavBarSettings')
    .GetParent();

  var openButton = $.CreatePanel('RadioButton', navParent, 'FilmmakerOpenMenuButton', {
    class: 'mainmenu-top-navbar__radio-iconbtn',
    group: 'NavBar',
    onactivate: 'MainMenu.NavigateToTab("FilmmakerMenuTab", "");'
  });

  $.CreatePanel('Image', openButton, '', {
    class: 'mainmenu-top-navbar__radio-btn__icon',
    src: 's2r://panorama/images/icons/ui/bug.vsvg'
  });
})();
```

For a filmmaker tool, replace the icon with a camera-like built-in icon if one exists in CS2's Panorama image assets.

## Reusing native CS2 styles

The native look mainly comes from using Valve's existing panel classes and controls.

Useful classes seen in the Osiris UI:

```text
mainmenu-content__container
mainmenu-top-navbar__radio-iconbtn
mainmenu-top-navbar__radio-btn__icon
content-navbar__tabs
content-navbar__tabs--dark
content-navbar__tabs--noflow
content-navbar__tabs__center-container
content-navbar__tabs__btn
full-width
full-height
vscroll
SettingsMenuTab
SettingsMenuTabContent
SettingsBackground
SettingsSectionTitleContianer
SettingsSectionTitleLabel
SettingsMenuDropdownContainer
HorizontalSlider
PopupButton
White
horizontal-separator
negativeColor
```

Useful built-in Panorama panel types:

```text
Panel
Label
Button
RadioButton
Image
Slider
TextEntry
CSGOSettingsEnumDropDown
MapItemPreviewPanel
MapPlayerPreviewPanel
```

These are what make the UI feel like CS2 instead of an overlay.

## Common UI helpers

### Section helper

```js
function createSection(parent, sectionName) {
  var background = $.CreatePanel('Panel', parent, '', {
    class: 'SettingsBackground'
  });

  var titleContainer = $.CreatePanel('Panel', background, '', {
    class: 'SettingsSectionTitleContianer'
  });

  $.CreatePanel('Label', titleContainer, '', {
    class: 'SettingsSectionTitleLabel',
    text: sectionName
  });

  var content = $.CreatePanel('Panel', background, '', {
    class: 'top-bottom-flow full-width'
  });

  return content;
}
```

### Separator helper

```js
function separator(parent) {
  $.CreatePanel('Panel', parent, '', {
    class: 'horizontal-separator'
  });
}
```

### Dropdown helper

```js
function createDropDown(parent, labelText, commandPrefix, feature, options) {
  var container = $.CreatePanel('Panel', parent, '', {
    class: 'SettingsMenuDropdownContainer'
  });

  $.CreatePanel('Label', container, '', {
    class: 'half-width',
    text: labelText
  });

  var dropdown = $.CreatePanel('CSGOSettingsEnumDropDown', container, feature, {
    class: 'PopupButton White'
  });

  for (var i = 0; i < options.length; ++i) {
    dropdown.AddOption($.CreatePanel('Label', dropdown, String(i), {
      value: i,
      text: options[i]
    }));
  }

  dropdown.SetPanelEvent('oninputsubmit', function () {
    $.Filmmaker.addCommand('set', commandPrefix + '/' + feature + '/' + dropdown.GetSelected().id);
  });
}
```

### Slider helper

```js
function createSlider(parent, name, commandPrefix, id, min, max) {
  var container = $.CreatePanel('Panel', parent, '', {
    class: 'SettingsMenuDropdownContainer'
  });

  $.CreatePanel('Label', container, '', {
    class: 'half-width',
    text: name
  });

  var sliderContainer = $.CreatePanel('Panel', container, id, {
    style: 'vertical-align: center; horizontal-align: right; flow-children: right; margin-right: 8px;'
  });

  var slider = $.CreatePanel('Slider', sliderContainer, '', {
    class: 'HorizontalSlider',
    style: 'width: 200px; vertical-align: center;',
    direction: 'horizontal'
  });

  slider.min = min;
  slider.max = max;
  slider.increment = 1.0;

  var textEntry = $.CreatePanel('TextEntry', sliderContainer, id + '_text', {
    maxchars: '3',
    textmode: 'numeric',
    style: 'width: 75px; margin-left: 10px; padding-left: 10px; text-align: center; font-size: 20px; color: #ccccccff; font-weight: bold; font-family: Stratum2, notosans, Arial; border: 2px solid #cccccc15;'
  });

  slider.SetPanelEvent('onvaluechanged', function () {
    $.Filmmaker.addCommand('set', commandPrefix + '/' + id + '/' + Math.floor(slider.value));
  });

  textEntry.SetPanelEvent('ontextentrysubmit', function () {
    $.Filmmaker.addCommand('set', commandPrefix + '/' + id + '_text/' + textEntry.text);
  });
}
```

## Workspace chrome: the column wash and the animated wordmark

Both live in the shared `WorkspaceShellJs.h`, spliced into the Camera Editor and CONFIG scripts, so
the three workspaces cannot drift apart.

**Column wash.** Every inspector column carries the Render workspace's background: CS2's GO-button
line map (`file://{resources}/videos/linemap.webm`) at 2% opacity over `rgba(15,17,20,1)`, tinted
with the workspace accent through `washColor`, grey for Render
(`CameraEditorRenderLayoutJs.h`), gold for the Camera Editor, blue for CONFIG
(`mvmColumnWash(inspector, S.accent, UI.col.pad)`). The wash is a separate first child, never a
property of the column, because `washColor` multiplies a panel's whole content and would tint every
control. The inspector is padded, resizable and re-heighted per frame, so `sync()` (called from
`render()`) places the layer at (-padding, -padding) and sizes it in px to the measured column.

**Wordmark.** The letterbox title ("CONFIG / SYSTEM", "CAMERA / EDITOR", "RENDER / OUTPUT") loops,
early-2000s soft-club style:

1. In (1.25 s): it streaks in from the left on an exponential ease-out. Its horizontal blur
   (`blur: gaussian(h, v, 1)`) is derived from its speed, so it is a smear at full whip and resolves
   to a soft focus as it lands; letter-spacing closes from wide to rest, three trailing echoes
   (`decorTrails`) sample its path a few ms in the past and collapse onto it, the code line decodes
   from scrambled glyphs, and the accent rule draws on.
2. Hold (6.5 s): readable, with a breathing `textShadow` bloom. The ghost word drifts behind.
3. Out (0.42 s): an exponential ease-in whip to the right, blurring again, then it repeats.

A title change (Editor -> Render) or reopening the workspace restarts the sweep.

- Panorama clips a panel's blur and text-shadow to the panel's own box, so every animated label is
  padded (`wmRoom`) and pulls its margins in by the same amount; without that the smear is cut off flat.
- Only paint properties move (transform, blur, glow, opacity, letter-spacing on a bottom-anchored
  label inside the clipped decor), so nothing outside the decor reflows.
- The frame loop checks `IsValid()` before touching the decor, and is wrapped so a throw ends the loop:
  a workspace can be torn down between two scheduled frames, and any property read on a deleted panel
  throws.
- `$.CamEditor.wordmark()` / `$.ConfigHud.wordmark()` replay it; with a number they freeze it at that
  loop time for captures (`mirv_filmmaker config eval $.ConfigHud.wordmark(0.35)`), which is also how
  `UiMockup/capture-mock.mjs` shoots `wordmark_streak` / `_settle` / `_rest` / `_out`. The mockup's
  blur is isotropic. The directional smear only shows in game.

## Command bridge from JS to C++

The simplest bridge is a string attribute on your root panel.

JavaScript writes commands:

```js
$.Filmmaker.addCommand('set', 'camera/sway/25');
```

That writes:

```text
set camera/sway/25
```

C++ reads it each frame:

```cpp
void FilmmakerPanoramaGui::run()
{
    auto panel = uiEngine.getPanelFromHandle(guiPanelHandle);
    if (!panel)
        return;

    const auto cmdSymbol = uiEngine.makeSymbol("cmd");
    const char* commands = panel.getAttributeString(cmdSymbol, "");

    dispatchCommands(commands);

    panel.setAttributeString(cmdSymbol, "");
}
```

Command parser shape:

```text
set camera/sway/25
set camera/fov/90
set demo/playback_speed/50
open campath/editor
close filmmaker
```

Recommended command format:

```text
verb section/feature/value
```

Examples:

```text
set camera/sway/25
set viewmodel/fov/68
set hud/xray/1
button campath/add_keyframe
button recording/start
button recording/stop
```

Keep this bridge plain and explicit. Do not let arbitrary Panorama JS execute arbitrary console commands.

## Cleanup

On unload, remove your UI panels and global JS object.

C++ cleanup:

```cpp
void FilmmakerPanoramaGui::onUnload()
{
    uiEngine.deletePanelByHandle(guiButtonHandle);
    uiEngine.deletePanelByHandle(guiPanelHandle);

    if (auto settings = uiEngine.getPanelFromHandle(settingsPanelHandle))
        uiEngine.runScript(settings, "delete $.Filmmaker;");
}
```

Why cleanup matters:

```text
Prevents duplicate buttons after reload
Prevents stale JS state
Prevents dangling panel handles
Makes development reloads tolerable
```

## Development layout

The code lives in `AfxHookSource2/Filmmaker/Panorama/`:

```text
PanoramaBridge.cpp/.h          engine wrapper (RunScript, attributes, context-panel pinning)
PanoramaFindPanel.cpp/.h       shared child-panel lookup
*Hud.cpp, FilmmakerMenu.cpp    per-panel bridges
*Js.h                          embedded Panorama JS, one raw-string header per panel or fragment
```

Edit the shared `*Js.h` headers. To rerun a JS file from disk without rebuilding, use
`mirv_filmmaker livejs` (see `UiLiveReload.cpp`).

## Minimal first milestone

Build the smallest useful proof of concept:

```text
1. Find main menu panel.
2. Run a small JS script through Panorama.
3. Create FilmmakerMenuTab.
4. Add one navbar button.
5. Add one Camera tab.
6. Add one slider named Camera Sway.
7. Slider writes set camera/sway/value to root panel attribute.
8. C++ reads the attribute and prints the parsed command to HLAE console.
9. Unload removes the panel and button cleanly.
```

Do not start with a full UI. First prove that native panel creation, command passing, config sync, and cleanup work.

## Suggested filmmaker UI tabs

After the first proof of concept works:

```text
Camera
  Camera sway
  Smooth follow
  FOV
  Roll
  Speed presets
  Attach to player or weapon attachment

Campath
  Add keyframe
  Remove keyframe
  Play path
  Ease type
  Export path
  Import path

Demo
  Play/pause
  Playback speed
  Jump tick
  Previous/next kill
  Previous/next round

Viewmodel
  Preview FOV
  Viewmodel offset
  Weapon/knife preview for offline filming

HUD
  X-ray toggle
  Clean HUD toggle
  Killfeed options
  Player labels

Recording
  Start take
  Stop take
  Output folder
  Motion blur / depth / streams status
```

## Applying config values back into UI

After the UI exists, C++ should push current config values into the controls.

Example:

```cpp
void FilmmakerPanoramaGui::updateFromConfig()
{
    auto root = uiEngine.getPanelFromHandle(guiPanelHandle);
    if (!root)
        return;

    setSliderValue(root.findChildInLayoutFile("camera_sway"), config.cameraSway);
    setDropdownValue(root.findChildInLayoutFile("camera_sway_enable"), config.cameraSwayEnabled ? 0 : 1);
}
```

The important rule:

```text
JS creates controls.
C++ owns config truth.
C++ pushes config into controls after creation or reload.
JS only reports user changes.
```

## Preview panels

CS2 has useful native 3D preview panel types:

```text
MapItemPreviewPanel
MapPlayerPreviewPanel
```

Osiris uses `MapItemPreviewPanel` for the viewmodel preview and `MapPlayerPreviewPanel` for player model previews.

A generic weapon preview panel looks like:

```js
var preview = $.CreatePanel('MapItemPreviewPanel', parent, 'WeaponPreview', {
  map: 'ui/xpshop_item',
  camera: 'camera_weapon_0',
  'require-composition-layer': true,
  player: false,
  initial_entity: 'item',
  mouse_rotate: false,
  sync_spawn_addons: true,
  'transparent-background': true,
  'pin-fov': 'vertical',
  style: 'width: 700px; height: 400px;'
});

preview.SetHideStaticGeometry(true);
```

For filmmaker work, preview panels are useful for:

```text
Viewmodel FOV preview
Weapon/knife preview
Player model preview
Agent/skin preview for offline film setup
```

### These panels do not work by themselves on a paused demo

A `MapPlayerPreviewPanel` spawns real client entities into a real scene, and all three of the
things that make that scene appear are driven by the game clock. On a paused demo (the normal
state while dressing a shot), the panel is created with the correct attributes, reports valid,
and still shows a black box, a frozen character, or a character's weapon with no character.

Panel attributes cannot fix this; the native `vanity-loadout.xml` matches our creation call
attribute-for-attribute and animates fine in the main menu, where nothing is paused. Three
engine-side gates have to be opened, and they are implemented in
`AfxHookSource2/Filmmaker/Panorama/PreviewRenderProbe`, `PreviewAnimTick` and `PreviewRenderFlag`.
Before adding a new 3D preview panel anywhere that can run under a paused demo, read
[customize-feature.md § Paused preview](../features/customize-feature.md#paused-preview). The same three gates
will apply, and the symptoms map one-to-one.

## Main risks

### Updates break internal access

Panorama JS code is fairly stable. The C++ access to the UI engine and panel objects is the fragile part.

Keep the fragile layer small:

```text
PanoramaUiEngineBridge
PanoramaUiPanel wrapper
Panel handle wrapper
Style property wrapper only if needed
```

Do not scatter raw internal calls across feature code.

### CS2 class names can change

Valve UI class names may change. Keep them in one JS/CSS layer so UI fixes do not touch C++.

### Duplicate panels during reload

Always delete existing panels before creating new ones.

### JS command injection

Do not execute arbitrary strings from Panorama. Parse a small command grammar and reject unknown commands.

### Online use

This belongs in offline/demo/HLAE use. Do not design it for VAC servers.

## Best implementation strategy for HLAE

Recommended plan:

```text
Phase 1: Research branch
  Add FilmmakerPanoramaBridge.
  Add runScript proof of concept.
  Create one tab and one slider.
  Print commands only.

Phase 2: Safe UI framework
  Add reload command.
  Add cleanup.
  Add config sync.
  Add command parser.
  Add error logging.

Phase 3: Filmmaker controls
  Camera tab.
  Demo tab.
  Campath tab.
  HUD tab.

Phase 4: HUD integration
  Try attaching panels under HUD root for demo playback.
  Keep main menu settings tab working as fallback.

Phase 5: Polish
  Use CS2-like spacing, icons, labels, and preview panels.
  Move large JS into resources.
  Add build stamp and debug panel.
```

## What to copy from Osiris conceptually

Copy these ideas:

```text
Use CS2's Panorama engine.
Run custom JS in an existing context panel.
Create panels with $.CreatePanel.
Reuse Valve CSS classes.
Use a root panel attribute as a command queue.
Have C++ poll and clear that command queue each frame.
Clean up panels on unload.
Use MapItemPreviewPanel and MapPlayerPreviewPanel for native previews.
```

Do not copy these parts blindly:

```text
Cheat feature hooks
Live gameplay features
Injection instructions
Pattern signatures
Anti-cheat related assumptions
Large global feature architecture
```

## Source references used for this guide

Public files inspected:

```text
Osiris README
https://github.com/danielkrupinski/Osiris/blob/master/README.md

Osiris Panorama GUI wrapper
https://github.com/danielkrupinski/Osiris/blob/master/Source/UI/Panorama/PanoramaGUI.h

Osiris embedded Panorama JavaScript
https://github.com/danielkrupinski/Osiris/blob/master/Source/UI/Panorama/CreateGUI.js

Osiris Panorama UI engine wrapper
https://github.com/danielkrupinski/Osiris/blob/master/Source/GameClient/Panorama/PanoramaUiEngine.h

Osiris panel wrapper
https://github.com/danielkrupinski/Osiris/blob/master/Source/GameClient/Panorama/PanoramaUiPanel.h

Osiris command bridge
https://github.com/danielkrupinski/Osiris/blob/master/Source/UI/Panorama/PanoramaCommandDispatcher.h
https://github.com/danielkrupinski/Osiris/blob/master/Source/UI/Panorama/SetCommandHandler.h

Osiris viewmodel preview panel
https://github.com/danielkrupinski/Osiris/blob/master/Source/UI/Panorama/Tabs/VisualsTab/ViewmodelModPreviewPanel.h

Osiris init and per-frame run location
https://github.com/danielkrupinski/Osiris/blob/master/Source/EntryPoints/EntryPoints.h
```

## One-sentence summary

To make native-looking CS2 UI, build a tiny C++ Panorama bridge, run custom JS inside an existing CS2 panel, create real Panorama panels with Valve classes and controls, then bridge UI actions back to your filmmaker code through a small command queue.
