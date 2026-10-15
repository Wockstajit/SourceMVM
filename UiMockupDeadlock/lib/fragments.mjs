// WHICH fragments the Deadlock workspace is made of, and in WHAT ORDER -- read out of the C++ that
// pushes them, never written down here.
//
// The order is load-bearing in a way that is easy to miss. All twelve fragments run into ONE
// JavaScript scope: the style fragment defines the helpers every other one calls, the drawing
// primitives have to exist before the timeline and the graph that draw with them, and the three
// docked editors must register before the shell, whose build() asks each of them to build into the
// dock body. A list maintained by hand would drift from the game the first time someone inserted a
// fragment in the C++ and not here -- and the symptom is a silently missing dock, which looks like
// a layout bug and is chased in the wrong file.
//
// So this parses DeadlockWorkspaces.cpp for its RunScript calls. If the C++ order changes, the
// mockup changes with it on the next extraction; if a fragment is added and the mockup cannot find
// which header defines it, extraction fails loudly instead of quietly dropping it.

import fs from 'node:fs';
import path from 'node:path';
import { tokenize } from './deadlock-macros.mjs';

// Reads the RunScript order. Deliberately NOT a C++ parser: it looks for the exact call shape the
// driver uses, and a line that is commented out is skipped, because a fragment temporarily disabled
// in the game must be disabled here too.
export function runScriptOrder(driverPath) {
  const src = fs.readFileSync(driverPath, 'utf8');
  const order = [];
  for (const line of src.split(/\r?\n/)) {
    if (/^\s*(\/\/|\/\*)/.test(line)) continue;
    const m = /RunScript\(\s*(DEADLOCK_[A-Z0-9_]+_JS)\s*\)/.exec(line);
    if (m && !order.includes(m[1])) order.push(m[1]);
  }
  if (!order.length) throw new Error(`${driverPath}: no RunScript(DEADLOCK_*_JS) calls found`);
  return order;
}

// macro name -> the header that #defines it, by reading every header rather than by a naming
// convention. DeadlockConfigRenderBodyJs.h defines two macros whose names match neither its own
// filename nor each other, so a convention would have to be wrong about at least one of them.
export function macroIndex(panoDir) {
  const index = {};
  for (const file of fs.readdirSync(panoDir)) {
    if (!/^Deadlock[A-Za-z0-9_]*Js\.h$/.test(file)) continue;
    const src = fs.readFileSync(path.join(panoDir, file), 'utf8');
    let macros;
    try {
      macros = tokenize(src, file);
    } catch {
      continue; // a *Js.h with no macro in it is not a fragment
    }
    for (const m of macros) {
      if (index[m.name]) {
        throw new Error(`macro ${m.name} is defined in both ${index[m.name]} and ${file}`);
      }
      index[m.name] = file;
    }
  }
  return index;
}

// The fragments as [{ macro, file }], in RunScript order, each resolved to the header it lives in.
export function fragments(panoDir, driverPath) {
  const index = macroIndex(panoDir);
  return runScriptOrder(driverPath).map((macro) => {
    const file = index[macro];
    if (!file) {
      throw new Error(
        `${macro} is pushed by the driver but no Deadlock*Js.h defines it -- ` +
        'the fragment was renamed or removed without updating the driver'
      );
    }
    return { macro, file };
  });
}
