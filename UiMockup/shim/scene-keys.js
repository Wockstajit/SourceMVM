// Browser command/state adapter. Metadata is generated from the native catalog.
(function (global) {
  'use strict';
  var catalog = global.MvmSceneCatalog || [], colors = ['#e4ba69','#8fd29c','#87bafa','#d8a0dd','#a8a3e8','#73cbd0'];
  function clone(x) { return JSON.parse(JSON.stringify(x)); }
  // Scene object rigs: pose channels obj.<id>.<key>, numbered above the catalog (SceneRigMath.h).
  // A light's settings follow its pose (Rig::LightChannel): brightness, range, beam angle, emitter size, panel.
  var POSE_KEYS = ['x', 'y', 'z', 'pitch', 'yaw', 'roll', 'fov', 'intensity', 'range', 'cone', 'radius', 'width', 'height'];
  var POSE_LABELS = ['X', 'Y', 'Z', 'Pitch', 'Yaw', 'Roll', 'Field of view', 'Brightness', 'Range', 'Beam angle', 'Emitter size', 'Width', 'Height'];
  var LIGHT_RANGES = [[0, 8, 1], [16, 4096, 400], [5, 170, 45], [0, 64, 0], [1, 1024, 64], [1, 1024, 64]];
  var OBJECT_BASE = 1000000, OBJECT_STRIDE = 16, OBJECT_CHANNELS = 13, objectSpecs = {};
  function objectSpec(object, c) {
    var id = 'obj.' + object + '.' + POSE_KEYS[c];
    if (objectSpecs[id]) return objectSpecs[id];
    var angle = c >= 3 && c <= 5, lr = c >= 7 ? LIGHT_RANGES[c - 7] : null;
    return objectSpecs[id] = { id: id, label: POSE_LABELS[c], family: 'objects', group: 'Object ' + object, groupId: String(object),
      type: angle ? 'angle' : 'float', components: 1, min: lr ? lr[0] : c === 6 ? 1 : angle ? -180 : -32768, max: lr ? lr[1] : c === 6 ? 179 : angle ? 180 : 32768,
      initial: lr ? lr[2] : c === 6 ? 90 : 0, editLinear: true, clamp: true, object: object, c: c, ch: OBJECT_BASE + object * OBJECT_STRIDE + c };
  }
  // The rows an object's layer has (Rig::ChannelsFor).
  function channelsFor(kind) {
    var out = [0, 1, 2, 3, 4, 5];
    if (kind === 'camera') out.push(6);
    else if (kind === 'point' || kind === 'spot' || kind === 'area') {
      out.push(7, 8); if (kind !== 'point') out.push(9);
      if (kind === 'area') out.push(11, 12); else out.push(10);
    }
    return out;
  }
  function objectOf(id) { var m = /^obj\.([1-9][0-9]{0,6})\.([a-z]+)$/.exec(id || ''); var c = m ? POSE_KEYS.indexOf(m[2]) : -1; return c >= 0 ? objectSpec(+m[1], c) : null; }
  function specOfCh(ch) { if (ch >= OBJECT_BASE) { var r = ch - OBJECT_BASE; return r % OBJECT_STRIDE < OBJECT_CHANNELS ? objectSpec(Math.floor(r / OBJECT_STRIDE), r % OBJECT_STRIDE) : null; } return catalog[ch]; }
  function meta(id) { return catalog.filter(function (s) { return s.id === id; })[0] || objectOf(id); }
  function stepped(s) { return ['bool','enum','asset'].indexOf(s.type) >= 0; }
  function baseline() { var out={};catalog.forEach(function(s){out[s.id]=clone(s.initial);});return out; }
  function create() { return {catalog:catalog,baseline:baseline(),resolved:baseline(),armed:{},byField:{},events:[],hidden:{},solo:{},selected:[],nextId:1,rev:1,selectedEvent:-1,save:'saved',undo:[],redo:[]}; }
  function snapshot(sk) { return clone({byField:sk.byField,armed:sk.armed,nextId:sk.nextId,attach:sk.attach||{}}); }
  function begin(sk) { if (!sk.before) sk.before=snapshot(sk); }
  function end(sk) {if(!sk.before)return;if(JSON.stringify(sk.before)!==JSON.stringify(snapshot(sk))){sk.undo.push(sk.before);sk.redo=[];++sk.rev;}delete sk.before;}
  function cubic(a,b,c,d,t) {var u=1-t;return u*u*u*a+3*u*u*t*b+3*u*t*t*c+t*t*t*d;}
  function curve(a,b,u,right,left) {
    if(!(right&&right.a)&&!(left&&left.a))return a+(b-a)*u;
    var x1=right&&right.a?right.tx:1/3, x2=left&&left.a?1-left.tx:2/3;
    var y1=right&&right.a?a+right.dv:a, y2=left&&left.a?b+left.dv:b;
    var lo=0,hi=1,t=u;for(var i=0;i<36;i++){t=(lo+hi)/2;if(cubic(0,x1,x2,1,t)<u)lo=t;else hi=t;}
    return cubic(a,y1,y2,b,t);
  }
  function lab(rgb) {
    var c=rgb.slice(0,3).map(function(n){n/=255;return n<=.04045?n/12.92:Math.pow((n+.055)/1.055,2.4);});
    var l=Math.cbrt(.4122214708*c[0]+.5363325363*c[1]+.0514459929*c[2]),m=Math.cbrt(.2119034982*c[0]+.6806995451*c[1]+.1073969566*c[2]),s=Math.cbrt(.0883024619*c[0]+.2817188376*c[1]+.6299787005*c[2]);
    return [.2104542553*l+.793617785*m-.0040720468*s,1.9779984951*l-2.428592205*m+.4505937099*s,.0259040371*l+.7827717662*m-.808675766*s];
  }
  function rgb(c) {
    var l=Math.pow(c[0]+.3963377774*c[1]+.2158037573*c[2],3),m=Math.pow(c[0]-.1055613458*c[1]-.0638541728*c[2],3),s=Math.pow(c[0]-.0894841775*c[1]-1.291485548*c[2],3);
    return [4.0767416621*l-3.3077115913*m+.2309699292*s,-1.2684380046*l+2.6097574011*m-.3413193965*s,-.0041960863*l-.7034186147*m+1.707614701*s].map(function(n){return Math.max(0,Math.min(255,255*(n<=.0031308?12.92*n:1.055*Math.pow(n,1/2.4)-.055)));});
  }
  function sample(s,keys,tick,base) {
    if(s.clamp){if(!keys||!keys.length)return clone(base);if(tick<=keys[0].tick)return clone(keys[0].value);if(tick>=keys[keys.length-1].tick)return clone(keys[keys.length-1].value);}
    else if(!keys||keys.length<2||tick<keys[0].tick||tick>keys[keys.length-1].tick)return clone(base);
    for(var i=0;i<keys.length;i++)if(keys[i].tick===tick)return clone(keys[i].value);
    for(i=0;i+1<keys.length;i++)if(tick>keys[i].tick&&tick<keys[i+1].tick)break;
    var a=keys[i],b=keys[i+1],u=(tick-a.tick)/(b.tick-a.tick);
    if(stepped(s)||a.hold)return clone(a.value);
    if(s.type==='colour'){var ease=Math.max(0,Math.min(1,curve(0,1,u,a.cr,b.cl))),la=lab(a.value),lb=lab(b.value);var out=rgb(la.map(function(v,j){return v+(lb[j]-v)*ease;}));if(s.components===4)out.push(a.value[3]+(b.value[3]-a.value[3])*ease);return out;}
    if(s.type==='angle'){var delta=((b.value-a.value)%360+360)%360;if(a.direction===2&&delta>0)delta-=360;else if(!a.direction&&delta>180)delta-=360;var angle=a.value+curve(0,delta,u,a.cr,b.cl);while(angle>s.max)angle-=360;while(angle<s.min)angle+=360;return angle;}
    return Math.max(s.min,Math.min(s.max,curve(a.value,b.value,u,a.cr,b.cl)));
  }
  function editable(sk,id,tick){var exact=(sk.byField[id]||[]).filter(function(k){return k.tick===Math.round(tick);})[0];return exact?exact.value:sk.resolved[id];}
  function sync(sk,tick) {
    sk.resolved=clone(sk.baseline);catalog.forEach(function(s){sk.resolved[s.id]=sample(s,sk.byField[s.id],tick,sk.baseline[s.id]);});
    var events={};Object.keys(sk.byField).forEach(function(id){sk.byField[id].forEach(function(k){(events[k.tick]||(events[k.tick]=[])).push({id:id,value:k.value});});});
    sk.events=Object.keys(events).map(function(t){return {tick:+t,fields:events[t]};}).sort(function(a,b){return a.tick-b.tick;});
    var anySolo=Object.keys(sk.solo).some(function(id){return sk.solo[id];});
    var objectIds={};Object.keys(sk.byField).concat(Object.keys(sk.armed).filter(function(id){return sk.armed[id];})).forEach(function(id){var o=objectOf(id);if(o)objectIds[o.object]=true;});
    var specs=catalog.map(function(s,ch){return {s:s,ch:ch,group:s.group};});
    // Every scene camera and light has its layer, keyed or not (SceneRig_TimelineObjects).
    Object.keys(sk.objectKinds||{}).forEach(function(id){var k=sk.objectKinds[id];if(k==='camera'||k==='point'||k==='spot'||k==='area')objectIds[id]=true;});
    // A removed object's layer goes with it (SceneKeys.cpp: SceneRig_ObjectExists); its keys stay.
    Object.keys(objectIds).map(Number).sort(function(a,b){return a-b;}).filter(function(object){return !sk.objectNames||sk.objectNames[object]!==undefined;}).forEach(function(object){channelsFor(sk.objectKinds&&sk.objectKinds[object]).forEach(function(c){var os=objectSpec(object,c);specs.push({s:os,ch:os.ch,group:(sk.objectNames&&sk.objectNames[object])||('Object '+object+' (removed)'),fit:c<=2||c===6,always:true});});});
    sk.sceneChannels=specs.map(function(e){var s=e.s,ch=e.ch,keys=sk.byField[s.id]||[],live=!!keys.length||!!sk.armed[s.id];if(!live&&!e.always)return null;var unit=s.type==='colour'||s.type==='asset',lo=unit?0:s.min,hi=unit?1:s.max,scale=1/Math.max(1,keys.length-1);
      if(e.fit){var rest=(sk.objectRest&&sk.objectRest[s.id]!=null)?sk.objectRest[s.id]:s.initial,a=rest,b=rest;keys.forEach(function(k){a=Math.min(a,k.value);b=Math.max(b,k.value);});var pad=Math.max((b-a)*.15,s.c===6?5:16);lo=Math.max(s.min,a-pad);hi=Math.min(s.max,b+pad);}
      return {
      ch:ch,fieldId:s.id,name:s.label,family:s.family,group:e.group,groupId:s.family+'.'+s.groupId,type:s.type,live:live,color:colors[ch%colors.length],vis:!sk.hidden[s.id]&&(!anySolo||!!sk.solo[s.id]),solo:!!sk.solo[s.id],edit:!anySolo||!!sk.solo[s.id],min:lo,max:hi,value:editable(sk,s.id,tick),
      keys:keys.map(function(k,i){var cl=clone(k.cl||{a:false,tx:1/3,dv:0}),cr=clone(k.cr||{a:false,tx:1/3,dv:0});if(s.type==='colour'){cl.dv*=scale;cr.dv*=scale;}return {id:k.id,t:k.tick,v:unit?i*scale:k.value,sel:sk.selected.indexOf(k.id)>=0,cl:cl,cr:cr,direction:k.direction||0,hold:!!k.hold&&!stepped(s),hin:i>0&&!!keys[i-1].hold&&!stepped(s),swatch:s.type==='colour'?'rgb('+k.value.slice(0,3).map(Math.round).join(',')+')':undefined};})};}).filter(Boolean);
    sk.canUndo=!!sk.undo.length;sk.canRedo=!!sk.redo.length;
  }
  function put(sk,s,tick,value) {
    if(!s||!isFinite(tick)||tick<0)return;var vals=Array.isArray(value)?value:[value];
    if(s.type==='asset'){if(typeof value!=='string'||!/^[a-zA-Z0-9_-]*$/.test(value))return;}
    else if(vals.length!==s.components||vals.some(function(n){return !isFinite(n)||n<s.min||n>s.max||(stepped(s)&&Math.floor(n)!==n);}))return;
    tick=Math.round(tick);var keys=sk.byField[s.id]||(sk.byField[s.id]=[]),k=keys.filter(function(k){return k.tick===tick;})[0];
    if(!k){k={id:sk.nextId++,tick:tick,cl:{a:false,tx:1/3,dv:0},cr:{a:false,tx:1/3,dv:0},direction:0,hold:false};keys.push(k);}k.value=clone(value);keys.sort(function(a,b){return a.tick-b.tick;});if(s.editLinear){var ki=keys.indexOf(k);k.hold=false;if(ki>0)keys[ki-1].hold=false;}++sk.rev;return k.id;
  }
  function selected(sk,fn){Object.keys(sk.byField).forEach(function(id){sk.byField[id].forEach(function(k){if(sk.selected.indexOf(k.id)>=0)fn(k,meta(id));});});}
  function remove(sk){Object.keys(sk.byField).forEach(function(id){sk.byField[id]=sk.byField[id].filter(function(k){return sk.selected.indexOf(k.id)<0;});if(!sk.byField[id].length)delete sk.byField[id];});sk.selected=[];}
  function move(sk,dt,dv){var before=clone(sk.byField);if(sk.before)sk.byField=clone(sk.before.byField);selected(sk,function(k,s){k.tick=Math.round(k.tick+dt);if(!stepped(s)&&s.type!=='colour')k.value=Math.max(s.min,Math.min(s.max,k.value+dv));});var valid=Object.keys(sk.byField).every(function(id){var seen={};return sk.byField[id].every(function(k){if(k.tick<0||seen[k.tick])return false;seen[k.tick]=true;return true;});});if(!valid)sk.byField=before;else Object.keys(sk.byField).forEach(function(id){sk.byField[id].sort(function(a,b){return a.tick-b.tick;});});}
  // Every property a target names: all, a catalog group, a scene object's layer (objects.<id>) or one id.
  function specsFor(target){var out=catalog.filter(function(s){return target==='all'||s.family+'.'+s.groupId===target||s.id===target;});var m=/^objects\.([0-9]+)$/.exec(target||''),one=objectOf(target);
    if(m)for(var c=0;c<OBJECT_CHANNELS;c++)out.push(objectSpec(+m[1],c));else if(one)out.push(one);return out;}
  function run(st,t) {
    var sk=st.sceneKeys,op=t[0],id=t[1],s=meta(id),tick=Math.round(st.tick);sync(sk,tick);
    if(op==='graph'){op=t[1];t=t.slice(1);s=specOfCh(+t[1]);id=s&&s.id;}
    if(op==='editbegin'){begin(sk);sk.gesture=true;return true;}
    if(op==='editend'){end(sk);sk.gesture=false;}
    else if(op==='undo'||op==='redo'){end(sk);sk.gesture=false;var from=op==='undo'?sk.undo:sk.redo,to=op==='undo'?sk.redo:sk.undo;if(from.length){to.push(snapshot(sk));Object.assign(sk,from.pop());sk.selected=[];}}
    else if(op==='chan'&&s){if(t[2]==='solo'||t[2]==='unsolo')sk.solo[id]=t[2]==='solo';else sk.hidden[id]=t[2]==='hide';sk.activeCh=+t[1];}
    else if(op==='select'||op==='selset'||op==='selall'||op==='selclear'){
      if(t[3]!=='add')sk.selected=[];
      if(op==='select'&&s){sk.selected.push(+t[2]);sk.activeCh=+t[1];sk.selectedEvent=-1;}
      else if(op==='select'){sk.selectedEvent=+t[1];var ev=sk.events[sk.selectedEvent];if(ev)Object.keys(sk.byField).forEach(function(id){sk.byField[id].forEach(function(k){if(k.tick===ev.tick)sk.selected.push(k.id);});});}
      if(op==='selall')Object.keys(sk.byField).forEach(function(id){sk.byField[id].forEach(function(k){sk.selected.push(k.id);});});
      if(op==='selset')for(var si=1;si+1<t.length;si+=2)sk.selected.push(+t[si+1]);
    } else {
      begin(sk);
      if(op==='arm'||op==='strip'){if(s)sk.armed[id]=t[2]==='on'?true:t[2]==='off'?false:!sk.armed[id];}
      else if(op==='group'){var group=specsFor(id),all=group.every(function(s){return sk.armed[s.id];});group.forEach(function(s){sk.armed[s.id]=!all;});}
      else if(op==='add'){sk.selected=[];catalog.forEach(function(s){if(sk.armed[s.id])sk.selected.push(put(sk,s,tick,editable(sk,s.id,tick)));});}
      // An object's row arms on its first value edit (SceneKeys.cpp), then keys like any armed row.
      else if(op==='set'&&s&&objectOf(id)&&!sk.armed[id]&&(sk.armed[id]=true,false)){}
      else if((op==='set'&&s&&sk.armed[id])||op==='graphadd'){var value=op==='graphadd'?editable(sk,id,+t[2]):s.type==='asset'?(t[2]==='default'?'':t[2]):s.components===1?+t[2]:t.slice(2).map(Number);var kid=put(sk,s,op==='graphadd'?+t[2]:tick,value);if(kid)sk.selected=[kid];}
      else if(op==='addkey'&&s&&!stepped(s)&&s.type!=='colour'){var kid=put(sk,s,+t[2],+t[3]);if(kid)sk.selected=[kid];}
      else if(op==='delsel')remove(sk);
      else if(op==='movesel')move(sk,+t[1],+t[2]);
      else if(op==='clear'){specsFor(id).forEach(function(s){delete sk.byField[s.id];delete sk.armed[s.id];});sk.selected=[];}
      else if(op==='handle'&&s&&!stepped(s)){var key=(sk.byField[id]||[]).filter(function(k){return k.id===+t[2];})[0];if(key){var hdv=+t[5];if(s.type==='colour')hdv*=Math.max(1,sk.byField[id].length-1);else hdv=Math.max(s.min,Math.min(s.max,key.value+hdv))-key.value;key[t[3]==='left'?'cl':'cr']={a:true,tx:Math.max(0,Math.min(1,+t[4])),dv:hdv};var hk=sk.byField[id],hi=hk.indexOf(key);if(t[3]!=='left')key.hold=false;else if(hi>0)hk[hi-1].hold=false;}}
      else if(op==='hold'){Object.keys(sk.byField).forEach(function(id){if(stepped(meta(id)))return;sk.byField[id].forEach(function(k){if(!sk.selected.length||sk.selected.indexOf(k.id)>=0)k.hold=true;});});}
      else if(['smooth','linear','ease','direction'].indexOf(op)>=0){Object.keys(sk.byField).forEach(function(id){if(stepped(meta(id)))return;sk.byField[id].forEach(function(k){if(sk.selected.length&&sk.selected.indexOf(k.id)<0)return;if(op==='direction'){k.direction=+t[1];return;}var sm=op==='smooth',how=op==='ease'?t[1]:'inout',ks=sk.byField[id],ki=ks.indexOf(k);if(how!=='in')k.hold=false;if(how!=='out'&&ki>0)ks[ki-1].hold=false;k.cl={a:op!=='linear'&&!(op==='ease'&&t[1]==='out'),auto:sm,tx:1/3,dv:0};k.cr={a:op!=='linear'&&!(op==='ease'&&t[1]==='in'),auto:sm,tx:1/3,dv:0};});});}
      else if(op==='macro'){var n=['dawn','day','golden','dusk','night','moonlit'].indexOf(id);if(n>=0){var col=[[255,180,140],[255,245,230],[255,185,95],[185,150,220],[90,115,190],[155,190,255]][n],yaw=[70,135,240,270,300,320][n],el=[8,40,12,2,-20,-8][n];var values={exposure:[.65,1,.8,.45,.12,.25][n],lightColor:col,lightAmount:.8,sunPositionOverride:1,'sunPosition.yaw':yaw,'sunPosition.pitch':el,'env.sunAzimuth':yaw,'env.sunElevation':el,'env.fogDensity':[.3,.05,.2,.35,.45,.3][n],skyTint:1,skyColor:col.concat([255]),'env.saturation':n<3?1.05:.8};sk.selected=[];Object.keys(values).forEach(function(id){sk.armed[id]=true;sk.selected.push(put(sk,meta(id),tick,values[id]));});}}
      // SceneKeys RefreshAutoTangents: Smooth sides follow their neighbours (colour = flat ease).
      Object.keys(sk.byField).forEach(function(fid){var fs=meta(fid);if(!fs||stepped(fs))return;var ks=sk.byField[fid],n=ks.length,u=[];
        ks.forEach(function(k,i){if(fs.type==='colour'){u.push(0);return;}if(!i||fs.type!=='angle'){u.push(k.value);return;}var d=((k.value-ks[i-1].value)%360+540)%360-180;u.push(u[i-1]+d);});
        ks.forEach(function(k,i){if(!(k.cl&&k.cl.auto)&&!(k.cr&&k.cr.auto))return;var a=Math.max(0,i-1),b=Math.min(n-1,i+1),m=(fs.type!=='colour'&&n>1&&ks[b].tick-ks[a].tick>1e-9)?(u[b]-u[a])/(ks[b].tick-ks[a].tick):0;
          var clampDv=function(dv){return fs.type==='float'?Math.max(fs.min,Math.min(fs.max,k.value+dv))-k.value:dv;};
          if(k.cl&&k.cl.auto)k.cl={a:i>0,auto:true,tx:1/3,dv:i>0?clampDv(-m*(k.tick-ks[i-1].tick)/3):0};
          if(k.cr&&k.cr.auto)k.cr={a:i+1<n,auto:true,tx:1/3,dv:i+1<n?clampDv(m*(ks[i+1].tick-k.tick)/3):0};});});
      if(!sk.gesture)end(sk);
    }
    ++sk.rev;sync(sk,st.scrubbing?st.playhead:st.tick);return true;
  }
  global.MvmSceneKeys={create:create,sync:sync,run:run,meta:meta,baseline:baseline,sample:sample,objectSpec:objectSpec,put:put,begin:begin,end:end};
})(window);
