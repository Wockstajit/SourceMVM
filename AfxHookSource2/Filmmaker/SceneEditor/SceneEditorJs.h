#pragma once

// Main Scene Editor Panorama UI shell: Top Bar + Right Inspector Sidebar (Outliner, Place Animation,
// Place Model, Transform, Delay & Cue Inspector). Strict ES5 only.

namespace Filmmaker {

inline const char *const kSceneEditorJsHeader = R"SEJS(
(function () {
  try {
    var existing = $('#SceneEditorRoot'); if (existing) existing.DeleteAsync(0);
    var ctx = $.GetContextPanel();

    var S = {
      accent: '#f0b323ff', accentDim: '#f0b32333',
      blue: '#80bdffff', green: '#6eebafff', red: '#ff5a5aff',
      bg: '#0e1116f0', bgCard: '#151a22ff', bgTile: '#1b212bff',
      border: '#ffffff1f', value: '#eef2f6ff', label: '#c8d3dfff',
      muted: '#9aa4b2ff', dim: '#6b7480ff', clear: 'rgba(0,0,0,0)',
      font: 'Stratum2, "Arial Unicode MS"',
      fontMonodigit: 'Stratum2 Bold Monodigit, Stratum2, "Arial Unicode MS"'
    };
    var TOPBAR_H = 44, SIDEBAR_W = 368;
    var st = null, animCatalog = null, animAsked = false;

    function cmd(c) {
      try { GameInterfaceAPI.ConsoleCommand(c); }
      catch (e) { $.Msg('[sceneeditor] cmd err: ' + e + '\n'); }
    }
    function mk(type, parent, id) {
      var p = $.CreatePanel(type, parent, id || '', {});
      return p;
    }
    function lbl(parent, text, color, size) {
      var l = mk('Label', parent); l.text = text || '';
      if (color) l.style.color = color;
      if (size) l.style.fontSize = size + 'px';
      l.style.fontFamily = S.font;
      return l;
    }
    function T(s) { return s; }

    var root = $.CreatePanel('Panel', ctx, 'SceneEditorRoot', {});
    root.hittest = false; root.visible = true;
    root.style.width = '100%'; root.style.height = '100%'; root.style.zIndex = '64';

    var textFocus = ''; // '', 'animSearch', 'propSearch'
    function setTextFocus(which) {
      textFocus = which || '';
      root.SetAttributeString('textcapture', textFocus ? '1' : '0');
    }
    function modalBlocking() { return false; }

    // ---- Top Header Bar ----
    var topBar = mk('Panel', root); topBar.hittest = true;
    topBar.style.backgroundColor = '#0c0f14f5';
    topBar.style.borderBottom = '1px solid #ffffff22';
    topBar.style.flowChildren = 'right';
    topBar.style.paddingLeft = '14px'; topBar.style.paddingRight = '12px';
    topBar.SetPanelEvent('onactivate', function () {});

    var mark = mk('Panel', topBar); mark.hittest = false;
    mark.style.flowChildren = 'right'; mark.style.verticalAlign = 'center'; mark.style.marginRight = '14px';
    var markRule = mk('Panel', mark); markRule.hittest = false;
    markRule.style.width = '3px'; markRule.style.height = '16px';
    markRule.style.backgroundColor = S.accent; markRule.style.marginRight = '8px';
    var markLbl = lbl(mark, 'SCENE EDITOR', S.value, 13); markLbl.hittest = false;
    markLbl.style.fontWeight = 'bold'; markLbl.style.letterSpacing = '2px';

    function topBtn(caption, accent, onClick) {
      var b = mk('Panel', topBar); b.hittest = true;
      b.style.verticalAlign = 'center'; b.style.marginRight = '6px';
      b.style.padding = '5px 11px'; b.style.borderRadius = '3px';
      b.style.backgroundColor = '#ffffff14'; b.style.border = '1px solid #ffffff1f';
      var l = lbl(b, caption, S.label, 12); l.hittest = false; l.style.fontWeight = 'bold';
      b.SetPanelEvent('onactivate', onClick);
      return { p: b, l: l, accent: accent || S.accent };
    }
    function paintTopBtn(b, active, caption) {
      if (caption !== undefined && b.l.text !== caption) b.l.text = caption;
      var a = b.accent.substring(0, 7);
      b.p.style.backgroundColor = active ? (a + '33') : '#ffffff14';
      b.p.style.border = '1px solid ' + (active ? b.accent : '#ffffff1f');
      b.l.style.color = active ? b.accent : S.label;
    }

    var tbRec = topBtn('● Record Take', S.red, function () { cmd('mirv_filmmaker sceneeditor record'); });
    var tbPlay = topBtn('▶ Play', S.green, function () {
      cmd((st && st.playing) ? 'mirv_filmmaker sceneeditor stop' : 'mirv_filmmaker sceneeditor play');
    });
    var tbLoop = topBtn('Loop', S.blue, function () {
      cmd('mirv_filmmaker sceneeditor loop ' + (st && st.loop ? 'off' : 'on'));
    });
    var tbFly = topBtn('Fly Cam', S.accent, function () { cmd('mirv_filmmaker sceneeditor flycam toggle'); });
    var tbMove = topBtn('Move', S.accent, function () { cmd('mirv_filmmaker sceneeditor mode move'); });
    var tbRot = topBtn('Rotate', S.accent, function () { cmd('mirv_filmmaker sceneeditor mode rotate'); });
    var tbSnap = topBtn('Snap Floor', S.green, function () {
      if (st && st.selKind === 'track' && st.selTrack >= 0) cmd('mirv_filmmaker sceneeditor snapfloor ' + st.selTrack);
      else if (st && st.selKind === 'prop' && st.selProp > 0) cmd('mirv_filmmaker props floor ' + st.selProp);
    });
    var tbSave = topBtn('Save', S.blue, function () { cmd('mirv_filmmaker takes save scene'); });
    var tbLoad = topBtn('Load', S.blue, function () { cmd('mirv_filmmaker takes load scene'); });
    var tbSp = mk('Panel', topBar); tbSp.hittest = false; tbSp.style.width = 'fill-parent-flow(1.0)';
    var tbClose = topBtn('Close', S.red, function () { cmd('mirv_filmmaker sceneeditor off'); });
)SEJS";

inline const char *const kSceneEditorJsBody = R"SEJS(
    // ---- Right Inspector Sidebar ----
    var side = mk('Panel', root); side.hittest = true;
    side.style.backgroundColor = S.bg;
    side.style.borderLeft = '1px solid #ffffff22';
    side.style.flowChildren = 'down';
    side.style.overflow = 'clip';
    side.SetPanelEvent('onactivate', function () {});

    var sideInner = mk('Panel', side); sideInner.hittest = false; sideInner.hittestchildren = true;
    sideInner.style.width = '100%'; sideInner.style.flowChildren = 'down';
    sideInner.style.padding = '10px 12px';
    var sideScrollY = 0;

    function mkCard(title, openDefault) {
      var card = mk('Panel', sideInner); card.hittest = true;
      card.style.width = '100%'; card.style.backgroundColor = S.bgCard;
      card.style.border = '1px solid ' + S.border; card.style.borderRadius = '4px';
      card.style.marginBottom = '8px'; card.style.flowChildren = 'down';
      var hdr = mk('Panel', card); hdr.hittest = true;
      hdr.style.width = '100%'; hdr.style.padding = '7px 10px';
      hdr.style.flowChildren = 'right'; hdr.style.backgroundColor = '#ffffff08';
      var t = lbl(hdr, title, S.value, 12); t.hittest = false; t.style.fontWeight = 'bold'; t.style.width = 'fill-parent-flow(1.0)';
      var note = lbl(hdr, '', S.muted, 11); note.hittest = false; note.style.marginRight = '8px';
      var caret = lbl(hdr, openDefault ? '▾' : '▸', S.muted, 12); caret.hittest = false;
      var body = mk('Panel', card); body.hittest = false; body.hittestchildren = true;
      body.style.width = '100%'; body.style.flowChildren = 'down'; body.style.padding = '8px 10px';
      body.visible = !!openDefault;
      hdr.SetPanelEvent('onactivate', function () {
        body.visible = !body.visible;
        caret.text = body.visible ? '▾' : '▸';
      });
      return { card: card, body: body, note: note };
    }
    function smallBtn(parent, cap, col, onClick) {
      var b = mk('Panel', parent); b.hittest = true;
      b.style.padding = '4px 8px'; b.style.borderRadius = '3px';
      b.style.backgroundColor = '#ffffff12'; b.style.border = '1px solid #ffffff1a';
      b.style.marginRight = '5px'; b.style.marginBottom = '4px';
      var l = lbl(b, cap, col || S.label, 11); l.hittest = false; l.style.fontWeight = 'bold';
      b.SetPanelEvent('onactivate', onClick);
      return { p: b, l: l };
    }
    function nudgeRow(parent, labelTxt, decCmdFn, incCmdFn) {
      var r = mk('Panel', parent); r.hittest = false; r.hittestchildren = true;
      r.style.width = '100%'; r.style.flowChildren = 'right'; r.style.marginBottom = '5px';
      var lb = lbl(r, labelTxt, S.label, 12); lb.hittest = false;
      lb.style.width = '68px'; lb.style.verticalAlign = 'center';
      var valLbl = lbl(r, '0.0', S.value, 12); valLbl.hittest = false;
      valLbl.style.width = 'fill-parent-flow(1.0)'; valLbl.style.verticalAlign = 'center';
      valLbl.style.fontFamily = S.fontMonodigit;
      smallBtn(r, '-10', S.label, function () { decCmdFn(10); });
      smallBtn(r, '-1', S.label, function () { decCmdFn(1); });
      smallBtn(r, '+1', S.label, function () { incCmdFn(1); });
      smallBtn(r, '+10', S.label, function () { incCmdFn(10); });
      return valLbl;
    }

    // 1. Outliner Card (Tracks & Props)
    var secOut = mkCard('OUTLINER · ACTORS & MODELS', true);
    var outAddRow = mk('Panel', secOut.body); outAddRow.hittest = false; outAddRow.hittestchildren = true;
    outAddRow.style.width = '100%'; outAddRow.style.flowChildren = 'right'; outAddRow.style.marginBottom = '6px';
    smallBtn(outAddRow, '+ Record', S.red, function () { cmd('mirv_filmmaker sceneeditor record'); });
    smallBtn(outAddRow, '+ Actor', S.green, function () { cmd('mirv_filmmaker sceneeditor placeactor hla_combat_idle'); });
    smallBtn(outAddRow, '+ Model', S.blue, function () { secMod.body.visible = true; setTextFocus('propSearch'); });

    var outRows = [];
    for (var oi = 0; oi < 14; oi++) {
      var orow = mk('Panel', secOut.body); orow.hittest = true; orow.visible = false;
      orow.style.width = '100%'; orow.style.flowChildren = 'right';
      orow.style.padding = '4px 6px'; orow.style.borderRadius = '3px'; orow.style.marginBottom = '2px';
      var odot = mk('Panel', orow); odot.hittest = false;
      odot.style.width = '8px'; odot.style.height = '8px'; odot.style.borderRadius = '4px';
      odot.style.verticalAlign = 'center'; odot.style.marginRight = '6px';
      var oname = lbl(orow, '', S.value, 12); oname.hittest = false; oname.style.width = 'fill-parent-flow(1.0)';
      var odel = lbl(orow, '×', S.red, 13); odel.hittest = true; odel.style.padding = '0px 5px';
      outRows.push({ row: orow, dot: odot, name: oname, del: odel });
    }

    // 2. Selected Transform & Timing Card
    var secTr = mkCard('SELECTED TRANSFORM & CUES', true);
    var trTitle = lbl(secTr.body, 'Select a track or placed model', S.muted, 12);
    trTitle.hittest = false; trTitle.style.marginBottom = '6px';
    var trWrap = mk('Panel', secTr.body); trWrap.hittest = false; trWrap.hittestchildren = true;
    trWrap.style.width = '100%'; trWrap.style.flowChildren = 'down'; trWrap.visible = false;

    function nudgeSelectedTrack(axisIdx, delta) {
      if (!st || st.selKind !== 'track' || !st.tracks || !st.tracks[st.selTrack]) return;
      var tk = st.tracks[st.selTrack];
      var op = tk.offsetPos || [0, 0, 0];
      var nx = op[0] + (axisIdx === 0 ? delta : 0);
      var ny = op[1] + (axisIdx === 1 ? delta : 0);
      var nz = op[2] + (axisIdx === 2 ? delta : 0);
      var nyaw = (tk.offsetYaw || 0) + (axisIdx === 3 ? delta : 0);
      cmd('mirv_filmmaker sceneeditor offset ' + st.selTrack + ' ' + nx.toFixed(1) + ' ' + ny.toFixed(1) + ' ' + nz.toFixed(1) + ' ' + nyaw.toFixed(1));
    }
    var valX = nudgeRow(trWrap, 'Pos X', function (d) { nudgeSelectedTrack(0, -d); }, function (d) { nudgeSelectedTrack(0, d); });
    var valY = nudgeRow(trWrap, 'Pos Y', function (d) { nudgeSelectedTrack(1, -d); }, function (d) { nudgeSelectedTrack(1, d); });
    var valZ = nudgeRow(trWrap, 'Pos Z', function (d) { nudgeSelectedTrack(2, -d); }, function (d) { nudgeSelectedTrack(2, d); });
    var valYaw = nudgeRow(trWrap, 'Yaw °', function (d) { nudgeSelectedTrack(3, -d); }, function (d) { nudgeSelectedTrack(3, d); });

    var trBtnRow = mk('Panel', trWrap); trBtnRow.hittest = false; trBtnRow.hittestchildren = true;
    trBtnRow.style.width = '100%'; trBtnRow.style.flowChildren = 'right'; trBtnRow.style.marginTop = '4px'; trBtnRow.style.marginBottom = '6px';
    var trBtnEn = smallBtn(trBtnRow, 'Enabled', S.green, function () {
      if (st && st.selKind === 'track' && st.tracks[st.selTrack])
        cmd('mirv_filmmaker sceneeditor enable ' + st.selTrack + ' ' + (st.tracks[st.selTrack].enabled ? 'off' : 'on'));
    });
    var trBtnTeam = smallBtn(trBtnRow, 'Team', S.blue, function () {
      if (st && st.selKind === 'track' && st.tracks[st.selTrack])
        cmd('mirv_filmmaker sceneeditor team ' + st.selTrack + ' ' + (st.tracks[st.selTrack].team === 2 ? 'ct' : 't'));
    });
    smallBtn(trBtnRow, 'Snap Floor', S.green, function () {
      if (st && st.selKind === 'track') cmd('mirv_filmmaker sceneeditor snapfloor ' + st.selTrack);
    });
    smallBtn(trBtnRow, 'Reset Pos', S.muted, function () {
      if (st && st.selKind === 'track') cmd('mirv_filmmaker sceneeditor resetoffset ' + st.selTrack);
    });

    var trDelayLbl = nudgeRow(trWrap, 'Delay (f)',
      function (d) { if (st && st.tracks[st.selTrack]) cmd('mirv_filmmaker sceneeditor delay ' + st.selTrack + ' ' + Math.max(0, st.tracks[st.selTrack].delayTicks - d)); },
      function (d) { if (st && st.tracks[st.selTrack]) cmd('mirv_filmmaker sceneeditor delay ' + st.selTrack + ' ' + (st.tracks[st.selTrack].delayTicks + d)); });

    // Cues on selected track
    var cueHdr = lbl(trWrap, 'Animation Cues on Track:', S.muted, 11); cueHdr.hittest = false; cueHdr.style.marginTop = '4px';
    var cueRows = [];
    for (var qi = 0; qi < 5; qi++) (function (cIdx) {
      var cr = mk('Panel', trWrap); cr.hittest = false; cr.hittestchildren = true; cr.visible = false;
      cr.style.width = '100%'; cr.style.flowChildren = 'right'; cr.style.marginTop = '3px';
      cr.style.padding = '3px 6px'; cr.style.backgroundColor = '#ffffff0a'; cr.style.borderRadius = '3px';
      var cl = lbl(cr, '', S.value, 11); cl.hittest = false; cl.style.width = 'fill-parent-flow(1.0)';
      smallBtn(cr, 'Loop', S.blue, function () {
        var tk = st && st.tracks && st.tracks[st.selTrack];
        var cq = tk && tk.cues && tk.cues[cIdx];
        if (cq) cmd('mirv_filmmaker sceneeditor cue edit ' + st.selTrack + ' ' + cIdx + ' ' + cq.speed + ' ' + (cq.loop ? 0 : 1) + ' ' + (cq.upper ? 1 : 0) + ' ' + cq.blendIn + ' ' + cq.blendOut);
      });
      smallBtn(cr, '×', S.red, function () {
        if (st && st.selKind === 'track') cmd('mirv_filmmaker sceneeditor cue remove ' + st.selTrack + ' ' + cIdx);
      });
      cueRows.push({ row: cr, lbl: cl });
    })(qi);

    // 3. Place Animation Card
    var secAnim = mkCard('PLACE HL:A ANIMATION', false);
    var animQuery = '';
    var animField = mk('Panel', secAnim.body); animField.hittest = true;
    animField.style.width = '100%'; animField.style.height = '28px'; animField.style.padding = '0px 8px';
    animField.style.backgroundColor = S.bgTile; animField.style.borderRadius = '3px'; animField.style.border = '1px solid ' + S.border;
    var animFieldLbl = lbl(animField, 'Search animations...', S.muted, 12); animFieldLbl.hittest = false; animFieldLbl.style.verticalAlign = 'center';
    animField.SetPanelEvent('onactivate', function () { setTextFocus(textFocus === 'animSearch' ? '' : 'animSearch'); });

    var animResults = [];
    for (var ari = 0; ari < 7; ari++) {
      var ar = mk('Panel', secAnim.body); ar.hittest = true; ar.visible = false;
      ar.style.width = '100%'; ar.style.flowChildren = 'right'; ar.style.padding = '4px 6px';
      ar.style.marginTop = '3px'; ar.style.backgroundColor = '#ffffff08'; ar.style.borderRadius = '3px';
      var al = lbl(ar, '', S.value, 11); al.hittest = false; al.style.width = 'fill-parent-flow(1.0)';
      var bPlace = smallBtn(ar, 'Place Actor', S.green, function () {});
      var bCue = smallBtn(ar, '+ Cue', S.accent, function () {});
      animResults.push({ row: ar, lbl: al, placeBtn: bPlace, cueBtn: bCue });
    }
    function refreshAnimResults() {
      if (!animCatalog || !animCatalog.length) return;
      var q = animQuery.toLowerCase();
      var shown = 0;
      for (var i = 0; i < animCatalog.length && shown < animResults.length; i++) {
        var c = animCatalog[i];
        if (q && c[0].toLowerCase().indexOf(q) < 0 && c[1].toLowerCase().indexOf(q) < 0) continue;
        (function (clipId, disp, rowObj) {
          rowObj.row.visible = true;
          rowObj.lbl.text = disp;
          rowObj.placeBtn.p.SetPanelEvent('onactivate', function () { cmd('mirv_filmmaker sceneeditor placeactor ' + clipId); });
          rowObj.cueBtn.p.SetPanelEvent('onactivate', function () {
            if (st && st.selKind === 'track' && st.selTrack >= 0)
              cmd('mirv_filmmaker sceneeditor cue add ' + st.selTrack + ' ' + clipId);
          });
        })(c[0], c[1], animResults[shown++]);
      }
      for (; shown < animResults.length; shown++) animResults[shown].row.visible = false;
    }

    // 4. Place Model / Prop Card
    var secMod = mkCard('PLACE MODEL / PROP', false);
    var propQuery = '';
    var propField = mk('Panel', secMod.body); propField.hittest = true;
    propField.style.width = '100%'; propField.style.height = '28px'; propField.style.padding = '0px 8px';
    propField.style.backgroundColor = S.bgTile; propField.style.borderRadius = '3px'; propField.style.border = '1px solid ' + S.border;
    var propFieldLbl = lbl(propField, 'Search CS2 props: crate, barrel, fence...', S.muted, 12); propFieldLbl.hittest = false; propFieldLbl.style.verticalAlign = 'center';
    propField.SetPanelEvent('onactivate', function () { setTextFocus(textFocus === 'propSearch' ? '' : 'propSearch'); });
    var propSolidRow = mk('Panel', secMod.body); propSolidRow.hittest = false; propSolidRow.hittestchildren = true;
    propSolidRow.style.width = '100%'; propSolidRow.style.flowChildren = 'right'; propSolidRow.style.marginTop = '5px';
    var btnMakeSolid = smallBtn(propSolidRow, 'Make Selected Solid', S.green, function () {
      if (st && st.props && st.props.selected) cmd('mirv_filmmaker props set ' + st.props.selected + ' solid 1');
    });
    var btnPropDel = smallBtn(propSolidRow, 'Delete Prop', S.red, function () {
      if (st && st.props && st.props.selected) cmd('mirv_filmmaker props remove ' + st.props.selected);
    });

    var propResults = [];
    for (var pri = 0; pri < 7; pri++) {
      var pr = mk('Panel', secMod.body); pr.hittest = true; pr.visible = false;
      pr.style.width = '100%'; pr.style.flowChildren = 'right'; pr.style.padding = '4px 6px';
      pr.style.marginTop = '3px'; pr.style.backgroundColor = '#ffffff08'; pr.style.borderRadius = '3px';
      var pl = lbl(pr, '', S.value, 11); pl.hittest = false; pl.style.width = 'fill-parent-flow(1.0)';
      var pPlace = smallBtn(pr, 'Place', S.blue, function () {});
      propResults.push({ row: pr, lbl: pl, btn: pPlace });
    }
)SEJS";

inline const char *const kSceneEditorJsFooter = R"SEJS(
    function syncSidebar(s) {
      var tracks = s.tracks || [];
      var placedProps = (s.props && s.props.placed) || [];
      secOut.note.text = tracks.length + ' tracks · ' + placedProps.length + ' props';

      var ri = 0;
      for (var ti = 0; ti < tracks.length && ri < outRows.length; ti++) (function (idx, tk, row) {
        row.row.visible = true;
        var sel = (s.selKind === 'track' && s.selTrack === idx);
        row.row.style.backgroundColor = sel ? '#f0b32324' : '#ffffff06';
        row.dot.style.backgroundColor = tk.placedActor ? S.green : (tk.team === 2 ? '#ffb044ff' : '#60c8ffff');
        row.name.text = tk.name + ' (' + (tk.placedActor ? 'Actor' : (tk.team === 2 ? 'T' : 'CT')) + ')';
        row.name.style.color = sel ? '#ffffffff' : S.value;
        row.row.SetPanelEvent('onactivate', function () { cmd('mirv_filmmaker sceneeditor select track ' + idx); });
        row.del.SetPanelEvent('onactivate', function () { cmd('mirv_filmmaker sceneeditor delete ' + idx); });
      })(ti, tracks[ti], outRows[ri++]);

      for (var pi = 0; pi < placedProps.length && ri < outRows.length; pi++) (function (prp, row) {
        row.row.visible = true;
        var sel = (s.selKind === 'prop' && s.selProp === prp.id);
        row.row.style.backgroundColor = sel ? '#80bdff24' : '#ffffff06';
        row.dot.style.backgroundColor = prp.solid ? S.green : S.blue;
        row.name.text = prp.name + (prp.solid ? ' [Solid]' : '');
        row.name.style.color = sel ? '#ffffffff' : S.value;
        row.row.SetPanelEvent('onactivate', function () {
          cmd('mirv_filmmaker props select ' + prp.id);
          cmd('mirv_filmmaker sceneeditor select prop ' + prp.id);
        });
        row.del.SetPanelEvent('onactivate', function () { cmd('mirv_filmmaker props remove ' + prp.id); });
      })(placedProps[pi], outRows[ri++]);

      for (; ri < outRows.length; ri++) outRows[ri].row.visible = false;

      var selT = (s.selKind === 'track' && tracks[s.selTrack]) ? tracks[s.selTrack] : null;
      trWrap.visible = !!selT;
      if (selT) {
        trTitle.text = selT.name + (selT.placedActor ? ' · Placed Actor' : ' · Recorded Take');
        trTitle.style.color = S.accent;
        var op = selT.offsetPos || [0, 0, 0];
        valX.text = op[0].toFixed(1) + ' u';
        valY.text = op[1].toFixed(1) + ' u';
        valZ.text = op[2].toFixed(1) + ' u';
        valYaw.text = (selT.offsetYaw || 0).toFixed(1) + '°';
        trDelayLbl.text = selT.delayTicks + ' ticks';
        trBtnEn.l.text = selT.enabled ? 'Enabled' : 'Muted';
        trBtnEn.l.style.color = selT.enabled ? S.green : S.dim;
        trBtnTeam.l.text = selT.team === 2 ? 'Team: T' : 'Team: CT';

        var cues = selT.cues || [];
        for (var ci = 0; ci < cueRows.length; ci++) {
          var cr = cueRows[ci], cq = cues[ci];
          cr.row.visible = !!cq;
          if (cq) cr.lbl.text = (cq.display || cq.clip) + ' @ ' + cq.startTick + 'f';
        }
      } else {
        trTitle.text = s.selKind === 'prop' ? 'Prop selected (use viewport arrows or Model card)' : 'Select a track in Outliner or 3D viewport';
        trTitle.style.color = S.muted;
      }

      // Search fields paint
      animFieldLbl.text = (animQuery + (textFocus === 'animSearch' ? ' |' : '')) || 'Search HL:A animations...';
      animFieldLbl.style.color = animQuery ? S.value : S.muted;
      animField.style.border = '1px solid ' + (textFocus === 'animSearch' ? S.accent : S.border);
      if (secAnim.body.visible) {
        if (!animCatalog && !animAsked) { animAsked = true; cmd('mirv_filmmaker sceneeditor catalog'); }
        refreshAnimResults();
      }

      propFieldLbl.text = (propQuery + (textFocus === 'propSearch' ? ' |' : '')) || 'Search CS2 props: box, crate, rock...';
      propFieldLbl.style.color = propQuery ? S.value : S.muted;
      propField.style.border = '1px solid ' + (textFocus === 'propSearch' ? S.blue : S.border);
      if (secMod.body.visible && s.props) {
        if (s.props.total === -1) cmd('mirv_filmmaker props search "' + propQuery + '" all 12');
        var res = s.props.results || [];
        for (var pri2 = 0; pri2 < propResults.length; pri2++) (function (item, prObj) {
          prObj.row.visible = !!item;
          if (!item) return;
          prObj.lbl.text = item.name || item.model;
          prObj.btn.p.SetPanelEvent('onactivate', function () {
            cmd('mirv_filmmaker props solidnew on');
            cmd('mirv_filmmaker props place ' + item.model);
          });
        })(res[pri2], propResults[pri2]);
      }
    }

    var api = {};
    api.setCatalog = function (arr) { animCatalog = arr || []; refreshAnimResults(); };
    api.wheel = function (notches) {
      sideScrollY = Math.max(0, sideScrollY - notches * 28);
      sideInner.style.transform = 'translateY(-' + sideScrollY + 'px)';
    };
    api.chars = function (csv) {
      if (!textFocus) return;
      var parts = String(csv || '').split(',');
      var changed = false;
      for (var i = 0; i < parts.length; i++) {
        var code = parseInt(parts[i], 10);
        if (code === 13 || code === 27) { setTextFocus(''); return; }
        if (code === 8) {
          if (textFocus === 'animSearch') animQuery = animQuery.slice(0, -1);
          else if (textFocus === 'propSearch') propQuery = propQuery.slice(0, -1);
          changed = true;
        } else if (code >= 32 && code < 127) {
          var ch = String.fromCharCode(code);
          if (textFocus === 'animSearch') animQuery += ch;
          else if (textFocus === 'propSearch') propQuery += ch;
          changed = true;
        }
      }
      if (changed) {
        if (textFocus === 'animSearch') refreshAnimResults();
        else if (textFocus === 'propSearch') cmd('mirv_filmmaker props search "' + propQuery + '" all 12');
      }
    };

    api.restoreHud = seRestoreHudViewport;
    api.render = function () {
      var raw = root.GetAttributeString('state', '');
      if (!raw) { root.visible = false; root.SetAttributeString('previewrect', ''); seRestoreHudViewport(); return; }
      try { st = JSON.parse(raw); } catch (e) { return; }
      root.visible = !!st.enabled;
      if (!st.enabled) { root.SetAttributeString('previewrect', ''); seRestoreHudViewport(); return; }

      var sx = ctx.actualuiscale_x || root.actualuiscale_x || 1;
      var sy = ctx.actualuiscale_y || root.actualuiscale_y || 1;
      var rawW = ctx.actuallayoutwidth || 0, rawH = ctx.actuallayoutheight || 0;
      if (rawW < 16) { rawW = root.actuallayoutwidth || 0; rawH = root.actuallayoutheight || 0; }
      var sw = rawW / sx, sh = rawH / sy;
      if (!(sw > 10 && sh > 10)) return;

      topBar.style.position = '0px 0px 0px'; topBar.style.width = sw + 'px'; topBar.style.height = TOPBAR_H + 'px';
      side.style.position = (sw - SIDEBAR_W) + 'px ' + TOPBAR_H + 'px 0px';
      side.style.width = SIDEBAR_W + 'px';
      side.style.height = Math.max(120, sh - TOPBAR_H - DOCK_H) + 'px';

      paintTopBtn(tbRec, !!st.recording);
      paintTopBtn(tbPlay, !!st.playing, st.playing ? '■ Stop' : '▶ Play');
      paintTopBtn(tbLoop, !!st.loop);
      paintTopBtn(tbFly, !!st.flyCam);
      paintTopBtn(tbMove, st.gizmoMode === 'move');
      paintTopBtn(tbRot, st.gizmoMode === 'rotate');

      renderViewportOverlay(st, sw, sh, sx, sy);
      renderDock(st, sw, sh);
      syncSidebar(st);
    };
    $.SceneEditor = api;
  } catch (e) {
    $.Msg('[sceneeditor] init error: ' + e + '\n');
  }
})();
)SEJS";

} // namespace Filmmaker
