import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import vm from 'node:vm';

const header = readFileSync(new URL('../../AfxHookSource2/Filmmaker/Panorama/EasterEggVegasJs.h', import.meta.url), 'utf8');
const source = header.match(/R"EGJS\(([\s\S]*?)\)EGJS"/)[1];

function harness({ stopThrows = false } = {}) {
  const panels = new Map(), calls = [];
  function panel(id) {
    const p = { style: {}, visible: true,
      SetMovie: value => calls.push(['movie', value]),
      UseAttachedAudioStream: () => {},
      Play: () => calls.push(['play']),
      Stop: () => { calls.push(['stop']); if (stopThrows) throw new Error('Stop failed'); } };
    panels.set(id, p);
    return p;
  }
  const c = vm.createContext({ st: {}, EGG: {}, now: () => 10,
    cssSize: () => ({ w: 1600, h: 1200 }), layer: panel, lbl: () => panel('label'),
    $: { CreatePanel: (_type, _parent, id) => panel(id), RegisterEventHandler: () => {}, Msg: () => {} } });
  vm.runInContext(source, c);
  return { panels, calls, sync: state => { c.st = state; vm.runInContext('vegasSync()', c); } };
}

test('cancel releases a playing movie once and permits a later intervention', () => {
  const h = harness();
  h.sync({ vegas: 1 });
  assert.equal(h.panels.get('MvmEggVegas').visible, true);
  h.sync({ vegas: 1, vegasCancelledThrough: 1 });
  h.sync({ vegas: 1, vegasCancelledThrough: 1 });
  assert.equal(h.panels.get('MvmEggVegas').visible, false);
  assert.equal(h.panels.get('MvmEggVegas').style.opacity, '0');
  assert.equal(h.calls.filter(c => c[0] === 'stop').length, 1);
  assert.deepEqual(h.calls.at(-1), ['movie', '']);
  h.sync({ vegas: 2, vegasCancelledThrough: 1 });
  assert.equal(h.panels.get('MvmEggVegas').visible, true);
  assert.equal(h.calls.filter(c => c[0] === 'play').length, 2);
});

test('cancellation before the first render never starts the cancelled clip', () => {
  const h = harness();
  h.sync({ vegas: 1, vegasCancelledThrough: 1 });
  assert.equal(h.panels.size, 0);
  assert.deepEqual(h.calls, []);
});

test('capture-hidden stops media and does not replay it when chrome returns', () => {
  const h = harness();
  h.sync({ vegas: 1 });
  h.sync({ vegas: 1, hidden: true });
  h.sync({ vegas: 1, hidden: false });
  assert.equal(h.panels.get('MvmEggVegas').visible, false);
  assert.equal(h.calls.filter(c => c[0] === 'play').length, 1);
  h.sync({ vegas: 2, hidden: true });
  h.sync({ vegas: 2, hidden: false });
  assert.equal(h.calls.filter(c => c[0] === 'play').length, 1);
});

test('movie source clears even when the player Stop method throws', () => {
  const h = harness({ stopThrows: true });
  h.sync({ vegas: 1 });
  h.sync({ vegas: 1, vegasCancelledThrough: 1 });
  assert.deepEqual(h.calls.at(-1), ['movie', '']);
});
