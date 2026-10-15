// Mock command/state contract: real UI commands, stable IDs and one undo transaction per drag.
import fs from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';
import assert from 'node:assert/strict';
function harness() {
 const c={window:{},console};vm.createContext(c);
 for(const f of ['scene-catalog.js','scene-keys.js','state.js','commands.js'])vm.runInContext(fs.readFileSync(new URL('../../UiMockup/shim/'+f,import.meta.url),'utf8'),c);
 const s=c.window.MvmState.make();
 const run=cmd=>c.window.MvmCommands.run(s,'mirv_filmmaker environment '+cmd);
 return {s,run,context:c,command:cmd=>c.window.MvmCommands.run(s,cmd),
   world:cmd=>c.window.MvmCommands.run(s,'mirv_filmmaker world '+cmd)};
}

test('Config helpers use the actual native eval verb',()=>{
 const {context,command}=harness();const calls=[];
 context.window.$={ConfigHud:{gotoTab:x=>calls.push(['tab',x]),
   card:(x,y)=>calls.push(['card',x,y]),cardPage:(x,y)=>calls.push(['page',x,y]),
   scrollTo:x=>calls.push(['scroll',x])}};
 for(const call of ["gotoTab('effects')","card('light.card',1)","cardPage('light.card',1)",'scrollTo(430)'])
   assert.equal(command('mirv_filmmaker config eval $.ConfigHud.'+call),true);
 assert.deepEqual(calls,[['tab','effects'],['card','light.card',1],['page','light.card',1],['scroll',430]]);
 assert.equal(command("mirv_filmmaker config ui_eval $.ConfigHud.gotoTab('effects')"),false);
});
test('native light, lightmap and shadow switches are independent',()=>{
 const {s,run,world}=harness();
 s.environment.climate.relighting=0;
 world('shadows off');assert.equal(s.bakedShadows,false);assert.equal(s.dynamicShadows,false);
 assert.equal(s.engineLights,true);assert.equal(s.bakedLighting,true);
 world('bakedshadows on');assert.equal(s.bakedShadows,true);assert.equal(s.dynamicShadows,false);
 world('light enabled off');assert.equal(s.engineLights,false);assert.equal(s.bakedShadows,true);
 run('climate enabled 1');run('climate bakedSuppressed 1');
 assert.equal(s.bakedLighting,false);assert.equal(s.bakedShadows,true);assert.equal(s.dynamicShadows,false);
 run('climate bakedSuppressed 0');assert.equal(s.bakedLighting,true);assert.equal(s.engineLights,false);
 run('climate relighting 1');assert.equal(s.bakedShadows,false);assert.equal(s.engineLights,false);
 run('climate relighting 0');assert.equal(s.bakedShadows,true);assert.equal(s.dynamicShadows,false);
 assert.equal(s.engineLights,false,'takeover restores the previous choice, not all-on');
});
test('create, select, edit, duplicate and delete use stable IDs',()=>{
 const {s,run}=harness();run('add point');const id=s.envSelected;
 assert.equal(s.environment.objects.find(o=>o.id===id).kind,'point');
 run('object '+id+' intensity 5');assert.equal(s.environment.objects.find(o=>o.id===id).intensity,5);
 run('duplicate '+id);const next=s.envSelected;assert.notEqual(next,id);
 run('remove '+id);assert.ok(s.environment.objects.some(o=>o.id===next));
 run('select '+next);assert.equal(s.envSelected,next);assert.ok(s.envGizmo);
 run('remove '+next);run('object '+next+' intensity 8');assert.equal(s.envSelected,0);
});
test('drag commits exactly one undo entry and remains axis constrained',()=>{
 const {s,run}=harness();run('select 1');run('edit start 1');
 const before=JSON.stringify(s.environment.objects[0].position);
 run('edit begin x 0.5 0.5 1.777');
 for(let i=1;i<=20;i++)run('edit drag '+(.5+i*.002)+' 0.5');
 assert.equal(s.envUndo.length,0);
 assert.notEqual(JSON.stringify(s.environment.objects[0].position),before);
 assert.equal(s.environment.objects[0].position[1],-220.5);
 run('edit commit');run('edit commit');assert.equal(s.envUndo.length,1);
 run('undo');assert.equal(JSON.stringify(s.environment.objects[0].position),before);
});
test('escape cancel and stopping restore drag without undo',()=>{
 const {s,run}=harness();run('edit start 1');const before=JSON.stringify(s.environment);
 run('edit begin z 0.5 0.5 1.777');run('edit drag 0.5 0.4');run('edit cancel');
 assert.equal(JSON.stringify(s.environment),before);assert.equal(s.envUndo.length,0);
 assert.equal(s.envEditing,1);run('edit cancel');assert.equal(s.envEditing,0);
 run('edit start 1');run('edit begin y 0.5 0.5 1.777');run('edit drag 0.6 0.4');run('edit stop');
 assert.equal(JSON.stringify(s.environment),before);assert.equal(s.envEditing,0);
});
test('scene names and fog values round trip; removed fields are rejected',()=>{
 const {s,run}=harness();run('climate fogStart 900');run('climate fogDensity 0.7');
 run('save "Fog Study"');run('climate fogStart 50');run('load "Fog Study"');
 assert.equal(s.environment.climate.fogStart,900);
 assert.equal(s.environment.climate.fogDensity,.7);
 run('climate quality 2');assert.ok(!('quality' in s.environment.climate));
 assert.equal(run('ray-preview render'),false);
});

test('shared gizmo cursor pump guards UI clicks and sends one commit',()=>{
 const c={renderHooks:[],activeTab:'environment',INSPECTOR_W:520,S:{label:'#fff'},sent:[]};
 const panel=()=>({style:{},visible:true,SetPanelEvent(){}});
 c.root={actualuiscale_x:1,actuallayoutwidth:1920,actuallayoutheight:1080,
   GetAttributeString:()=>'',GetPositionWithinWindow:()=>({x:0,y:0})};
 c.$={CreatePanel:panel};c.mk=panel;c.lbl=panel;c.settingsPopup={isOpen:()=>false};
 c.cfgDragging=()=>false;c.envCommand=cmd=>c.sent.push(cmd);
 vm.createContext(c);
 const code=fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Panorama/ConfigHudEnvironmentGizmoJs.h',import.meta.url),'utf8').split('R"CFJS(')[1].split(')CFJS"')[0];
 vm.runInContext(code,c);
 const s={envEditing:1,envEditRevision:1,envDragging:false,enabled:true,cursor:true,lmb:false,mx:960,my:540,
  envGizmo:{origin:[.5,.5,true],axes:[[.58,.5,true],[.5,.58,true],[.45,.44,true]],bounds:Array(8).fill([.5,.5,true])}};
 const frame=()=>c.renderHooks[0](s);
 frame();s.mx=1600;s.lmb=true;frame();assert.equal(c.sent.length,0,'inspector press ignored');
 s.lmb=false;frame();s.mx=1040;s.lmb=true;frame();
 assert.match(c.sent[0],/^edit begin x /,'axis tokens match native command');
 s.envDragging=true;s.envEditRevision=2;s.mx=1090;frame();assert.match(c.sent.at(-1),/^edit drag /);
 s.lmb=false;frame();frame();assert.equal(c.sent.filter(x=>x==='edit commit').length,1);
 c.sent.length=0;s.envDragging=false;s.lmb=true;c.settingsPopup.isOpen=()=>true;frame();
 assert.equal(c.sent.length,0,'popup cannot start a world drag');
});
test('Escape revision cancels a held gesture without a later commit',()=>{
 // Native transaction tests cover restoration; the UI must not re-commit after native cancel.
 const source=fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Panorama/ConfigHudEnvironmentGizmoJs.h',import.meta.url),'utf8');
 assert.match(source,/s.envEditRevision > envDragRevision && !s.envDragging/);
 const scene=fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Environment/EnvironmentScene.cpp',import.meta.url),'utf8');
 assert.match(scene,/const Vec3 delta=.*moved->position-g_drag.origin/);
 assert.doesNotMatch(scene,/SerializeScene\(g_scene\)!=SerializeScene\(g_dragBefore\)/);
});
