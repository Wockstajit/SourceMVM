import fs from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';
import assert from 'node:assert/strict';

test('hidden menu context does not capture input; show/hide events own the gate', () => {
  const source=fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Panorama/FilmmakerMenu.cpp',import.meta.url),'utf8');
  const fragment=source.split('static const char* kPauseMenuTrackerJs =')[1].split('if (m_bridge.RunScript')[0];
  const script=[...fragment.matchAll(/"(?:[^"\\]|\\.)*"/g)].map(m=>JSON.parse(m[0])).join('');
  const values={},events={};
  const panel={GetAttributeString:(k,f)=>values[k]??f,SetAttributeString:(k,v)=>values[k]=v};
  const context={$:{GetContextPanel:()=>panel,RegisterForUnhandledEvent:(k,fn)=>events[k]=fn}};
  vm.runInNewContext(script,context);
  assert.equal(values.mvmPauseMenuOpen,'0');
  for(const type of ['Pause','Main']) {
    events['CSGOShow'+type+'Menu']();assert.equal(values.mvmPauseMenuOpen,'1');
    events['CSGOHide'+type+'Menu']();assert.equal(values.mvmPauseMenuOpen,'0');
  }
  events.CSGOShowPauseMenu();vm.runInNewContext(script,context);
  assert.equal(values.mvmPauseMenuOpen,'1','reinstall must not clear a genuinely open menu');
});
