import fs from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';
import assert from 'node:assert/strict';
const source=fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Panorama/ConfigHudWidgetsJs.h',import.meta.url),'utf8');
test('slider hit testing rejects clipped rows beneath pinned chrome',()=>{
 // Slice to the NEXT function declaration, not to a comment: a comment is prose and moves,
 // and indexOf returning -1 silently hands vm an empty program and a passing-looking fail.
 const from=source.indexOf('    function cfgOverPanel(');
 const to=source.indexOf('    function cfgSyncHitTest(',from);
 assert.ok(from>=0&&to>from,'cfgOverPanel must precede cfgSyncHitTest');
 const code=source.slice(from,to);
 const context={cfgPanelOnScreen:p=>p.visible,scrollView:{GetPositionWithinWindow:()=>({x:100,y:100}),actuallayoutwidth:300,actuallayoutheight:400}};
 vm.createContext(context);vm.runInContext(code,context);
 const panel={visible:true,GetPositionWithinWindow:()=>({x:100,y:80}),actuallayoutwidth:300,actuallayoutheight:50};
 assert.equal(context.cfgOverPanel(panel,150,90),false);
 assert.equal(context.cfgOverPanel(panel,150,110),true);
 panel.visible=false;assert.equal(context.cfgOverPanel(panel,150,110),false);
 panel.visible=true;panel.GetPositionWithinWindow=()=>({x:100,y:480});
 assert.equal(context.cfgOverPanel(panel,150,510),false);
 assert.equal(context.cfgOverPanel(panel,150,490),true);
});
