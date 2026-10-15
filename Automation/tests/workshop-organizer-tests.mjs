// Tests for the workshop map ORGANISER: the Play page's sort modes, keyword categories and
// "hide training maps" toggle, plus the contract that joins the injected Panorama JS to the
// C++ that backs it.
//
// Two kinds of check live here, same split as demo-organizer-tests.mjs:
//
//   1. CONTRACT checks, comparing PlayMenu/*Js.h against WorkshopOrganizer.cpp,
//      WorkshopOrganizerStore.cpp and PlayMenuHud.cpp. The two sides agree only by convention:
//      the sort-mode ids, the "ws ..." command verbs and the host attribute names are plain
//      strings crossing a JSON / command-queue boundary with no compiler to catch a typo. A
//      mismatch degrades SILENTLY -- the sidebar keeps working and simply stops persisting.
//
//   2. BEHAVIOUR checks, running the SHIPPING filter fragment against stub Panorama panels.
//      Classification, the category/hide-training interaction, composition with the native
//      search box and the sort order are all exercised through M.wsApply(), not a
//      reimplementation.
//
// Run: node --test Automation/tests/workshop-organizer-tests.mjs

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const testDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(testDirectory, "..", "..");
const playMenuDirectory = path.join(repositoryRoot, "AfxHookSource2", "Filmmaker", "PlayMenu");

const stripJsSource = fs.readFileSync(path.join(playMenuDirectory, "PlayMenuStripJs.h"), "utf8");
const filterJsSource = fs.readFileSync(path.join(playMenuDirectory, "WorkshopFilterJs.h"), "utf8");
const editorJsSource = fs.readFileSync(path.join(playMenuDirectory, "WorkshopKeywordEditorJs.h"), "utf8");
const hudSource = fs.readFileSync(path.join(playMenuDirectory, "PlayMenuHud.cpp"), "utf8");
const organizerSource = fs.readFileSync(
  path.join(repositoryRoot, "AfxHookSource2", "Filmmaker", "Workshop", "WorkshopOrganizer.cpp"),
  "utf8",
);
const storeSource = fs.readFileSync(
  path.join(repositoryRoot, "AfxHookSource2", "Filmmaker", "Config", "WorkshopOrganizerStore.cpp"),
  "utf8",
);

// Pulls the raw-string-literal fragments a *Js.h header is built from.
function rawStringFragments(source, tag) {
  const pattern = new RegExp(`R"${tag}\\(([\\s\\S]*?)\\)${tag}"`, "g");
  const fragments = [];
  let match;
  while ((match = pattern.exec(source)) !== null) fragments.push(match[1]);
  assert.ok(fragments.length > 0, `${tag} fragments must exist`);
  return fragments;
}

const stripJs = rawStringFragments(stripJsSource, "FMJS").join("\n");
const filterJs = rawStringFragments(filterJsSource, "FMJS").join("\n");
const editorJs = rawStringFragments(editorJsSource, "FMJS").join("\n");

// Comment-stripped views, for the checks that ban an API by name. The fragments document WHY a
// given API is banned, and a naive text scan would flag that explanation and make the guard
// unusable.
function withoutComments(js) {
  return js
    .split("\n")
    .filter((line) => !/^\s*\/\//.test(line))
    .join("\n");
}
const stripCode = withoutComments(stripJs);
const filterCode = withoutComments(filterJs);
const editorCode = withoutComments(editorJs);

// ---------------------------------------------------------------------------
// Stub Panorama panels. Only the surface the filter fragment actually touches.
// ---------------------------------------------------------------------------
function makeTile(attributes, title) {
  const tile = {
    visible: true,
    attributes: { ...attributes },
    GetAttributeString(name, fallback) {
      return name in this.attributes ? this.attributes[name] : fallback;
    },
    FindChildTraverse(id) {
      return id === "MapGroupName" ? { text: title } : null;
    },
    IsValid: () => true,
  };
  return tile;
}

function makeContainer(tiles) {
  const order = [...tiles];
  return {
    order,
    IsValid: () => true,
    Children() {
      return [...this.order];
    },
    GetChild(index) {
      return this.order[index] || null;
    },
    MoveChildBefore(child, before) {
      const from = this.order.indexOf(child);
      if (from >= 0) this.order.splice(from, 1);
      const to = this.order.indexOf(before);
      this.order.splice(to < 0 ? this.order.length : to, 0, child);
    },
  };
}

const sampleWords = ["aim", "surf", "warmup", "config", "generator"];

// Evaluates the SHIPPING filter fragment against stubs and hands back its public surface.
// A fresh instance per test, because the fragment keeps module-level state (the sort
// signature that suppresses redundant reordering).
function loadFilter({ tiles, payload, searchText = "" }) {
  const container = makeContainer(tiles);
  const searchEntry = { text: searchText, visible: true, IsValid: () => true };
  const commands = [];

  const M = {
    host: { GetAttributeString: () => JSON.stringify(payload) },
    hooks: [],
    cmd: (line) => commands.push(line),
    find(id) {
      if (id === "gameModeButtonContainer_workshop") return container;
      if (id === "WorkshopSearchTextEntry") return searchEntry;
      return null;
    },
  };

  const dollar = {
    MvmPlay: M,
    Schedule: () => {},
    RegisterEventHandler: () => {},
    CreatePanel: () => ({ style: {}, SetPanelEvent: () => {}, AddOption: () => {} }),
    Msg: () => {},
  };

  // eslint-disable-next-line no-new-func
  new Function("$", filterJs)(dollar);
  M.renderWs();
  return { M, container, searchEntry, commands, visibleNames: () =>
    container.order.filter((t) => t.visible).map((t) => t.GetAttributeString("addon", "")) };
}

function payloadWith(overrides = {}) {
  return {
    sort: "name",
    hideTraining: false,
    root: "F:\\SteamLibrary\\steamapps\\workshop\\content\\730",
    words: [...sampleWords],
    items: {},
    ...overrides,
  };
}

// Three representative tiles: one aim map, one movement map, one plain competitive map.
function sampleTiles() {
  return [
    makeTile({ mapname: "@workshop/2/aim_botz", addon: "2", "data-tooltip": "Aim training map" }, "Aim Botz"),
    makeTile({ mapname: "@workshop/3/surf_beginner", addon: "3", "data-tooltip": "" }, "Surf Beginner"),
    makeTile({ mapname: "@workshop/4/de_cache", addon: "4", "data-tooltip": "Classic defuse map" }, "Cache"),
  ];
}

// ---------------------------------------------------------------------------
// Behaviour
// ---------------------------------------------------------------------------
test("no filtering shows every tile", () => {
  const h = loadFilter({ tiles: sampleTiles(), payload: payloadWith() });
  h.M.wsApply();
  assert.deepEqual(h.visibleNames().sort(), ["2", "3", "4"]);
});

test("hide training maps hides anything a keyword matches", () => {
  const h = loadFilter({ tiles: sampleTiles(), payload: payloadWith({ hideTraining: true }) });
  h.M.wsApply();
  // aim_botz matches "aim", surf_beginner matches "surf"; de_cache matches nothing.
  assert.deepEqual(h.visibleNames(), ["4"]);
});

test("keywords match the map name and title ONLY, never the description", () => {
  // The in-game regression this exists to stop: "Echolab" was hidden because its workshop
  // DESCRIPTION mentioned training. A blurb about a map is not a statement of what the map is.
  const tiles = [
    makeTile(
      { mapname: "@workshop/9/echolab", addon: "9", "data-tooltip": "Great for practising your aim and warmup routines" },
      "Echolab",
    ),
    makeTile({ mapname: "@workshop/8/de_cache", addon: "8", "data-tooltip": "" }, "Cache"),
  ];
  const h = loadFilter({ tiles, payload: payloadWith({ hideTraining: true }) });
  h.M.wsApply();
  assert.deepEqual(h.visibleNames().sort(), ["8", "9"]);
});

test("keywords match the workshop title even when the map name is opaque", () => {
  // The other half of the same regression: "config generator" survived hiding because nothing
  // matched. Its map name is meaningless, so the title has to be matched too.
  const tiles = [
    makeTile({ mapname: "@workshop/7/cfggen_v3", addon: "7", "data-tooltip": "" }, "Config Generator"),
    makeTile({ mapname: "@workshop/8/de_cache", addon: "8", "data-tooltip": "" }, "Cache"),
  ];
  const h = loadFilter({ tiles, payload: payloadWith({ hideTraining: true }) });
  h.M.wsApply();
  assert.deepEqual(h.visibleNames(), ["8"]);
});

test("the search box still searches the description, matching native behaviour", () => {
  // Only the KEYWORD match is name-restricted. The user typed the search text on purpose, and
  // native _UpdateWorkshopMapFilter searches modes and the tooltip too.
  const tiles = [
    makeTile({ mapname: "@workshop/9/echolab", addon: "9", "data-tooltip": "A hostage scenario" }, "Echolab"),
    makeTile({ mapname: "@workshop/8/de_cache", addon: "8", "data-tooltip": "" }, "Cache"),
  ];
  const h = loadFilter({ tiles, payload: payloadWith(), searchText: "hostage" });
  h.M.wsApply();
  assert.deepEqual(h.visibleNames(), ["9"]);
});

test("our filter composes with the native search box instead of overriding it", () => {
  const h = loadFilter({ tiles: sampleTiles(), payload: payloadWith(), searchText: "surf" });
  h.M.wsApply();
  assert.deepEqual(h.visibleNames(), ["3"]);
});

test("search and hide-training intersect", () => {
  const h = loadFilter({
    tiles: sampleTiles(),
    payload: payloadWith({ hideTraining: true }),
    searchText: "surf",
  });
  h.M.wsApply();
  // "surf" passes the search but is hidden as a training map, so nothing survives both.
  assert.deepEqual(h.visibleNames(), []);
});

test("an empty keyword list makes the hide toggle a no-op instead of hiding everything", () => {
  const h = loadFilter({ tiles: sampleTiles(), payload: payloadWith({ hideTraining: true, words: [] }) });
  h.M.wsApply();
  assert.deepEqual(h.visibleNames().sort(), ["2", "3", "4"]);
});

test("recently installed sorts newest first", () => {
  const items = { 2: { installed: 100, played: 0 }, 3: { installed: 300, played: 0 }, 4: { installed: 200, played: 0 } };
  const h = loadFilter({ tiles: sampleTiles(), payload: payloadWith({ sort: "installed", items }) });
  h.M.wsApply();
  assert.deepEqual(h.container.order.map((t) => t.GetAttributeString("addon", "")), ["3", "4", "2"]);
});

test("recently played sorts newest first and keeps never-played tiles in name order", () => {
  const items = { 2: { installed: 0, played: 0 }, 3: { installed: 0, played: 500 }, 4: { installed: 0, played: 0 } };
  const h = loadFilter({ tiles: sampleTiles(), payload: payloadWith({ sort: "played", items }) });
  h.M.wsApply();
  // Surf played most recently; the two never-played tiles fall back to title order (Aim, Cache).
  assert.deepEqual(h.container.order.map((t) => t.GetAttributeString("addon", "")), ["3", "2", "4"]);
});

test("name sort is codepoint order on the workshop title", () => {
  const h = loadFilter({ tiles: sampleTiles(), payload: payloadWith({ sort: "name" }) });
  h.M.wsApply();
  assert.deepEqual(h.container.order.map((t) => t.GetAttributeString("addon", "")), ["2", "4", "3"]);
});

test("tiles without a mapname attribute are left alone", () => {
  // The native page drops a "no workshop maps" placeholder into the same container; hiding or
  // reordering it would break the empty state.
  const tiles = [...sampleTiles(), makeTile({ addon: "" }, "")];
  const h = loadFilter({ tiles, payload: payloadWith({ hideTraining: true }) });
  h.M.wsApply();
  assert.equal(tiles[3].visible, true);
});

// ---------------------------------------------------------------------------
// Contract
// ---------------------------------------------------------------------------
test("sort ids match the C++ whitelist", () => {
  // WorkshopOrganizerStore::SetSort rejects anything not in kModes, so a sort id the sidebar
  // offers but the store refuses would silently never persist.
  const modes = [...storeSource.matchAll(/"(name|installed|played)"/g)].map((m) => m[1]);
  for (const mode of ["name", "installed", "played"]) {
    assert.ok(modes.includes(mode), `${mode} must be in the C++ sort whitelist`);
    assert.ok(filterJs.includes(`'${mode}'`), `${mode} must be offered by the sidebar`);
  }
});

test("every command the JS emits is handled in C++", () => {
  const emitted = [...`${filterJs}\n${editorJs}`.matchAll(/M\.cmd\('ws ([a-z]+)/g)].map((m) => m[1]);
  assert.ok(emitted.length >= 5, "the sidebar must emit the ws commands");
  for (const verb of new Set(emitted)) {
    assert.ok(
      organizerSource.includes(`verb == "${verb}"`),
      `"ws ${verb}" is emitted by the sidebar but not handled by WorkshopOrganizer_HandleCommand`,
    );
  }
});

test("the command prefix agrees on both sides", () => {
  assert.ok(organizerSource.includes(`line.rfind("ws ", 0)`), "C++ must claim the ws prefix");
  assert.ok(/M\.cmd\('ws /.test(filterJs), "JS must emit ws-prefixed commands");
});

test("host attribute names match the symbols C++ interns", () => {
  assert.ok(hudSource.includes('MakeSymbol("ws")'), "C++ must intern the ws payload attribute");
  assert.ok(hudSource.includes('MakeSymbol("cmd")'), "C++ must intern the cmd queue attribute");
  assert.ok(filterJs.includes("GetAttributeString('ws'"), "JS must read the ws payload attribute");
  assert.ok(stripJs.includes("GetAttributeString('cmd'"), "JS must read the cmd queue attribute");
  assert.ok(stripJs.includes("SetAttributeString('cmd'"), "JS must append to the cmd queue");
});

test("the host panel id agrees on both sides", () => {
  assert.ok(hudSource.includes('"MvmPlayMenuHost"'), "C++ must look the host panel up by id");
  assert.ok(stripJs.includes("'MvmPlayMenuHost'"), "JS must create the host panel with that id");
});

test("C++ pushes the payload through the entry point the JS defines", () => {
  assert.ok(hudSource.includes("$.MvmPlay.renderWs()"), "C++ must call the render entry point");
  assert.ok(/M\.renderWs = function/.test(filterJs), "JS must define renderWs");
});

test("the strip targets the native Play tab ids", () => {
  // If CS2 renames these the strip silently stops working, so keep them in one visible place.
  for (const id of ["PlayTypeTopNav", "Play-official", "Play-listen", "PlayCommunity"]) {
    assert.ok(stripJs.includes(`'${id}'`), `${id} must be referenced by the strip`);
  }
});

test("the strip never replaces a native tab handler", () => {
  // Replacing onactivate would desync PlayMenu's own server-setting bookkeeping. Activation is
  // dispatched instead, which runs the native handler.
  // The third argument is load-bearing: native mainmenu.js dispatches ('Activated', panel,
  // 'mouse'), and without the source string the event never reaches onactivate -- the radio
  // highlight moves but PlayMenu's own m_serverSetting stays on the tab we just hid.
  assert.ok(
    stripJs.includes("$.DispatchEvent('Activated', p, 'mouse')"),
    "activation must be dispatched with the 'mouse' source argument",
  );
  assert.ok(
    !/Play-listen'\)[\s\S]{0,200}SetPanelEvent/.test(stripJs),
    "the strip must not SetPanelEvent on a native tab",
  );
});

test("the search box handler is added, not replaced", () => {
  // SetPanelEvent('ontextentrychange') would clobber native _UpdateWorkshopMapFilter and its
  // clear button; RegisterEventHandler runs alongside it.
  assert.ok(filterJs.includes("$.RegisterEventHandler('TextEntryChanged'"));
  assert.ok(!filterCode.includes("ontextentrychange"), "never take over the native text entry event");
});

test("keyword edits commit on Enter, not on activation", () => {
  // onactivate is BUTTON activation and never fires on a TextEntry; Enter is oninputsubmit.
  assert.ok(editorJs.includes("'oninputsubmit'"), "the keyword entries must commit on Enter");
});

test("newlines are stripped before a keyword list enters the command queue", () => {
  // The queue is newline separated, so a pasted newline would split the command in half and the
  // tail would be parsed as its own (unknown) command.
  assert.ok(/replace\(\/\[\\r\\n\]\/g/.test(editorJs));
});

test("game modes are re-enabled on the Workshop tab", () => {
  // Native disables every game-mode radio while the Workshop tab is up, which locks workshop
  // maps to the last-used mode -- usually Wingman, a 2-player mode with no room for bots.
  assert.match(filterCode, /GameModeSelectionRadios/);
  assert.match(filterCode, /id-navbar-tabs/);
  // Must be re-asserted per tick: native re-runs its disabling pass on every tab switch.
  const hook = filterJs.match(/M\.hooks\.push\(function \(\)[\s\S]*?\n    \}\);/);
  assert.ok(hook, "the tick hook must be parsed by this test");
  assert.match(hook[0], /GameModeSelectionRadios/, "re-enabling must live in the per-tick hook");
});

test("no ICU-dependent JS APIs (Panorama's V8 has no ICU data)", () => {
  // Same regression class as the demo browser: localeCompare threw "RangeError: Internal error.
  // Icu error." in-game while every Node test passed, because Node ships full ICU. Static
  // prohibition is the only thing that can catch it off-engine.
  const banned = [
    "localeCompare",
    "toLocaleUpperCase",
    "toLocaleLowerCase",
    "toLocaleDateString",
    "toLocaleTimeString",
    "toLocaleString",
    "Intl.",
  ];
  const code = [stripCode, filterCode, editorCode].join("\n");

  for (const api of banned) {
    assert.ok(
      !code.includes(api),
      `${api} depends on ICU and throws in Panorama's V8; use a plain codepoint compare`,
    );
  }
});

test("the keyword list lives only in C++", () => {
  // The sidebar is data-driven off the payload. Hard-coding a keyword in the JS would give the
  // editor a second source of truth that the store could not overwrite.
  const table = storeSource.match(/kDefaultWords\s*=\s*([\s\S]*?);/);
  assert.ok(table, "the C++ default keyword list must be parsed by this test");
  const words = [...table[1].matchAll(/[a-z ]{2,}/g)]
    .flatMap((m) => m[0].split(","))
    .map((w) => w.trim())
    .filter((w) => w.length >= 4);
  assert.ok(words.length >= 10, "the C++ default keyword list must carry the built-in words");
  for (const word of words) {
    assert.ok(!filterCode.includes(`'${word}'`), `keyword "${word}" must not be hard-coded in the sidebar`);
  }
});

test("no bare 'train' keyword (it would hide de_train)", () => {
  // Substring matching over map names means "train" is not a safe word; "trainer"/"training"
  // are. This was a real false positive class before matching was restricted to names.
  const table = storeSource.match(/kDefaultWords\s*=\s*([\s\S]*?);/);
  const words = table[1]
    .split(",")
    .map((chunk) => chunk.replace(/["\s]/g, ""))
    .filter(Boolean);
  assert.ok(!words.includes("train"), "a bare 'train' keyword hides de_train");
});

test("the keyword editor is a centred overlay, not an inline sidebar section", () => {
  // The play page's left sidebar is a narrow fixed column; a wide text field inline in it
  // stretched the whole sidebar and pushed the sort/hide controls out of view.
  //
  // The centring, z-order and click-eating backdrop all live in the shared M.makeOverlay helper
  // now, so this asserts the editor USES it rather than re-checking the styles here -- the
  // helper itself lives in PlayMenuStripJs.h and is checked below.
  assert.match(editorJs, /M\.makeOverlay\('MvmWsKeywordPopup'/);
  assert.ok(
    !editorJs.includes("horizontalAlign = 'center'"),
    "the editor must not hand-roll centring alongside the shared helper",
  );
  const strip = fs.readFileSync(path.join(playMenuDirectory, "PlayMenuStripJs.h"), "utf8");
  assert.match(strip, /horizontalAlign = 'center'/, "the shared helper must centre the panel");
  assert.match(strip, /zIndex/, "the shared helper must set a z-order");
});

test("no category controls remain in the sidebar", () => {
  // The categories were removed on purpose; one toggle over one list replaced seven toggles.
  assert.ok(!filterCode.includes("MvmWsCat_"), "category toggles must be gone");
  assert.ok(!/M\.cmd\('ws cat /.test(filterCode), "the ws cat command must be gone");
  assert.ok(!organizerSource.includes('verb == "cat"'), "C++ must not still handle ws cat");
});
