// Console-command router.
//
// In game, every control writes through GameInterfaceAPI.ConsoleCommand and the C++ side writes
// the result back into the next state frame. The mockup closes that same loop locally, so a click
// changes state and the next render() reflects it -- which is what makes the panels genuinely
// interactive instead of a static picture.
//
// Most of the table is DERIVED (generated/bindings.json) rather than written here; this file only
// owns the commands that do something structural, like moving the playhead, adding a keyframe, or
// opening a workspace. The structural verbs write the NAVIGATION model (st.nav / st.dock / st.movie
// / st.marker -- see shim/state.js); app.js turns that model into built and torn-down panels once
// per frame the way Filmmaker::RunMainThreadFrame does, so the router never touches a panel.

(function (global) {
  'use strict';

  var bindings = [];
  var byPrefix = [];
  var unhandled = {};
  var configResets = {}, climateDefaults = {}, climateResetOff = [];

  function loadBindings(json) {
    bindings = (json && json.bindings) || [];
    configResets = (json && json.configResets) || {};
    climateDefaults = (json && json.climateDefaults) || {};
    climateResetOff = (json && json.climateResetOff) || [];
    // Longest prefix first, so "world light amount" wins over a shorter "world light".
    byPrefix = bindings.slice().sort(function (a, b) { return b.prefix.length - a.prefix.length; });
  }

  // "mirv_filmmaker settings ui config.fx.tab vfx" also feeds the dedicated mirror fields the
  // scripts hydrate from, not just the generic ui{} bag.
  var UI_MIRROR = {
    'config.tab': 'uiConfigTab',
    'config.fx.tab': 'uiConfigFxTab',
    'config.smooth.tab': 'uiConfigSmoothTab',
    'editor.inspector': 'uiEditorInspector',
    'editor.advanced': 'uiEditorAdvanced'
  };
  var UI_MIRROR_BOOL = {
    'config.section.smoothview': 'uiConfigSmoothOpen',
    'config.section.handheld': 'uiConfigHandheldOpen'
  };
  // Keys the engine hands back as a NUMBER on the next state frame, not as the ui{} string it was
  // stored under. The inspector column's width is the one that matters: the grip persists it on
  // release (InspectorResizeJs.h) and C++ republishes it as st.inspW (InspectorWidth.h), which is
  // what every panel syncs from -- the editor, the docked timeline and the graph each keep their
  // own copy and only agree because that number comes back. Without the mirror the width lived in
  // ui.inspectorW where nothing reads it, so a dragged column snapped back to the default the
  // moment a panel rebuilt: switch to the Graph tab after a drag and it opened at 520.
  var UI_MIRROR_NUM = { inspectorW: 'inspW' };

  function num(v, dflt) { var n = parseFloat(v); return isFinite(n) ? n : (dflt || 0); }
  function boolWord(w, cur) {
    if (w === 'on' || w === '1' || w === 'true' || w === 'open') return true;
    if (w === 'off' || w === '0' || w === 'false' || w === 'close') return false;
    return !cur; // "toggle", or a bare command
  }
  function warn(s) { if (global.MvmMockup && global.MvmMockup.onLog) global.MvmMockup.onLog('[engine] ' + s); }

  // TimelineDock_Pick (Movie/TimelineDock.cpp). The tab is remembered across closes; whether it
  // is APPLIED (the timeline shown, the graph enabled) is the dock host's business, and app.js
  // re-runs Claim/Apply every frame exactly as the HUD drivers do.
  //   native|cs2|off|regular -> native, timeline|camera -> camera, graph -> graph,
  //   toggle -> camera<->graph (never lands on "no editor").
  function pickTab(st, name) {
    var m = String(name || '').toLowerCase();
    if (m === 'toggle') m = (st.dock.tab === 'camera') ? 'graph' : 'camera';
    else if (m === 'timeline') m = 'camera';
    else if (m === 'cs2' || m === 'off' || m === 'regular') m = 'native';
    if (m !== 'native' && m !== 'camera' && m !== 'graph') {
      warn('dock: unknown tab "' + name + '" (native|camera|graph|toggle).');
      return false;
    }
    st.dock.tab = m;
    return true;
  }

  // The two workspace switches. Mirrors the C++ command handlers, which are where the mutual
  // exclusion lives: `config on` closes the editor (ConfigHud_RunCommand), and CameraEditor_Set(true)
  // closes Config (Filmmaker.cpp). The per-frame yield in ConfigHud::RunFrame is in app.js.
  function setConfig(st, on) {
    if (!on && st.envEditing) environment(st, "mirv_filmmaker environment edit stop");
    st.nav.configOpen = !!on;
    if (st.nav.configOpen && st.nav.editorOpen) st.nav.editorOpen = false;
  }
  function setEditor(st, on) {
    st.nav.editorOpen = !!on;
    if (st.nav.editorOpen && st.nav.configOpen) st.nav.configOpen = false;
  }


  function syncEnvironment(st) {
    var climate = st.environment.climate;
    var active = st.environment.enabled && climate.enabled;
    st.bakedLighting = !(active && climate.bakedSuppressed);
    var takeover = !!(active && climate.relighting);
    if (takeover && !st.envNativeBefore) {
      st.envNativeBefore = [st.engineLights, st.bakedShadows, st.dynamicShadows];
    }
    if (takeover) st.engineLights = st.bakedShadows = st.dynamicShadows = false;
    else if (st.envNativeBefore) {
      st.engineLights = st.envNativeBefore[0]; st.bakedShadows = st.envNativeBefore[1];
      st.dynamicShadows = st.envNativeBefore[2]; st.envNativeBefore = null;
    }
    st.shadows = st.bakedShadows && st.dynamicShadows;
    var o = st.environment.objects.find(function (o) { return o.id === st.envSelected; });
    if (!o) { st.envGizmo = null; return; }
    // Representative world projection, not a mock renderer. Editing uses the same axis geometry
    // and cursor pump as Panorama; native projection/drag math is tested separately.
    var x = 0.49 + (o.position[0] - 512) * 0.00013 + (o.position[1] + 220.5) * 0.00008;
    var y = 0.52 + (o.position[0] - 512) * 0.00004 - (o.position[1] + 220.5) * 0.00004 - (o.position[2] - 64) * 0.0002;
    var p = function (x, y) { return [x, y, true]; };
    var bounds = [];
    for (var i = 0; i < 8; i++) {
      var a = (i & 1) ? 1 : -1, b = (i & 2) ? 1 : -1, c = (i & 4) ? 1 : -1;
      bounds.push(p(x + a * 0.025 + b * 0.014, y + a * 0.013 - b * 0.012 - c * 0.042));
    }
    st.envGizmo = { origin: p(x, y), axes: [p(x + 0.067, y + 0.037), p(x - 0.045, y + 0.05), p(x, y - 0.09)], bounds: bounds };
  }
  function environment(st, raw) {
    var t = (raw.match(/"[^"]*"|\S+/g) || []).map(function (s) { return s.replace(/^"|"$/g, ''); }).slice(2);
    var op = t[0], s = st.environment;
    var copy = function (s) { return JSON.parse(JSON.stringify(s)); };
    st.envUndo = st.envUndo || [];
    var selected = function (id) { return s.objects.find(function (o) { return o.id === +id; }); };
    var push = function (before) { st.envUndo.push(before); if (st.envUndo.length > 32) st.envUndo.shift(); st.envDirty = true; };
    var cancel = function () {
      if (st.envDragBefore) { st.environment = s = st.envDragBefore; st.envDragBefore = null; }
      st.envDragging = false; st.envEditRevision++;
    };
    if (op === 'edit') {
      var verb = t[1], o = selected(st.envSelected);
      if (verb === 'start' && selected(t[2])) { cancel(); st.envSelected = st.envEditing = +t[2]; }
      else if (verb === 'begin' && st.envEditing && o && !st.envDragging) {
        st.envDragBefore = copy(s); st.envDragging = true;
        st.envDragAxis = ['x', 'y', 'z'].indexOf(t[2]); st.envDragStart = [+t[3], +t[4]];
        st.envEditRevision++;
      } else if (verb === 'drag' && st.envDragging && o) {
        var a = st.envDragAxis, du = +t[2] - st.envDragStart[0], dv = +t[3] - st.envDragStart[1];
        var axis = [[0.067, 0.037], [-0.045, 0.05], [0, -0.09]][a];
        var units = (du * axis[0] + dv * axis[1]) / (axis[0] * axis[0] + axis[1] * axis[1]) * 180;
        o.position[a] = st.envDragBefore.objects.find(function (p) { return p.id === o.id; }).position[a] + units;
      } else if (verb === 'commit' && st.envDragging) {
        if (JSON.stringify(s) !== JSON.stringify(st.envDragBefore)) push(st.envDragBefore);
        st.envDragBefore = null; st.envDragging = false; st.envEditRevision++;
      } else if (verb === 'cancel' || verb === 'stop') {
        var dragging = st.envDragging; cancel();
        if (!dragging || verb === 'stop') st.envEditing = 0;
      }
      syncEnvironment(st); return true;
    }
    if (st.envDragging) cancel();
    var before = copy(s), o;
    if (op === 'select') { st.envSelected = selected(t[1]) ? +t[1] : 0; st.envEditing = 0; }
    else if (op === 'add' || op === 'duplicate') {
      o = op === 'duplicate' ? selected(t[1]) : s.objects[0];
      if (!o) return true;
      o = copy(o);
      st.envNextId = Math.max(st.envNextId || 0, ...s.objects.map(function (p) { return p.id; })) + 1;
      o.id = st.envNextId; o.enabled = true;
      if (op === 'add') {
        o.kind = t[1]; o.asset = t[1] === 'obj' ? t[2] : ''; o.position = [512, -220.5, 64];
        o.scale = ['point', 'spot', 'area'].indexOf(o.kind) >= 0 ? [1, 1, 1] : [48, 48, 48];
        o.intensity = 4; o.range = 1200;
      }
      // Environment.cpp: a light joins whichever lighting runs; only a mesh turns Environment on.
      s.objects.push(o); st.envSelected = o.id; st.envEditing = 0;
      if (['point', 'spot', 'area'].indexOf(o.kind) < 0) s.enabled = true;
      push(before);
    } else if (op === 'remove') {
      s.objects = s.objects.filter(function (o) { return o.id !== +t[1]; });
      if (st.envSelected === +t[1]) st.envSelected = 0;
      st.envEditing = 0; push(before);
    } else if (op === 'undo') {
      if (st.envUndo.length) st.environment = s = st.envUndo.pop();
      st.envSelected = 0; st.envEditing = 0;
    } else if (op === 'object') {
      o = selected(t[1]); if (!o) return true;
      var key = t[2], value = +t[3], i;
      if ((i = ['x','y','z'].indexOf(key)) >= 0) o.position[i] = value;
      else if ((i = ['pitch','yaw','roll'].indexOf(key)) >= 0) o.angles[i] = value;
      else if ((i = ['sx','sy','sz'].indexOf(key)) >= 0) o.scale[i] = value;
      else if ((i = ['r','g','b'].indexOf(key)) >= 0) o.color[i] = value;
      else if (key === 'group') o.group = t[3] === '-' ? '' : t[3];          // outliner
      else if (key === 'enabled' || key === 'render' || key === 'locked') o[key] = value !== 0;
      else o[key] = value;
      push(before);
    } else if (op === 'climate') {
      if (!(t[1] in s.climate)) { st.envMessage = 'Unknown climate setting: ' + t[1]; return true; }
      s.climate[t[1]] = +t[2]; s.enabled = true; push(before);
    } else if (op === 'set') {
      var key = t[1], index = ['R','G','B'].indexOf(key.slice(-1));
      if (key.indexOf('fog') === 0 && index >= 0) s.fogColor[index] = +t[2];
      else if (key.indexOf('tint') === 0 && index >= 0) s.tint[index] = +t[2];
      else if (key in s) s[key] = +t[2];
      s.enabled = true; push(before);
    } else if (op === 'weather') {
      if (t[1] === 'set') s.weather[t[2]] = +t[3];
      else if (t[1] === 'wind') { s.weather.windX = +t[2] * Math.cos(+t[3]*Math.PI/180); s.weather.windY = +t[2] * Math.sin(+t[3]*Math.PI/180); }
      else { s.weather.type = ['clear','rain','snow','ash','embers','motes','smoke'].indexOf(t[1]); }
      s.enabled = true; push(before);
    } else if (op === 'on' || op === 'off') { s.enabled = op === 'on'; push(before); }
    else if (op === 'save') {
      st.envStored = st.envStored || {}; st.envStored[t[1]] = copy(s);
      if (st.envPresets.indexOf(t[1]) < 0) st.envPresets.push(t[1]);
      st.envSaved = t[1]; st.envDirty = false;
    } else if (op === 'load') {
      if (st.envStored && st.envStored[t[1]]) { st.environment = s = copy(st.envStored[t[1]]); push(before); }
      st.envSelected = st.envEditing = 0; st.envSaved = t[1];
    } else if (op === 'reset') {
      if (t[1] === 'props' || t[1] === 'all') s.objects = [];
      if (t[1] === 'all') {
        s.enabled = false; s.weather.type = 0; s.exposure = 1; s.tint = [1,1,1];
        s.climate = Object.assign({}, climateDefaults);
        climateResetOff.forEach(function(k){s.climate[k]=0;});
      }
      st.envSelected = st.envEditing = 0; push(before);
    } else if (op === 'look') {
      s.enabled = true; s.climate.enabled = 1;
      var looks = {storm:[215,18,0.8,0.85,0.4],winter:[200,22,1.8,0.65,0.25],sunset:[260,8,3,0.3,0.15],night:[210,-12,0,0.25,0.2]};
      var look = looks[t[1]];
      if (look) { ['sunAzimuth','sunElevation','sunPower','coverage','fogDensity'].forEach(function(k,i){s.climate[k]=look[i];}); push(before); }
    } else if (op === 'texture') {
      o = selected(t[1]); if (o) { o[t[2]+'Texture'] = t[3] === '-' ? '' : t[3]; push(before); }
    } else if (['rescan','folder','reload-geometry','status'].indexOf(op) < 0) return false;
    syncEnvironment(st); return true;
  }

  function applySceneGraphMode(st, mode) {
    var scene = mode === 'scene';
    st.graphMode = scene ? 'scene' : 'camera';
    if (!st.cameraChannels) st.cameraChannels = st.channels;
    if (scene && st.sceneKeys) {
      if (global.MvmState && global.MvmState.rebuildSceneDerived) {
        global.MvmState.rebuildSceneDerived(st.sceneKeys);
      }
      st.channels = st.sceneKeys.sceneChannels || [];
    } else {
      st.channels = (global.MvmState && global.MvmState.makeCameraChannels)
        ? global.MvmState.makeCameraChannels(st.selected, st.markers)
        : (st.cameraChannels || st.channels);
      st.cameraChannels = st.channels;
    }
    st.selCh = 0;
  }

  function run(st, raw) {
    var cmd = String(raw).trim().replace(/\s+quiet$/, '');
    var t = cmd.split(/\s+/);

    // ---- playback ---------------------------------------------------------
    if (t[0] === 'demo_pause') { st.playing = false; st.paused = true; return true; }
    if (t[0] === 'demo_resume') { st.playing = true; st.paused = false; return true; }
    if (t[0] === 'demo_togglepause') { st.paused = !st.paused; st.playing = !st.paused; return true; }
    if (t[0] === 'demo_gototick') { st.tick = num(t[1], st.tick); st.scrubTick = st.tick; st.playhead = st.tick; return true; }
    if (t[0] === 'demo_timescale') { st.timescale = num(t[1], 1); return true; }
    if (t[0] === 'cl_demo_predict') { global.MvmFakeCvars = global.MvmFakeCvars || {}; global.MvmFakeCvars['cl_demo_predict'] = t[1]; return true; }
    if (t[0] === 'mirv_relight') return true;
    if (t[0] === 'mirv_viewmodel' && t[1] === 'reset') { st.viewmodelDefault = true; return true; }
    if (t[0] === 'mirv_fov') {
      if (!t[1] || t[1] === 'default') { st.camFovOn = false; return true; }
      if (t[1] === 'handleZoom') return true;
      var fov = Math.round(num(t[1], NaN));
      if (isFinite(fov)) { st.camFovOn = true; st.camFov = fov; }
      return true;
    }

    // Filmmaker/Props/Props.cpp RunCommand, over a small stand-in of the game's model catalog.
    function propsCommand(st, t) {
      var CATALOG = ['models/anubis/foliage/palm_tree_01.vmdl', 'models/cs_italy/foliage/fern/fern_plant_1.vmdl',
        'models/cs_italy/foliage/tree/generic_tree_large_1.vmdl', 'models/props/de_dust/hr_dust/dust_crates/dust_crate_style_01_32x32x32.vmdl',
        'models/props/de_dust/hr_dust/dust_cart/dust_cart.vmdl', 'models/props_vehicles/car001a.vmdl',
        'models/de_dust2/foliage/bush_01.vmdl', 'models/props/de_dust/palm_tree_short.vmdl'];
      var p = st.props, sub = t[2];
      var name = function (m) { return m.split('/').pop().replace('.vmdl', '').replace(/_/g, ' '); };
      var folder = function (m) { var s = m.split('/'); return s.slice(1, -1).join('/'); };
      if (sub === 'search') {
        p.query = (t[3] || '').replace(/^"|"$/g, ''); p.category = t[4] || 'all'; p.limit = +t[5] || 24;
        var hits = CATALOG.filter(function (m) {
          var fol = /foliage|tree|palm|bush|fern/.test(m);
          if (p.category === 'foliage' && !fol) return false;
          if (p.category === 'props' && fol) return false;
          if (p.category === 'map' && m.indexOf('dust') < 0) return false;
          return p.query.toLowerCase().split(' ').every(function (w) { return !w || m.indexOf(w) >= 0; });
        });
        p.total = hits.length;
        p.results = hits.slice(0, p.limit).map(function (m) { return { model: m, name: name(m), folder: folder(m) }; });
        return true;
      }
      // In-view placement: the copy follows the cursor until `drop` (Props.cpp place/hover/drop).
      if (sub === 'place') { p.placing = true; p.placingModel = t[3]; p.placingName = name(t[3]); p.selected = 0; return true; }
      if (sub === 'cancel') { p.placing = false; p.placingName = ''; p.grab = ''; return true; }
      if (sub === 'drop' && p.placing) { p.placing = false; t = ['', '', 'add', p.placingModel]; sub = 'add'; }
      if (sub === 'hover' || sub === 'drag' || sub === 'release' || sub === 'grab' || sub === 'pick') return true;
      if (sub === 'add') {
        var id = (p.placed.reduce(function (n, q) { return Math.max(n, q.id); }, 0)) + 1;
        p.placed.push({ id: id, model: t[3], name: name(t[3]), folder: folder(t[3]), pos: [0, 0, 0], ang: [0, 0, 0], scale: 1, visible: true, live: true });
        p.selected = id; return true;
      }
      var q = p.placed.filter(function (x) { return x.id === +t[3]; })[0];
      if (sub === 'select') { p.selected = +t[3]; return true; }
      if (sub === 'remove' && q) { p.placed.splice(p.placed.indexOf(q), 1); if (p.selected === q.id) p.selected = 0; return true; }
      if (sub === 'duplicate' && q) { var c = JSON.parse(JSON.stringify(q)); c.id = q.id + 100; p.placed.push(c); p.selected = c.id; return true; }
      if (sub === 'set' && q) {
        var v = +t[5];
        if (t[4] === 'yaw') q.ang[1] = v; else if (t[4] === 'scale') q.scale = v; else if (t[4] === 'visible') q.visible = v !== 0;
        else if (t[4] === 'render' || t[4] === 'locked') q[t[4]] = v !== 0;
        else if (t[4] === 'group') { var g = t.slice(5).join(' ').replace(/^"|"$/g, ''); q.group = g === '-' ? '' : g; }
        return true;
      }
      if (sub === 'floor' || sub === 'here') return true;
      return true;
    }

    if (t[0] !== 'mirv_filmmaker' && t[0] !== 'mirv_sky' && t[0] !== 'mirv_colors') {
      return false;
    }

    var a = t[1], b = t[2], c = t[3], d = t[4];
    if (a === 'props') return propsCommand(st, t);
    if (a === 'effects' && b === 'reset' && configResets[c]) {
      configResets[c].forEach(function(command){run(st, command);}); return true;
    }
    var resetKey = a === 'settings' && b === 'reset' ? c : b === 'reset' ? a : '';
    var resetPatches = {
      smoothView: {svOn:false,smoothViewDefault:true,svBreathX:1,svBreathY:1,svStandAmp:0.2,svWalkAmp:0.4,svRunAmp:0.8},
      smoothview: {svOn:false,smoothViewDefault:true},
      viewFx: {swayPct:0,floatAmt:0,deadzonePct:0,dzXhair:false},
      handheld: {handheldOn:false,handheldDefault:true,hhVmDecoupled:false,hhCenterLockOn:false},
      weaponbob: {wbOn:false,weaponBobDefault:true},
      weaponrecoil: {wrOn:false,weaponRecoilDefault:true,wrOverrides:[],wrScope:'all',wrScopeKey:''},
      deathanim: {daOn:false}, nervous: {nsOn:false},
      ragdollweapon: {ragdollKeepWeapon:false,ragdollDeathFire:false}, methodacting: {actingOn:false}
    };
    if (resetPatches[resetKey]) {Object.assign(st,resetPatches[resetKey]);return true;}
    if (a === 'pivotorb') {st[b === 'bob' ? 'wbPivotOrb' : 'wrPivotOrb']=boolWord(c, false);return true;}
    if (a === 'pivot' && c === 'reset') return true;
    if (a === 'lens' && b === 'reset') {
      st.lens = st.lens || {};
      if (c !== 'flare') {st.lens['dof.on']=0;st.lens.dofDefault=true;}
      if (c !== 'dof') {st.lens['flare.on']=0;st.lens.flareDefault=true;}
      return true;
    }
    if (a === 'world' && c === 'reset') {
      if (b === 'modulate') {Object.keys(st).forEach(function(k){if(/^mod_.*_on$/.test(k))st[k]=false;});return true;}
      if (b === 'customshadow') {st.shaderShadows=false;return true;}
      if (b === 'skyorient') {st.skyYaw=st.skyPitch=0;st.skyMirror=false;return true;}
      if (b === 'clouds') {st.cloudDensity=st.cloudBright=st.cloudDrift=st.cloudSize=1;return true;}
      if (b === 'sun') {st.sunVisible=true;st.sunYaw=st.sunPitch=0;st.sunGlowSize=1;return true;}
    }
    if (a === 'world' && b === 'sky' && c === 'default') {st.skyCurrent='';return true;}
    if (a === 'world' && b === 'exposure' && c === 'auto') {st.exposure=-1;return true;}
    if (t[0] === 'mirv_colors' && a === 'smokes' && c === 'default') {st[b === 't' ? 'smokeT' : 'smokeCT']='default';return true;}
    if (a === 'world' && b === 'light' && c === 'enabled') {
      st.engineLights = boolWord(d, st.engineLights); return true;
    }
    if (a === 'world' && ['shadows','bakedshadows','dynamicshadows'].indexOf(b) >= 0) {
      if (b === 'shadows') st.bakedShadows = st.dynamicShadows = c === 'reset' ? true : boolWord(c, st.shadows);
      else {
        var layer = b === 'bakedshadows' ? 'bakedShadows' : 'dynamicShadows';
        st[layer] = boolWord(c, st[layer]);
      }
      st.shadows = st.bakedShadows && st.dynamicShadows; return true;
    }
    if (a === 'world' && b === 'light' && c === 'reset') {
      st.engineLights = true; st.lightR = st.lightG = st.lightB = 255;
      st.lightAmount = 1; st.lightUniform = true; st.sunYaw = st.sunPitch = 0; return true;
    }
    // Narrow mirror of the registered Config automation helpers, never arbitrary evaluation.
    if (a === 'config' && b === 'eval') {
      var tab = /ConfigHud\.gotoTab\('([^']+)'\)/.exec(cmd);
      if (tab && global.$ && global.$.ConfigHud) { global.$.ConfigHud.gotoTab(tab[1]); return true; }
      var call = /ConfigHud\.(cardPage|card)\('([^']+)',([0-9]+)\)/.exec(cmd);
      if (call && global.$ && global.$.ConfigHud) {
        global.$.ConfigHud[call[1]](call[2], +call[3]); return true;
      }
      var scroll = /ConfigHud\.scrollTo\(([0-9]+)\)/.exec(cmd);
      if (scroll && global.$ && global.$.ConfigHud) { global.$.ConfigHud.scrollTo(+scroll[1]); return true; }
      var search = /ConfigHud\.search\('([^']*)'\)/.exec(cmd);
      if (search && global.$ && global.$.ConfigHud) { global.$.ConfigHud.search(search[1]); return true; }
      // The letterbox wordmark (WorkspaceShellJs.h): replay, or freeze at a loop time.
      var wm = /ConfigHud\.wordmark\(([0-9.]*)\)/.exec(cmd);
      if (wm && global.$ && global.$.ConfigHud && global.$.ConfigHud.wordmark) {
        global.$.ConfigHud.wordmark(wm[1] === '' ? undefined : +wm[1]); return true;
      }
      // The per-player death animation picker's automation helpers (ConfigHudDeathAnimPickerJs.h).
      var dap = /ConfigHudDap\.(open|close|area|select|player)\(\s*'?([^')]*)'?\s*\)/.exec(cmd);
      if (dap && global.$ && global.$.ConfigHudDap) { global.$.ConfigHudDap[dap[1]](dap[2]); return true; }
      var drop = /ConfigHud\.dropdown\('([^']*)'\)/.exec(cmd);
      if (drop && global.$ && global.$.ConfigHud) { global.$.ConfigHud.dropdown(drop[1]); return true; }
      var color = /ConfigHud\.color\('([^']*)'\)/.exec(cmd);
      if (color && global.$ && global.$.ConfigHud) { global.$.ConfigHud.color(color[1]); return true; }
      var chars = /ConfigHud\.chars\('([0-9,]*)'\)/.exec(cmd);
      if (chars && global.$ && global.$.ConfigHud) { global.$.ConfigHud.chars(chars[1]); return true; }
      return false;
    }
    if (t[0] === "mirv_filmmaker" && a === "environment") return environment(st, cmd);
    if (t[0] === 'mirv_filmmaker' && (a === 'scenekeys' || (a === 'grapheditor' && st.uiEditorInspector === 'scene' && ['on','off','toggle'].indexOf(b)<0))) {
      return global.MvmSceneKeys.run(st, a === 'scenekeys' ? t.slice(2) : ['graph'].concat(t.slice(2)));
    }

    // ---- persisted UI keys (tabs, collapsible cards) ----------------------
    if (a === 'settings' && b === 'ui') {
      var key = c, val = t.slice(4).join(' ');
      st.ui = st.ui || {};
      st.ui[key] = val;
      if (UI_MIRROR[key]) st[UI_MIRROR[key]] = val;
      if (UI_MIRROR_BOOL[key]) st[UI_MIRROR_BOOL[key]] = (val === '1' || val === 'true');
      if (UI_MIRROR_NUM[key]) {
        var n = parseFloat(val);
        if (isFinite(n) && n > 0) st[UI_MIRROR_NUM[key]] = Math.round(n);
      }
      if (key === 'editor.graph') applySceneGraphMode(st, val);
      // Time snap is one shared value for both graph modes; the graph feed reads st.snap.
      if (key === 'editor.graph.snap') st.snap = val;
      return true;
    }
    if (a === 'settings' && b === 'reset') {
      if (c === 'camFov') { st.camFovOn = false; st.camFov = 90; return true; }
      if (c === 'viewmodel') {
        st.vmPerPlayer = true; st.viewmodelDefault = true;
        st.vmFov = 68; st.vmX = 0; st.vmY = 0; st.vmZ = 0; st.vmLeftHanded = false;
        return true;
      }
    }

    // ---- particle FX ------------------------------------------------------
    if (a === 'fx') {
      if (b === 'set') { st['fx_' + c] = d; return true; }
      if (b === 'helights' || b === 'flashlights') { var lightKey = b === 'helights' ? 'fxHeLights' : 'fxFlashLights'; st[lightKey] = boolWord(c, st[lightKey]); if (st[lightKey]) st.fxOn = true; return true; }
      if (b === 'mollylights') { st.fxMollyLights = boolWord(c, st.fxMollyLights); if (st.fxMollyLights) st.fxOn = true; return true; }
      if (b === 'moneyshot') { st.fxMoneyshot = boolWord(c, st.fxMoneyshot); return true; }
      // ParticleFx.cpp: the per-shot debug squares (FxDebugHud), a diagnostic that is off until
      // asked for.
      if (b === 'debughud') { st.fxdbg.enabled = boolWord(c, st.fxdbg.enabled); return true; }
      if (b === 'on' || b === 'off') { st.fxOn = (b === 'on'); return true; }
    }

    // ---- playback loop ----------------------------------------------------
    if (a === 'loop') {
      if (b === 'toggle') { st.loopOn = !st.loopOn; return true; }
      if (b === 'on' || b === 'off') { st.loopOn = (b === 'on'); return true; }
      if (b === 'in') { st.loopIn = Math.round(num(c, st.loopIn)); st.loopReady = true; return true; }
      if (b === 'out') { st.loopOut = Math.round(num(c, st.loopOut)); st.loopReady = true; return true; }
      if (b === 'range') {
        var lo = Math.round(num(c, st.loopIn)), hi = Math.round(num(d, st.loopOut));
        st.loopIn = Math.min(lo, hi); st.loopOut = Math.max(lo, hi); st.loopReady = true; return true;
      }
      if (b === 'clear') { st.loopReady = false; st.loopOn = false; return true; }
    }

    // ---- the workspaces ---------------------------------------------------
    // ConfigHud_RunCommand: on|open|1 / off|close|0 / anything else toggles. Opening Config closes
    // the Camera Editor (they draw the same right-side column).
    if (a === 'config' && (b === undefined || /^(on|open|1|off|close|0|toggle)$/.test(b))) {
      setConfig(st, boolWord(b, st.nav.configOpen));
      return true;
    }
    if (a === 'editor') {
      // CameraEditorCommand.cpp: on|open|1 / off|close|0 / toggle.
      if (b === undefined || /^(on|open|1|off|close|0|toggle)$/.test(b)) {
        setEditor(st, boolWord(b, st.nav.editorOpen));
        return true;
      }
      // 'editor render' calls Render_SetModalOpen(TRUE) and CameraEditor_Set(true) -- it opens,
      // it does not toggle. Toggling meant pressing RENDER twice from inside the workspace closed
      // it, which the real command never does.
      if (b === 'render') { setEditor(st, true); st.nav.renderOpen = true; st.render.open = true; return true; }
      // 'editor customize' opens the editor (CameraEditor_Set) and asks for the Customize modal;
      // app.js calls $.CamEditor.openCustomize() once the editor has built, the way
      // CameraEditorHud::RunFrame honours m_openCustomizeRequested.
      if (b === 'customize') { setEditor(st, true); st.nav.customizeOpen = true; return true; }
      if (b === 'hud') { st.hudView = c || 'full'; return true; }
      // The older spelling of the dock tabs ('editor curveeditor <mode>'); the ribbon sends
      // 'dock <tab>' now, and both go through TimelineDock_Pick.
      if (b === 'curveeditor') return pickTab(st, c);
    }
    // EasterEggs_RunCommand: `mirv_filmmaker egg ...` -- the overlay's automation surface. Hints
    // come from a C++ table; the mockup carries a few samples of the same shape.
    if (a === 'egg') {
      var eg = st.eggs;
      if (b === 'cancel') {
        eg.vegasCancelledThrough = eg.vegas;
        eg.egirl = false; eg.disco = false; eg.wes.on = false;
        eg.clippy.show = false;
        return true;
      }
      var samples = [
        { title: 'UWU?', code: '// 01 . whisper it', sub: 'the console gets lonely. be nice to it.' },
        { title: 'IT LOOKS LIKE YOU' + String.fromCharCode(39) + 'RE...', code: '// 03 . 1997 called', sub: 'someone' + String.fromCharCode(39) + 's waiting for you to stop clicking.' },
        { title: 'WOULD YOU KINDLY', code: '// 05 . render responsibly', sub: 'we can see your taskbar.' }
      ];
      if (b === 'egirl') { eg.egirl = boolWord(c, eg.egirl); return true; }
      if (b === 'disco') { eg.disco = boolWord(c, eg.disco); if (eg.disco) eg.wes.on = false; return true; }
      if (b === 'michaelbay') { eg.bay++; return true; }   // the mockup has no deaths: one blast per call
      if (b === 'wes' || b === 'chapter') {
        if (b === 'wes') eg.wes.on = boolWord(c, eg.wes.on);
        if (eg.wes.on) {
          eg.disco = false;
          eg.wes.chapter++;
          eg.wes.title = ['The Pistol Round', 'In Which Nobody Buys', 'A Rather Unfortunate Rush'][(eg.wes.chapter - 1) % 3];
        }
        return true;
      }
      if (b === 'hint') {
        var h = samples[(+c || 0) % samples.length];
        eg.hint = { id: eg.hint.id + 1, loud: d === 'loud', title: h.title, code: h.code, sub: h.sub };
        return true;
      }
      if (b === 'vegas') { eg.vegas++; return true; }
      if (b === 'clippy') {
        if (c === 'show' || c === undefined) { eg.clippy = { id: eg.clippy.id + 1, show: true }; return true; }
        eg.clippy = { id: eg.clippy.id, show: false };
        if (c === 'yes' || c === 'absolutely' || c === 'bass') eg.egirl = true;
        return true;
      }
      return true;
    }
    // TimelineDock_RunCommand: `mirv_filmmaker dock [native|camera|graph|toggle]`. No argument
    // reports; an unknown tab warns and changes nothing.
    if (a === 'dock') {
      if (b === undefined) return true;
      return pickTab(st, b);
    }
    // The director card (F8's old job): `mirv_filmmaker hud on|off|toggle`.
    if (a === 'hud' && (b === 'on' || b === 'off' || b === 'toggle' || b === undefined || b === '1' || b === '0')) {
      st.movie.visible = boolWord(b, st.movie.visible);
      return true;
    }
    // F8's console twin: clear every overlay (ours, the gameplay HUD and the demo bar). Nothing is
    // closed -- a second press puts the screen back exactly as it was.
    if (a === 'blackout') {
      if (b === 'status') return true;
      st.nav.blackout = boolWord(b, st.nav.blackout);
      return true;
    }
    // The speed dial: every mounted copy reads the engine's timescale back each frame, so the
    // value it just sent has to land in the feed or the handle snaps back to 1x.
    if (a === 'timescale' && b !== undefined) {
      var ts = parseFloat(b);
      if (isFinite(ts) && ts > 0) { st.speed = ts; st.timescale = ts; st.speedMul = ts; }
      return true;
    }

    // ---- the marker HUD ------------------------------------------------------
    // The subset of Marker_RunCommand that changes what is on screen: the card, the mode, and the
    // hotkey paths (K places, J arms/plays, Esc stops).
    if (a === 'marker') {
      if (b === 'close') { st.marker.menuOpen = false; return true; }
      if (b === 'open' || b === 'menu') { st.marker.menuOpen = true; return true; }
      if (b === 'preview') { st.marker.mode = 'previewArmed'; st.marker.notice = ''; return true; }
      if (b === 'play') {
        if (st.markers.length < 2) { st.marker.notice = 'Need at least 2 camera markers to play path.'; return true; }
        st.marker.mode = 'previewPlaying'; st.playing = true; st.paused = false; st.marker.notice = ''; return true;
      }
      if (b === 'previewstop') { st.marker.mode = 'editing'; return true; }
      if (b === 'reposition') { st.marker.mode = 'reposition'; return true; }
      if (b === 'repositioncancel' || b === 'repositionplace') { st.marker.mode = 'editing'; return true; }
      if (b === 'place') { return run(st, 'mirv_filmmaker camtl addkey'); }
      if (b === 'delete') {
        var di = c === undefined ? st.selected : Math.round(num(c, st.selected));
        if (st.markers[di]) { st.markers.splice(di, 1); st.selected = Math.max(0, Math.min(st.selected, st.markers.length - 1)); syncSel(st); }
        return true;
      }
      if (b === 'clear') { run(st, 'mirv_filmmaker camtl clear'); st.marker.notice = 'Camera path cleared.'; return true; }
      // The path-wide settings the docked timeline's option rows write (MarkerCommand.cpp). The
      // feed carries the NAMES CameraPath publishes -- SpeedModeName() 'Manual' | 'Constant' |
      // 'Per-Segment', TimingName() 'Live' | 'Freeze' -- and the timeline lights the segment whose
      // name matches, so the router has to answer in the same spelling.
      if (b === 'speedmode') {
        var modes = ['Manual', 'Constant', 'Per-Segment'];
        var want = { manual: 'Manual', constant: 'Constant', persegment: 'Per-Segment' }[String(c || '').toLowerCase()];
        st.speedMode = want || modes[(modes.indexOf(st.speedMode) + 1) % modes.length];
        return true;
      }
      if (b === 'timing') {
        var tw = String(c || '').toLowerCase();
        st.timing = tw === 'live' ? 'Live' : (tw === 'freeze' ? 'Freeze' : (st.timing === 'Freeze' ? 'Live' : 'Freeze'));
        return true;
      }
      if (b === 'interp') {
        var iw = String(c || '').toLowerCase();
        st.interp = iw === 'bezier' ? true : (iw === 'linear' ? false : !st.interp);
        return true;
      }
      if (b === 'speedmul') {
        var mul = Math.max(0.2, Math.min(1, num(c, st.selSpeedMul)));
        if (st.markers[st.selected]) st.markers[st.selected].speedMul = mul;
        syncSel(st);
        return true;
      }
      if (b === 'constspeed') {
        var steps = [0.5, 0.75, 1, 1.5, 2];
        if (c === undefined || c === 'cycle') st.constSpeed = steps[(steps.indexOf(st.constSpeed) + 1) % steps.length];
        else st.constSpeed = num(c, st.constSpeed);
        return true;
      }
      if (b === 'autosnap') { st.autoSnap = boolWord(c, st.autoSnap); return true; }
    }

    // ---- camera timeline / path ------------------------------------------
    if (a === 'camtl') {
      if (b === 'play' || b === 'playtest') { st.playing = true; st.paused = false; return true; }
      if (b === 'pause') { st.playing = false; st.paused = true; return true; }
      if (b === 'cursor') { st.cursor = boolWord(c, st.cursor); return true; }
      // `camtl view` only resets the timeline's own view index in game (CameraTimelineCommand.cpp);
      // it never moves the editor's bottom band. Routing it onto bottomMode left every hosted
      // timeline reading 'timeline' where the engine says 'camera'.
      if (b === 'view') return true;
      // CameraTimelineCommand.cpp: the standalone (floating) camera timeline. The docked one is
      // the editor's business (TimelineDock_Apply), so these only matter while nothing hosts it.
      if (b === 'open') { st.dock.tlVisible = true; return true; }
      if (b === 'close') { st.dock.tlVisible = false; return true; }
      if (b === 'toggle') { st.dock.tlVisible = !st.dock.tlVisible; return true; }
      if (b === 'scrub') { st.tick = Math.round(num(c, st.tick)); st.scrubTick = st.tick; st.playhead = st.tick; return true; }
      if (b === 'speed') { st.speedMul = num(c, st.speedMul); return true; }
      if (b === 'select') {
        var i = Math.round(num(c, st.selected));
        if (i >= 0 && i < st.markers.length) { st.selected = i; syncSel(st); }
        return true;
      }
      if (b === 'selectdelta') {
        var j = st.selected + Math.round(num(c, 0));
        if (j >= 0 && j < st.markers.length) { st.selected = j; syncSel(st); }
        return true;
      }
      if (b === 'addkey') {
        st.markers.push({
          tick: Math.round(st.tick), x: 0, y: 0, z: 96, pitch: 0, yaw: 0, roll: 0,
          fov: 90, ease: 2, speedMul: 1.0
        });
        st.markers.sort(function (m, n) { return m.tick - n.tick; });
        st.selCount = st.markers.length; st.count = st.markers.length;
        st.selected = st.markers.findIndex(function (m) { return m.tick === Math.round(st.tick); });
        syncSel(st);
        return true;
      }
      if (b === 'delkey') {
        if (st.markers.length) {
          st.markers.splice(st.selected, 1);
          st.selCount = st.markers.length;
          st.selected = Math.max(0, Math.min(st.selected, st.markers.length - 1));
          syncSel(st);
        }
        return true;
      }
      if (b === 'clear') { st.markers = []; st.selCount = 0; st.selected = -1; syncSel(st); return true; }
      if (b === 'ease') { if (st.markers[st.selected]) st.markers[st.selected].ease = Math.round(num(c, 2)); syncSel(st); return true; }
      if (b === 'interp') { st.interp = boolWord(c, st.interp); return true; }
      if (b === 'movekey') {
        if (st.markers[st.selected]) st.markers[st.selected].tick = Math.round(num(c, st.markers[st.selected].tick));
        syncSel(st); return true;
      }
      if (b === 'undo' || b === 'redo') return true;
    }

    // ---- render workspace modal -------------------------------------------
    if (a === 'render') {
      // RenderCommand.cpp: 'open' is refused outside the Camera Editor ("the workspace lives
      // inside the Camera Editor"); 'close' is what the workspace's own ✕ Back sends.
      if (b === 'open') {
        if (!st.nav.editorOpen) { warn('render open: the Render workspace lives inside the Camera Editor -- open it first (mirv_filmmaker editor on).'); return true; }
        st.nav.renderOpen = true; st.render.open = true; return true;
      }
      if (b === 'close') { st.nav.renderOpen = false; st.render.open = false; return true; }
      // The RANGE / CAMERA / HUD / OUTPUT tabs (CameraEditorRenderJs.h), read back as
      // state.render.tab.
      if (b === 'tab') { st.render.tab = Math.max(0, Math.min(3, Math.round(num(c, 0)))); return true; }
      if (b === 'cancel' || b === 'stop') { st.render.jobActive = false; st.renderJobActive = false; return true; }
      // RenderCommand.cpp's range / camera / extension / In-Out / output verbs, written back as the
      // indices Render_BuildStateJson reports. The resolved span follows the stored In/Out for
      // Custom and keeps the seeded span for the other modes.
      var rr = st.render, ro = rr.out;
      if (b === 'range') {
        var rmi = ['full', 'round', 'path', 'segment', 'custom'].indexOf(String(c));
        if (rmi >= 0) rr.rangeMode = rmi;
        if (rmi === 0) { rr.resStart = rr.demoStart; rr.resEnd = rr.demoEnd; }
        else if (rmi === 4) { rr.resStart = rr.startTick; rr.resEnd = rr.endTickExclusive; }
        rr.resOk = rr.resEnd > rr.resStart;
        return true;
      }
      if (b === 'in' || b === 'out') {
        if (b === 'in') rr.startTick = Math.round(num(c, rr.startTick)); else rr.endTickExclusive = Math.round(num(c, rr.endTickExclusive));
        rr.rangeMode = 4; rr.resStart = rr.startTick; rr.resEnd = rr.endTickExclusive; rr.resOk = rr.resEnd > rr.resStart;
        return true;
      }
      if (b === 'clearrange') { rr.startTick = 0; rr.endTickExclusive = 0; if (rr.rangeMode === 4) { rr.resStart = 0; rr.resEnd = 0; rr.resOk = false; } return true; }
      if (b === 'camera') {
        var csi = ['current', 'pov', 'thirdperson', 'free', 'path', 'follow'].indexOf(String(c));
        if (csi >= 0) rr.cameraSource = csi;
        return true;
      }
      if (b === 'extension') {
        var exi = ['fail', 'holdfirst', 'holdlast', 'clamp'].indexOf(String(c));
        if (exi >= 0) rr.extension = exi;
        return true;
      }
      if (b === 'output' && ro) {
        var ov = t[4];
        if (c === 'folder') { ro.folder = ro.folder || 'C:/Users/ExampleUser/Videos/SourceMVM'; return true; }
        if (c === 'format') {
          ro.format = String(ov);
          ro.formatName = { mp4: 'MP4 (H.264)', avi: 'AVI', gif: 'GIF', exr: 'EXR sequence', tga: 'TGA sequence',
            bmp: 'BMP sequence', png: 'PNG sequence', tiff: 'TIFF sequence' }[ro.format] || ro.format;
          ro.formatNote = { mp4: 'H.264 video, encoded while it renders.', avi: 'Uncompressed AVI. Large files.',
            gif: 'Animated GIF. 256 colours, best for short clips.' }[ro.format] || 'One image file per frame, written into the take folder.';
          return true;
        }
        if (c === 'fps') { ro.fps = Math.max(1, Math.round(num(ov, ro.fps))); ro.captureFps = ro.fps; return true; }
        if (c === 'size') { ro.width = Math.round(num(ov, 0)); ro.height = Math.round(num(t[5], 0)); return true; }
        if (c === 'quality') { ro.crf = Math.round(num(ov, ro.crf)); return true; }
        if (c === 'bitrate') { ro.bitrateKbps = Math.max(0, Math.round(num(ov, 0))); return true; }
        if (c === 'encoder') { var ei = { cpu: 0, gpu: 1, auto: 2 }[String(ov)]; if (ei != null) ro.encoder = ei; return true; }
        if (c === 'speed') { var si = { quality: 0, balanced: 1, fast: 2 }[String(ov)]; if (si != null) ro.speed = si; return true; }
        return true;
      }
      // RenderCommand.cpp RunPassCommand: the Passes tab's grid, key colour, depth range, preview.
      if (b === 'pass') {
        if (!st.render.pass || typeof st.render.pass !== 'object' || !st.render.pass.list)
          st.render.pass = { hud: 0, cameraData: true, blood: true, key: [0, 255, 0], depthNear: 8, depthFar: 4096, preview: { type: 'beauty', elements: 'all' },
            list: [{ type: 'beauty', elements: 'all', stream: 'beauty' }], rendersPerFrame: 1 };
        var ps = st.render.pass;
        var parts = cmd.split(/\s+/);
        var op = parts[3];
        if (op === 'toggle' || op === 'add' || op === 'remove') {
          var t = parts[4], e = parts[5], at = -1;
          for (var pi = 0; pi < ps.list.length; pi++) if (ps.list[pi].type === t && ps.list[pi].elements === e) at = pi;
          var want = op === 'add' || (op === 'toggle' && at < 0);
          if (want && at < 0) ps.list.push({ type: t, elements: e, stream: t + (e === 'all' ? '' : '_' + e) });
          if (!want && at >= 0) {
            ps.list.splice(at, 1);
            if (ps.preview && ps.preview.type === t && ps.preview.elements === e) ps.preview = { type: 'beauty', elements: 'all' };
          }
          var keys = {};
          for (var pk = 0; pk < ps.list.length; pk++) {
            var el = ps.list[pk].elements;
            keys[el === 'gun' || el === 'worldplayers' ? 'nogun' : el] = true;
          }
          keys.all = true;
          ps.rendersPerFrame = Object.keys(keys).length;
          return true;
        }
        if (op === 'key') { ps.key = [num(parts[4], 0), num(parts[5], 0), num(parts[6], 0)]; return true; }
        if (op === 'depth') { ps.depthNear = num(parts[4], ps.depthNear); ps.depthFar = num(parts[5], ps.depthFar); return true; }
        if (op === 'preview') {
          if (parts[4] === 'off') ps.preview = { type: 'beauty', elements: 'all' };
          else if (parts[5] && parts[5] !== 'gun') ps.preview = { type: parts[4], elements: parts[5] };
          return true;
        }
        if (op === 'hud') { ps.hud = { none: 0, game: 1, full: 2 }[parts[4]] || 0; return true; }
        if (op === 'cameradata') { ps.cameraData = num(parts[4], 1) !== 0; return true; }
        if (op === 'blood') { ps.blood = num(parts[4], 1) !== 0; return true; }
        return true;
      }
    }

    if (a === 'follow' && b === 'select') { st.follow.enabled = true; return true; }
    if (a === 'grapheditor') {
      if (b === 'playhead') { st.playhead = Math.round(num(c, st.playhead)); return true; }
      // GraphEditorCommand.cpp: the channel list's name (solo) and eye (hide) buttons, and the
      // Smooth / Linear pair. The graph reads `vis` and `solo` off each entry of st.channels.
      if (b === 'chan') {
        var ch = st.channels && st.channels[Math.round(num(c, -1))];
        var op = String(d || '').toLowerCase();
        if (!ch) return true;
        if (op === 'hide') ch.vis = false;
        else if (op === 'show') ch.vis = true;
        else if (op === 'solo') { st.channels.forEach(function (x) { x.solo = false; }); ch.solo = true; st.selCh = st.channels.indexOf(ch); }
        else if (op === 'unsolo') ch.solo = false;
        return true;
      }
      if (b === 'smooth' || b === 'linear') { st.interp = (b === 'smooth'); return true; }
      // GraphEditorExperiment_Set: the standalone toggle lands on the graph tab or leaves it.
      if (b === 'on') return pickTab(st, 'graph');
      if (b === 'off') return pickTab(st, st.dock.tab === 'graph' ? 'camera' : st.dock.tab);
      if (b === 'undo' || b === 'redo' || b === 'selall' || b === 'delsel') return true;
    }

    if (t.slice(0,4).join(' ') === 'mirv_filmmaker world smoke appearance') {
      var shapeKey = t[4];
      if (shapeKey === 'reset') {
        st.smokeAppearance = false;
        st.smoke_density=0.85; st.smoke_wispy=0; st.smoke_noise=0.8; st.smoke_speed=1.5;
        st.smoke_scale_low=3; st.smoke_scale_high=7; st.smoke_strength_low=0.5; st.smoke_strength_high=0.35;
        st.smoke_boil=-0.3; st.smoke_ray_step=6; st.smoke_spread=1; st.smoke_resolution=32; st.smoke_voxel_size=20; st.smoke_voxel_count=32768; return true;
      }
      if (shapeKey === 'toggle' || shapeKey === 'on' || shapeKey === 'off') {
        st.smokeAppearance = shapeKey === 'toggle' ? !st.smokeAppearance : shapeKey === 'on'; return true;
      }
      if (shapeKey === 'spread' || shapeKey === 'resolution' || shapeKey === 'voxel_size') {
        var value = num(t[5], st['smoke_' + shapeKey]);
        if (shapeKey === 'spread') st.smoke_spread = Math.max(0.25, Math.min(2, value));
        else if (shapeKey === 'resolution') st.smoke_resolution = Math.round(Math.max(4, Math.min(64, value)));
        else if (value > 0) st.smoke_resolution = Math.round(Math.max(4, Math.min(64, 640 * st.smoke_spread / value)));
        else return true;
        st.smoke_voxel_size = 640 * st.smoke_spread / st.smoke_resolution;
        st.smoke_voxel_count = st.smoke_resolution * st.smoke_resolution * st.smoke_resolution;
        st.smokeAppearance = true; return true;
      }
      if (Object.prototype.hasOwnProperty.call(st, 'smoke_' + shapeKey)) {
        st['smoke_' + shapeKey] = num(t[5], st['smoke_' + shapeKey]); st.smokeAppearance = true; return true;
      }
    }
    // ---- per-player death animations (Movie/DeathAnimations.cpp) ----------
    // `catalog` answers through ConfigHud's script queue in game (CatalogScript ->
    // $.ConfigHudDap.setCatalog); here the packs are parsed in the browser (shim/preview3d.js) and the
    // same call is made once they load. `view` poses every 3D preview (DeathAnimPreview_Set).
    // `player <key> <clip|auto>` is DeathAnimations_SetPlayerClip, read back through st.daPlayers.
    if (a === 'deathanim' && (b === 'catalog' || b === 'view' || b === 'player')) {
      var P3 = global.MvmPreview3D;
      if (b === 'view') { if (P3 && c && c !== 'status') P3.setClip(c); return true; }
      if (b === 'catalog') {
        if (P3) P3.loadPacks().then(function (lib) {
          if (global.$ && global.$.ConfigHudDap) global.$.ConfigHudDap.setCatalog(P3.catalogRows(lib));
        }, function (e) { warn('deathanim catalog: ' + e.message); });
        return true;
      }
      if (c && d) {
        st.daPlayerClips = st.daPlayerClips || {};
        if (d === 'auto') delete st.daPlayerClips[c]; else st.daPlayerClips[c] = d;
        st.daAssigned = Object.keys(st.daPlayerClips).length;
      }
      return true;
    }
    // ---- Customize Player commits (Cosmetics/CosmeticCommands) -------------
    // The game applies the override to the player's entities, and the next state frame reports the
    // new loadout through customizeTarget / customizePlayers (CosmeticUiQueries.cpp). Reopening the
    // modal therefore shows what Apply wrote, and Reset to default brings back the recorded loadout.
    if (a === 'cosmetics' && (b === 'player' || b === 'clearPlayer')) {
      applyCosmetics(st, b, t.slice(3));
      return true;
    }
    if (a === 'cosmetics') return true;   // enabled / uilog: nothing to show

    // ---- derived bindings (sliders / toggles / segmented) -----------------
    for (var k = 0; k < byPrefix.length; k++) {
      var bind = byPrefix[k];
      if (cmd.indexOf(bind.prefix) !== 0) continue;
      var tail = cmd.slice(bind.prefix.length).trim().split(/\s+/).filter(Boolean);
      var last = tail[tail.length - 1];

      if (bind.kind === 'slider') { st[bind.field] = num(last, st[bind.field]); return true; }
      if (bind.kind === 'toggle') { st[bind.field] = boolWord(last, st[bind.field]); return true; }
      if (bind.kind === 'segmented' || bind.kind === 'cycle') { st[bind.field] = last; return true; }
    }

    unhandled[cmd.split(/\s+/).slice(0, 3).join(' ')] = (unhandled[cmd.split(/\s+/).slice(0, 3).join(' ')] || 0) + 1;
    return false;
  }

  // Every reported copy of one player: the roster entry and, when it is the spectated player, the
  // target. `who` is a SteamID or `current` (the spectated player).
  function cosmeticCopies(st, who) {
    var out = [], target = st.customizeTarget, players = st.customizePlayers || {};
    var steam = who === 'current' ? (target && target.steamId) : who;
    for (var k in players) if (players[k] && players[k].steamId && players[k].steamId === steam) out.push(players[k]);
    if (target && (who === 'current' || target.steamId === steam)) out.push(target);
    return out;
  }
  var PISTOL_DEFS = [1, 2, 3, 4, 30, 32, 36, 61, 63, 64];
  function applyCosmetics(st, verb, args) {
    var who = args[0];
    if (!who) return;
    var copies = cosmeticCopies(st, who);
    if (!copies.length) { warn('cosmetics: no player ' + who); return; }
    var key = copies[0].steamId || who;
    st.__cosmeticOriginal = st.__cosmeticOriginal || {};
    if (verb === 'clearPlayer') {
      var orig = st.__cosmeticOriginal[key];
      if (orig) copies.forEach(function (p) { p.weapons = JSON.parse(JSON.stringify(orig.weapons)); p.agentModel = orig.agentModel; });
      delete st.__cosmeticOriginal[key];
      return;
    }
    if (!st.__cosmeticOriginal[key]) {
      st.__cosmeticOriginal[key] = { weapons: JSON.parse(JSON.stringify(copies[0].weapons || {})), agentModel: copies[0].agentModel };
    }
    // player <who> weapon|knife|gloves <def> paint <pk> wear <w> seed <s> | player <who> agent <model|default>
    var kind = args[1], def = parseInt(args[2], 10) || 0;
    var opt = {};
    for (var i = 3; i + 1 < args.length; i += 2) opt[args[i]] = args[i + 1];
    copies.forEach(function (p) {
      if (kind === 'agent') {
        p.agentModel = args[2] === 'default'
          ? (p.team === 3 ? 'agents/models/ctm_sas/ctm_sas.vmdl' : 'agents/models/tm_phoenix/tm_phoenix.vmdl')
          : args[2];
        return;
      }
      p.weapons = p.weapons || {};
      var slot = kind === 'knife' ? 'knife' : kind === 'gloves' ? 'gloves' : null;
      if (!slot) {
        if (p.weapons.primary && p.weapons.primary.defIndex === def) slot = 'primary';
        else if (p.weapons.secondary && p.weapons.secondary.defIndex === def) slot = 'secondary';
        else slot = PISTOL_DEFS.indexOf(def) >= 0 ? 'secondary' : 'primary';
      }
      var cur = p.weapons[slot] || {};
      p.weapons[slot] = Object.assign({}, cur, { defIndex: def, paintKit: parseInt(opt.paint, 10) || 0,
        wear: Math.round((parseFloat(opt.wear) || 0) * 100) / 100 });
    });
  }

  // DeathAnimations.cpp AppendPlayersJson: one row per player on a team, keyed by SteamID
  // ("s<id>", or "n<name>" when the demo has none), with their own clip and their agent model.
  function deathAnimPlayers(st) {
    var out = [], players = st.customizePlayers || {}, clips = st.daPlayerClips || {};
    Object.keys(players).map(Number).sort(function (x, y) { return x - y; }).forEach(function (k) {
      var p = players[k];
      if (!p || !(p.team >= 2)) return;
      var key = p.steamId ? 's' + p.steamId : '';
      if (!key && p.name && p.name !== 'Player') key = 'n' + p.name.replace(/[^A-Za-z0-9-]/g, '_');
      if (!key || /cs_observer/.test(p.agentModel || '')) return;
      out.push({ k: key, n: p.name || '?', t: p.team, c: clips[key] || '', m: p.agentModel || '' });
    });
    return out;
  }

  // The selected-keyframe mirror the inspectors read (st.sel + the flat selX fields).
  function syncSel(st) {
    var m = st.markers[st.selected];
    st.selCount = st.markers.length;
    st.count = st.markers.length;
    if (!m) {
      st.sel = { tick: 0, fov: 90, roll: 0, ease: 2, speedMul: 1, isLast: false };
      st.selEase = 2; st.selSpeedMul = 1; st.selIsLast = false;
      return;
    }
    // Whole marker, so the key editor's POS/ANG rows follow the selection too.
    st.sel = Object.assign({}, m, { isLast: st.selected === st.markers.length - 1 });
    st.selEase = m.ease; st.selSpeedMul = m.speedMul;
    st.selIsLast = st.sel.isLast;
  }

  global.MvmCommands = {
    load: loadBindings,
    syncEnvironment: syncEnvironment,
    run: run,
    pickTab: pickTab,
    deathAnimPlayers: deathAnimPlayers,
    unhandled: function () { return unhandled; },
    bindings: function () { return bindings; }
  };
})(window);
