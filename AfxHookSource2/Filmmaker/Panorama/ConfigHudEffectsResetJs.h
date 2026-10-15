#pragma once
// Whole Effects tab: VFX, Lighting, Atmosphere and Weather & Sky.
R"CFJS(
    var fxFooter=footerCard(pageEffects);
    var fxKeep=keepToggle(fxFooter,'effects','fxKeep','VFX, lighting, smoke, tint and sky overrides');
    var fxReset=resetFooter(fxFooter,function(){return {label:'EFFECTS',
      cmds:['mirv_filmmaker effects reset effects'],
      local:function(){
        for(var key in fxResetLocal) if(fxResetLocal[key]) fxResetLocal[key]();
      }
    };},'effects');
    renderHooks.push(function(st){fxReset.paint();fxKeep.sync(st);});
)CFJS"
