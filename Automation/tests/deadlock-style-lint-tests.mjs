// Does the Deadlock UI keep growing its own copies of CS2's literal colours, or does it use
// DeadlockStyleJs.h's own S token table like the plan says it should?
//
//   node --test Automation/tests/deadlock-style-lint-tests.mjs
//
// P8a's brief was four shared UI primitives (toast, destructive confirm, context-menu shell,
// settings popup shell) plus this lint. The four primitives are new code and are held to a hard
// zero: nothing added in that pass may introduce a raw hex colour literal outside the one file that
// is allowed to define them.
//
// The REST of the Deadlock UI is a different story. Every *Js.h fragment before this pass grew its
// own ad hoc palette -- DeadlockTimelineJs.h's own copied CS2 `Q` table is one example, and it is far
// from the only one -- and driving that to zero is explicitly future phases' job (P8b/P8d/P8e), not
// this one's. So rather than a check that already passes vacuously (asserting some huge ceiling) or
// one that fails on day one over pre-existing debt this pass was never asked to pay down, the count
// is pinned to a BASELINE recorded the moment this file was written: today's real number, computed by
// the same scan below, immediately before P8a's four primitives were added. A later phase that fixes
// a file's colours should lower BASELINE to match; a change that adds a new raw hex literal anywhere
// in the Deadlock UI should fail here even though the number is still "small" in absolute terms.

import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';

const REPO = path.resolve(import.meta.dirname, '..', '..');
const PANO = path.join(REPO, 'AfxHookSource2', 'Filmmaker', 'Deadlock');

// DeadlockStyleJs.h is the one file allowed to spell out a hex literal: it IS the token table every
// other fragment is supposed to read S.* out of instead. Every other *Js.h fragment shares that one
// scope (DeadlockStyleJs.h's own header comment) and is meant to draw its colours from it.
const EXEMPT_FILE = 'DeadlockStyleJs.h';

// A hex colour written directly into a style string: '#1f1e19', '#FFED7959', '#000000ee ...' (a
// boxShadow's colour prefix), '2px 2px 0px 1.0 #000000' (a textShadow's colour suffix), and so on.
const HEX_RE = /#[0-9a-fA-F]{3,8}/g;

// Every JS string literal in these fragments uses single quotes -- 'value', never "value". Double
// quotes belong to the surrounding C++ `R"CFJS( ... )CFJS"` raw-string delimiters, and matching those
// as "quoted strings" would treat an entire multi-thousand-line fragment as a single string spanning
// from the first `R"CFJS(` to the last `)CFJS"` in the file. Bounded to one line: a real JS string
// literal here never contains a literal newline.
const STRING_RE = /'(?:[^'\\\n]|\\.)*'/g;

function jsFiles() {
  return fs.readdirSync(PANO)
    .filter((f) => /Js\.h$/.test(f) && f !== EXEMPT_FILE)
    .sort();
}

// Counts every hex-in-a-quoted-string occurrence in one file's raw text (the same text the C++
// preprocessor and MSVC see -- not just the extracted JS body, since a violation belongs to the FILE
// regardless of which side of the R"CFJS( ... )CFJS" delimiter it happens to fall on).
function countHexViolations(source) {
  let count = 0;
  const hits = [];
  STRING_RE.lastIndex = 0;
  let m;
  while ((m = STRING_RE.exec(source))) {
    const literal = m[0];
    const hexHits = literal.match(HEX_RE);
    if (hexHits) {
      count += hexHits.length;
      hits.push(literal);
    }
  }
  return { count, hits };
}

function scan() {
  const perFile = new Map();
  let total = 0;
  for (const file of jsFiles()) {
    const src = fs.readFileSync(path.join(PANO, file), 'utf8');
    const { count, hits } = countHexViolations(src);
    if (count > 0) perFile.set(file, { count, hits });
    total += count;
  }
  return { total, perFile };
}

// Recorded 2026-09-13, the moment this lint was written, from the same scan() above run against the
// tree immediately before P8a's four shared primitives were added (notice/armedButton/confirmModal/
// contextMenuShell/settingsPopup went into DeadlockControlsJs.h using S tokens throughout, so they do
// not move this number). Lower this when a later phase fixes a file's colours; never raise it to make
// a new violation pass.
const BASELINE = 86;

test('DeadlockStyleJs.h itself is exempt and is not scanned', () => {
  assert.ok(fs.existsSync(path.join(PANO, EXEMPT_FILE)), 'DeadlockStyleJs.h is missing');
  assert.ok(!jsFiles().includes(EXEMPT_FILE), 'the exempt file leaked into the scanned list');
});

test('the scan actually finds real hex literals, not zero from a broken regex', () => {
  // A regex that stopped matching (e.g. because someone "fixed" it into matching nothing) would let
  // every violation through silently. DeadlockTimelineJs.h's own copied CS2 palette --
  // Q = { accent: '#f0b323ff', ... } -- is a real, known violation that must always be found.
  const { perFile } = scan();
  const timeline = perFile.get('DeadlockTimelineJs.h');
  assert.ok(timeline && timeline.count > 0, 'expected DeadlockTimelineJs.h to have known violations');
  assert.ok(timeline.hits.includes("'#f0b323ff'"), 'did not find the known Q.accent literal');
});

test('the Deadlock UI has no more raw hex colour literals than the recorded baseline', () => {
  const { total, perFile } = scan();
  if (total > BASELINE) {
    const breakdown = [...perFile.entries()]
      .map(([f, v]) => `  ${f}: ${v.count}`)
      .join('\n');
    assert.fail(
      `${total} raw hex colour literals found, more than the recorded baseline of ${BASELINE}. ` +
      `A new violation was added somewhere in AfxHookSource2/Filmmaker/Deadlock -- use S.* tokens ` +
      `instead (add one to DeadlockStyleJs.h's S table if the colour is new). Breakdown:\n${breakdown}`
    );
  }
  // Not asserted equal: a future phase that fixes a file's colours should lower BASELINE above, and
  // this must keep passing in the meantime rather than demanding the fix happen everywhere at once.
  assert.ok(total <= BASELINE);
});

test('the P8a shared primitives use S tokens and introduce no violations of their own', () => {
  // The four primitives this lint shipped alongside (DeadlockControlsJs.h: notice, armedButton,
  // confirmModal, contextMenuShell, settingsPopup) needed two colours DeadlockStyleJs.h did not
  // already have a token for -- a modal scrim and a hard drop-shadow pair -- so those became new S
  // tokens rather than new inline literals. This pins that down directly, rather than only trusting
  // the aggregate count above to notice if it regressed.
  const style = fs.readFileSync(path.join(PANO, EXEMPT_FILE), 'utf8');
  for (const token of ['scrimDim', 'textShadowHard', 'boxShadowFloat']) {
    assert.match(style, new RegExp(`\\b${token}\\s*:`), `DeadlockStyleJs.h is missing the ${token} token`);
  }

  const controls = fs.readFileSync(path.join(PANO, 'DeadlockControlsJs.h'), 'utf8');
  for (const fn of ['buildNotice', 'armedButton', 'buildConfirmModal', 'buildContextMenuShell', 'buildSettingsPopup']) {
    assert.match(controls, new RegExp(`function ${fn}\\(`), `DeadlockControlsJs.h is missing ${fn}`);
  }
  for (const exported of ['notice:', 'armedButton:', 'confirmModal:', 'contextMenuShell:', 'settingsPopup:']) {
    assert.ok(controls.includes(exported), `$.MvmControls does not export ${exported.replace(':', '')}`);
  }

  // Each new function's OWN body -- not the whole file, which already carries the pre-existing
  // violations counted above -- must use S.* tokens rather than a fresh literal.
  const starts = ['function buildNotice(', 'function armedButton(', 'function buildConfirmModal(',
    'function buildContextMenuShell(', 'function buildSettingsPopup('];
  const firstStart = Math.min(...starts.map((s) => controls.indexOf(s)));
  const exportStart = controls.indexOf('$.MvmControls = {');
  assert.ok(firstStart >= 0 && exportStart > firstStart, 'could not locate the P8a primitives block');
  const newCode = controls.slice(firstStart, exportStart);
  const { count, hits } = countHexViolations(newCode);
  assert.equal(count, 0, `the P8a primitives introduced ${count} raw hex literal(s): ${hits.join(', ')}`);
});
