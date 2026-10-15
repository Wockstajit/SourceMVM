#!/usr/bin/env node
// Derive the console-command -> state-field bindings straight out of the extracted UI code, so
// clicking a control in the mockup changes the same state field the game would change.
//
// The widget helpers declare both halves in one place, which is what makes this mechanical:
//
//     var svFaMax = sliderRow(page, { label: 'Cone Size', cmd: 'mirv_filmmaker smoothview faMax',
//                                     min: 1, max: 45, unit: ' deg' });
//     ...
//     svFaMax.sync(st.svFaMax);          <- the field that command writes
//
// So: find the widget construction, read its `cmd`, then find the matching `.sync()` / `.set()`
// to learn the field. Hand-maintaining this table would go stale on the first rename; deriving it
// means a rename shows up as a dropped binding in the report instead of a wrong mockup.
//
// Run:  node UiMockup/derive-bindings.mjs   (after extract-ui.mjs)

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const GEN = path.join(HERE, 'generated');

// Walk forward from an open paren to its match, respecting quotes.
function callBody(src, openIdx) {
  let depth = 0, i = openIdx, q = null;
  for (; i < src.length; i++) {
    const c = src[i];
    if (q) {
      if (c === '\\') { i++; continue; }
      if (c === q) q = null;
      continue;
    }
    if (c === '"' || c === "'") { q = c; continue; }
    if (c === '(') depth++;
    else if (c === ')') { depth--; if (depth === 0) return src.slice(openIdx + 1, i); }
  }
  return '';
}

function findAll(src, re) {
  const out = [];
  let m;
  re.lastIndex = 0;
  while ((m = re.exec(src))) out.push(m);
  return out;
}

// Controls whose readback is fully computed (the slider parks at a neutral position while the
// feature is on "auto", so its sync argument never names a state field). Written out by hand
// because there is nothing in the source to derive them from -- kept small and explicit.
const ALIASES = {
  'mirv_filmmaker world exposure': 'exposure',
  'mirv_filmmaker world light amount': 'lightAmount',
  'mirv_filmmaker world light yaw': 'lightYaw',
  'mirv_filmmaker world light pitch': 'lightPitch',
  'mirv_filmmaker world sun glow_size': 'sunGlowSize',
  'mirv_filmmaker world sun yaw': 'sunYaw',
  'mirv_filmmaker world sun pitch': 'sunPitch',
  'mirv_filmmaker world customshadow strength': 'shaderShadowStrength',
  'mirv_filmmaker world customshadow reach': 'shaderShadowThickness',
  'mirv_filmmaker world customshadow bias': 'shaderShadowStepPixels',
  'mirv_filmmaker world customshadow steps': 'shaderShadowSteps',
  'mirv_filmmaker world modulate strength': 'mod_strength',
  'mirv_filmmaker world skyorient yaw': 'skyYaw',
  'mirv_filmmaker world skyorient pitch': 'skyPitch',
  'mirv_filmmaker world clouds density': 'cloudDensity',
  'mirv_filmmaker world clouds brightness': 'cloudBright',
  'mirv_filmmaker world clouds drift': 'cloudDrift',
  'mirv_filmmaker world clouds size': 'cloudSize',
  'mirv_filmmaker world shadows toggle': 'shadows',
  'mirv_filmmaker world customshadow toggle': 'shaderShadows',
  'mirv_filmmaker world skyorient mirror': 'skyMirror',
  'mirv_filmmaker world modulate world': 'mod_world_on',
  'mirv_filmmaker effects keep': 'fxKeep',
};

// The widget's own variable name is the last resort, accepted ONLY when it names a field that
// really exists in the state feed -- so a guess can never invent a field the UI never reads.
const STATE_FIELDS = new Set(
  [...fs.readFileSync(path.join(HERE, 'shim', 'state.js'), 'utf8')
    .matchAll(/([A-Za-z_][\w]*)\s*:/g)].map(m => m[1])
);

const bindings = [];
const unbound = [];

for (const file of fs.readdirSync(GEN).filter(f => f.endsWith('.js'))) {
  const surface = path.basename(file, '.js');
  const src = fs.readFileSync(path.join(GEN, file), 'utf8');

  const widgets = [
    { kind: 'slider', re: /\bvar\s+([A-Za-z_$][\w$]*)\s*=\s*sliderRow\s*\(/g, read: 'sync' },
    { kind: 'toggle', re: /\bvar\s+([A-Za-z_$][\w$]*)\s*=\s*togglePill\s*\(/g, read: 'set' },
    { kind: 'segmented', re: /\bvar\s+([A-Za-z_$][\w$]*)\s*=\s*fxSeg\s*\(/g, read: 'sync' },
    { kind: 'cycle', re: /\bvar\s+([A-Za-z_$][\w$]*)\s*=\s*cycleRow\s*\(/g, read: 'sync' },
  ];

  // Several controls sync through a computed local (`expSlider.sync(isAuto ? 1.0 : v)`), so the
  // field is read out of the whole sync ARGUMENT rather than assumed to be its first token.
  const fieldFromRead = (name, method) => {
    const at = src.indexOf(name + '.' + method + '(');
    if (at < 0) return null;
    const arg = callBody(src, at + name.length + method.length + 1);
    const m = /\bst\.([\w]+)/.exec(arg);
    return m ? m[1] : null;
  };

  for (const w of widgets) {
    for (const m of findAll(src, w.re)) {
      const name = m[1];
      const body = callBody(src, m.index + m[0].length - 1);

      // The command this widget issues.
      const cmdM = /cmd\s*:\s*'([^']+)'/.exec(body) || /cmd\s*\(\s*'([^']+)'/.exec(body);
      // fxSeg passes the category key positionally: fxSeg(parent, label, 'impacts', [...])
      const catM = w.kind === 'segmented' ? /,\s*'([a-z]+)'\s*,/.exec(body) : null;

      // The state field it reads back, found at the widget's own sync/set call site.
      const readField = fieldFromRead(name, w.read);

      const label = (/label\s*:\s*'([^']*)'/.exec(body) || [])[1]
        || (/,\s*'([^']{2,60})'\s*,/.exec(body) || [])[1] || '';

      const cmd = cmdM ? cmdM[1] : (catM ? 'mirv_filmmaker fx set ' + catM[1] : null);
      const entry = {
        surface, kind: w.kind, widget: name, label, cmd,
        field: readField
          || (cmd ? ALIASES[cmd.trim()] : null)
          || (STATE_FIELDS.has(name) ? name : null),
      };

      if (w.kind === 'slider') {
        const num = k => { const r = new RegExp(k + '\\s*:\\s*(-?[\\d.]+)').exec(body); return r ? parseFloat(r[1]) : undefined; };
        entry.min = num('min'); entry.max = num('max'); entry.decimals = num('decimals');
        const u = /unit\s*:\s*'([^']*)'/.exec(body); if (u) entry.unit = u[1];
      }

      if (entry.cmd && entry.field) bindings.push(entry);
      else unbound.push(entry);
    }
  }
}

// Toggles issue "<cmd> toggle" / "<cmd> on|off"; sliders append a value. Both are matched on the
// command PREFIX at runtime, so store the prefix explicitly.
for (const b of bindings) b.prefix = b.cmd.replace(/\s+(toggle|on|off)$/i, '');

// Structural Config resets come from the backend lists used by the real footers.
const resetSource = fs.readFileSync(path.join(HERE, '../AfxHookSource2/Filmmaker/Config/EffectsPersistence.cpp'), 'utf8');
const configResets = {};
for (const [group, symbol] of [['camera','Camera'],['effects','Effects'],['ragdolls','Ragdoll']]) {
  const body = resetSource.split('k' + symbol + 'Reset[] = {')[1].split('};')[0];
  configResets[group] = [...body.matchAll(/"(mirv_[^"]+)"/g)].map(m => m[1]);
}
const climateSource = fs.readFileSync(path.join(HERE, '../AfxHookSource2/Filmmaker/Environment/EnvironmentClimateMath.h'), 'utf8');
const climateDefaults = {};
for (const m of climateSource.matchAll(/\{"(\w+)", "[^"]+", ([.\d-]+)f?[,}]/g)) climateDefaults[m[1]] = +m[2];
const climateResetOff = [...climateSource.split('for (ClimateField field : {')[1].split('})')[0].matchAll(/CL_(\w+)/g)].map(m => m[1]);
fs.writeFileSync(path.join(GEN, 'bindings.json'), JSON.stringify({ bindings, unbound, configResets, climateDefaults, climateResetOff }, null, 2), 'utf8');

const byKind = {};
for (const b of bindings) byKind[b.kind] = (byKind[b.kind] || 0) + 1;
console.log('bound:', bindings.length, JSON.stringify(byKind));
console.log('unbound:', unbound.length);
for (const u of unbound.slice(0, 12)) {
  console.log('   ?', u.surface, u.kind, u.widget, '| cmd:', u.cmd || '-', '| field:', u.field || '-');
}
