import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { parseMenuCapture, sanitizeMenuFixture } from '../../UiMockup/import-menu-fixture.mjs';
import { prepareMenu } from '../../UiMockup/prepare-menu.mjs';

function rows(kind, value, size = 13) {
  const source = JSON.stringify(value), out = [];
  for (let offset = 0; offset < source.length; offset += size)
    out.push('netcon MENUFIXTURE ' + JSON.stringify({ kind, offset, end: offset + size >= source.length, chunk: source.slice(offset, offset + size) }));
  return out;
}
const capture = () => [...rows('demos', { demos: [{ fileName: 'match.dem', folder: 'C:/private/replays' }] }), ...rows('maps', [{ name: 'Training map' }])];

test('menu capture reconstructs chunked Unicode and repeated tile records', () => {
  const result = parseMenuCapture([...capture(), ...rows('tile', { title: 'Élévation' }), ...rows('tile', { title: 'Boulder' })].join('\n'));
  assert.deepEqual(result.tiles.map(t => t.title), ['Élévation', 'Boulder']);
  assert.equal(result.demos.demos[0].fileName, 'match.dem');
});
test('truncated, missing and restarted chunks are rejected instead of importing partial data', () => {
  const valid = capture();
  assert.throws(() => parseMenuCapture(valid.slice(0, -1).join('\n')), /Incomplete/);
  assert.throws(() => parseMenuCapture(valid.filter((_, i) => i !== 1).join('\n')), /Incomplete/);
  assert.throws(() => parseMenuCapture([valid[0], valid[0], ...valid.slice(1)].join('\n')), /Incomplete/);
});
test('unknown capture kinds cannot write object prototype fields', () => {
  assert.throws(() => parseMenuCapture(rows('__proto__', {}).join('\n')), /Invalid/);
});
test('sanitization removes machine paths while retaining visible demo organization', () => {
  const data = parseMenuCapture(capture().join('\n'));
  data.demos.installRoot = 'C:/private/game'; data.demos.folders = ['C:/private/replays'];
  data.workshop = { root: 'D:/private/workshop' };
  data.organizer = { demos: { 'c:\\private\\replays\\match.dem': { group: 'Movies' }, 'c:\\private\\hidden.dem': { group: 'Hidden' } } };
  const result = sanitizeMenuFixture(data);
  assert.equal(JSON.stringify(result).includes('private'), false);
  assert.deepEqual(result.organizer.demos, { 'mock-game\\replays\\match.dem': { group: 'Movies' } });
});
test('native menu bundle contains the Workshop GO dialog and scoreboard layouts', () => {
  const output = fs.mkdtempSync(path.join(os.tmpdir(), 'mvm-menu-test-'));
  try {
    prepareMenu(output);
    const bundle = JSON.parse(fs.readFileSync(path.join(output, 'native-menu.json'), 'utf8'));
    assert.match(bundle.layouts['popups/popup_workshop_mode_select'], /CancelButton/);
    assert.match(bundle.layouts.matchinfo, /players-table-CT/);
    assert.match(bundle.layouts['matchtiles/player'], /MatchTile/);
  } finally { fs.rmSync(output, { recursive: true, force: true }); }
});
