#pragma once

// Bottom Timeline & Curve Graph dock for Scene Editor (styled consistently with GraphEditorJs.h).
// Strict ES5 only. Shares the root IIFE scope of SceneEditorJs.h.

namespace Filmmaker {

inline const char *const kSceneEditorTimelineJsA = R"SEJS(
    // ---- Bottom Timeline + Graph Dock ----
    var DOCK_H = 264, TL_LEFT_W = 240, TL_HEAD_H = 34, TL_RULER_H = 24, TL_ROW_H = 28;
    var dock = mk('Panel', root);
    dock.hittest = true;
    dock.style.backgroundColor = '#10141af2';
    dock.style.borderTop = '1px solid #ffffff22';
    dock.style.flowChildren = 'down';
    dock.SetPanelEvent('onactivate', function () {});

    // Dock Header Bar
    var tlBar = mk('Panel', dock); tlBar.hittest = true;
    tlBar.style.width = '100%'; tlBar.style.height = TL_HEAD_H + 'px';
    tlBar.style.backgroundColor = '#0b0e12ff';
    tlBar.style.borderBottom = '1px solid #ffffff1a';
    tlBar.style.flowChildren = 'right';
    tlBar.style.paddingLeft = '12px'; tlBar.style.paddingRight = '12px';

    function tlTabBtn(caption, onClick) {
      var b = mk('Panel', tlBar); b.hittest = true;
      b.style.verticalAlign = 'center'; b.style.marginRight = '6px';
      b.style.padding = '4px 10px'; b.style.borderRadius = '3px';
      b.style.backgroundColor = '#ffffff12'; b.style.border = '1px solid #ffffff1a';
      var l = lbl(b, caption, S.label, 12); l.hittest = false;
      l.style.fontWeight = 'bold'; l.style.letterSpacing = '1px';
      b.SetPanelEvent('onactivate', onClick);
      return { p: b, l: l };
    }
    var btnViewTl = tlTabBtn('TIMELINE', function () { cmd('mirv_filmmaker sceneeditor view timeline'); });
    var btnViewGr = tlTabBtn('GRAPH', function () { cmd('mirv_filmmaker sceneeditor view graph'); });

    var tlDiv1 = mk('Panel', tlBar); tlDiv1.hittest = false;
    tlDiv1.style.width = '1px'; tlDiv1.style.height = '18px'; tlDiv1.style.verticalAlign = 'center';
    tlDiv1.style.backgroundColor = '#ffffff1f'; tlDiv1.style.margin = '0px 8px';

    var chButtons = {};
    var chDefs = [['z', 'HEIGHT Z'], ['speed', 'SPEED'], ['yaw', 'YAW'], ['anim', 'ANIM WEIGHT']];
    for (var ci = 0; ci < chDefs.length; ci++) (function (key, cap) {
      chButtons[key] = tlTabBtn(cap, function () { cmd('mirv_filmmaker sceneeditor view graph;mirv_filmmaker sceneeditor channel ' + key); });
    })(chDefs[ci][0], chDefs[ci][1]);

    var tlSpacer = mk('Panel', tlBar); tlSpacer.hittest = false; tlSpacer.style.width = 'fill-parent-flow(1.0)';
    var tlReadout = lbl(tlBar, '0:00.0  ·  Tick 0 / 64', S.value, 12);
    tlReadout.hittest = false; tlReadout.style.verticalAlign = 'center';
    tlReadout.style.fontFamily = S.fontMonodigit; tlReadout.style.marginRight = '10px';

    // Dock Body (left labels + right ruler/canvas)
    var tlBody = mk('Panel', dock); tlBody.hittest = true;
    tlBody.style.width = '100%'; tlBody.style.height = 'fill-parent-flow(1.0)';
    tlBody.style.flowChildren = 'right';

    var tlLeft = mk('Panel', tlBody); tlLeft.hittest = true;
    tlLeft.style.width = TL_LEFT_W + 'px'; tlLeft.style.height = '100%';
    tlLeft.style.backgroundColor = '#0d1116ff';
    tlLeft.style.borderRight = '1px solid #ffffff1a';
    tlLeft.style.flowChildren = 'down';
    var tlLeftHdr = mk('Panel', tlLeft); tlLeftHdr.hittest = false;
    tlLeftHdr.style.width = '100%'; tlLeftHdr.style.height = TL_RULER_H + 'px';
    tlLeftHdr.style.borderBottom = '1px solid #ffffff14';
    tlLeftHdr.style.paddingLeft = '12px';
    var tlLeftTitle = lbl(tlLeftHdr, 'SCENE TRACKS', S.muted, 11);
    tlLeftTitle.hittest = false; tlLeftTitle.style.verticalAlign = 'center'; tlLeftTitle.style.fontWeight = 'bold';

    var tlCanvas = mk('Panel', tlBody); tlCanvas.hittest = true;
    tlCanvas.style.width = 'fill-parent-flow(1.0)'; tlCanvas.style.height = '100%';
    tlCanvas.style.backgroundColor = '#0b0e12ff'; tlCanvas.style.overflow = 'clip';

    var tlRuler = mk('Panel', tlCanvas); tlRuler.hittest = true;
    tlRuler.style.width = '100%'; tlRuler.style.height = TL_RULER_H + 'px';
    tlRuler.style.backgroundColor = '#12161dff';
    tlRuler.style.borderBottom = '1px solid #ffffff1a';
    var SE_RULER_TICKS = 16, seRulerMarks = [];
    for (var rmi = 0; rmi < SE_RULER_TICKS; rmi++) {
      var rk = mk('Panel', tlRuler); rk.hittest = false;
      rk.style.width = '1px'; rk.style.height = '8px'; rk.style.backgroundColor = '#ffffff33';
      var rkl = lbl(tlRuler, '', S.muted, 10); rkl.hittest = false;
      rkl.style.fontFamily = S.fontMonodigit;
      seRulerMarks.push({ tick: rk, lbl: rkl });
    }

    // Track lane pools (up to 16 tracks visible at once)
    var SE_MAX_LANES = 12, seLanes = [];
    for (var li = 0; li < SE_MAX_LANES; li++) (function (idx) {
      var lRow = mk('Panel', tlLeft); lRow.hittest = true; lRow.visible = false;
      lRow.style.width = '100%'; lRow.style.height = TL_ROW_H + 'px';
      lRow.style.borderBottom = '1px solid #ffffff0c'; lRow.style.flowChildren = 'right';
      lRow.style.paddingLeft = '10px'; lRow.style.paddingRight = '8px';
      var dot = mk('Panel', lRow); dot.hittest = false;
      dot.style.width = '8px'; dot.style.height = '8px'; dot.style.borderRadius = '4px';
      dot.style.verticalAlign = 'center'; dot.style.marginRight = '8px';
      var nm = lbl(lRow, '', S.value, 12); nm.hittest = false;
      nm.style.verticalAlign = 'center'; nm.style.width = 'fill-parent-flow(1.0)';
      var meta = lbl(lRow, '', S.muted, 11); meta.hittest = false;
      meta.style.verticalAlign = 'center'; meta.style.fontFamily = S.fontMonodigit;
      lRow.SetPanelEvent('onactivate', function () { cmd('mirv_filmmaker sceneeditor select track ' + idx); });

      var laneStrip = mk('Panel', tlCanvas); laneStrip.hittest = false; laneStrip.visible = false;
      laneStrip.style.width = '100%'; laneStrip.style.height = TL_ROW_H + 'px';
      laneStrip.style.borderBottom = '1px solid #ffffff08';
      var bar = mk('Panel', laneStrip); bar.hittest = false;
      bar.style.height = '18px'; bar.style.borderRadius = '3px';
      var barLbl = lbl(bar, '', S.value, 10); barLbl.hittest = false;
      barLbl.style.verticalAlign = 'center'; barLbl.style.marginLeft = '5px';

      var cueBlocks = [];
      for (var ci = 0; ci < 6; ci++) {
        var cb = mk('Panel', laneStrip); cb.hittest = false; cb.visible = false;
        cb.style.height = '12px'; cb.style.borderRadius = '2px';
        cb.style.backgroundColor = '#80bdff88'; cb.style.border = '1px solid #80bdffff';
        var cl = lbl(cb, '', '#ffffffff', 9); cl.hittest = false;
        cl.style.marginLeft = '3px'; cl.style.verticalAlign = 'center';
        cueBlocks.push({ box: cb, lbl: cl });
      }
      seLanes.push({ leftRow: lRow, dot: dot, name: nm, meta: meta, strip: laneStrip, bar: bar, barLbl: barLbl, cues: cueBlocks, rect: null });
    })(li);

    // Graph View Overlay inside tlCanvas (below ruler)
    var grArea = mk('Panel', tlCanvas); grArea.hittest = false; grArea.visible = false;
    grArea.style.width = '100%';
    var grMinLbl = lbl(grArea, '', S.muted, 11); grMinLbl.hittest = false; grMinLbl.style.fontFamily = S.fontMonodigit;
    var grMaxLbl = lbl(grArea, '', S.muted, 11); grMaxLbl.hittest = false; grMaxLbl.style.fontFamily = S.fontMonodigit;
    var grMidLine = mk('Panel', grArea); grMidLine.hittest = false;
    grMidLine.style.width = '100%'; grMidLine.style.height = '1px'; grMidLine.style.backgroundColor = '#ffffff14';
    var SE_MAX_SEGS = 128, grSegs = [];
    for (var gi = 0; gi < SE_MAX_SEGS; gi++) {
      var gs = mk('Panel', grArea); gs.hittest = false; gs.visible = false;
      gs.style.backgroundColor = '#f0b323ff';
      grSegs.push(gs);
    }

    // Playhead line + handle
    var playheadLine = mk('Panel', tlCanvas); playheadLine.hittest = false;
    playheadLine.style.width = '2px'; playheadLine.style.backgroundColor = '#ff5a5aee';
    var playheadCap = mk('Panel', tlRuler); playheadCap.hittest = false;
    playheadCap.style.width = '10px'; playheadCap.style.height = '12px';
    playheadCap.style.backgroundColor = '#ff5a5aff'; playheadCap.style.borderRadius = '2px';

    var tlDragMode = '', tlDragTrack = -1, tlDragCue = -1, tlDragStartTick = 0, tlDragOrigTick = 0, tlLmbWas = false;

    function tickToX(tk, totalTicks, canvasW) {
      return Math.max(0, Math.min(canvasW, (tk / Math.max(1, totalTicks)) * (canvasW - 16) + 8));
    }
    function xToTick(lx, totalTicks, canvasW) {
      return Math.max(0, Math.round(((lx - 8) / Math.max(1, canvasW - 16)) * Math.max(1, totalTicks)));
    }
)SEJS";

inline const char *const kSceneEditorTimelineJsB = R"SEJS(
    function renderDock(s, sw, sh) {
      dock.style.position = '0px ' + (sh - DOCK_H) + 'px 0px';
      dock.style.width = sw + 'px';
      dock.style.height = DOCK_H + 'px';

      var isGraph = s.viewMode === 'graph';
      var tracks = s.tracks || [];
      var rate = s.tickRate || 64;
      var totalTicks = Math.max(64, s.lengthTicks || 64);
      var phTick = s.playheadTick || 0;

      // Paint header buttons
      btnViewTl.p.style.backgroundColor = !isGraph ? '#f0b32333' : '#ffffff12';
      btnViewTl.p.style.border = '1px solid ' + (!isGraph ? '#f0b323ff' : '#ffffff1a');
      btnViewTl.l.style.color = !isGraph ? '#f0b323ff' : S.label;
      btnViewGr.p.style.backgroundColor = isGraph ? '#f0b32333' : '#ffffff12';
      btnViewGr.p.style.border = '1px solid ' + (isGraph ? '#f0b323ff' : '#ffffff1a');
      btnViewGr.l.style.color = isGraph ? '#f0b323ff' : S.label;

      for (var k in chButtons) {
        if (!chButtons.hasOwnProperty(k)) continue;
        var cbBtn = chButtons[k];
        cbBtn.p.visible = isGraph;
        var on = isGraph && s.graphChannel === k;
        cbBtn.p.style.backgroundColor = on ? '#80bdff33' : '#ffffff10';
        cbBtn.p.style.border = '1px solid ' + (on ? '#80bdffff' : '#ffffff1a');
        cbBtn.l.style.color = on ? '#80bdffff' : S.label;
      }

      tlReadout.text = (phTick / rate).toFixed(2) + 's  ·  Tick ' + phTick + ' / ' + totalTicks + (s.playing ? '  ·  PLAY' : '  ·  SCRUB');
      tlLeftTitle.text = isGraph
        ? ('CURVE  ·  ' + ((s.selKind === 'track' && tracks[s.selTrack]) ? tracks[s.selTrack].name.toUpperCase() : 'NO TRACK'))
        : 'SCENE TRACKS (' + tracks.length + ')';

      var canvasW = Math.max(200, sw - TL_LEFT_W);
      var bodyH = DOCK_H - TL_HEAD_H;
      var areaH = Math.max(40, bodyH - TL_RULER_H);

      // Ruler ticks
      var stepSec = (totalTicks / rate) > 16 ? 2.0 : ((totalTicks / rate) > 6 ? 1.0 : 0.5);
      var stepTicks = Math.max(1, Math.round(stepSec * rate));
      var rIdx = 0;
      for (var rt = 0; rt <= totalTicks && rIdx < SE_RULER_TICKS; rt += stepTicks) {
        var rx = tickToX(rt, totalTicks, canvasW);
        var mkR = seRulerMarks[rIdx++];
        mkR.tick.visible = true; mkR.lbl.visible = true;
        mkR.tick.style.position = rx.toFixed(1) + 'px ' + (TL_RULER_H - 8) + 'px 0px';
        mkR.lbl.text = (rt / rate).toFixed(1) + 's';
        mkR.lbl.style.position = (rx + 4).toFixed(1) + 'px 4px 0px';
      }
      for (; rIdx < SE_RULER_TICKS; rIdx++) {
        seRulerMarks[rIdx].tick.visible = false;
        seRulerMarks[rIdx].lbl.visible = false;
      }

      // Playhead positioning
      var phX = tickToX(phTick, totalTicks, canvasW);
      playheadLine.style.position = phX.toFixed(1) + 'px ' + TL_RULER_H + 'px 0px';
      playheadLine.style.height = areaH + 'px';
      playheadCap.style.position = (phX - 5).toFixed(1) + 'px ' + (TL_RULER_H - 12) + 'px 0px';

      // Timeline lanes vs Graph curve
      grArea.visible = isGraph;
      for (var li = 0; li < SE_MAX_LANES; li++) {
        var lane = seLanes[li], tk = tracks[li];
        var show = !isGraph && !!tk;
        lane.leftRow.visible = show; lane.strip.visible = show;
        if (!show) continue;
        var sel = (s.selKind === 'track' && s.selTrack === li);
        var col = tk.placedActor ? '#6eebafff' : (tk.team === 2 ? '#ffb044ff' : '#60c8ffff');
        var barBg = tk.placedActor ? '#6eebaf44' : (tk.team === 2 ? '#ffb04444' : '#60c8ff44');
        lane.leftRow.style.backgroundColor = sel ? '#f0b3231f' : '#00000000';
        lane.dot.style.backgroundColor = tk.enabled ? col : '#6b7480ff';
        lane.name.text = tk.name; lane.name.style.color = sel ? '#ffffffff' : (tk.enabled ? S.value : S.dim);
        lane.meta.text = (tk.delayTicks / rate).toFixed(1) + 's';

        var rowY = TL_RULER_H + li * TL_ROW_H;
        lane.strip.style.position = '0px ' + rowY + 'px 0px';
        var bx0 = tickToX(tk.delayTicks, totalTicks, canvasW);
        var bx1 = tickToX(tk.delayTicks + Math.max(8, tk.durationTicks), totalTicks, canvasW);
        var bw = Math.max(14, bx1 - bx0);
        lane.bar.style.position = bx0.toFixed(1) + 'px 4px 0px';
        lane.bar.style.width = bw.toFixed(1) + 'px';
        lane.bar.style.backgroundColor = barBg;
        lane.bar.style.border = sel ? ('1px solid ' + col) : '1px solid #ffffff2a';
        lane.barLbl.text = tk.placedActor ? 'Actor' : 'Take';
        lane.rect = { x0: bx0, x1: bx0 + bw, y0: rowY, y1: rowY + TL_ROW_H, cues: [] };

        var cues = tk.cues || [];
        for (var ci = 0; ci < 6; ci++) {
          var cb = lane.cues[ci], cue = cues[ci];
          cb.box.visible = !!cue;
          if (!cue) continue;
          var cx0 = tickToX(tk.delayTicks + cue.startTick, totalTicks, canvasW);
          var cx1 = tickToX(tk.delayTicks + cue.startTick + cue.durationTicks, totalTicks, canvasW);
          var cw = Math.max(10, cx1 - cx0);
          cb.box.style.position = cx0.toFixed(1) + 'px 13px 0px';
          cb.box.style.width = cw.toFixed(1) + 'px';
          cb.lbl.text = cue.display || cue.clip;
          lane.rect.cues.push({ x0: cx0, x1: cx0 + cw, startTick: cue.startTick });
        }
      }

      if (isGraph) {
        grArea.style.position = '0px ' + TL_RULER_H + 'px 0px';
        grArea.style.height = areaH + 'px';
        var gr = s.graph;
        var pts = (gr && gr.samples) || [];
        var minV = gr ? gr.minVal : 0, maxV = gr ? gr.maxVal : 1;
        var spanV = Math.max(0.001, maxV - minV);
        grMaxLbl.text = maxV.toFixed(1);
        grMaxLbl.style.position = '8px 6px 0px';
        grMinLbl.text = minV.toFixed(1);
        grMinLbl.style.position = '8px ' + Math.max(20, areaH - 20) + 'px 0px';
        grMidLine.style.position = '0px ' + Math.round(areaH * 0.5) + 'px 0px';

        var padT = 18, padB = 18, plotH = Math.max(16, areaH - padT - padB);
        for (var gi = 0; gi < SE_MAX_SEGS; gi++) {
          var seg = grSegs[gi];
          if (gi + 1 >= pts.length) { seg.visible = false; continue; }
          var p0 = pts[gi], p1 = pts[gi + 1];
          var gx0 = tickToX(p0[0], totalTicks, canvasW), gx1 = tickToX(p1[0], totalTicks, canvasW);
          var gy0 = padT + (1 - (p0[1] - minV) / spanV) * plotH;
          var gy1 = padT + (1 - (p1[1] - minV) / spanV) * plotH;
          var gdx = gx1 - gx0, gdy = gy1 - gy0, gl = Math.sqrt(gdx * gdx + gdy * gdy);
          seg.visible = gl > 0.2;
          if (!seg.visible) continue;
          seg.style.position = gx0.toFixed(1) + 'px ' + gy0.toFixed(1) + 'px 0px';
          seg.style.width = gl.toFixed(1) + 'px';
          seg.style.height = '2px';
          seg.style.transformOrigin = '0% 50%';
          seg.style.transform = 'rotateZ(' + (Math.atan2(gdy, gdx) * 180 / Math.PI).toFixed(2) + 'deg)';
        }
      }

      // Interactive mouse scrub & bar/cue drag inside tlCanvas
      var scale = root.actualuiscale_x || 1;
      var origin = root.GetPositionWithinWindow ? root.GetPositionWithinWindow() : { x: 0, y: 0 };
      var mx = ((s.mx || 0) - origin.x) / scale, my = ((s.my || 0) - origin.y) / scale;
      var canX = mx - TL_LEFT_W, canY = my - (sh - DOCK_H + TL_HEAD_H);
      var down = !!s.lmb, pressed = down && !tlLmbWas;
      var inCanvas = !!s.cursor && canX >= 0 && canX <= canvasW && canY >= 0 && canY <= bodyH && !modalBlocking();

      if (inCanvas && pressed) {
        if (canY <= TL_RULER_H || isGraph) {
          tlDragMode = 'scrub';
          cmd('mirv_filmmaker sceneeditor scrub ' + xToTick(canX, totalTicks, canvasW));
        } else {
          tlDragMode = 'scrub';
          for (var li2 = 0; li2 < tracks.length && li2 < SE_MAX_LANES; li2++) {
            var lr = seLanes[li2].rect;
            if (!lr || canY < lr.y0 || canY > lr.y1) continue;
            cmd('mirv_filmmaker sceneeditor select track ' + li2);
            var hitCue = -1;
            if (canY >= lr.y0 + 12) {
              for (var q = 0; q < lr.cues.length; q++) {
                if (canX >= lr.cues[q].x0 && canX <= lr.cues[q].x1) { hitCue = q; break; }
              }
            }
            if (hitCue >= 0) {
              tlDragMode = 'cue'; tlDragTrack = li2; tlDragCue = hitCue;
              tlDragStartTick = xToTick(canX, totalTicks, canvasW);
              tlDragOrigTick = lr.cues[hitCue].startTick;
            } else if (canX >= lr.x0 && canX <= lr.x1) {
              tlDragMode = 'track'; tlDragTrack = li2;
              tlDragStartTick = xToTick(canX, totalTicks, canvasW);
              tlDragOrigTick = tracks[li2].delayTicks;
            }
            break;
          }
          if (tlDragMode === 'scrub') cmd('mirv_filmmaker sceneeditor scrub ' + xToTick(canX, totalTicks, canvasW));
        }
      } else if (tlDragMode && down) {
        var curT = xToTick(canX, totalTicks, canvasW);
        if (tlDragMode === 'scrub') {
          cmd('mirv_filmmaker sceneeditor scrub ' + curT);
        } else if (tlDragMode === 'track' && tlDragTrack >= 0) {
          cmd('mirv_filmmaker sceneeditor delay ' + tlDragTrack + ' ' + Math.max(0, tlDragOrigTick + curT - tlDragStartTick));
        } else if (tlDragMode === 'cue' && tlDragTrack >= 0 && tlDragCue >= 0) {
          cmd('mirv_filmmaker sceneeditor cue move ' + tlDragTrack + ' ' + tlDragCue + ' ' + Math.max(0, tlDragOrigTick + curT - tlDragStartTick));
        }
      } else if (!down) {
        tlDragMode = '';
      }
      tlLmbWas = down;
    }
)SEJS";

} // namespace Filmmaker
