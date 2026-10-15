import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';

const source = fs.readFileSync(new URL('../livejs/deadlock-dump-ui-geometry.js', import.meta.url), 'utf8');
function panel(id, children = []) {
  return { id, children, visible: true, paneltype: 'Panel', actualxoffset: 4, actualyoffset: 6,
    actuallayoutwidth: 100, actuallayoutheight: 50, actualuiscale_x: 1, actualuiscale_y: 1,
    GetParent: () => null, GetChildCount() { return this.children.length; },
    GetChild(i) { return this.children[i]; } };
}
function run(root) {
  const lines = [];
  const ctx = panel('Hud');
  ctx.FindChildTraverse = id => id === 'MvmWorkspaceRoot' ? root : null;
  vm.runInNewContext(source, { $: { GetContextPanel: () => ctx, Msg: s => lines.push(s.trimEnd()) } }, { timeout: 1000 });
  assert.equal(lines.some(s => s.startsWith('GEOMERR')), false);
  return lines;
}
test('geometry keeps preorder and accumulated design coordinates', () => {
  const lines = run(panel('MvmWorkspaceRoot', [panel('child')]));
  const rows = lines.filter(s => s.startsWith('GEOM\t')).map(s => s.split('\t'));
  assert.deepEqual(rows.map(r => r.slice(1, 8)), [
    ['0', 'MvmWorkspaceRoot', 'Panel', '4', '6', '100', '50'],
    ['1', 'child', 'Panel', '8', '12', '100', '50'],
  ]);
  assert.ok(lines.includes('GEOMEND\t2'));
  assert.equal(lines.some(s => s.startsWith('GEOMTRUNCATED')), false);
});
test('wide tree stops fetching children at the shared row budget', () => {
  const root = panel('MvmWorkspaceRoot', Array.from({ length: 5000 }, (_, i) => panel('n' + i)));
  let reads = 0;
  root.GetChild = i => { reads++; return root.children[i]; };
  const lines = run(root);
  assert.equal(reads, 3999);
  assert.equal(lines.filter(s => s.startsWith('GEOM\t')).length, 4000);
  assert.ok(lines.includes('GEOMTRUNCATED\tpanel-limit'));
  assert.ok(lines.includes('GEOMEND\t4000'));
});
test('exactly full tree is not reported as truncated', () => {
  const lines = run(panel('MvmWorkspaceRoot', Array.from({ length: 3999 }, () => panel('leaf'))));
  assert.ok(lines.includes('GEOMEND\t4000'));
  assert.equal(lines.some(s => s.startsWith('GEOMTRUNCATED')), false);
});
test('cyclic or excessively deep tree stops without recursive stack growth', () => {
  const root = panel('MvmWorkspaceRoot');
  root.children.push(root);
  const lines = run(root);
  assert.ok(lines.includes('GEOMTRUNCATED\tdepth-limit'));
  assert.ok(lines.includes('GEOMEND\t65'));
});
test('null children cannot bypass the work budget', () => {
  const root = panel('MvmWorkspaceRoot');
  let reads = 0;
  root.GetChildCount = () => 100000;
  root.GetChild = () => { reads++; return null; };
  const lines = run(root);
  assert.equal(reads, 4000);
  assert.ok(lines.includes('GEOMTRUNCATED\tchild-limit'));
  assert.ok(lines.includes('GEOMEND\t1'));
});
