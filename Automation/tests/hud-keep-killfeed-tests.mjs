// F8 blackout and GAME UI: Hide All keep the kill feed (and the radar) when CS2's own
// "draw this with the HUD off" switch is 1 (cl_drawhud_force_deathnotices / cl_drawhud_force_radar).
// Runs the timeline script's real hide pass (CameraTimelineJs.h, from SPEC_PANELS through
// applyHudView) against a fake HUD tree laid out like CS2's hud.xml: the kill feed sits inside
// HudTopRight and the radar inside HudTopLeft, so keeping one means hiding only its siblings.
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import vm from "node:vm";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const read = (rel) => fs.readFileSync(path.join(root, rel), "utf8");
const timeline = read("AfxHookSource2/Filmmaker/Panorama/CameraTimelineJs.h");

// The hide pass, from `var SPEC_PANELS` to the closing brace of applyHudView.
function hidePassSource() {
  const start = timeline.indexOf("var SPEC_PANELS");
  const fn = timeline.indexOf("function applyHudView(", start);
  assert.ok(start >= 0 && fn > start, "hide pass not found");
  let depth = 0;
  for (let i = timeline.indexOf("{", fn); i < timeline.length; i++) {
    if (timeline[i] === "{") depth++;
    else if (timeline[i] === "}" && --depth === 0) return timeline.slice(start, i + 1);
  }
  throw new Error("applyHudView has no closing brace");
}

class Panel {
  constructor(id, children = [], visible = true) {
    this.id = id; this.children = children; this.visible = visible; this.style = {};
    for (const c of children) c.parent = this;
  }
  GetChildCount() { return this.children.length; }
  GetChild(i) { return this.children[i]; }
  GetParent() { return this.parent || null; }
  IsValid() { return true; }
  FindChildTraverse(id) {
    for (const c of this.children) {
      if (c.id === id) return c;
      const hit = c.FindChildTraverse(id);
      if (hit) return hit;
    }
    return null;
  }
}

function hud() {
  const ctx = new Panel("Hud", [
    new Panel("HudTopRight", [new Panel("HudAutoDisconnect"), new Panel("HudDeathNotice")]),
    new Panel("HudTopLeft", [new Panel("HudRadar"), new Panel("HudGameIcons")]),
    new Panel("HudBottomCenter", [new Panel("HudAlerts")]),
    new Panel("MovieHudRoot"),
    new Panel("NativeDemoBar", [new Panel("SliderRow")]),
  ]);
  const sandbox = { ctx, OURS: { MovieHudRoot: 1 } };
  sandbox.findNative = (id) => ctx.FindChildTraverse(id);
  vm.createContext(sandbox);
  vm.runInContext(hidePassSource() + "\nthis.setHudKeep = setHudKeep; this.applyHudView = applyHudView;", sandbox);
  const shown = (id) => ctx.FindChildTraverse(id).visible;
  return { ...sandbox, shown };
}

test("Hide All with no switch on hides the kill feed's whole group, as before", () => {
  const h = hud();
  h.setHudKeep({});
  h.applyHudView("hidden");
  assert.equal(h.shown("HudTopRight"), false);
  assert.equal(h.shown("HudTopLeft"), false);
  assert.equal(h.shown("HudBottomCenter"), false);
  assert.equal(h.shown("MovieHudRoot"), true, "our own panels are never hidden");
  assert.equal(h.shown("NativeDemoBar"), true, "the native demo bar is left alone");
});

test("cl_drawhud_force_deathnotices 1 keeps the kill feed and hides its neighbours", () => {
  const h = hud();
  h.setHudKeep({ forceDeathNotices: 1 });
  h.applyHudView("hidden");
  assert.equal(h.shown("HudTopRight"), true);
  assert.equal(h.shown("HudDeathNotice"), true);
  assert.equal(h.shown("HudAutoDisconnect"), false);
  assert.equal(h.shown("HudTopLeft"), false, "the radar's group still goes");
  assert.equal(h.shown("HudBottomCenter"), false);
});

test("cl_drawhud_force_radar 1 keeps the radar the same way", () => {
  const h = hud();
  h.setHudKeep({ forceRadar: 1 });
  h.applyHudView("hidden");
  assert.equal(h.shown("HudTopLeft"), true);
  assert.equal(h.shown("HudRadar"), true);
  assert.equal(h.shown("HudGameIcons"), false);
  assert.equal(h.shown("HudTopRight"), false);
});

test("flipping the switch while hidden takes effect on the next pass, and full view restores all", () => {
  const h = hud();
  h.setHudKeep({ forceDeathNotices: 1 });
  h.applyHudView("hidden");
  assert.equal(h.shown("HudDeathNotice"), true);
  h.setHudKeep({ forceDeathNotices: 0 });
  h.applyHudView("hidden");
  assert.equal(h.shown("HudTopRight"), false, "switch off: the group is hidden again");
  h.setHudKeep({ forceDeathNotices: 1 });
  h.applyHudView("hidden");
  assert.equal(h.shown("HudTopRight"), true, "switch on again: the group comes back");
  assert.equal(h.shown("HudAutoDisconnect"), false);
  h.applyHudView("full");
  for (const id of ["HudTopRight", "HudAutoDisconnect", "HudDeathNotice", "HudTopLeft", "HudRadar", "HudGameIcons", "HudBottomCenter", "HudAlerts"])
    assert.equal(h.shown(id), true, id + " restored");
});

test("-1 (CS2's force off) is left to CS2: the hidden view treats it like 0", () => {
  const h = hud();
  h.setHudKeep({ forceDeathNotices: -1 });
  h.applyHudView("hidden");
  assert.equal(h.shown("HudTopRight"), false);
});

test("the switches come from C++ state and are only read, never written", () => {
  const hudCpp = read("AfxHookSource2/Filmmaker/Panorama/CameraTimelineHud.cpp");
  assert.match(hudCpp, /"forceDeathNotices\\":" << HudBlackout_ForceDrawSetting\("cl_drawhud_force_deathnotices"\)/);
  assert.match(hudCpp, /"forceRadar\\":" << HudBlackout_ForceDrawSetting\("cl_drawhud_force_radar"\)/);
  const blackout = read("AfxHookSource2/Filmmaker/Movie/HudBlackout.cpp");
  assert.doesNotMatch(blackout, /ExecClientCmd\("cl_drawhud/, "the kill feed switch is the user's; never set it");
  assert.match(timeline, /setHudKeep\(st\);[^\n]*\n\s*var blackout = !!st\.blackout;/, "read before the blackout and Hide All paths");
});
