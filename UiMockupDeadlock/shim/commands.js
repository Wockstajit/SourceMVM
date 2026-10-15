// `mirv_filmmaker ...` -> the state the next frame renders from.
//
// This is the other half of the bridge. A control in the panel issues a command as a marked console
// line, the shim splits the marker off, and this decides what the game would have done about it.
// Without it the mockup would be a picture: clicking CAMERA would log a command and nothing would
// move, which is exactly the half-working state that makes a mockup untrustworthy.
//
// The handler list is derived from the real command surface, not invented -- every `M.cmd(...)` in
// AfxHookSource2/Filmmaker/Deadlock/*Js.h is covered, and anything that arrives unrecognised is
// recorded as a GAP rather than ignored, so a new control with no mockup behaviour is visible in the
// Dev panel instead of silently dead.
//
// Where a command's real effect lives in C++ that this cannot reproduce (starting a render, saving a
// path to disk), the handler does the part that is observable in the UI and says so.

(function (global) {
  'use strict';

  var TPS = 64;

  function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }
  function num(s, d) { var v = parseFloat(s); return isFinite(v) ? v : d; }

  // Keep the marker list sorted and the selection pointing at the same marker it pointed at before.
  function sortMarkers(m) {
    var sel = m.markerList[m.selected];
    m.markerList.sort(function (a, b) { return a.tick - b.tick; });
    m.selected = sel ? m.markerList.indexOf(sel) : clamp(m.selected, -1, m.markerList.length - 1);
  }

  function seekTo(m, tick) {
    m.tick = Math.round(clamp(tick, m.demoStartTick, m.demoEndTick));
    m.playhead = m.tick;
  }

  var HANDLERS = {
    // ---- which workspace is open ----
    workspace: function (m, a) {
      var w = (a[0] || '').toLowerCase();
      if (w === 'off') { m.open = false; return; }
      if (w === 'toggle') { m.open = !m.open; return; }
      if (w === 'camera' || w === 'config' || w === 'render' || w === 'graph') {
        m.open = true;
        m.workspace = w;
      }
    },

    // ---- which editor is docked ----
    dock: function (m, a) {
      var what = (a[0] || '').toLowerCase();
      // The panel measures the game's replay bar and reports it, because only Panorama can. Believed
      // inside the same band the C++ trusts.
      if (what === 'nativeheight') { m.measuredNativeH = Math.round(num(a[1], 0)); return; }
      // The band the workspace laid out, which the graph hit-tests against. Recorded so the Dev
      // panel can show it; nothing here derives from it, because the projection solves it afresh.
      if (what === 'rect') { m.dockRect = a.slice(1).map(function (x) { return Math.round(num(x, 0)); }); return; }
      // The ribbon's gear. In game (DeadlockCommands.cpp) this prints Dock_Report() to the console
      // and opens nothing, so there is nothing to show here either; recorded, not invented.
      if (what === 'settings') { m.lastDockReport = (m.lastDockReport || 0) + 1; return; }
      if (what === 'native' || what === 'camera' || what === 'graph') m.dockTab = what;
    },

    // ---- F8 ----
    // HudBlackout: a render-time flag. Nothing closes; a second press puts the screen back.
    blackout: function (m, a) {
      var w = (a[0] || 'toggle').toLowerCase();
      if (w === 'on' || w === '1') m.blackout = true;
      else if (w === 'off' || w === '0') m.blackout = false;
      else if (w === 'toggle') m.blackout = !m.blackout;
    },

    // ---- playback ----
    speed: function (m, a) { m.speed = clamp(num(a[0], 1), 0.1, 16); },
    play: function (m) { m.paused = !m.paused; },
    pause: function (m) { m.paused = true; },
    // Seek the DEMO to an absolute tick. Distinct from `scrub`, which drags the camera shot's
    // playhead: they are two different playheads, and a marker click that reached for the wrong one
    // put the demo 44000 ticks from the kill it was aimed at.
    seek: function (m, a) { seekTo(m, num(a[0], m.tick)); },
    seekrel: function (m, a) { seekTo(m, m.tick + num(a[0], 0)); },
    scrub: function (m, a) { seekTo(m, num(a[0], m.tick)); },
    scrubtick: function (m, a) { seekTo(m, m.tick + num(a[0], 0)); },
    scrubend: function () { /* the drag finished; the engine only needs it to stop seeking */ },

    // ---- the shot's markers ----
    marker: function (m, a) {
      var what = (a[0] || '').toLowerCase();
      if (what === 'add') {
        m.markerList.push({
          tick: m.tick,
          // A real marker carries the camera's position. The mockup has no camera, so it records a
          // position derived from the tick -- deterministic, so captures stay comparable.
          pos: (m.tick % 1000) + ' ' + ((m.tick * 3) % 800 - 400) + ' ' + (64 + m.tick % 200)
        });
        sortMarkers(m);
        m.selected = m.markerList.findIndex(function (x) { return x.tick === m.tick; });
        return;
      }
      if (what === 'delete') {
        if (m.selected >= 0 && m.selected < m.markerList.length) {
          m.markerList.splice(m.selected, 1);
          m.selected = clamp(m.selected, -1, m.markerList.length - 1);
        }
        return;
      }
      if (what === 'clear') { m.markerList = []; m.selected = -1; return; }
      if (what === 'goto') {
        var i = Math.round(num(a[1], -1));
        if (i >= 0 && i < m.markerList.length) { m.selected = i; seekTo(m, m.markerList[i].tick); }
        return;
      }
      if (what === 'gotosel') {
        if (m.selected >= 0 && m.selected < m.markerList.length) seekTo(m, m.markerList[m.selected].tick);
        return;
      }
      if (what === 'update') {
        if (m.selected >= 0 && m.selected < m.markerList.length) m.markerList[m.selected].tick = m.tick;
        sortMarkers(m);
        return;
      }
      // Step the selection. The PATH card's nav row and the timeline both write this, so a single
      // subcommand owns it rather than each surface computing the next index itself.
      if (what === 'sel') {
        var dir = (a[1] || '').toLowerCase();
        if (!m.markerList.length) { m.selected = -1; return; }
        var next = m.selected;
        if (dir === 'prev') next = m.selected <= 0 ? m.markerList.length - 1 : m.selected - 1;
        else if (dir === 'next') next = (m.selected + 1) % m.markerList.length;
        else next = clamp(Math.round(num(a[1], m.selected)), 0, m.markerList.length - 1);
        m.selected = next;
        seekTo(m, m.markerList[next].tick);
        return;
      }
      // Per-marker values. They live on the marker in game; the mockup keeps one set because it
      // only ever shows the selected one.
      if (what === 'fov') { m.cam.fov = clamp(num(a[1], m.cam.fov), 20, 130); return; }
      if (what === 'roll') { m.cam.roll = clamp(num(a[1], m.cam.roll), -45, 45); return; }
      if (what === 'ease') {
        var names = ['none', 'in', 'out', 'inout'];
        var idx = names.indexOf((a[1] || '').toLowerCase());
        var key = a.length > 2 ? Math.round(num(a[2], -1)) : m.selected;
        if (idx >= 0 && m.markerList[key]) {
          m.markerList[key].ease = idx;
          if (key === m.selected) m.cam.ease = idx;
        }
        return;
      }
      // Arms a placement: the next click puts the selected camera where the free cam is. Nothing
      // to simulate in the browser beyond recording that it was asked for.
      if (what === 'reposition') { m.lastRepositionArmed = true; }
    },

    // ---- the path ----
    path: function (m, a) {
      var what = (a[0] || '').toLowerCase();
      if (what === 'interp') { m.pathSmooth = (a[1] || '').toLowerCase() === 'cubic'; return; }
      if (what === 'play') {
        m.playing = !m.playing;
        m.pathOwnsView = m.playing;
        if (m.playing && m.markerList.length) { m.paused = false; seekTo(m, m.markerList[0].tick); }
        return;
      }
      // Disk work. The observable part in the panel is the notice, so the command is recorded and
      // nothing is pretended.
      if (what === 'save' || what === 'load') { m.lastPathIo = what; return; }
      if (what === 'speedmode') {
        var modes = { manual: 0, constant: 1, perseg: 2 };
        var v = modes[(a[1] || '').toLowerCase()];
        if (v !== undefined) m.cam.speedMode = v;
        return;
      }
      if (what === 'draw') { m.cam.drawPath = num(a[1], 0) !== 0; return; }
      if (what === 'drawmarkers') { m.cam.drawMarkers = num(a[1], 0) !== 0; return; }
      // Holding the tick freezes replay time so a camera can be placed without the action moving.
      if (what === 'freeze') { m.cam.freeze = num(a[1], 0) !== 0; m.paused = m.cam.freeze || m.paused; }
    },

    pathshake: function (m, a) {
      var what=(a[0]||'').toLowerCase();
      if(what==='on'||what==='off'||what==='toggle'){m.pathShakeOn=what==='toggle'?!m.pathShakeOn:what==='on';return;}
      if(what==='clearall'){m.shakes=[];return;}
      var index=(a[1]==='selected')?m.selected:Math.round(num(a[1],-1));
      if(index<0)return;
      var found=m.shakes.find(function(x){return x.i===index;});
      if(what==='clear'){m.shakes=m.shakes.filter(function(x){return x.i!==index;});return;}
      if(what==='applyall'&&found){m.shakes=m.markerList.map(function(_,i){return{i:i,s:found.s,n:found.n,p:found.p};});return;}
      if(what==='set'){var row={i:index,s:Math.round(num(a[2],0)),n:clamp(Math.round(num(a[3],5)),1,10),p:clamp(Math.round(num(a[4],50)),10,100)};if(found)Object.assign(found,row);else m.shakes.push(row);}
    },

    handheld: function (m,a){
      var what=(a[0]||'').toLowerCase(),v=num(a[1],0);
      if(what==='on'||what==='off'||what==='toggle'){m.hhOn=what==='toggle'?!m.hhOn:what==='on';return;}
      if(what==='reset'){m.hhOn=false;m.hhDrift=35;m.hhDriftDir=0;m.hhCurve=100;m.hhTilt=100;m.hhRegrip=100;m.hhSpeed=100;m.hhVariance=40;m.hhPeriod=9.5;return;}
      if(what==='centerlock'){m.hhCenterLockOn=(a[1]==='toggle')?!m.hhCenterLockOn:(a[1]==='on'||a[1]==='1');return;}
      var fields={centerlockpct:'hhCenterLock',drift:'hhDrift',driftdir:'hhDriftDir',curve:'hhCurve',tilt:'hhTilt',regrip:'hhRegrip',speed:'hhSpeed',variance:'hhVariance',period:'hhPeriod'};
      if(fields[what]){m[fields[what]]=v;return;}
      if(what==='preset'){var k={idle:'hhIdle',walk:'hhWalk',run:'hhRun'}[a[1]];if(k)m[k]=a[2]||m[k];}
    },

    // ---- the follow camera ----
    //
    // A PLACED camera that locks on to a hero or rides a point on its model. Every subcommand here
    // writes one field, because the UI writes one field at a time and a compound setter would make
    // "which control changed this" unanswerable.
    follow: function (m, a) {
      var f = m.follow, what = (a[0] || '').toLowerCase();
      var setNum = function (key, lo, hi) { f[key] = clamp(num(a[1], f[key]), lo, hi); };
      if (what === 'toggle') { f.on = !f.on; return; }
      if (what === 'live') { f.on = true; return; }
      if (what === 'place') { f.on = true; f.placed = true; return; }
      if (what === 'reposition') { f.placed = true; return; }
      if (what === 'clear') { f.on = false; f.placed = false; f.hero = null; return; }
      if (what === 'mode') { f.mode = (a[1] === 'attach') ? 'attach' : 'lockon'; return; }
      if (what === 'hero') { f.hero = a[1] || null; return; }
      if (what === 'attach') { f.attach = a[1] || 'chest'; return; }
      if (what === 'nearest') { f.hero = m.heroes.length ? String(m.heroes[0].id) : null; return; }
      if (what === 'goto') { return; }
      if (what === 'target') { if ((a[1] || '') === 'clear') f.hero = null; return; }
      if (what === 'yaw') { setNum('yaw', -180, 180); return; }
      if (what === 'pitch') { setNum('pitch', -80, 80); return; }
      if (what === 'dist') { setNum('dist', 40, 1200); return; }
      if (what === 'height') { setNum('height', -200, 400); return; }
      if (what === 'lateral') { setNum('lateral', -400, 400); return; }
      if (what === 'fov') { setNum('fov', 20, 130); return; }
      if (what === 'look') { setNum('look', 0, 1); return; }
      if (what === 'pos') { setNum('pos', 0, 1); return; }
      if (what === 'predict') { setNum('predict', 0, 0.5); return; }
      if (what === 'deadzone') { setNum('deadzone', 0, 10); return; }
      if (what === 'maxturn') { setNum('maxturn', 0, 1440); return; }
      if (what === 'deathhold') { setNum('deathhold', 0, 10); return; }
      if (what === 'releaseondeath') { f.releaseOnDeath = num(a[1], 0) !== 0; return; }
      if (what === 'holdlast') { f.holdLast = num(a[1], 0) !== 0; return; }
      if (what === 'retarget') { f.retarget = num(a[1], 0) !== 0; return; }
      if (what === 'resetframing') {
        f.yaw = 35; f.pitch = 22; f.dist = 340; f.height = 60; f.lateral = 90; f.fov = 68;
        return;
      }
      if (what === 'resettracking') {
        f.look = 0.35; f.pos = 0.4; f.predict = 0.08;
        f.deadzone = 1.5; f.maxturn = 420; f.deathhold = 2;
      }
    },

    // ---- the loop ----
    loop: function (m, a) {
      var what = (a[0] || '').toLowerCase();
      if (what === 'toggle') { m.loopOn = !m.loopOn; return; }
      if (what === 'in') { m.loopIn = m.tick; return; }
      if (what === 'out') { m.loopOut = m.tick; return; }
      if (what === 'inat') { m.loopIn = Math.round(num(a[1], m.loopIn)); return; }
      if (what === 'outat') { m.loopOut = Math.round(num(a[1], m.loopOut)); return; }
      if (what === 'clear') { m.loopOn = false; m.loopIn = 0; m.loopOut = 0; return; }
      if (what === 'torender') { m.renderStart = m.loopIn; m.renderEnd = m.loopOut; }
    },

    // ---- the graph ----
    graph: function (m, a) {
      var what = (a[0] || '').toLowerCase();
      if (what === 'channel') { m.graphChannel = Math.round(num(a[1], 0)); return; }
      if (what === 'solo') {
        var i = Math.round(num(a[1], -1));
        m.graphSolo = (m.graphSolo === i) ? -1 : i;
        return;
      }
      if (what === 'drive') { m.graphDrive = !m.graphDrive; return; }
      if (what === 'smooth') { m.pathSmooth = true; return; }
      if (what === 'linear') { m.pathSmooth = false; return; }
      if (what === 'seed') { m.graphSeeded = true; return; }
      // Undo and redo operate on a curve store that lives in C++. Counted so the buttons are seen to
      // fire; the curves here are derived from the markers, so there is nothing to roll back.
      if (what === 'undo' || what === 'redo') { m.graphEdits = (m.graphEdits || 0) + 1; }
    },

    // ---- the render job ----
    render: function (m, a) {
      var what = (a[0] || '').toLowerCase();
      if (what === 'fps') { m.renderFps = Math.round(num(a[1], m.renderFps)); return; }
      // `range` is deliberately two commands under one name, split on argument count exactly the
      // way the game's router splits it: two numbers is an explicit tick range, one word is the
      // range MODE the option rows pick.
      if (what === 'range' && a.length >= 3) {
        m.renderStart = Math.round(num(a[1], m.renderStart));
        m.renderEnd = Math.round(num(a[2], m.renderEnd));
        return;
      }
      var OPTS = {
        range: ['all', 'shot', 'segment', 'loop', 'custom'],
        camsrc: ['current', 'free', 'path', 'follow'],
        hud: ['none', 'game', 'full'],
        format: ['png', 'tga', 'jpg', 'mp4']
      };
      var FIELD = { range: 'renderRange', camsrc: 'renderCamSrc', hud: 'renderHud', format: 'renderFormat' };
      if (OPTS[what] && OPTS[what].indexOf((a[1] || '').toLowerCase()) >= 0) {
        m[FIELD[what]] = (a[1] || '').toLowerCase();
        return;
      }
      if (what === 'quality') { m.renderQuality = clamp(num(a[1], m.renderQuality), 1, 100); return; }
      if (what === 'names') { m.renderNames = num(a[1], 0) !== 0; return; }
      if (what === 'damage') { m.renderDamage = num(a[1], 0) !== 0; return; }
      if (what === 'pauseondrop') { m.renderPauseOnDrop = num(a[1], 0) !== 0; return; }
      if (what === 'beep') { m.renderBeep = num(a[1], 0) !== 0; return; }
      // Choosing a folder opens a picker in game. There is nothing to open here, so the command is
      // taken and the path left alone rather than invented.
      if (what === 'dir') { return; }
      if (what === 'rangefromshot') {
        if (m.markerList.length) {
          m.renderStart = m.markerList[0].tick;
          m.renderEnd = m.markerList[m.markerList.length - 1].tick;
        }
        return;
      }
      if (what === 'start') {
        m.renderState = 'Capturing';
        m.renderFrames = 0;
        m.renderDropped = 0;
        m.renderTake++;
        m.renderDir = 'C:\\Temp\\take' + String(m.renderTake).padStart(4, '0');
        return;
      }
      if (what === 'stop') { m.renderState = 'Idle'; }
    },

    // ---- input and the camera ----
    mouse: function (m, a) {
      var what = (a[0] || '').toLowerCase();
      if (what === 'toggle') m.uiMouse = !m.uiMouse;
      else if (what === 'ui' || what === 'on') m.uiMouse = true;
      else if (what === 'game' || what === 'off') m.uiMouse = false;
    },
    freecam: function (m) { m.freecam = !m.freecam; },

    // ---- the game HUD inside the preview (DeadlockCommands.cpp "hud": scaled|hidden|toggle) ----
    hud: function (m, a) {
      var op = (a[0] || '').toLowerCase();
      if (op === 'hidden' || op === 'hide') m.hudHidden = true;
      else if (op === 'scaled' || op === 'show') m.hudHidden = false;
      else if (op === 'toggle') m.hudHidden = !m.hudHidden;
    },

    // ---- the panel reporting its own measurements ----
    ui: function (m, a) {
      if ((a[0] || '').toLowerCase() === 'inspectorw') {
        // The column grip. C++ clamps against the screen; so does the projection's solver.
        var w = Math.round(num(a[1], 0));
        if (w >= 320 && w <= 4096) m.inspectorW = w;
        return;
      }
      if ((a[0] || '').toLowerCase() === 'screen') {
        // In game this is the ONLY channel between the panel's measurement and the C++ that owns
        // every number derived from it. The mockup sets the stage size itself, so the report is a
        // round trip -- and that is worth keeping, because a mismatch here means the shim is
        // measuring differently from the way Panorama does.
        m.reportedScreenW = Math.round(num(a[1], 0));
        m.reportedScreenH = Math.round(num(a[2], 0));
      }
    },

    // ---- the preview ----
    preview: function (m, a) {
      var what = (a[0] || '').toLowerCase();
      if (what === 'hud') { m.hudHidden = (a[1] || '').toLowerCase() === 'hidden'; return; }
      if (what === 'scaled') { m.previewScaled = (a[1] || '').toLowerCase() !== 'off'; }
    }
  };

  function Router(model) {
    this.model = model;
    this.log = [];
    this.gaps = [];
  }

  Router.prototype.run = function (line) {
    this.log.push(line);
    if (this.log.length > 400) this.log.shift();

    // The native bar's own functions come through as a recorded call rather than a command. The
    // Regular Timeline delegates to them on purpose -- the game owns skipping and speed stepping --
    // so they are not gaps.
    if (line.indexOf('(native) ') === 0) return;

    var parts = line.trim().split(/\s+/);
    // The hotkeys (DeadlockHotkeys.cpp) run a few engine commands directly: SPACE resolves to
    // demo_pause / demo_resume, the arrows to `mirv_skip time <s>`.
    if (parts[0] === 'demo_pause') { this.model.paused = true; return; }
    if (parts[0] === 'demo_resume') { this.model.paused = false; return; }
    if (parts[0] === 'mirv_skip' && (parts[1] || '').toLowerCase() === 'time') {
      seekTo(this.model, this.model.tick + num(parts[2], 0) * TPS); return;
    }
    if (parts[0] !== 'mirv_filmmaker') { this.note(line, 'not a mirv_filmmaker command'); return; }
    var sub = (parts[1] || '').toLowerCase();
    var fn = HANDLERS[sub];
    if (!fn) { this.note(line, 'no mockup handler for `' + sub + '`'); return; }
    try {
      fn(this.model, parts.slice(2));
    } catch (e) {
      this.note(line, String(e));
    }
  };

  Router.prototype.note = function (line, why) {
    this.gaps.push({ line: line, why: why });
    if (this.gaps.length > 200) this.gaps.shift();
  };

  // Advances the fake demo clock. Matched to the real thing in the two ways a panel can tell: the
  // rate follows the speed dial, and the loop wraps at its out point, which is what makes the loop
  // controls testable without the game.
  Router.prototype.tick = function (dtSeconds) {
    var m = this.model;
    if (m.paused || !m.demo) return;
    var advance = dtSeconds * TPS * (m.speed || 1);
    var next = m.tick + advance;
    if (m.loopOn && m.loopOut > m.loopIn && next >= m.loopOut) {
      next = m.loopIn + (next - m.loopOut);
    }
    if (next > m.demoEndTick) next = m.demoStartTick;
    m.tick = Math.round(next);
    m.playhead = m.tick;
    if (m.renderState === 'Capturing') {
      m.renderFrames += Math.max(1, Math.round(dtSeconds * m.renderFps));
      if (m.tick >= m.renderEnd) m.renderState = 'Idle';
    }
  };

  global.MvmDeadlockCommands = { Router: Router, handlers: Object.keys(HANDLERS) };
})(window);
