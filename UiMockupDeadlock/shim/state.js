// The per-frame state the panels render from.
//
// In game this is a JSON blob composed by BuildStateJson() in DeadlockWorkspaces.cpp and passed
// straight into `$.MvmWorkspace.render(...)`. There is no attribute feed and no polling: one call,
// one object. So the mockup's job is to produce the SAME object, field for field, and anything the
// panels read that is missing here is a field the mockup would silently render wrong.
//
// Every field below is named after the one in BuildStateJson, in the same order, so the two can be
// read side by side. Where C++ derives a value rather than reading it (startTick/endTick from the
// marker list, `live` from who owns the view, the whole of `layout`), this derives it the same way
// instead of storing an answer -- a stored answer is how a mockup starts disagreeing with the game
// about states nobody thought to set by hand.
//
// The LAYOUT is the one thing that is genuinely re-implemented rather than fed in, because in game
// C++ solves it. See lib/workspace-layout.mjs for why that port exists and what keeps it honest.

(function (global) {
  'use strict';

  // Dock metrics, from DeadlockDock.cpp. Literals there, literals here, and the mockup test asserts
  // they still agree with the C++.
  var RIBBON_H = 36;
  var NATIVE_FALLBACK_H = 94;
  var CAMERA_H = 172;
  var GRAPH_H = 386;

  // From DeadlockWorkspaces.cpp.
  var INSPECTOR_W = 520;
  var HEADER_H = 44;
  // Mirrors kBottomBandH in DeadlockWorkspaces.cpp: the accent band across the foot of the area,
  // which the solver reserves so the picture is never letterboxed underneath it.
  var BOTTOM_BAND_H = 46;
  // kJobColumnW in DeadlockWorkspaces.cpp / JOB_W in DeadlockConfigRenderBodyJs.h.
  var JOB_COLUMN_W = 340;

  var TICKS_PER_SECOND = 64;

  var GRAPH_CHANNELS = [
    { name: 'X',     color: '#FF410D' },
    { name: 'Y',     color: '#5FE69E' },
    { name: 'Z',     color: '#1A9FFF' },
    { name: 'Pitch', color: '#FFED79' },
    { name: 'Yaw',   color: '#CE90FF' },
    { name: 'Roll',  color: '#3FDCFF' },
    { name: 'FOV',   color: '#DEC5A0' }
  ];

  // A plausible replay: a Blitz match on one of Deadlock's own maps, paused part way in, with a few
  // camera markers laid down. Values are the shape a real session reports, not round numbers, so a
  // panel that only lays out correctly for tidy input shows it here.
  function initial() {
    return {
      // --- what the workspace is doing ---
      // Closed, like the game after a replay loads: the replay bar with our buttons, the director
      // card, nothing else. `workspace` is what opens when a button is pressed.
      open: false,
      workspace: 'camera',            // camera | config | render | graph
      dockTab: 'native',              // native | camera | graph
      blackout: false,                // F8 (HudBlackout_Active): render-time only, nothing closes

      // --- the demo ---
      demo: true,
      paused: true,
      level: 'street_test',
      tick: 17801,
      demoStartTick: 8200,            // the replay's own span, which the game's bar runs on
      demoEndTick: 41600,

      // --- input ---
      uiMouse: true,
      mx: 640, my: 900, lmb: false, rmb: false, shift: false,
      // The digit / Backspace / Enter / Escape bitmask the value tiles read (DeadlockWorkspaces.cpp
      // polls GetAsyncKeyState while focused and in UI-mouse): bits 0-9 digits, 10 Backspace,
      // 11 Enter, 12 Escape.
      numberKeys: 0,
      // The free cam and its pose, for the director card (DeadlockMovieHud.cpp).
      freecam: false,
      camSpeed: 1.0,
      camPose: { ox: 512.0, oy: -128.0, oz: 240.0, pitch: 4.0, yaw: 118.0, roll: 0.0 },

      // --- playback ---
      speed: 1.0,
      pathSmooth: true,

      // --- the shot ---
      markerList: [
        { tick: 17000, pos: '120 -400 96', selected: false },
        { tick: 17800, pos: '512 -128 240', selected: true },
        { tick: 18500, pos: '900 300 64', selected: false }
      ],
      selected: 1,

      // Per-marker and path-wide camera settings, mirroring CamMarker / PathSettings. speedMode is
      // the PathSettings enum: 0 Manual, 1 Constant, 2 PerSegment.
      cam: {
        fov: 70, roll: 0, ease: 0, speedMode: 0,
        freeze: false, drawPath: true, drawMarkers: true
      },
      pathShakeOn: true,
      shakes: [{ i: 1, s: 0, n: 5, p: 50 }],
      shakeStyles: [[0, 'Hand Shake', 'HAND_SHAKE'], [1, 'Road Vibration', 'ROAD_VIBRATION']],
      hhOn: false, hhCenterLockOn: false, hhCenterLock: 100,
      hhDrift: 35, hhDriftDir: 0, hhCurve: 100, hhTilt: 100, hhRegrip: 100,
      hhSpeed: 100, hhVariance: 40, hhPeriod: 9.5,
      hhIdle: 'INVESTIGATION', hhWalk: 'WALK_TO_THE_STORE', hhRun: 'HANDYCAM_RUN',
      handheldCurves: [['INVESTIGATION','Investigation'],['WALK_TO_THE_STORE','Walk to the Store'],['HANDYCAM_RUN','Handycam Run']],

      // The follow camera. Deadlock's replay is third-person, so this is a PLACED camera that
      // locks on to a hero or attaches to a point on its model -- never an orbit, never a POV.
      follow: {
        on: false, mode: 'lockon', hero: null, attach: 'chest',
        yaw: 35, pitch: 22, dist: 340, height: 60, lateral: 90, fov: 68,
        look: 0.35, pos: 0.4, predict: 0.08, deadzone: 1.5, maxturn: 420, deathhold: 2,
        releaseOnDeath: true, holdLast: false, retarget: true
      },

      // Who is in this replay. In game these come from the entity list; the Deadlock port cannot
      // enumerate entities yet, so the game sends an empty array and the picker says so.
      heroes: [],

      playhead: 17801,
      playing: false,
      pathOwnsView: false,

      // --- the preview ---
      hudHidden: false,
      previewScaled: true,
      viewportW: 1920,
      viewportH: 1080,
      worldAspect: 16 / 9,

      // --- the dock ---
      measuredNativeH: 0,             // reported by the panel once the bar has laid out

      // --- the render job's shape, as the Render column states it ---
      renderRange: 'all',
      renderCamSrc: 'current',
      renderHud: 'game',
      renderFormat: 'png',
      renderQuality: 92,
      renderNames: true,
      renderDamage: true,
      renderPauseOnDrop: true,
      renderBeep: false,

      // --- the loop ---
      loopOn: true,
      loopIn: 17200,
      loopOut: 18000,

      // --- the graph ---
      graphDrive: false,
      graphChannel: 0,
      graphSolo: -1,
      // The right-click Ease/Align/Delete menu. C++ opens it (DeadlockGraph.cpp OpenContextMenu)
      // and publishes it in the graph body's LOCAL space; here a right-click on the graph band
      // opens it and any click closes it.
      graphMenu: { open: false, x: 0, y: 0 },

      // --- the render job ---
      renderState: 'Idle',            // Idle | Capturing | Stopping
      renderFps: 60,
      renderFrames: 0,
      renderDropped: 0,
      renderTake: 3,
      renderStart: 17000,
      renderEnd: 18500,
      renderDir: 'C:\\Temp\\take0003',

      // --- the inspector column's requested width, dragged by its grip ---
      inspectorW: INSPECTOR_W,

      // --- the screen, which the panel measures and reports in game ---
      screenW: 1920,
      screenH: 1080
    };
  }

  // Dock_EditorHeightFor, including the clamp. A bar reported as 4px tall was measured before layout
  // settled; one reported as 900px is the wrong panel. Either would leave the preview overlapping
  // the bar or squeezed to nothing, so neither is believed.
  function editorHeight(model) {
    if (model.dockTab === 'camera') return CAMERA_H;
    if (model.dockTab === 'graph') return GRAPH_H;
    var m = model.measuredNativeH;
    return (m >= 40 && m <= 400) ? m : NATIVE_FALLBACK_H;
  }

  function dockHeight(model) {
    return RIBBON_H + editorHeight(model);
  }

  // The ribbon's right-hand line: what only the active tab knows. The dock cannot work it out
  // itself, and asking each panel for it would mean three panels each keeping their own copy of the
  // shot's state.
  // The exact strings DeadlockDock.cpp publishes, per tab -- the ribbon's meta label and the spacer
  // beside it lay out from their width.
  function dockMeta(model) {
    if (model.dockTab === 'graph') {
      // Counted the same way graphChannels decides visibility, so the ribbon and the graph's own
      // title cannot report different numbers for the same channels.
      var on = model.graphSolo >= 0 ? 1 : 5;
      return on + ' of ' + GRAPH_CHANNELS.length + ' channels';
    }
    if (model.dockTab === 'camera') {
      return model.markerList.length + (model.markerList.length === 1 ? ' marker' : ' markers');
    }
    return (model.level || 'no replay') + ' · ' + TICKS_PER_SECOND + ' tick';
  }

  // A curve per channel, shaped from the markers so the graph is drawing the same shot the timeline
  // is. Normalised 0..1 in both axes, which is what the graph body expects.
  function graphChannels(model) {
    var n = model.markerList.length;
    return GRAPH_CHANNELS.map(function (ch, ci) {
      var visible = model.graphSolo < 0 ? (ci < 5) : (ci === model.graphSolo);
      if (!visible || n < 2) return { name: ch.name, color: ch.color, visible: visible, lo: 0, hi: 1, current: 0.5, pts: [], keyPts: [] };
      var keyPts = model.markerList.map(function (m, i) {
        var t = i / (n - 1);
        // Deterministic, so a screenshot comparison is repeatable -- a random curve would differ on
        // every capture and every diff against the game would be noise.
        var v = 0.5 + 0.36 * Math.sin((i + 1) * (ci + 1) * 0.9);
        return { t: t, v: Math.max(0.02, Math.min(0.98, v)), sel: i === model.selected };
      });
      var pts = [];
      for (var s = 0; s <= 48; ++s) {
        var t = s / 48;
        var pos = t * (n - 1);
        var i0 = Math.min(n - 2, Math.floor(pos));
        var f = pos - i0;
        var a = keyPts[i0].v, b = keyPts[i0 + 1].v;
        // Smoothstep when the path is smooth, straight when it is linear -- the same distinction the
        // timeline's Linear | Smooth pair lights, so the two views cannot disagree.
        var e = model.pathSmooth ? f * f * (3 - 2 * f) : f;
        pts.push(t, a + (b - a) * e);
      }
      return {
        name: ch.name, color: ch.color, visible: visible,
        lo: 0, hi: 1,
        current: keyPts[Math.min(keyPts.length - 1, model.selected < 0 ? 0 : model.selected)].v,
        pts: pts, keyPts: keyPts
      };
    });
  }

  // The object the C++ builds. Deliberately assembled in BuildStateJson's order.
  function project(model, layoutSolver) {
    var markers = model.markerList.length;
    var first = markers ? model.markerList[0].tick : model.tick;
    var last = markers ? model.markerList[markers - 1].tick : model.tick + 1;
    if (last <= first) last = first + 1;

    var dockH = dockHeight(model);
    // Every workspace has the inspector column. Render ALSO has the JOB column on the left, which
    // insets the band the same way -- so the preview is what is left between the two.
    var inspectorW = model.inspectorW || INSPECTOR_W;
    var layout = layoutSolver({
      screenW: model.screenW,
      screenH: model.screenH,
      inspectorW: model.open ? inspectorW : 0,
      leftW: (model.open && model.workspace === 'render') ? JOB_COLUMN_W : 0,
      // EVERY TAB INSETS THE PREVIEW. The Regular Timeline used to be exempt, on the reasoning
      // that it is Deadlock's own translucent bar with the game rendering behind it. It is our own
      // opaque panel now and the game's bar is force-hidden on all three tabs, so exempting it just
      // ran the picture under the dock.
      dockH: model.open ? dockH : 0,
      headerH: model.open ? HEADER_H : 0,
      bottomBandH: model.open ? BOTTOM_BAND_H : 0,
      worldAspect: model.worldAspect
    });

    return {
      open: !!model.open,
      // F8. Kept separate from `open` exactly as the C++ does: a render-time flag the JS side
      // checks to hide every panel without forgetting what was actually open.
      blackout: !!model.blackout,
      workspace: model.workspace,
      demo: !!model.demo,
      paused: !!model.paused,
      uiMouse: !!model.uiMouse,
      speed: model.speed,
      pathSmooth: !!model.pathSmooth,
      mx: model.mx, my: model.my,
      lmb: !!model.lmb, rmb: !!model.rmb, shift: !!model.shift,
      numberKeys: model.uiMouse ? (model.numberKeys | 0) : 0,
      tick: model.tick,
      time: model.tick / TICKS_PER_SECOND,
      level: model.level,
      markers: markers,
      startTick: first,
      endTick: last,
      playhead: model.playhead,
      playing: !!model.playing,
      selected: model.selected,
      hudHidden: !!model.hudHidden,
      previewScaled: !!model.previewScaled,
      // The camera path owns the view while a shot plays; otherwise the frame is the game's own
      // camera. The preview frame changes colour on this, so "which camera am I looking at" is never
      // a guess.
      live: !(model.pathOwnsView || model.playing || model.graphDrive),
      viewportW: model.viewportW,
      viewportH: model.viewportH,

      loopOn: !!model.loopOn,
      loopIn: model.loopIn,
      loopOut: model.loopOut,
      loopReady: model.loopOut > model.loopIn,

      markerList: model.markerList.map(function (m, i) {
        return { tick: m.tick, pos: m.pos, ease: m.ease || 0, selected: i === model.selected };
      }),

      cam: {
        fov: model.cam.fov, roll: model.cam.roll, ease: model.cam.ease,
        speedMode: model.cam.speedMode, freeze: !!model.cam.freeze,
        drawPath: !!model.cam.drawPath, drawMarkers: !!model.cam.drawMarkers
      },
      follow: JSON.parse(JSON.stringify(model.follow)),
      heroes: model.heroes.slice(),
      pathShakeOn: !!model.pathShakeOn,
      shakes: model.shakes.map(function(x){return Object.assign({},x);}),
      shakeStyles: model.shakeStyles.slice(), shakeEssentialsCount: 5,
      hhOn: !!model.hhOn, hhCenterLockOn: !!model.hhCenterLockOn, hhCenterLock:model.hhCenterLock,
      hhDrift:model.hhDrift, hhDriftDir:model.hhDriftDir, hhCurve:model.hhCurve, hhTilt:model.hhTilt,
      hhRegrip:model.hhRegrip, hhSpeed:model.hhSpeed, hhVariance:model.hhVariance, hhPeriod:model.hhPeriod,
      hhIdle:model.hhIdle, hhWalk:model.hhWalk, hhRun:model.hhRun, handheldCurves:model.handheldCurves.slice(),

      layout: {
        valid: layout.valid,
        screenW: model.screenW, screenH: model.screenH,
        inspectorW: layout.inspectorW,
        leftW: layout.leftW,
        dockH: model.open ? dockH : 0,
        headerH: model.open ? HEADER_H : 0,
        frameTopY: layout.frameTopY, frameBottomY: layout.frameBottomY,
        bandBottomY: layout.bandBottomY,
        areaX: layout.areaX, areaY: layout.areaY, areaW: layout.areaW, areaH: layout.areaH,
        previewX: layout.previewX, previewY: layout.previewY,
        previewW: layout.previewW, previewH: layout.previewH
      },

      dock: {
        tab: model.dockTab,
        ribbonH: RIBBON_H,
        editorH: editorHeight(model),
        meta: dockMeta(model)
      },

      graph: {
        // The plot rect the graph hit-tests against: the dock band, below the ribbon. In game C++
        // publishes the rectangle the PANEL reported, which is why it is derived from the same
        // numbers here rather than guessed.
        plotX: 0,
        plotY: model.screenH - editorHeight(model),
        plotW: layout.areaW,
        plotH: editorHeight(model),
        drive: !!model.graphDrive,
        smooth: !!model.pathSmooth,
        keys: markers * GRAPH_CHANNELS.length,
        selection: model.selected,
        playhead: last > first ? (model.playhead - first) / (last - first) : 0,
        channels: graphChannels(model),
        menu: { open: !!model.graphMenu.open, x: model.graphMenu.x, y: model.graphMenu.y }
      },

      // DeadlockMovieHud.cpp MovieHud_AppendStateJson: the director card is FOR the moment the
      // workspace is closed. Hidden on F8, while a take is writing frames, and whenever a
      // workspace is open; the owner word is the view-owner resolver's.
      director: {
        visible: !!model.demo && !model.blackout && model.renderState === 'Idle' && !model.open,
        playing: !!model.demo && !model.paused,
        paused: !!model.paused,
        owner: model.pathOwnsView || model.playing ? 'Shot' : (model.graphDrive ? 'Graph'
          : (model.follow.on ? 'Follow' : (model.freecam ? 'FreeCam' : 'Game'))),
        freecam: !!model.freecam,
        speed: model.camSpeed,
        ox: model.camPose.ox, oy: model.camPose.oy, oz: model.camPose.oz,
        pitch: model.camPose.pitch, yaw: model.camPose.yaw, roll: model.camPose.roll,
        fov: model.cam.fov,
        tick: model.tick,
        time: model.tick / TICKS_PER_SECOND
      },

      render: {
        state: model.renderState,
        running: model.renderState !== 'Idle' && model.renderState !== 'Failed',
        fps: model.renderFps,
        frames: model.renderFrames,
        frame: model.renderFrames,
        dropped: model.renderDropped,
        take: model.renderTake,
        startTick: model.renderStart,
        endTick: model.renderEnd,
        dir: model.renderDir,
        // The job's shape, in the same words the option rows use.
        range: model.renderRange,
        camSrc: model.renderCamSrc,
        hud: model.renderHud,
        format: model.renderFormat,
        quality: model.renderQuality,
        names: !!model.renderNames,
        damage: !!model.renderDamage,
        pauseOnDrop: !!model.renderPauseOnDrop,
        beep: !!model.renderBeep
      }
    };
  }

  global.MvmDeadlockState = {
    initial: initial,
    project: project,
    dockHeight: dockHeight,
    editorHeight: editorHeight,
    constants: {
      RIBBON_H: RIBBON_H,
      NATIVE_FALLBACK_H: NATIVE_FALLBACK_H,
      CAMERA_H: CAMERA_H,
      GRAPH_H: GRAPH_H,
      INSPECTOR_W: INSPECTOR_W,
      HEADER_H: HEADER_H,
      TICKS_PER_SECOND: TICKS_PER_SECOND
    }
  };
})(window);
