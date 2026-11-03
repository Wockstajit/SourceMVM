// The CS:S UI uses CS2's geometry: every metric in UiTheme.h's `Metrics` must equal the same field
// of the `UI` table in PanoramaUiMetricsJs.h. A change on either side that is not made on the
// other fails here, so the two games cannot drift apart. Static, runs without either game.
import fs from 'node:fs';
import test from 'node:test';
import assert from 'node:assert/strict';

const read = (path) => fs.readFileSync(new URL(path, import.meta.url), 'utf8');
const js = read('../../AfxHookSource2/Filmmaker/Panorama/PanoramaUiMetricsJs.h');
const cpp = read('../../AfxHookSource/Filmmaker/Ui/UiTheme.h');

// `name: { a: 1, b: 2.5, c: 'text' }` entries of the JS UI table, numbers only.
function jsTable() {
  const table = {};
  for (const m of js.matchAll(/^\s*(\w+):\s*\{([^}]*)\}/gm)) {
    const fields = {};
    for (const f of m[2].matchAll(/(\w+):\s*(-?\d+(?:\.\d+)?)/g)) fields[f[1]] = Number(f[2]);
    table[m[1]] = fields;
  }
  return table;
}

// `struct Btn { float padV, padH, ...; };` gives the field order of each C++ metric struct.
function cppStructs() {
  const structs = {};
  for (const m of cpp.matchAll(/struct (\w+) \{ float ([^;]+); \};/g)) {
    structs[m[1]] = m[2].split(',').map((s) => s.trim());
  }
  return structs;
}

// `constexpr Btn btn = { 5, 12, 15, 3, 5 };` and `constexpr float tabBarH = 42;`
function cppMetrics() {
  const structs = cppStructs();
  const metrics = {};
  for (const m of cpp.matchAll(/constexpr (\w+) (\w+) = \{([^}]*)\};/g)) {
    const fields = structs[m[1]];
    assert.ok(fields, `UiTheme.h declares struct ${m[1]}`);
    const values = m[3].split(',').map((s) => Number(s.trim().replace(/f$/, '')));
    assert.equal(values.length, fields.length, `${m[2]} initialises every field of ${m[1]}`);
    metrics[m[2]] = Object.fromEntries(fields.map((f, i) => [f, values[i]]));
  }
  for (const m of cpp.matchAll(/constexpr float (\w+) = (-?\d+(?:\.\d+)?)f?;/g)) metrics[m[1]] = Number(m[2]);
  return metrics;
}

test('every CS:S metric equals CS2\'s', () => {
  const ui = jsTable();
  const metrics = cppMetrics();
  assert.ok(Object.keys(metrics).length >= 12, 'UiTheme.h metrics were found');
  for (const [name, value] of Object.entries(metrics)) {
    if (typeof value === 'number') {
      const m = js.match(new RegExp(`\\b${name}:\\s*(-?\\d+(?:\\.\\d+)?)`));
      assert.ok(m, `PanoramaUiMetricsJs.h has ${name}`);
      assert.equal(value, Number(m[1]), name);
      continue;
    }
    assert.ok(ui[name], `PanoramaUiMetricsJs.h has a ${name} table`);
    for (const [field, v] of Object.entries(value)) {
      // btnCompact and tabTop omit fields their C++ struct shares with btn and tab; the C++ side
      // fills those with 0 (btnCompact) or the parent's value (tabTop underline).
      if (!(field in ui[name])) continue;
      assert.equal(v, ui[name][field], `${name}.${field}`);
    }
  }
});

test('the default inspector width is CS2\'s', () => {
  const editor = read('../../AfxHookSource2/Filmmaker/Panorama/CameraEditorJs.h');
  const inspector = Number(editor.match(/var INSPECTOR_W = (\d+)/)[1]);
  assert.equal(cppMetrics().col.w, inspector);
  const math = read('../../AfxHookSource2/Filmmaker/Panorama/InspectorWidthMath.h');
  assert.equal(Number(math.match(/kDefault = (\d+(?:\.\d+)?)/)[1]), inspector);
});
