import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import vm from 'node:vm';

const header = readFileSync(new URL('../../AfxHookSource2/Filmmaker/Panorama/EasterEggHintJs.h', import.meta.url), 'utf8');
const source = header.match(/R"EGJS\(([\s\S]*?)\)EGJS"/)[1];

function harness(loud) {
  const panels = [];
  function panel() {
    const values = {};
    const p = { values, text: '', deleted: false, actuallayoutwidth: 400, actualuiscale_x: 1,
      IsValid() { return !this.deleted; }, DeleteAsync() { this.deleted = true; },
      style: new Proxy(values, {
        get: (target, key) => key === 'transform' && target[key]
          // Observed CS2 getter serialization; the setter rejects this space-separated form.
          ? target[key].replaceAll(',', '') : target[key],
        set: (target, key, value) => {
          if (key === 'transform') assert.match(value, /^translate3d\(-?\d+(?:\.\d+)?px, 0px, 0px\)$/);
          target[key] = value;
          return true;
        }
      }) };
    panels.push(p);
    return p;
  }
  const context = vm.createContext({
    st: { hint: { id: 1, loud, title: 'DISCO INFERNO', sub: 'the floor is yours.', code: '1976, burn baby burn' } },
    EGG: { hot: '#ff5fc8', sky: '#9fe6ff', lav: '#c9a7ff', mint: '#a8ffd8', pink: '#ff8fd8', gold: '#f0b323', blue: '#80bdff', cream: '#fff4fb' },
    layer: panel, lbl: (_parent, text) => Object.assign(panel(), { text }),
    $: { CreatePanel: panel }, now: () => 0, pick: a => a[0], blur: () => {},
    clamp01: x => Math.max(0, Math.min(1, x)),
    expoOut: t => t >= 1 ? 1 : 1 - Math.pow(2, -11 * t),
    expoIn: t => t <= 0 ? 0 : Math.pow(2, 11 * (t - 1)),
    size: { w: 1600, h: 1200 }, time: 0
  });
  vm.runInContext(source, context);
  return { context, panels, frame: time => { context.time = time; vm.runInContext('hintFrame(time, size)', context); } };
}

for (const loud of [false, true]) {
  test(`${loud ? 'loud' : 'faint'} hint renders entry, hold and exit despite Panorama transform readback`, () => {
    const h = harness(loud);
    h.frame(0);
    const hint = h.context.hint;
    for (let frame = 1; frame <= 600; frame++) {
      h.frame(frame / 60);
      if (h.context.hint) assert.equal(hint.code.values.transform, hint.sub.values.transform);
    }
    assert.equal(h.context.hint, null);
    assert.equal(hint.box.deleted, true);
    assert.equal(hint.sub.text, 'the floor is yours.');
  });
}

test('cancelled hints stop immediately and do not replay on a rebuilt overlay', () => {
  const h = harness(true);
  h.frame(0);
  const box = h.context.hint.box;
  h.context.st.hintCancelledThrough = 1;
  h.frame(0.1);
  assert.equal(box.deleted, true);
  assert.equal(h.context.hint, null);
  h.frame(0.2);
  assert.equal(h.context.hint, null);
  const rebuilt = harness(true);
  rebuilt.context.st.hintCancelledThrough = 1;
  rebuilt.frame(0);
  assert.equal(rebuilt.context.hint, null);
  rebuilt.context.st.hint.id = 2;
  rebuilt.frame(0.1);
  assert.ok(rebuilt.context.hint);
});
