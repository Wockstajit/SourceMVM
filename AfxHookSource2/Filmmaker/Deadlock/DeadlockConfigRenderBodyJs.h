#pragma once

// The CONFIG and RENDER columns. Two fragments in one file because they are each small and they
// are the same shell with different contents; the C++ pushes them as two separate macros so the
// order in DeadlockWorkspaces.cpp still reads as one fragment per RunScript.
//
// CONFIG is an empty workspace shell. It has no feature controls or feature state bindings.
//
// RENDER IS ONE BEAUTY PASS. No pass splitting, no re-lighting, no first-person source -- the CS2
// render feature was cut back to beauty-only and the Deadlock port starts there rather than
// re-growing what was removed. The column is four groups on one flat page: Range, Camera, HUD,
// Output.
//
// ONE REASON, ONE STRING. "Cannot render yet" has exactly one source: blockedReason() below, derived
// from the shot the DOCK publishes. The JOB column, the blocked option row and the START CAPTURE
// button all read that same string, so a ribbon saying "4 markers" can never sit beside a warning
// that there are fewer than two.

#define DEADLOCK_CONFIG_BODY_JS R"CFJS(
(function () {
  if ($.MvmConfigBody) return;
  var M = $.MvmStyle;
  var X = $.MvmControls;
  if (!M || !X) return;
  var S = M.S;

  var wrap = null, col = null;

  // Its own, like every other fragment's: the fragments' IIFEs do not share locals, so a `cmd`
  // from the render fragment below is not in scope here (the RENDER action and GAME UI chip threw
  // a ReferenceError that smallBtn's try/catch swallowed).
  function cmd(text) { M.cmd(text); }

  function build(parent) {
    wrap = M.make(parent, 'Panel', 'MvmConfigBody', { width: '100%', height: '100%', flowChildren: 'down' });
    if (!wrap) return;
    wrap.visible = false;

    col = X.column(wrap, {
      id: 'MvmConfigColumn',
      name: 'CONFIG',
      accent: S.blue,
      emptyText: '',
      // CS2's GAME UI chip. Deadlock's HUD has two states inside the preview -- scaled into the
      // picture, or hidden for a clean plate -- so the cycle is SHOW ALL <-> HIDE ALL.
      midChip: { label: 'GAME UI: SHOW ALL', onActivate: function () { cmd('mirv_filmmaker hud toggle'); } }
    });
    if (!col) return;
    col.setVisible(true);
    col.setEmpty(false);
    // CS2's CONFIG carries the same RENDER action row as the editor (ConfigHudJs.h mvmActionRow).
    col.footer.set('', [
      { cap: 'Render', colour: S.blue, run: function () { cmd('mirv_filmmaker workspace render'); } }
    ]);

  }

  function setVisible(v) {
    if (wrap) { try { wrap.visible = !!v; } catch (e) {} }
  }

  function render(st) {
    if (!col) return;
    col.setMouseMode(!!st.uiMouse);
    col.setMidChip('GAME UI: ' + (st.hudHidden ? 'HIDE ALL' : 'SHOW ALL'));
  }

  $.MvmConfigBody = { build: build, setVisible: setVisible, render: render };
})();
)CFJS"

#define DEADLOCK_RENDER_BODY_JS R"CFJS(
(function () {
  if ($.MvmRenderBody) return;
  var M = $.MvmStyle;
  var X = $.MvmControls;
  if (!M || !X) return;
  var S = M.S;

  // The JOB column's width. Fixed, and the same number the layout solver insets the preview by --
  // C++ holds the authoritative copy in DeadlockWorkspaces.cpp; if the two disagree the world is
  // drawn underneath the column, which is visible immediately and is why it is a named constant on
  // both sides rather than a literal in a style string.
  var JOB_W = 340;

  function cmd(t) { M.cmd(t); }

  var wrap = null, col = null, card = null;
  var job = null, jobTitle = null, jobRows = {}, jobStatus = null, jobProblem = null, startBtn = null, startLabel = null;
  var running = false, blocked = '';
  var last = null;

  var RANGE_NAMES = { all: 'Whole replay', shot: 'Camera path', segment: 'Selected segment',
                      loop: 'Playback loop', custom: 'Custom in/out' };
  var CAM_NAMES = { current: 'Current view', free: 'Free camera', path: 'Camera path', follow: 'Follow camera' };
  var HUD_NAMES = { none: 'No HUD', game: 'In-game', full: 'Full HUD' };
  var FORMAT_NAMES = {png:'PNG sequence', tga:'TGA sequence', jpg:'JPEG sequence', mp4:'MP4', avi:'AVI lossless', gif:'GIF', tiff:'TIFF sequence', bmp:'BMP sequence', exr:'EXR (unavailable)'};
  var FORMATS = [['mp4','MP4','H.264 video'],['avi','AVI','Lossless HuffYUV'],['gif','GIF','Looping animation'],['png','PNG sequence','Lossless'],['tiff','TIFF sequence','Lossless'],['tga','TGA sequence','Uncompressed'],['bmp','BMP sequence','Uncompressed'],['jpg','JPEG sequence','Compressed']];
  var RATES = [['24','24 fps',''],['30','30 fps',''],['50','50 fps',''],['60','60 fps',''],['120','120 fps',''],['240','240 fps','']];

  // --- the one source of "cannot render yet" ---------------------------------------------------
  //
  // Each check returns '' when it is satisfied, so they are COLLECTED rather than returned from: an
  // early return on a satisfied range reported "fine" and skipped the camera checks entirely.
  function noPath(st) {
    return ((st.markers || 0) < 2) ? 'Needs 2 or more markers. Place them with K.' : '';
  }
  function noSegment(st) {
    var list = st.markerList || [];
    var sel = (st.selected === undefined) ? -1 : st.selected;
    return (sel >= 0 && sel < list.length - 1) ? '' : 'Select a marker that has a NEXT marker.';
  }
  function noFollow(st) {
    var f = st.follow || {};
    return (f.on && f.hero !== null && f.hero !== undefined)
      ? '' : 'No follow camera set up yet — pick a hero in the camera editor.';
  }
  function blockedReason(st) {
    var r = st.render || {};
    var reasons = [
      r.format === 'gif' && r.fps > 100 ? 'GIF supports at most 100 fps. Choose a lower rate or MP4.' : '',
      r.range === 'shot' ? noPath(st) : '',
      r.range === 'segment' ? noSegment(st) : '',
      r.camSrc === 'path' ? noPath(st) : '',
      r.camSrc === 'follow' ? noFollow(st) : ''
    ];
    for (var i = 0; i < reasons.length; ++i) if (reasons[i]) return reasons[i];
    return '';
  }

  // --- Set In / Set Out / Clear ------------------------------------------------------------------
  //
  // CS2's Range tab has its own footer trio (CameraEditorRenderJs.h: setInBtn/setOutBtn/clearRangeBtn)
  // that each send ONE bound at a time (`render in <tick>` / `render out <tick>` / `render clearrange`).
  // Deadlock's console has no such split -- DeadlockCommands.cpp's "render" dispatcher only accepts
  // the pair together (`render range <start> <end>`; a single numeric argument instead reads as the
  // STRING range-mode setter, e.g. `render range shot`) -- so Set In/Set Out here read the OTHER bound
  // back off the last-pushed state and resend both, and Clear sends 0 0 (RenderSettings' own "unset"
  // value: startTick<=0 resolves to the shot/current tick, endTick<=0 resolves to the shot's last tick).
  function setRangeIn() {
    if (!last || typeof last.tick !== 'number') return;
    var r = last.render || {};
    var end = (typeof r.endTick === 'number') ? r.endTick : 0;
    cmd('mirv_filmmaker render range ' + (last.tick | 0) + ' ' + end);
  }
  function setRangeOut() {
    if (!last || typeof last.tick !== 'number') return;
    var r = last.render || {};
    var start = (typeof r.startTick === 'number') ? r.startTick : 0;
    cmd('mirv_filmmaker render range ' + start + ' ' + (last.tick | 0));
  }
)CFJS" R"CFJS(
  // --- the JOB column --------------------------------------------------------------------------
  //
  // Built into the workspace ROOT, not into the inspector column, because it is pinned to the left
  // of the stage. The shell places it from the solved layout.
  function buildJob(root) {
    job = M.make(root, 'Panel', 'MvmJobColumn', {
      backgroundColor: S.panelBg, borderRight: '1px solid ' + S.border,
      flowChildren: 'down', horizontalAlign: 'left', verticalAlign: 'top', zIndex: 54
    });
    if (!job) return null;
    job.visible = false;

    // CS2 pairs its RENDER title with a live "RENDER · RUNNING" badge (renderTitle.text =
    // jobActive ? 'RENDER · RUNNING' : 'RENDER', CameraEditorRenderJs.h) -- reproduced on THIS
    // column's own JOB title below (jobTitle), since CS2's JOB column carries no chip of its own
    // otherwise. The "go back to the editor" chip CS2 pairs with its RENDER title on that OTHER
    // panel's row now lives there too (DeadlockControlsJs.h's buildColumn, extraChip) rather than
    // here -- it used to live on this row, on the opposite side of the screen from CS2's.
    var head = M.make(job, 'Panel', '', {
      width: '100%', flowChildren: 'right', padding: '14px 18px 12px 18px', borderBottom: '1px solid ' + S.border
    });
    jobTitle = M.label(head, '', 'JOB', {
      fontFamily: S.fontSerif, fontSize: '16px', letterSpacing: '2px', color: S.soul,
      width: 'fill-parent-flow(1.0)', verticalAlign: 'middle'
    });

    var bodyPanel = M.make(job, 'Panel', '', {
      width: '100%', height: 'fill-parent-flow(1.0)', flowChildren: 'down', padding: '18px'
    });

    var KEYS = [['RANGE', 15], ['LENGTH', 15], ['FRAMES', 15], ['CAMERA', 15],
                ['HUD', 15], ['OUTPUT', 15], ['FOLDER', 12]];
    for (var i = 0; i < KEYS.length; ++i) {
      var row = M.make(bodyPanel, 'Panel', '', { width: '100%', flowChildren: 'down', marginBottom: '13px' });
      M.label(row, '', KEYS[i][0], {
        fontSize: '10px', letterSpacing: '1.6px', color: S.textMuted, marginBottom: '3px', width: '100%'
      });
      jobRows[KEYS[i][0]] = M.label(row, '', '', {
        fontFamily: S.fontMono, fontSize: KEYS[i][1] + 'px', color: S.text, width: '100%'
      });
    }

    M.make(bodyPanel, 'Panel', '', {
      width: '100%', height: '1px', backgroundColor: S.border, marginTop: '4px', marginBottom: '14px'
    });
    jobStatus = M.label(bodyPanel, '', 'Ready to render', {
      fontFamily: S.fontSerif, fontSize: '15px', color: S.text, marginBottom: '6px', width: '100%'
    });
    jobProblem = M.label(bodyPanel, '', '', { fontSize: '12px', color: S.warn, width: '100%' });
    M.make(bodyPanel, 'Panel', '', { width: '100%', height: 'fill-parent-flow(1.0)' });

    startBtn = M.make(bodyPanel, 'Panel', 'MvmJobStart', {
      width: '100%', minHeight: '52px', borderRadius: '8px',
      backgroundColor: S.soul, border: '1px solid ' + S.soul
    });
    startLabel = M.label(startBtn, '', 'Start capture', {
      fontSize: '14px', letterSpacing: '1.8px', textTransform: 'uppercase', color: '#12140f',
      textShadow: '2px 2px 0px 1.0 #000000',
      horizontalAlign: 'center', verticalAlign: 'middle', width: '100%'
    });
    X.on(startBtn, 'onactivate', function () {
      if (running) { cmd('mirv_filmmaker render stop'); return; }
      // Inert while blocked. The button does not offer to start something it already said cannot
      // start; the reason is two lines above it.
      if (blocked) return;
      cmd('mirv_filmmaker render start');
    });
    return job;
  }

  // THE COLUMN STOPS AT THE DOCK, not at the bottom of the screen.
  //
  // The dock runs the full width from the left edge, so a JOB column that reached the bottom would
  // be underneath it. Ending above it is also what the original design did, and it is the reason the dock
  // gets its whole width back for the transport.
  function placeJob(g, visible) {
    if (!job) return;
    if (!visible) { X.show(job, false); return; }
    // From the TOP of the screen, like CS2's JOB column. Starting at areaY left the header band's
    // strip above it unpainted -- a 44px black notch over the column.
    var top = 0;
    var dockTop = Math.floor((g.screenH || 0) - (g.dockH || 0));
    var h = Math.max(0, dockTop - top);
    M.set(job, {
      position: '0px ' + top + 'px 0px',
      width: Math.floor(g.leftW || JOB_W) + 'px',
      height: h + 'px'
    });
    X.show(job, true);
  }

  // --- the column ------------------------------------------------------------------------------

  function build(parent) {
    wrap = M.make(parent, 'Panel', 'MvmRenderBody', { width: '100%', height: '100%', flowChildren: 'down' });
    if (!wrap) return;
    wrap.visible = false;

    // extraChip: CS2 parity, see DeadlockControlsJs.h's buildColumn -- the "go back to the editor"
    // affordance CS2 pairs with its own RENDER title, now living on the same row here too instead
    // of in the JOB column below.
    // CS2's render header is the title and ONE chip, '✕ Back' to the Camera Editor
    // (CameraEditorRenderJs.h RndBack). '×' rather than '✕': the Deadlock face has Latin-1, not
    // the dingbat block.
    col = X.column(wrap, {
      id: 'MvmRenderColumn', name: 'RENDER', accent: S.soul, backOnly: true,
      extraChip: { label: '× Back', onActivate: function () { cmd('mirv_filmmaker workspace camera'); } },
      // CS2's RANGE / CAMERA / HUD / OUTPUT live directly under the title on the column's one rule,
      // not inside the card below it.
      subTabs: [['range', 'Range'], ['camera', 'Camera'], ['hud', 'HUD'], ['output', 'Output']],
      tabFont: 14, tabLs: 1.6, tabGap: 26,
      onSubTab: function (k, idx) {
        if (card) card.selectGroup(idx);
        if (col) { col.setSubTab(k); col.footer.show(idx === 0); }
      }
    });
    if (!col) return;
    col.setVisible(true);
    // CS2's pinned renderFooter: Set In / Set Out / Clear under a rule, outside the scroller, and
    // only on the Range tab (`renderFooter.visible = (tab === 0)`). Starting a job is the JOB
    // column's button alone -- CS2 has no second start control in this column.
    col.footer.set('', [
      { cap: 'Set In', run: function () { setRangeIn(); } },
      { cap: 'Set Out', run: function () { setRangeOut(); } },
      { cap: 'Clear', run: function () { cmd('mirv_filmmaker render range 0 0'); } }
    ]);

    function optPick(group) {
      return function (k) { cmd('mirv_filmmaker render ' + group + ' ' + k); };
    }

    card = X.card(col.cards, {
      key: 'render', title: '', edge: S.soul, always: true, flat: true, hideTabs: true,
      onGroup: function (i) { if (col) col.footer.show(i === 0); },
      groups: [
        { head: 'Range', rows: [
          X.cap('What to render'),
          X.opt('range', 'all', 'Whole Replay', 'The whole replay, start to finish.', { onPick: optPick('range') }),
          X.opt('range', 'shot', 'Camera Path', 'First marker to the last.', { onPick: optPick('range') }),
          X.opt('range', 'segment', 'Selected Segment', 'Selected marker to the next one.', { onPick: optPick('range') }),
          X.opt('range', 'loop', 'Playback Loop', 'The loop set on the replay bar.', { onPick: optPick('range') }),
          X.opt('range', 'custom', 'Custom In/Out', 'Your own in / out, set from the playhead.', { onPick: optPick('range') }),
          X.big('In  -   →   Out  -', 'bigRange'),
          // Set In / Set Out / Clear are the column's pinned footer (above), as in CS2.
          // Deadlock's own semantics, verified against Render_RunRenderFrame: endTick is INCLUSIVE
          // (the frame sampled AT that tick is still written before the job stops), unlike CS2's
          // exclusive Out. Copying CS2's "first frame NOT rendered" wording here would be wrong.
          X.note('Out is inclusive: that tick is the last frame rendered.')
        ] },
        { head: 'Camera', rows: [
          X.cap('Camera source'),
          X.opt('camSrc', 'current', 'Current Viewer', 'Whatever is on screen right now. Nothing is switched.', { onPick: optPick('camsrc') }),
          X.opt('camSrc', 'free', 'Free Camera', 'Switches to the free fly camera.', { onPick: optPick('camsrc') }),
          X.opt('camSrc', 'path', 'Camera Path', 'Plays the authored path. Needs 2+ markers.', { onPick: optPick('camsrc') }),
          X.opt('camSrc', 'follow', 'Follow Camera', 'Tracks the follow target.', { onPick: optPick('camsrc') })
        ] },
        { head: 'HUD', rows: [
          X.cap('Game HUD'),
          X.opt('hud', 'none', 'No HUD', 'Clean plate — nothing but the world.', { onPick: optPick('hud') }),
          X.opt('hud', 'game', 'In-game', 'What the game draws while spectating.', { onPick: optPick('hud') }),
          X.opt('hud', 'full', 'Full HUD', 'Everything, scoreboard included.', { onPick: optPick('hud') }),
          X.cap('Subjects'),
          X.sw('renderNames', 'Hero names and health', {
            tip: 'The floating nameplates and bars above heroes.',
            onToggle: function (v) { cmd('mirv_filmmaker render names ' + (v ? 1 : 0)); }
          }),
          X.sw('renderDamage', 'Damage numbers', {
            tip: 'The floating numbers on hits.',
            onToggle: function (v) { cmd('mirv_filmmaker render damage ' + (v ? 1 : 0)); }
          })
        ] },
        { head: 'Output', rows: [
          X.cap('Output folder'),
          X.act('', [{ cap: 'Choose folder…', run: function () { cmd('mirv_filmmaker render folder'); } }], 1),
          X.note('…', 'dirNote'),
          X.drop('format', 'Format', {
            onOpen: function (anchor) {
              X.openMenu(anchor, {
                options: FORMATS,
                note: 'EXR HDR requires a pre-tonemap capture source and is not available yet.'
              }, (last && last.render && last.render.format) || 'png',
                 function (k) { cmd('mirv_filmmaker render format ' + k); },
                 function () { if (card.w.format) card.w.format.setOpen(false); });
              if (card.w.format) card.w.format.setOpen(true);
            }
          }),
          X.drop('fps', 'Frame rate', {
            onOpen: function (anchor) {
              X.openMenu(anchor, {
                options: RATES,
                note: 'Recording FPS. Render sets capture speed automatically — no timeline dial needed.'
              }, String((last && last.render && last.render.fps) || 64),
                 function (k) { cmd('mirv_filmmaker render fps ' + k); },
                 function () { if (card.w.fps) card.w.fps.setOpen(false); });
              if (card.w.fps) card.w.fps.setOpen(true);
            }
          }),
          X.number('customFps', 'Custom frame rate (1–1000 fps)', {min:1,max:1000,onChange:function(v){cmd('mirv_filmmaker render fps '+v);}}),
          X.note('…', 'captureNote'),
          X.grid('resolution', 'Resolution', [['0','Source'],['720','720p'],['1080','1080p'],['1440','1440p'],['2160','4K']], 3, {onPick:function(k){var h=Number(k), aspect=(last.viewportW||1920)/(last.viewportH||1080);cmd('mirv_filmmaker render size '+(h ? Math.round(h*aspect/2)*2 : 0)+' '+h);}}),
          X.number('outputWidth', 'Width (0 = source)', {min:0,max:16384,onChange:function(v){cmd('mirv_filmmaker render size '+v+' '+((last.render||{}).height||0));}}),
          X.number('outputHeight', 'Height (0 = source)', {min:0,max:16384,onChange:function(v){cmd('mirv_filmmaker render size '+((last.render||{}).width||0)+' '+v);}}),
          X.note('Output dimensions are limited to the source. MP4 uses even dimensions. Aspect ratio is preserved.'),
          X.grid('qualityPreset', 'MP4 quality', [['14','Highest'],['18','High'],['23','Medium'],['28','Small file']], 2, {onPick:optPick('quality')}),
          X.number('quality', 'MP4 quality / CRF (lower is better)', {min:0,max:51,onChange:function(v){cmd('mirv_filmmaker render quality '+v);}}),
          X.number('bitrate', 'MP4 bitrate (kbps; 0 = quality mode)', {min:0,max:400000,onChange:function(v){cmd('mirv_filmmaker render bitrate '+v);}}),
          X.mode('encoder', 'MP4 encoder', [['auto','Auto'],['cpu','CPU'],['gpu','GPU']], {onPick:optPick('encoder')}),
          X.note('Auto currently uses CPU. GPU requires NVIDIA NVENC.'),
          X.mode('encodeSpeed', 'Encode speed', [['quality','Best'],['balanced','Balanced'],['fast','Fastest']], {onPick:optPick('speed')}),
          X.cap('While capturing'),
          X.sw('renderPause', 'Pause on drop', {
            tip: 'Stops the capture if a frame is dropped rather than writing a gap.',
            onToggle: function (v) { cmd('mirv_filmmaker render pauseondrop ' + (v ? 1 : 0)); }
          }),
          X.sw('renderBeep', 'Sound when done', {
            tip: 'A single tone when the last frame is written.',
            onToggle: function (v) { cmd('mirv_filmmaker render beep ' + (v ? 1 : 0)); }
          })
        ] }
      ]
    }, S.soul);
  }

  function setVisible(v) {
    if (wrap) { try { wrap.visible = !!v; } catch (e) {} }
    if (!v) { X.closeMenu(); X.show(job, false); }
  }
)CFJS" R"CFJS(
  // --- render ----------------------------------------------------------------------------------

  function clock(ticks) {
    var sec = ticks / 64;
    var m = Math.floor(sec / 60), s2 = sec - m * 60;
    return (m > 0 ? (m + 'm ') : '') + s2.toFixed(1) + 's';
  }

  function render(st) {
    if (!col || !card) return;
    last = st;
    col.setMouseMode(!!st.uiMouse);

    var r = st.render || {};
    var w = card.w;
    running = !!r.running;
    blocked = blockedReason(st);

    // Options: selection and the blocked reason, both from the same derivation.
    function setGroup(group, current, blocks) {
      for (var key in blocks) {
        var wid = w['opt:' + group + ':' + key];
        if (!wid) continue;
        wid.set(key === current);
        wid.setBlocked(blocks[key]);
      }
    }
    setGroup('range', r.range || 'all', {
      all: '', shot: noPath(st), segment: noSegment(st), loop: '', custom: ''
    });
    setGroup('camSrc', r.camSrc || 'current', {
      current: '', free: '', path: noPath(st), follow: noFollow(st)
    });
    setGroup('hud', r.hud || 'game', { none: '', game: '', full: '' });

    if (w.renderNames) w.renderNames.set(!!r.names);
    if (w.renderDamage) w.renderDamage.set(!!r.damage);
    if (w.renderPause) w.renderPause.set(!!r.pauseOnDrop);
    if (w.renderBeep) w.renderBeep.set(!!r.beep);
    if (w.quality) w.quality.set(typeof r.quality === 'number' ? r.quality : 18);
    if (w.format) w.format.set(FORMAT_NAMES[r.format || 'png'] || 'PNG sequence');
    if (w.customFps) w.customFps.set(r.fps || 32);
    if (w.outputWidth) w.outputWidth.set(r.width || 0);
    if (w.outputHeight) w.outputHeight.set(r.height || 0);
    if (w.bitrate) w.bitrate.set(r.bitrateKbps || 0);
    if (w.encoder) w.encoder.set(r.encoder || 'auto');
    if (w.encodeSpeed) w.encodeSpeed.set(r.speed || 'balanced');
    if (w.fps) w.fps.set(((r.fps || 64) + ' fps'));

    var inTick = (typeof r.startTick === 'number') ? r.startTick : (st.startTick || 0);
    var outTick = (typeof r.endTick === 'number') ? r.endTick : (st.endTick || 0);
    var span = Math.max(0, outTick - inTick);

    // The range readout in the Range group and the folder note in Output.
    if (w.bigRange) w.bigRange.set('In  ' + inTick + '   →   Out  ' + outTick);
    if (w.dirNote) w.dirNote.set(r.folderPending ? 'Choosing folder…' : (r.dir || 'Videos/SourceMVM/Deadlock'));

    // --- the JOB column
    // fps drives both mux and host_framerate; dial is preview-only.
    var fps = r.fps || 64;
    var captureFps = (r.captureFps > 0) ? r.captureFps : fps;
    if (w.captureNote) {
      w.captureNote.set('Capture at ' + captureFps + ' fps — set automatically when you render');
    }
    X.txt(jobRows.RANGE, (RANGE_NAMES[r.range || 'all'] || '-') + '  ·  ' + inTick + ' → ' + outTick);
    X.txt(jobRows.LENGTH, clock(span) + ' @ 64 tick');
    var frameCount = Math.floor(span / 64 * captureFps + 0.0000001) + 1;
    X.txt(jobRows.FRAMES, frameCount + ' frames @ ' + fps + ' fps');
    X.txt(jobRows.CAMERA, CAM_NAMES[r.camSrc || 'current'] || '-');
    X.txt(jobRows.HUD, HUD_NAMES[r.hud || 'game'] || '-');
    X.txt(jobRows.OUTPUT, (FORMAT_NAMES[r.format || 'png'] || '-') + '  ·  ' +
      (st.viewportW || 0) + '×' + (st.viewportH || 0));
    X.txt(jobRows.FOLDER, r.dir || '…/citadel/mvm');
    if (r.range === 'all') {
      X.txt(jobRows.RANGE, 'Whole replay · Start → End');
      X.txt(jobRows.LENGTH, 'Full replay');
      X.txt(jobRows.FRAMES, 'Until replay ends @ ' + fps + ' fps');
    }
    X.txt(jobRows.OUTPUT, (FORMAT_NAMES[r.format || 'png'] || '-') + ' · ' +
      Math.min(r.width || st.viewportW || 0, st.viewportW || 0) + '×' + Math.min(r.height || st.viewportH || 0, st.viewportH || 0));

    // The live "is a job running" badge CS2 paints on its RENDER title (renderTitle.text =
    // jobActive ? 'RENDER · RUNNING' : 'RENDER') lives on the JOB column's own title here instead --
    // this file cannot reach into the shared column shell's title label (DeadlockControlsJs.h is out
    // of scope), but it fully owns this one. S.blue mirrors CS2's own running-state blue (4e9fff).
    if (jobTitle) {
      X.txt(jobTitle, running ? 'JOB · RENDERING' : 'JOB');
      M.set(jobTitle, { color: running ? S.blue : S.soul });
    }

    // Every DISTINCT RenderState gets its own headline, not just a running/not-running split --
    // Arming (waiting for the playhead), Capturing and Finishing used to all read as the same
    // "Capturing — frame N" text, and Failed was not shown at all: blockedReason() is empty once
    // AllocateTakeDir's failure has already happened, so the status silently fell through to
    // "Ready to render" as if nothing went wrong. DeadlockRender.cpp stores no failure STRING
    // anywhere (only the Warning it logs to the console), so the failed line says that honestly
    // rather than inventing a specific reason CS2's pg.lastError would have and Deadlock does not.
    var state = r.state || 'Idle';
    var problemText = '';
    if (state === 'Arming') {
      X.txt(jobStatus, 'ARMING');
      M.set(jobStatus, { color: S.text });
      problemText = 'Waiting for the playhead to reach the start tick.';
    } else if (state === 'Capturing') {
      X.txt(jobStatus, 'CAPTURING  ·  frame ' + (r.frames || 0));
      M.set(jobStatus, { color: S.soul });
      if ((r.dropped || 0) > 0) problemText = r.dropped + ' frame(s) dropped.';
    } else if (state === 'Finishing') {
      X.txt(jobStatus, 'FINISHING');
      M.set(jobStatus, { color: S.soul });
      problemText = 'Flushing the last queued frames.';
    } else if (state === 'Failed') {
      X.txt(jobStatus, 'Render FAILED');
      M.set(jobStatus, { color: S.danger });
      problemText = st.renderError || 'Capture failed — see the console and encoder.log in the take folder.';
    } else if (blocked) {
      X.txt(jobStatus, 'Cannot render yet');
      M.set(jobStatus, { color: S.text });
      problemText = blocked;
    } else {
      X.txt(jobStatus, 'Ready to render');
      M.set(jobStatus, { color: S.text });
      if ((r.frames || 0) > 0) {
        problemText = 'Last render: ' + r.frames + ' frame(s) written'
          + (((r.dropped || 0) > 0) ? (', ' + r.dropped + ' dropped') : '') + '.';
      }
    }
    X.txt(jobProblem, problemText);
    X.show(jobProblem, !!problemText);

    // The button carries the state in its colour: soul when it will start, red while it is running,
    // and dead grey while it cannot.
    X.txt(startLabel, running ? 'Cancel capture' : 'Start capture');
    if (running) {
      M.set(startBtn, { backgroundColor: 'rgba(90,42,42,0.8)', border: '1px solid ' + S.danger });
      M.set(startLabel, { color: '#ffb3b3' });
    } else if (blocked) {
      M.set(startBtn, { backgroundColor: '#26251f', border: '1px solid ' + S.border });
      M.set(startLabel, { color: X.C.chevron });
    } else {
      M.set(startBtn, { backgroundColor: S.soul, border: '1px solid ' + S.soul });
      M.set(startLabel, { color: '#12140f' });
    }
  }

  $.MvmRenderBody = {
    build: build, buildJob: buildJob, placeJob: placeJob,
    setVisible: setVisible, render: render, JOB_W: JOB_W
  };
})();
)CFJS"
