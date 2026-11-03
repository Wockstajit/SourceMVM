// Tests for the demo browser ORGANISER (sort modes, folder grouping, and the demo key that
// joins the Panorama UI to the persisted C++ state).
//
// Two kinds of check live here:
//
//   1. CONTRACT checks, comparing the JS in Panorama/DemoOrganizerJs.h against the C++ that
//      backs it. The UI and the store agree only by convention -- the sort-mode ids, the "org
//      ..." command verbs and the normalised demo key are all plain strings crossing a JSON /
//      command-queue boundary with no compiler to catch a mismatch. A typo on either side
//      degrades SILENTLY (demos quietly stop belonging to their folder), which is exactly the
//      kind of bug that only shows up after a user has filed fifty demos.
//
//   2. BEHAVIOUR checks, running the fragment's pure functions for real. The fragment is
//      evaluated with stubs for the Panorama globals, so orgKeyOf / orgCompare / orgBuildPlan /
//      orgPlanSig are the SHIPPING code, not a reimplementation.
//
// Run: node --test Automation/tests/demo-organizer-tests.mjs

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const testDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(testDirectory, "..", "..");
const panoramaDirectory = path.join(repositoryRoot, "AfxHookSource2", "Filmmaker", "Panorama");

const organizerJsSource = fs.readFileSync(path.join(panoramaDirectory, "DemoOrganizerJs.h"), "utf8");
const guiJsSource = fs.readFileSync(path.join(panoramaDirectory, "FilmmakerGuiJs.h"), "utf8");
const menuSource = fs.readFileSync(path.join(panoramaDirectory, "FilmmakerMenu.cpp"), "utf8");
const organizerStoreSource = fs.readFileSync(
  path.join(repositoryRoot, "AfxHookSource2", "Filmmaker", "Config", "DemoOrganizerStore.cpp"),
  "utf8",
);
const demoRepairSource = fs.readFileSync(
  path.join(repositoryRoot, "AfxHookSource2", "Filmmaker", "Demo", "DemoRepair.cpp"),
  "utf8",
);
const demoRepairHeader = fs.readFileSync(
  path.join(repositoryRoot, "AfxHookSource2", "Filmmaker", "Demo", "DemoRepair.h"),
  "utf8",
);
const demoEntrySource = fs.readFileSync(
  path.join(repositoryRoot, "AfxHookSource2", "Filmmaker", "Demo", "DemoEntry.h"),
  "utf8",
);
const demoLibrarySource = fs.readFileSync(
  path.join(repositoryRoot, "AfxHookSource2", "Filmmaker", "Demo", "DemoLibrary.cpp"),
  "utf8",
);
const filmmakerSource = fs.readFileSync(
  path.join(repositoryRoot, "AfxHookSource2", "Filmmaker", "Filmmaker.cpp"),
  "utf8",
);
const cameraPathSource = fs.readFileSync(
  path.join(repositoryRoot, "AfxHookSource2", "Filmmaker", "Movie", "CameraPath.cpp"),
  "utf8",
);
const cameraShakeSource = fs.readFileSync(
  path.join(repositoryRoot, "AfxHookSource2", "Filmmaker", "Movie", "CamPathShake.cpp"),
  "utf8",
);
const playbackLoopSource = fs.readFileSync(
  path.join(repositoryRoot, "AfxHookSource2", "Filmmaker", "Movie", "PlaybackLoop.cpp"),
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

const organizerJs = rawStringFragments(organizerJsSource, "FMORG").join("\n");

// ---------------------------------------------------------------------------
// Load the fragment for real, with the Panorama globals stubbed out.
//
// The fragment's top level only declares functions and simple values, so evaluating it needs
// nothing from the engine. `with` puts the surrounding IIFE's helpers (demos, mapDisplay, ...)
// in scope exactly as the concatenated script sees them; a Function-constructor body is
// non-strict, so `with` is legal here even though this module is not.
// ---------------------------------------------------------------------------
function loadOrganizer() {
  const noop = () => {};
  const externals = {
    demos: [],
    tilePanels: [],
    listPanel: null,
    host: { GetAttributeString: () => "" },
    ctx: null,
    api: { addCommand: noop },
    watchPage: () => null,
    cropName: (s) => String(s ?? ""),
    setMapIcon: noop,
    mapDisplay: (map) => String(map ?? "Demo"),
    demoName: (demo) => String((demo && demo.fileName) || "").replace(/\.dem$/i, ""),
    UiToolkitAPI: { ShowSimpleContextMenu: noop, ShowGenericPopupOkCancel: noop, HideTextTooltip: noop },
    $: Object.assign(() => null, {
      CreatePanel: () => ({ style: {}, Data: () => ({}) }),
      Msg: noop,
      Schedule: noop,
      DispatchEvent: noop,
      RegisterEventHandler: noop,
      Localize: (s) => s,
    }),
  };

  const factory = new Function(
    "externals",
    `with (externals) {
       ${organizerJs}
       return {
         externals: externals,
         orgKeyOf: orgKeyOf,
         orgCompare: orgCompare,
         orgBuildPlan: orgBuildPlan,
         orgPlanSig: orgPlanSig,
         orgAgo: orgAgo,
         orgLatestPlayedKey: orgLatestPlayedKey,
         orgIsLatestPlayed: orgIsLatestPlayed,
         orgIsCollapsed: orgIsCollapsed,
         orgSetCollapsedLocal: orgSetCollapsedLocal,
         orgCollapsedMap: orgCollapsedMap,
         sorts: ORG_SORTS,
         setState: function (s) { orgState = s; },
         getState: function () { return orgState; }
       };
     }`,
  );
  return factory(externals);
}

// A demo row shaped like one entry of DemoLibrary::BuildJson()'s "demos" array.
function demo(fileName, { folder = "C:\\demos", map = "de_dust2", date = 0 } = {}) {
  return { fileName, folder, map, date, index: 0 };
}

// ---------------------------------------------------------------------------
// 1. Contract: JS <-> C++
// ---------------------------------------------------------------------------

test("every sort mode the UI can send is accepted by the C++ whitelist", () => {
  const { sorts } = loadOrganizer();
  // DemoOrganizerStore::SetSort rejects anything outside kModes, so a mode the menu can emit
  // but the store drops would leave the sort control showing a selection that never applies.
  const whitelist = organizerStoreSource.match(/kModes\[\]\s*=\s*\{([^}]*)\}/);
  assert.ok(whitelist, "DemoOrganizerStore must declare a kModes whitelist");
  const accepted = [...whitelist[1].matchAll(/"([a-z]+)"/g)].map((m) => m[1]);

  for (const [mode] of sorts) {
    assert.ok(accepted.includes(mode), `sort mode "${mode}" is not in the C++ whitelist`);
  }
  assert.equal(sorts.length, accepted.length, "menu and whitelist must cover the same modes");
});

test("every command the organiser UI emits has a handler", () => {
  // "org <verb> ..." goes to DemoOrganizer_HandleCommand, which hands the verb to
  // DemoOrganizerStore::Apply; anything else falls through to FilmmakerMenu::PollCommands. A verb
  // with no handler is silently swallowed (Apply returns false), so this is the only place it can
  // be caught.
  const emitted = [...organizerJs.matchAll(/addCommand\('([a-z]+)(?: ([a-z]+))?/g)].map((m) => ({
    head: m[1],
    verb: m[2],
  }));
  assert.ok(emitted.length > 0, "the organiser must emit commands");

  for (const { head, verb } of emitted) {
    if (head === "org") {
      assert.ok(verb, "an 'org' command must carry a verb");
      assert.match(
        organizerStoreSource,
        new RegExp(`verb == "${verb}"`),
        `DemoOrganizerStore::Apply has no handler for "org ${verb}"`,
      );
    } else {
      assert.match(
        menuSource,
        new RegExp(`line == "${head}"`),
        `FilmmakerMenu.cpp has no handler for "${head}"`,
      );
    }
  }
});

test("the organiser payload is pushed and the UI reads the same attribute", () => {
  // FilmmakerMenu::PushOrganizer writes the "org" attribute and calls renderOrg(); if either
  // half is renamed the folders simply never arrive, with no error anywhere.
  assert.match(menuSource, /MakeSymbol\("org"\)/, "the org attribute symbol must be created");
  assert.match(menuSource, /\$\.Filmmaker && \$\.Filmmaker\.renderOrg\(\)/);
  assert.match(organizerJs, /host\.GetAttributeString\('org', ''\)/);
  assert.match(guiJsSource, /api\.renderOrg = function/);
});

test("Watch stays disabled for the full compatibility job", () => {
  assert.match(guiJsSource, /var repairStatus = null, watchBusy = false, repairJobBusy = false/);
  assert.match(guiJsSource, /anyRepairJob = state === 1 \|\| state === 2/);
  assert.match(guiJsSource, /repairJobBusy = ownsStatus && anyRepairJob/);
  assert.match(
    guiJsSource,
    /\$\.Schedule\(1\.5, function \(\) \{ if \(watchBusy && !repairJobBusy\) setWatchBusy\(false\); \}\)/,
  );
});

test("selecting an unchecked legacy demo starts preparation and shows honest progress", () => {
  assert.match(menuSource, /line\.rfind\("prepare ", 0\) == 0/);
  assert.match(menuSource, /PrepareDemo/);
  assert.match(filmmakerSource, /void PrepareDemo\(std::size_t index\)/);
  assert.match(guiJsSource, /api\.addCommand\('prepare ' \+ demo\.index\)/);
  assert.match(guiJsSource, /function maybeAutoPrepare/);
  assert.match(guiJsSource, /FmRepairProgressTrack/);
  assert.match(guiJsSource, /FmRepairProgressFill/);
  assert.match(guiJsSource, /function startRepairProgress/);
  assert.match(guiJsSource, /Checking now; repair starts automatically if required/);
  assert.match(guiJsSource, /repairLbl\.style\.fontSize = '17px'/);
  assert.match(guiJsSource, /repairDetailLbl\.style\.fontSize = '14px'/);
  assert.match(guiJsSource, /repairPanel\.style\.height = '50px'/);
});

test("the demo repair worker is joined before reuse and shutdown", () => {
  const reset = demoRepairSource.slice(demoRepairSource.indexOf('void DemoRepair_Reset()'),
    demoRepairSource.indexOf('std::wstring DemoRepair_ResolvePlayable'));
  assert.doesNotMatch(reset, /g_worker\.detach\(\)/);
  assert.match(reset, /g_worker\.join\(\)/);
  assert.match(filmmakerSource, /void Shutdown\(\)[\s\S]{0,500}DemoRepair_Reset\(\)/);
});

test("demo rows expose compatibility before Watch is clicked", () => {
  assert.match(demoRepairHeader, /enum class DemoCompatibility/);
  assert.match(demoRepairHeader, /DemoRepair_ClassifyProtocol/);
  assert.match(demoEntrySource, /int networkProtocol = 0/);
  assert.match(demoEntrySource, /int buildNum = 0/);
  assert.match(demoLibrarySource, /e\.networkProtocol = header\.networkProtocol/);
  assert.match(demoLibrarySource, /StringField\("compatibility"/);
  assert.match(demoLibrarySource, /StringField\("repairCache"/);
});

test("repair messages belong only to the demo that produced them", () => {
  assert.match(demoRepairHeader, /DemoRepair_ActiveDemo/);
  assert.match(demoLibrarySource, /IntField\("demoIndex"/);
  assert.match(guiJsSource, /repairStatus\.demoIndex === demo\.index/);
});

test("repaired playback shares the original demo's project.mvm sections", () => {
  assert.match(
    filmmakerSource,
    /CanonicalDemoPath\(ResolveDemoProjectPath\(eng\)\)/,
    "the public playing-demo path must map a validated repair back to its original",
  );
  assert.match(cameraPathSource, /DemoProjectStore_ReadSection/);
  assert.match(cameraPathSource, /if \(ok\) \{/);
  assert.match(cameraShakeSource, /DemoProjectStore_ReadSection/);
  assert.match(playbackLoopSource, /DemoProjectStore_ReadSection/);
});

test("the selected demo can be shown in Explorer, and the button survives a matchinfo reload", () => {
  assert.match(guiJsSource, /api\.addCommand\('openfolder ' \+ idx\)/);
  assert.match(guiJsSource, /Show demo in Explorer/);
  assert.match(menuSource, /line\.rfind\("openfolder ", 0\) == 0/);
  assert.match(menuSource, /OpenDemoProjectFolder/);
  // The button is CS2's own id-mi-copy, and matchinfo.xml is re-created under FmMatchInfo as the
  // page runs -- so setting it up once at build time leaves the handler on a dead panel. It has
  // to be re-asserted per selection, and the icon must not eat the click.
  assert.match(guiJsSource, /function ensureFolderBtn/);
  assert.match(guiJsSource, /__fmFolderReady/);
  assert.match(guiJsSource, /hittestchildren = false/);
});

test("showing a demo in Explorer asks the shell, and does it in its own COM apartment", () => {
  const shellOpenSource = fs.readFileSync(
    path.join(repositoryRoot, "AfxHookSource2", "Filmmaker", "Platform", "ShellOpen.cpp"), "utf8",
  );
  assert.match(filmmakerSource, /ShowInExplorerAsync\(showPath\)/);
  // THREE mechanisms were tried and rejected, all of them shelling out to explorer.exe:
  // ShellExecuteW reported success (>32) and opened nothing, because its dispatch depends on the
  // calling thread's COM state and inside an injected DLL that is whatever the game left it as;
  // CreateProcessW opened a window but always a NEW one, always behind the game; and raising it
  // by window title is guesswork ("<folder> - File Explorer", or the full path when the user has
  // that turned on). Verified live: from zero Explorer windows the shell path opens one AND it
  // becomes the foreground window, and with one already open it is reused rather than duplicated.
  assert.doesNotMatch(shellOpenSource, /ShellExecuteW\(nullptr, L"open", L"explorer\.exe"/);
  assert.doesNotMatch(shellOpenSource, /CreateProcessW\(nullptr, &cmd\[0\]/);
  assert.match(shellOpenSource, /SHOpenFolderAndSelectItems/);
  assert.match(shellOpenSource, /ILCreateFromPathW/);
  // Our own apartment, on our own thread -- the part every earlier attempt was missing.
  assert.match(shellOpenSource, /CoInitializeEx\(nullptr, COINIT_APARTMENTTHREADED/);
  // An Explorer already open on a different folder follows the selection instead of going stale.
  assert.match(shellOpenSource, /IShellWindows/);
  assert.match(shellOpenSource, /Navigate2/);
  // Activation across processes is refused without both of these.
  assert.match(shellOpenSource, /AllowSetForegroundWindow\(ASFW_ANY\)/);
  assert.match(shellOpenSource, /AttachThreadInput/);
});

test("a demo can be renamed on disk from its title, with its sidecars and user state", () => {
  // The title IS the file name, so editing it in place is the rename.
  assert.match(guiJsSource, /function beginTitleRename/);
  assert.match(guiJsSource, /function endTitleRename/);
  assert.match(guiJsSource, /api\.addCommand\('renamedemo ' \+ demo\.index \+ ' ' \+ text\)/);
  // The context menu steals focus back as it closes, so one SetFocus lands on nothing.
  assert.match(guiJsSource, /BHasKeyFocus/);
  assert.match(organizerJsSource, /Rename demo/);
  // THE KEY FORMAT MATTERS. This fragment keys demos by orgKeyOf (folder + '\\' + fileName,
  // lower-cased, to match the C++ store) while the list indexes by tileKey (folder + '|' +
  // fileName, original case). Passing the organiser's key to beginTitleRename meant indexOfKey
  // never matched and "Rename demo" did nothing at all -- not even select the demo.
  assert.match(organizerJsSource, /beginTitleRename\(dk\); \}; \}\)\(tileKey\(demo\)\)/);
  assert.match(menuSource, /line\.rfind\("renamedemo ", 0\) == 0/);
  // A demo is not one file: the .info/.fmjson sidecars and the repaired copy travel with it,
  // and the path-keyed user state has to be carried over or the demo leaves its folder.
  assert.match(filmmakerSource, /void RenameDemoFile/);
  assert.match(filmmakerSource, /SanitizeDemoFileName/);
  assert.match(filmmakerSource, /DemoOrganizer_NoteRenamed/);
  assert.match(filmmakerSource, /NoteRenamed\(oldPath, newFs\.wstring\(\)\)/);
});

test("demo compatibility markers are explicit and the played edge is visible", () => {
  assert.match(guiJsSource, /function paintCompatibilityBadge/);
  assert.match(guiJsSource, /var nativeSlot = tile\.FindChildTraverse\('id-download-state'\)/);
  assert.match(guiJsSource, /warning\.vsvg/);
  assert.match(guiJsSource, /SetHasClass\('download-animation', !special\)/);
  assert.match(guiJsSource, /UNSUPPORTED DEMO/);
  assert.match(guiJsSource, /LEGACY DEMO/);
  assert.match(guiJsSource, /VALIDATED REPAIR/);
  assert.match(guiJsSource, /watchUnavailable/);
  assert.match(organizerJsSource, /'3px solid rgba\(240,179,35,0\.9\)'/);
});

test("the gold played rail marks only the latest visible demo", () => {
  const org = loadOrganizer();
  const a = demo("a.dem");
  const b = demo("b.dem");
  const c = demo("c.dem");
  org.externals.demos.push(a, b, c);
  org.setState(stateWith({
    [org.orgKeyOf(a)]: { played: 100 },
    [org.orgKeyOf(b)]: { played: 300 },
    [org.orgKeyOf(c)]: { played: 200 },
  }));

  assert.equal(org.orgLatestPlayedKey(), org.orgKeyOf(b));
  assert.equal(org.orgIsLatestPlayed(a), false);
  assert.equal(org.orgIsLatestPlayed(b), true);
  assert.equal(org.orgIsLatestPlayed(c), false);
});

test("orgKeyOf reproduces the C++ NormalizeDemoKey form", () => {
  const { orgKeyOf } = loadOrganizer();
  // NormalizeDemoKey: lower-case, '/' -> '\', trailing separators stripped.
  assert.match(organizerStoreSource, /c == L'\/' \? L'\\\\'/, "C++ must still fold '/' to '\\'");

  assert.equal(orgKeyOf(demo("Match.dem", { folder: "C:\\Demos" })), "c:\\demos\\match.dem");
  assert.equal(orgKeyOf(demo("Match.dem", { folder: "C:/Demos" })), "c:\\demos\\match.dem");
  // A demo at a drive root: parent_path() is "C:\", which must not produce a doubled separator.
  assert.equal(orgKeyOf(demo("a.dem", { folder: "C:\\" })), "c:\\a.dem");
  assert.equal(orgKeyOf(demo("a.dem", { folder: "C:\\demos\\" })), "c:\\demos\\a.dem");
});

// ---------------------------------------------------------------------------
// 2. Behaviour: ordering and grouping
// ---------------------------------------------------------------------------

const NEVER_PLAYED = 0;

function stateWith(records = {}, groups = [], sort = "date") {
  return { sort, rootCollapsed: false, groups, demos: records };
}

test("Recently played puts played demos first, newest play first", () => {
  const org = loadOrganizer();
  const a = demo("a.dem", { date: 100 });
  const b = demo("b.dem", { date: 900 });
  const c = demo("c.dem", { date: 500 });
  org.setState(
    stateWith(
      {
        [org.orgKeyOf(a)]: { g: -1, played: 50 },
        [org.orgKeyOf(b)]: { g: -1, played: NEVER_PLAYED },
        [org.orgKeyOf(c)]: { g: -1, played: 900 },
      },
      [],
      "played",
    ),
  );
  const order = [a, b, c].sort(org.orgCompare).map((d) => d.fileName);
  // c played most recently, then a; b was never played through the tool so it falls to the
  // date tiebreak at the bottom.
  assert.deepEqual(order, ["c.dem", "a.dem", "b.dem"]);
});

test("never-played demos fall back to newest-first, not to an arbitrary order", () => {
  const org = loadOrganizer();
  const older = demo("older.dem", { date: 10 });
  const newer = demo("newer.dem", { date: 20 });
  org.setState(stateWith({}, [], "played"));
  assert.deepEqual([older, newer].sort(org.orgCompare).map((d) => d.fileName), ["newer.dem", "older.dem"]);
});

test("date sorts run both ways and name/map sorts are case-insensitive", () => {
  const org = loadOrganizer();
  const first = demo("a.dem", { date: 10, map: "de_nuke" });
  const second = demo("B.dem", { date: 20, map: "de_Ancient" });

  org.setState(stateWith({}, [], "date"));
  assert.deepEqual([first, second].sort(org.orgCompare).map((d) => d.date), [20, 10]);

  org.setState(stateWith({}, [], "dateasc"));
  assert.deepEqual([first, second].sort(org.orgCompare).map((d) => d.date), [10, 20]);

  org.setState(stateWith({}, [], "name"));
  assert.deepEqual([second, first].sort(org.orgCompare).map((d) => d.fileName), ["a.dem", "B.dem"]);

  org.setState(stateWith({}, [], "map"));
  assert.deepEqual([first, second].sort(org.orgCompare).map((d) => d.map), ["de_Ancient", "de_nuke"]);
});

test("with no folders the plan is a flat list with no headers", () => {
  const org = loadOrganizer();
  org.externals.demos = [demo("a.dem", { date: 2 }), demo("b.dem", { date: 1 })];
  org.setState(stateWith());
  const plan = org.orgBuildPlan();
  // A fresh install must look exactly like the pre-organiser native list.
  assert.equal(plan.filter((e) => e.t === "h").length, 0);
  assert.deepEqual(plan.map((e) => e.pos), [0, 1]);
});

test("folders group their members and everything else lands under No folder", () => {
  const org = loadOrganizer();
  const inFolder = demo("clip.dem", { date: 1 });
  const loose = demo("loose.dem", { date: 2 });
  org.externals.demos = [loose, inFolder];
  org.setState(
    stateWith({ [org.orgKeyOf(inFolder)]: { g: 7, played: 0 } }, [{ id: 7, name: "Clips", collapsed: false }]),
  );

  const plan = org.orgBuildPlan();
  assert.deepEqual(
    plan.map((e) => (e.t === "h" ? `H:${e.name}:${e.n}` : `D:${org.externals.demos[e.pos].fileName}`)),
    ["H:Clips:1", "D:clip.dem", "H:No folder:1", "D:loose.dem"],
  );
  // Positions must index `demos`, not a sorted copy -- tilePanels[] is indexed that way.
  assert.equal(plan[1].pos, 1);
  assert.equal(plan[3].pos, 0);
});

test("a demo assigned to a folder that no longer exists falls back to No folder", () => {
  const org = loadOrganizer();
  const orphan = demo("orphan.dem");
  org.externals.demos = [orphan];
  org.setState(
    stateWith({ [org.orgKeyOf(orphan)]: { g: 99, played: 0 } }, [{ id: 7, name: "Clips", collapsed: false }]),
  );
  const plan = org.orgBuildPlan();
  const headers = plan.filter((e) => e.t === "h");
  assert.equal(headers[0].n, 0, "the surviving folder must be empty");
  assert.equal(headers[1].n, 1, "the orphan must be counted under No folder");
  assert.equal(plan[plan.length - 1].gid, -1);
});

test("collapsing a folder does NOT change the plan signature", () => {
  // This is the whole reason collapse is a visibility toggle: if collapse changed the
  // signature, reconcileList() would rebuild the list and reset scroll + selection on every
  // expand/collapse.
  const org = loadOrganizer();
  const d = demo("a.dem");
  org.externals.demos = [d];
  const records = { [org.orgKeyOf(d)]: { g: 3, played: 0 } };

  org.setState(stateWith(records, [{ id: 3, name: "Clips", collapsed: false }]));
  const open = org.orgPlanSig(org.orgBuildPlan());

  org.setState(stateWith(records, [{ id: 3, name: "Clips", collapsed: true }]));
  const shut = org.orgPlanSig(org.orgBuildPlan());

  assert.equal(open, shut);
});

test("sort, membership and rename DO change the plan signature", () => {
  // The mirror of the test above: these must force a rebuild, or the list would keep showing
  // the old order/folder after the user changed it.
  const org = loadOrganizer();
  const d = demo("a.dem");
  org.externals.demos = [d];
  const key = org.orgKeyOf(d);
  const sig = (state) => {
    org.setState(state);
    return org.orgPlanSig(org.orgBuildPlan());
  };

  const base = sig(stateWith({ [key]: { g: 3, played: 0 } }, [{ id: 3, name: "Clips", collapsed: false }]));
  const sorted = sig(
    stateWith({ [key]: { g: 3, played: 0 } }, [{ id: 3, name: "Clips", collapsed: false }], "played"),
  );
  const moved = sig(stateWith({}, [{ id: 3, name: "Clips", collapsed: false }]));
  const renamed = sig(
    stateWith({ [key]: { g: 3, played: 0 } }, [{ id: 3, name: "Highlights", collapsed: false }]),
  );

  assert.notEqual(base, sorted, "changing the sort must rebuild");
  assert.notEqual(base, moved, "moving a demo out of a folder must rebuild");
  assert.notEqual(base, renamed, "renaming a folder must rebuild");
});

test("a folder can be collapsed and reopened repeatedly", () => {
  // Regression: the header's click handler used to capture entry.collapsed at BUILD time.
  // Collapsing deliberately does not rebuild the list, so that capture went stale after the
  // first click and every later click re-sent "collapse 1" -- a shut folder could never be
  // reopened. Toggling must read live state, which is what orgIsCollapsed exists for.
  const org = loadOrganizer();
  org.setState(stateWith({}, [{ id: 4, name: "Clips", collapsed: false }]));

  const toggle = (id) => {
    const next = !org.orgIsCollapsed(id);
    org.orgSetCollapsedLocal(id, next);
    return next; // what the UI would send as "org collapse <id> <next>"
  };

  assert.equal(toggle(4), true, "first click closes");
  assert.equal(toggle(4), false, "second click must reopen, not re-close");
  assert.equal(toggle(4), true, "third click closes again");

  // The No-folder bucket (-1) rides rootCollapsed and must toggle the same way.
  assert.equal(toggle(-1), true);
  assert.equal(toggle(-1), false);
  assert.equal(org.getState().rootCollapsed, false);
});

test("text fields commit on Panorama's Enter event, not on onactivate", () => {
  // 'onactivate' is BUTTON activation and never fires for a TextEntry; native CS2 (chat.js,
  // #ChatInput) uses 'oninputsubmit'. Wiring commit to onactivate silently does nothing, which
  // is invisible in review and obvious the moment someone types a folder name.
  const textEntryCreations = [...organizerJs.matchAll(/\$\.CreatePanel\('TextEntry'/g)];
  assert.equal(textEntryCreations.length, 2, "the organiser creates the rename and new-folder fields");

  // Asserted per-field rather than as a global count of 'oninputsubmit': the sort DropDown
  // legitimately uses that event too, so a total would drift with unrelated controls.
  for (const name of ["entry", "orgNewFolderInput"]) {
    assert.match(
      organizerJs,
      new RegExp(`${name}\\.SetPanelEvent\\('oninputsubmit'`),
      `${name} must commit on oninputsubmit (Panorama's Enter event for a TextEntry)`,
    );
    assert.doesNotMatch(
      organizerJs,
      new RegExp(`${name}\\.SetPanelEvent\\('onactivate'`),
      `${name} is a TextEntry; onactivate never fires on it`,
    );
  }
});

test("a collapsed state is never honoured without a header to undo it", () => {
  // Regression: deleting the last folder while the No-folder bucket was collapsed left
  // rootCollapsed=true persisted, but with zero folders the plan renders NO headers at all --
  // so every demo stayed hidden behind a control that was no longer on screen. The browser
  // looked empty and there was no way back short of editing the store file by hand.
  const org = loadOrganizer();

  org.setState(stateWith({}, [], "date"));
  org.getState().rootCollapsed = true;
  assert.equal(
    org.orgCollapsedMap()["-1"],
    false,
    "with no folders there is no No-folder header, so nothing may be hidden",
  );

  // With a folder present the header exists, so the persisted state is honoured again.
  org.setState(stateWith({}, [{ id: 2, name: "Clips", collapsed: true }]));
  org.getState().rootCollapsed = true;
  const map = org.orgCollapsedMap();
  assert.equal(map["-1"], true);
  assert.equal(map["2"], true);
});

test("the sort control is a native DropDown, not a context menu", () => {
  // Regression: built as a Button that opened UiToolkitAPI.ShowSimpleContextMenu. That API
  // positions a menu like a RIGHT-CLICK menu -- it appeared up and to the right, covering the
  // button it came from. A context menu cannot be made to drop down; CS2's own sort controls
  // (inventory, crafting, leaderboard) are DropDown panels with menuclass="DropDownMenu".
  assert.match(
    organizerJs,
    /\$\.CreatePanel\('DropDown', bar, 'FmSortBtn'/,
    "the sort control must be a DropDown panel",
  );
  assert.match(organizerJs, /menuclass: 'DropDownMenu'/);
  assert.match(organizerJs, /orgSortDrop\.SetPanelEvent\('oninputsubmit'/);

  // Every sort mode must reach the dropdown as an option id the submit handler can decode.
  const { sorts } = loadOrganizer();
  assert.match(organizerJs, /ORG_SORT_OPT \+ ORG_SORTS\[s\]\[0\]/);
  assert.ok(sorts.length >= 5);

  // And SetSelected must be guarded, or pushing persisted state back into the control would
  // re-enter oninputsubmit and fire a redundant "org sort" every payload push.
  assert.match(organizerJs, /if \(orgSyncingSort\) return;/);
});

test("no ICU-dependent JS APIs (Panorama's V8 has no ICU data)", () => {
  // Regression, found only by running in-game: localeCompare threw
  //   "RangeError: Internal error. Icu error."
  // inside orgCompare, which propagated through orgBuildPlan -> reconcileList -> renderInto and
  // killed the ENTIRE demo list for the Map-name and Demo-name sorts. Node ships full ICU, so
  // every unit test here passed while the shipping UI was broken -- static prohibition is the
  // only thing that can catch it off-engine.
  const banned = [
    "localeCompare",
    "toLocaleUpperCase",
    "toLocaleLowerCase",
    "toLocaleDateString",
    "toLocaleTimeString",
    "toLocaleString",
    "Intl.",
  ];
  // Comments are stripped first: the fix itself documents WHY localeCompare is banned, and a
  // naive text scan would flag that explanation and make the guard unusable.
  const code = organizerJs
    .split("\n")
    .filter((line) => !/^\s*\/\//.test(line))
    .join("\n");

  for (const api of banned) {
    assert.ok(
      !code.includes(api),
      `${api} depends on ICU and throws in Panorama's V8; use a plain codepoint compare`,
    );
  }
});

test("text sorts still order correctly without localeCompare", () => {
  // The ICU fix must not quietly change behaviour: lower-cased codepoint order has to give the
  // same answer as before for the names this browser actually shows.
  const org = loadOrganizer();
  const a = demo("Ancient_clip.dem", { date: 1, map: "de_ancient" });
  const b = demo("bravo.dem", { date: 2, map: "de_Nuke" });
  const c = demo("charlie.dem", { date: 3, map: "de_mirage" });

  org.setState(stateWith({}, [], "name"));
  assert.deepEqual(
    [c, b, a].sort(org.orgCompare).map((d) => d.fileName),
    ["Ancient_clip.dem", "bravo.dem", "charlie.dem"],
  );

  org.setState(stateWith({}, [], "map"));
  assert.deepEqual(
    [b, c, a].sort(org.orgCompare).map((d) => d.map),
    ["de_ancient", "de_mirage", "de_Nuke"],
  );
});

test("relative last-played text degrades sensibly", () => {
  const { orgAgo } = loadOrganizer();
  const now = Math.floor(Date.now() / 1000);
  assert.equal(orgAgo(0), "never");
  assert.equal(orgAgo(now - 10), "just now");
  assert.match(orgAgo(now - 600), /min ago$/);
  assert.match(orgAgo(now - 7200), /hr ago$/);
  assert.match(orgAgo(now - 400000), /days ago$/);
});
