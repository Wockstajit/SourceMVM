import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';

const source = fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Panorama/CameraEditorCustomizeJs.h', import.meta.url), 'utf8');
const handler = source.slice(source.indexOf('    function randomizeAgents() {'), source.indexOf('    var agentRandomBtn ='));

function run(agent, team = 'CT') {
  const commands = [];
  vm.runInNewContext(handler + '\nrandomizeAgents();', {
    COSMETICS: { agent }, custTargetTeam: team,
    decoratedOptions: () => agent.filter(row => row[2]?.team === team),
    cmd: command => commands.push(command), previewLog() {},
  });
  return commands;
}

test('randomize registers both teams even when customizing a CT or T', () => {
  const catalog = [
    ['Default', 'default', {}],
    ['CT', 'ct', { model: 'agents/models/ctm_sas/ctm_sas.vmdl', team: 'CT' }],
    ['T', 't', { model: 'agents/models/tm_phoenix/tm_phoenix.vmdl', team: 'T' }],
  ];
  for (const team of ['CT', 'T']) {
    const commands = run(catalog, team);
    const registered = commands.filter(c => c.startsWith('mirv_filmmaker cosmetics agentpool add ')).join(' ');
    for (const row of catalog.slice(1)) assert.ok(registered.includes(row[2].model));
    assert.ok(commands.indexOf('mirv_filmmaker cosmetics agentpool clear') < commands.findIndex(c => c.includes('agentpool add')));
    assert.ok(commands.findIndex(c => c.includes('allplayers agent random')) > commands.findLastIndex(c => c.includes('agentpool add')));
  }
});

test('variable-length paths reach the backend intact within the command limit', () => {
  const models = Array.from({ length: 83 }, (_, i) => `agents/models/ctm_${'a'.repeat(20 + i % 45)}/variant_${i}.vmdl`);
  const commands = run(models.map(model => [model, model, { model }]));
  const prefix = 'mirv_filmmaker cosmetics agentpool add ';
  const batches = commands.filter(c => c.startsWith(prefix));
  assert.ok(batches.every(c => c.length <= 240));
  assert.deepEqual(batches.flatMap(c => c.slice(prefix.length).split(' ')), models);
});

test('empty catalogs do not clear the existing pool or randomize', () => {
  assert.deepEqual(run([]), []);
});
