// The CONFIG card's recoil scope picker and the engine's scope model must name the same things.
//
// The Weapon Recoil scope selector keys an override by the engine's own category names
// ("pistol", "primary"), and the resolver matches them against the category it derives from the
// weapon that fired. So a key in the Panorama list that the engine never derives is not a build
// error, a missing panel, or a visible mistake of any kind -- it is a button you can press, tune,
// and watch do nothing, forever. The reverse gap is quieter still: a category the engine can
// derive with no button is a set of guns nobody can configure, and nothing anywhere says so.
//
// The per-WEAPON scope was removed 2026-09-01, so what has to line up now is the category lists
// plus the style tokens. Neither side can be generated from the other at runtime without shipping
// the whole catalogue through the per-frame state JSON, so they are hand-written lists kept
// honest here instead.

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, "..", "..");
const read = (...parts) => fs.readFileSync(path.join(repo, ...parts), "utf8");

const scopeHeader = read("AfxHookSource2", "Filmmaker", "Movie", "WeaponRecoilScope.h");
const styleHeader = read("AfxHookSource2", "Filmmaker", "Movie", "WeaponRecoilStyles.h");
const recoilCpp = read("AfxHookSource2", "Filmmaker", "Movie", "WeaponRecoil.cpp");
const cardJs = read("AfxHookSource2", "Filmmaker", "Panorama", "ConfigHudCameraTabJs.h");

// Rows look like: { "ak47", 1.15, RecoilWeaponType::Rifle },
function engineWeapons() {
  const start = scopeHeader.indexOf("static const RecoilWeaponEntry kTable[]");
  assert.notEqual(start, -1, "kTable not found in WeaponRecoilScope.h");
  const end = scopeHeader.indexOf("};", start);
  assert.notEqual(end, -1, "kTable is not terminated");
  const body = scopeHeader.slice(start, end);
  const rows = [...body.matchAll(/\{\s*"([a-z0-9_]+)"\s*,\s*[\d.]+\s*,\s*RecoilWeaponType::(\w+)\s*\}/g)]
    .map((m) => ({ needle: m[1], type: m[2].toLowerCase() }));
  assert.ok(rows.length > 20, `expected a full weapon table, parsed ${rows.length}`);
  return rows;
}

// Rows look like: ['pistol', 'Pistols'],
function cardList(name) {
  const start = cardJs.indexOf(`var ${name} = [`);
  assert.notEqual(start, -1, `${name} not found in ConfigHudCameraTabJs.h`);
  const end = cardJs.indexOf("];", start);
  assert.notEqual(end, -1, `${name} is not terminated`);
  const body = cardJs.slice(start, end);
  const keys = [...body.matchAll(/\[\s*'([a-z0-9]+)'\s*,\s*'([^']+)'\s*\]/g)].map((m) => m[1]);
  assert.ok(keys.length > 1, `expected entries in ${name}, parsed ${keys.length}`);
  return keys;
}

test("every category the engine can derive has a button", () => {
  // This is what the scope picker's live preselect depends on: the card is handed the derived
  // category of the gun in shot and looks for the matching button. A type the table produces but
  // the card does not list silently falls back to the head of the list -- the bug this pair of
  // lists exists to prevent.
  const types = new Set(cardList("WR_TYPES"));
  const missing = [...new Set(engineWeapons().map((w) => w.type))].filter((t) => !types.has(t));
  assert.deepEqual(missing, [],
    "these weapon categories cannot be configured from the UI: " + missing.join(", "));
});

test("every card category is one the engine actually names", () => {
  for (const key of [...cardList("WR_TYPES"), ...cardList("WR_SLOTS")])
    assert.ok(scopeHeader.includes(`return "${key}";`),
      `'${key}' is not returned by RecoilWeaponTypeName/RecoilSlotName, so no shot can match it`);
});

test("neither list repeats a key", () => {
  for (const name of ["WR_TYPES", "WR_SLOTS"]) {
    const seen = new Set();
    const dupes = cardList(name).filter((k) => (seen.has(k) ? true : (seen.add(k), false)));
    assert.deepEqual(dupes, [], `${name} repeats: ${dupes.join(", ")}`);
  }
});

test("the scope kinds the card sends are the ones RecoilParseScope accepts", () => {
  for (const kind of ["all", "slot", "type"])
    assert.ok(scopeHeader.includes(`if (s == "${kind}")`),
      `RecoilParseScope does not accept '${kind}'`);
  // The per-weapon scope is gone. A card that still offered it would build a scope command the
  // parser rejects, which shows up as a picker that silently will not move.
  assert.ok(!scopeHeader.includes(`if (s == "weapon")`),
    "RecoilParseScope still accepts the removed 'weapon' scope");
  // Scoped to the scope picker itself: 'weapon' is also the name of one of the card's SUB-TABS,
  // which is unrelated and must not trip this.
  const segStart = cardJs.indexOf("var wrScopeSeg = segmented(");
  assert.notEqual(segStart, -1, "the scope picker was not found");
  const seg = cardJs.slice(segStart, cardJs.indexOf("'100%'", segStart));
  assert.ok(!seg.includes("'weapon'"), "the card still offers the removed 'weapon' scope");
});

test("the style tokens the card sends are the ones the engine parses and emits", () => {
  const start = cardJs.indexOf("var wrStyleSeg = segmented(");
  assert.notEqual(start, -1, "the style picker was not found");
  const body = cardJs.slice(start, cardJs.indexOf(");", start));
  const keys = [...body.matchAll(/\[\s*'([a-z0-9]+)'\s*,/g)].map((m) => m[1]);
  assert.deepEqual(keys, ["style1", "style2"], "the card's style tokens changed");
  for (const key of keys)
    assert.ok(styleHeader.includes(`eq("${key}")`), `RecoilStyleParse does not accept '${key}'`);
  // The render hook compares against what RecoilStyleName EMITS, which is a separate spelling
  // from what the parser accepts -- a mismatch leaves the picker permanently on the default.
  for (const key of keys)
    assert.ok(styleHeader.includes(`? "style2" : "style1"`),
      `RecoilStyleName does not emit '${key}'`);
  assert.match(cardJs, /st\.wrStyle \|\| 'style1'/);
  assert.match(cardJs, /wrStyle === 'style2'/);
});

test("the card no longer names the MW build years the tables were derived from", () => {
  // Provenance belongs in WeaponRecoilStyles.h, where a retune reads it. In the card it read as
  // a version picker for something this tool does not ship.
  const start = cardJs.indexOf("subHead(wrShared, 'STYLE')");
  const end = cardJs.indexOf("RECOIL APPLIES TO", start);
  const styleBlock = cardJs.slice(start, end);
  assert.ok(!/'[^']*MW ?20\d\d[^']*'/.test(styleBlock), "an MW year is still shown in the card");
  assert.ok(!/'[^']*original port[^']*'/.test(cardJs), "the 'original port' note is still shown");
});

test("a Test Shot uses the scope being edited, not a fixed weapon", () => {
  // A hardcoded weapon here is why Slot and Type looked like they did not gate: the card said the
  // override existed, the sliders moved, and then the test fired something the scope did not
  // cover -- so only "All weapons" ever appeared to work.
  assert.match(recoilCpp, /wr\.TestWeapon\(\)/);
  assert.ok(!recoilCpp.includes('argc >= 4 ? value : "ak47"'),
    "the test command still falls back to a fixed weapon");
  assert.match(recoilCpp, /std::string WeaponRecoil::TestWeapon\(\) const[\s\S]{0,900}RecoilRepresentativeWeapon/);
});

test("the card can see which gun the selected scope would reach", () => {
  // Without these, an override on a category nothing in shot belongs to is indistinguishable
  // from a broken one.
  for (const field of ["wrLive", "wrLiveType", "wrLiveSlot", "wrScopeCovers"]) {
    assert.ok(recoilCpp.includes(`"${field}`), `${field} is not published by the engine`);
    assert.ok(cardJs.includes(`st.${field}`), `${field} is not read by the CONFIG card`);
  }
  // The live categories are what the scope picker preselects from.
  assert.match(cardJs, /function wrDefaultKey\(kind\)[\s\S]{0,400}wrLiveSlot[\s\S]{0,200}wrLiveType/);
  assert.match(cardJs, /wrScopeKey = \(kind === 'all'\) \? '' : wrDefaultKey\(kind\)/);
});
