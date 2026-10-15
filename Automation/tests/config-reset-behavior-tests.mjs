import fs from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';
import assert from 'node:assert/strict';
const root = new URL('../../', import.meta.url);
const read = file => fs.readFileSync(new URL(file, root), 'utf8');
const nav = read('AfxHookSource2/Filmmaker/Panorama/ConfigHudEnvironmentNavJs.h');
const widgets = read('AfxHookSource2/Filmmaker/Panorama/ConfigHudWidgetsJs.h');
function functionSource(source, name) {
  const start = source.indexOf('function ' + name + '(');
  assert.ok(start >= 0, name);
  let i = source.indexOf('{', start), depth = 0;
  for (; i < source.length; i++) {
    if (source[i] === '{') depth++;
    if (source[i] === '}' && --depth === 0) return source.slice(start, i + 1);
  }
  throw new Error('Incomplete function ' + name);
}
function fixture(extra = {}) {
  const names = ['env.sky','env.light','env.fog','env.looks'];
  const registered = {}, commands = [];
  const context = {commands, registered, envPassResetters:[],
    envCommand: t => commands.push('mirv_filmmaker environment ' + t),
    cmd: t => commands.push(t), $:{Msg(){}}, envPassPill(){},
    envState(){return context.scene;}, envClimate(){return context.scene.climate;}};
  for (const name of ['envSkySec','envLightSec','envFogSec','envGradeSec']) {
    const key = names.shift();
    context[name] = {__headerReset(spec){registered[key] = spec; return {};}};
  }
  vm.createContext(context);
  const defaults = nav.match(/var CL_DEFAULT = \{[\s\S]*?\n    \};/)[0];
  const neutral = nav.match(/var PASS_NEUTRAL = \[[\s\S]*?\n    \];/)[0];
  const setup = defaults + neutral + `
    var SCENE_DEFAULT={fogR:.45,fogG:.5,fogB:.6,exposure:1,tintR:1,tintG:1,tintB:1};
    var PASS_OFF={};
    for(var i=0;i<PASS_NEUTRAL.length;i++)PASS_OFF[PASS_NEUTRAL[i][0]]=PASS_NEUTRAL[i][1];
  `;
  const helpers = ['envAnyPassOn','envReleaseIfIdle','envResetCmds','envAfterReset','envSceneResetCmds'];
  vm.runInContext(setup + helpers.map(n => functionSource(nav,n)).join('\n')
    + functionSource(widgets,'cfgRunReset'), context);
  context.scene = {enabled:true, exposure:1, tint:[1,1,1], objects:[], weather:{type:0},
    climate:vm.runInContext('JSON.parse(JSON.stringify(CL_DEFAULT))',context), ...extra};
  context.scene.climate.enabled = 1;
  const cards = nav.slice(nav.indexOf('    var envSkyReset ='), nav.indexOf('    renderHooks.push',nav.indexOf('    var envSkyReset =')));
  vm.runInContext(cards, context);
  return context;
}
function replay(c) {
  // Model engine command side effects independently of the UI mirror: every
  // climate write enables Scene, and sun fields couple sky + lighting on.
  const out = structuredClone(c.scene);
  for (const command of c.commands) {
    const t = command.split(' ').slice(1);
    if (t[0] !== 'environment') continue;
    if (t[1] === 'climate') {
      out.enabled=true;out.climate[t[2]]=+t[3];
      if (/^(sunAzimuth|sunElevation|sunPower|sunColor[RGB])$/.test(t[2])) {
        out.climate.enabled=out.climate.sky=out.climate.relighting=1;
      }
    }
    if (t[1] === 'off') out.enabled=false;
    if (t[1] === 'set') {
      if(t[2]==='exposure')out.exposure=+t[3];
      if(/^tint[RGB]$/.test(t[2]))out.tint['RGB'.indexOf(t[2].slice(-1))]=+t[3];
    }
  }
  return out;
}
for (const name of ['env.sky','env.light','env.fog','env.looks']) {
  test(name + ' reset leaves an otherwise idle renderer off', () => {
    const c=fixture();
    if(name==='env.sky')c.scene.climate.sky=c.scene.climate.clouds=1;
    if(name==='env.light')c.scene.climate.relighting=c.scene.climate.worldGi=1;
    if(name==='env.fog')c.scene.climate.fogDensity=.4;
    if(name==='env.looks'){c.scene.exposure=2;c.scene.tint=[.3,.4,.5];c.scene.climate.bloom=.5;}
    c.cfgRunReset(c.registered[name]);
    const result=replay(c);
    assert.equal(result.enabled,false);
    assert.equal(result.climate.enabled,0);
    if(name==='env.sky'){assert.equal(result.climate.sky,0);assert.equal(result.climate.clouds,0);assert.equal(result.climate.relighting,0);}
    if(name==='env.light'){assert.equal(result.climate.ao,1);assert.equal(result.climate.contactShadows,1);assert.equal(result.climate.skyOcclusion,1);assert.equal(result.climate.worldGi,0);}
  });
}
test('reset never starts a disabled environment, including latent weather', () => {
  const c=fixture({enabled:false,weather:{type:2}});
  c.cfgRunReset(c.registered['env.sky']);
  assert.equal(replay(c).enabled,false);
});
test('reset preserves unrelated fog and placed objects', () => {
  for(const other of ['fog','object']) {
    const c=fixture();
    if(other==='fog')c.scene.climate.fogDensity=.4;
    else c.scene.objects=[{enabled:true,id:4}];
    c.scene.climate.sky=c.scene.climate.clouds=1;
    c.cfgRunReset(c.registered['env.sky']);
    const result=replay(c);
    assert.equal(result.enabled,true);
    assert.equal(result.climate.clouds,0);
    if(other==='fog')assert.equal(result.climate.fogDensity,.4);
    else assert.equal(result.objects.length,1);
  }
});
