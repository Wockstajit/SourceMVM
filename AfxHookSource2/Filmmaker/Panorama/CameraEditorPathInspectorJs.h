#pragma once

// PATH CAMERA inspector: the SELECTED CAMERA, PATH and SHORTCUTS cards. JS fragment concatenated
// (via #include, as an adjacent raw string literal) into the kCameraEditorJs body assembled in
// CameraEditorJs.h -- not a standalone script; it shares scope with the root IIFE. MUST follow
// CameraEditorCardsJs.h, whose card/tile builders it uses.
//
// LAYOUT (Docs/features/camera-editor-feature.md): two rail items.
//
//   KEYS / SELECTED CAMERA   Camera   nav, THIS CAMERA actions, Reposition, FOV, Roll
//                            Curve    Curve, Ease
//                            Speed    Mode, Amount, Timing
//   HELP / SHORTCUTS         the four key bindings
//
// SELECTED CAMERA and PATH used to be two cards; they are one rail item now.
//
// NAME COLLISIONS ARE REAL HERE. Every *Js.h fragment is concatenated into ONE function scope,
// so a `var` in this file is visible to -- and clobberable by -- the render workspace, the
// Customize modal and the follow inspector. A plain `speedRow` here was silently overwritten
// by CameraEditorRenderJs.h's own `speedRow` (a Panel), and the PATH tab then stopped syncing
// entirely. Everything this fragment introduces is prefixed `path`.
//
// RULES KEPT FROM THE 2026-07-29 PASS: mutually-exclusive settings are segmented strips, never
// cycle buttons or dropdowns; and rows never appear or disappear as a value changes -- a control
// that does not apply is DISABLED in place, because a vanishing row reflows everything under it.

R"EDJS(
    // ===================== SELECTED CAMERA ==============================
    // ONE card (Docs/features/camera-editor-feature.md): the old SELECTED CAMERA and PATH cards merged into pages Camera /
    // Curve / Speed, so everything about the path is one rail item.
    var selSec = edCard(inspectorBody, 'SELECTED CAMERA', { edge: S.accent, rail: { tab: 'path', key: 'selcam', group: 'Keys' } });
    var pathSec = selSec;   // the merged card answers to both names
    var addBtn = selSec.__headerAction('+ Add', function () { cmd('mirv_filmmaker camtl addkey'); });
    mvmTourAdd('editor.addkey', addBtn);

    var pathSelPage = selSec.__page('Camera');

    // The prev / next / which-one tile: ◀  Key #n / count + its tick  ▶
    var pathNavRow = mk('Panel', pathSelPage); pathNavRow.hittest = false; pathNavRow.hittestchildren = true;
    pathNavRow.style.flowChildren = 'right'; pathNavRow.style.width = '100%';
    pathNavRow.style.height = '52px'; pathNavRow.style.marginTop = '4px';
    pathNavRow.style.paddingLeft = '6px'; pathNavRow.style.paddingRight = '6px';
    pathNavRow.style.borderRadius = '3px';
    pathNavRow.style.backgroundColor = S.tileBg;
    edEndTileLine(pathSelPage);
    function pathNavBtn(glyph, delta) {
      var b = btn(pathNavRow, glyph, function () { cmd('mirv_filmmaker camtl selectdelta ' + delta); }, S.value);
      b.style.width = '44px'; b.style.height = '38px'; b.style.verticalAlign = 'center';
      b.style.paddingTop = '0px'; b.style.paddingBottom = '0px'; b.style.marginRight = '0px';
      b.__lbl.style.horizontalAlign = 'center'; b.__lbl.style.verticalAlign = 'center';
      b.__lbl.style.fontSize = '15px';
      return b;
    }
    var navPrev = pathNavBtn('◀', -1);
    var pathNavMid = mk('Panel', pathNavRow); pathNavMid.hittest = false;
    pathNavMid.style.flowChildren = 'down'; pathNavMid.style.width = 'fill-parent-flow(1.0)';
    pathNavMid.style.verticalAlign = 'center';
    var selLbl = lbl(pathNavMid, 'No cameras', S.value, 15);
    // Centred by ALIGNMENT, not text-align: in game a 100%-wide label in this column kept its
    // text at the left edge.
    selLbl.hittest = false; selLbl.style.fontWeight = 'bold';
    selLbl.style.horizontalAlign = 'center'; selLbl.style.whiteSpace = 'nowrap';
    var selTickLbl = lbl(pathNavMid, '', S.muted, 11);
    selTickLbl.hittest = false; selTickLbl.style.horizontalAlign = 'center';
    selTickLbl.style.marginTop = '2px'; selTickLbl.style.fontFamily = S.fontMonodigit;
    var navNext = pathNavBtn('▶', 1);
    navNext.style.marginRight = '0px';

    // THIS CAMERA: one row of equal-width buttons under a small heading (Docs/features/camera-editor-feature.md; original rail fix).
    // Disabled in place, never hidden, so the row never reflows under the pointer.
    var pathKeyActions = edEvenActions(pathSelPage, 'This camera', [
      ['Delete', function () { if (st && st.selected >= 0) cmd('mirv_filmmaker camtl delkey ' + st.selected); }],
      ['Move to playhead', function () {
        if (st && st.selected >= 0) cmd('mirv_filmmaker camtl movekey ' + st.selected + ' ' + st.tick);
      }],
      ['Clear all', function () { showClearConfirm(); }, 'danger']
    ]);
    var delBtn = pathKeyActions.btns[0], retimeBtn = pathKeyActions.btns[1], clearAllBtn = pathKeyActions.btns[2];

    // `marker reposition` is the INTERACTIVE move behind the marker popup: arm, fly the free cam,
    // left-click (or F) to drop it.
    var pathReposGroup = edActionGroup(pathSelPage, 'Placement', [
      ['⟲  Reposition', function () { if (st && st.selected >= 0) cmd('mirv_filmmaker marker reposition'); }]
    ], 1, 'Fly the free cam to where you want it, then click (or press F) to drop the camera there.');
    var reposBtn = pathReposGroup.btns[0];
    mvmTourAdd('editor.repos', reposBtn);

    var pathFovTile = edValueTile(pathSelPage, {
      label: 'FOV', min: 1, max: 170, decimals: 1, unit: '', fill: S.axis[6],
      tip: 'Field of view on the selected camera.',
      onChange: function (v) {
        if (st && st.selected >= 0) {
          cmd('mirv_filmmaker camtl setvalpreview ' + st.selected + ' ' + CH_FOV + ' ' + v.toFixed(3));
        }
      },
      onBegin: function () { if (st && st.selected >= 0) cmd('mirv_filmmaker camtl editbegin'); },
      onEnd: function () { cmd('mirv_filmmaker camtl editend'); }
    });
    mvmTourAdd('editor.fov', pathFovTile.tile);
    var pathRollTile = edValueTile(pathSelPage, {
      label: 'Roll', min: -180, max: 180, decimals: 1, unit: '°', fill: S.axis[5],
      tip: 'Camera roll on the selected camera.',
      onChange: function (v) {
        if (st && st.selected >= 0) {
          cmd('mirv_filmmaker camtl setvalpreview ' + st.selected + ' ' + CH_ROLL + ' ' + v.toFixed(3));
        }
      },
      onBegin: function () { if (st && st.selected >= 0) cmd('mirv_filmmaker camtl editbegin'); },
      onEnd: function () { cmd('mirv_filmmaker camtl editend'); }
    });
    var lensHint = edNote(pathSelPage, '');
)EDJS"
R"EDJS(
    // ===================== CURVE / SPEED (pages of the same card) ======================
    // Curve is the whole path; Ease belongs to the SELECTED key. Both mirror onto the graph
    // editor and the timeline, so the three surfaces are views of one value rather than copies.
    var pathCurvePage = selSec.__page('Curve');
    var INTERP = [['Linear', 'Linear'], ['Smooth', 'Smooth']];
    var pathInterpRow = edModeRow(pathCurvePage, 'Curve', INTERP, null, function (which) {
      // The path interp is binary; mirror the value it is ABOUT to become onto the graph editor.
      cmd('mirv_filmmaker marker interp cycle');
      cmd('mirv_filmmaker grapheditor ' + (which === 'Linear' ? 'linear' : 'smooth'));
    }, 'How the camera moves between keys.');

    var EASE = ['none', 'in', 'out', 'inout'];
    var pathEaseRow = edModeRow(pathCurvePage, 'Ease',
      [['none', 'None'], ['in', 'In'], ['out', 'Out'], ['inout', 'In/Out']], null,
      function (which) {
        if (!st || st.selected < 0) return;
        cmd('mirv_filmmaker camtl ease ' + st.selected + ' ' + which);
        cmd('mirv_filmmaker grapheditor ' + (which === 'none' ? 'linear' : ('ease ' + which + ' all')));
      },
      'Ease on the SELECTED key — the same value the timeline’s Ease button and its right-click '
      + 'menu write.');
    edNote(pathCurvePage, 'Ease belongs to the selected key; Curve is the whole path. Both mirror onto '
      + 'the graph editor and the timeline.');

    var pathSpeedPage = selSec.__page('Speed');
    var SPEEDMODE = [['Manual', 'Manual'], ['Per-Segment', 'Per-Seg'], ['Constant', 'Constant']];
    var pathSpeedRow = edModeRow(pathSpeedPage, 'Mode', SPEEDMODE, null, function (which) {
      // The console command only cycles, so step it until it lands on the requested mode.
      for (var i = 0; i < SPEEDMODE.length; i++) {
        if (st && st.speedMode === which) break;
        cmd('mirv_filmmaker marker speedmode cycle');
      }
    }, 'Manual follows the keys; Per-Segment and Constant drive the multiplier below.');

    // One multiplier stepper serving both Per-Segment and Constant -- a LADDER (0.20 / 0.50 /
    // 0.80 / 1.00), not a slider. Disabled in Manual rather than removed.
    var pathMulRow = mk('Panel', pathSpeedPage); pathMulRow.hittest = false; pathMulRow.hittestchildren = true;
    pathMulRow.style.flowChildren = 'right'; pathMulRow.style.width = '100%';
    pathMulRow.style.marginTop = (UI.line.gap + 1) + 'px';
    pathMulRow.style.paddingLeft = UI.line.padH + 'px';
    edEndTileLine(pathSpeedPage);
    var pathMulName = lbl(pathMulRow, 'Amount', S.label, 13);
    pathMulName.style.width = UI.line.label + 'px'; pathMulName.style.verticalAlign = 'center';
    pathMulName.style.whiteSpace = 'nowrap';
    edTip(pathMulName, 'Steps through 0.20 / 0.50 / 0.80 / 1.00.');
    var pathMulBox = mk('Panel', pathMulRow); pathMulBox.hittest = false; pathMulBox.hittestchildren = true;
    pathMulBox.style.flowChildren = 'right'; pathMulBox.style.width = 'fill-parent-flow(1.0)';
    pathMulBox.style.marginLeft = UI.line.colGap + 'px'; pathMulBox.style.height = '30px';
    pathMulBox.style.borderRadius = '3px'; pathMulBox.style.border = '1px solid #ffffff1a';
    var mulMinus = btn(pathMulBox, '−', function () { stepMul(-1); }, S.accent);
    var mulLbl = lbl(pathMulBox, '—', S.value, 13);
    mulLbl.hittest = false; mulLbl.style.width = 'fill-parent-flow(1.0)';
    mulLbl.style.textAlign = 'center'; mulLbl.style.verticalAlign = 'center';
    mulLbl.style.fontWeight = 'bold'; mulLbl.style.whiteSpace = 'nowrap';
    mulLbl.style.fontFamily = S.fontMonodigit;
    var mulPlus = btn(pathMulBox, '+', function () { stepMul(1); }, S.accent);
    [mulMinus, mulPlus].forEach(function (b) {
      b.style.width = '34px'; b.style.height = '100%'; b.__lbl.style.horizontalAlign = 'center';
      b.__lbl.style.verticalAlign = 'center';
      b.style.paddingTop = '0px'; b.style.paddingBottom = '0px';
      b.style.marginRight = '0px'; b.style.borderRadius = '0px';
      b.__bgBase = S.clear; b.__brBase = S.clear; b.__accent = S.clear; btnVisual(b);
      b.__lbl.style.fontSize = '15px';
    });
    function stepMul(d) {
      if (!st) return;
      var perSeg = (st.speedMode === 'Per-Segment');
      var steps = [0.2, 0.5, 0.8, 1.0];
      var cur = perSeg ? (st.sel ? st.sel.speedMul : 1) : st.constSpeed;
      var idx = 0, best = 1e9;
      for (var i = 0; i < steps.length; i++) {
        var dd = Math.abs(steps[i] - cur);
        if (dd < best) { best = dd; idx = i; }
      }
      idx += (d > 0 ? 1 : -1);
      if (idx < 0) idx = 0;
      if (idx >= steps.length) idx = steps.length - 1;
      if (perSeg) {
        if (st.selected < 0) return;
        cmd('mirv_filmmaker camtl speed ' + st.selected + ' ' + steps[idx].toFixed(2));
      } else {
        cmd('mirv_filmmaker marker constspeed ' + steps[idx].toFixed(2));
      }
    }

    var TIMING = [['Live', 'Live'], ['Freeze', 'Freeze']];
    var pathTimingRow = edModeRow(pathSpeedPage, 'Timing', TIMING, null, function (which) {
      if (!st || st.timing !== which) cmd('mirv_filmmaker marker timing toggle');
    }, 'Live plays the demo along the path; Freeze holds the tick.');
    var pathHint = edNote(pathSpeedPage, '');

    // ===================== SHORTCUTS ====================================
    var playSec = edCard(inspectorBody, 'SHORTCUTS', { edge: '#3f4855ff', rail: { tab: 'path', key: 'keys', group: 'Help' } });
    playSec.__note('K place · Space play · J arm the path');
    var pathKeysPage = playSec;
    function pathKeyRow(parent, keys, what) {
      var r = mk('Panel', parent); r.hittest = false;
      r.style.flowChildren = 'right'; r.style.width = '100%';
      r.style.marginTop = '6px'; r.style.verticalAlign = 'middle';
      edEndTileLine(parent);
      var chip = mk('Panel', r); chip.hittest = false;
      chip.style.width = '88px'; chip.style.verticalAlign = 'center';
      chip.style.paddingTop = '6px'; chip.style.paddingBottom = '6px';
      chip.style.backgroundColor = S.tileBg;
      chip.style.borderRadius = '3px';
      chip.style.border = '1px solid ' + S.cardBorderOpen;
      chip.style.marginRight = '12px';
      var k = lbl(chip, keys, S.value, 13);
      k.hittest = false; k.style.horizontalAlign = 'center'; k.style.verticalAlign = 'center';
      k.style.fontWeight = 'bold';
      var d = lbl(r, what, S.label, 13);
      d.hittest = false; d.style.width = 'fill-parent-flow(1.0)'; d.style.verticalAlign = 'center';
      return r;
    }
    pathKeyRow(pathKeysPage, 'K', 'Place a camera at the free cam');
    pathKeyRow(pathKeysPage, 'L', 'Look at a path camera to delete it');
    pathKeyRow(pathKeysPage, 'F', 'Look at a path camera to select and edit it');
    pathKeyRow(pathKeysPage, 'Space', 'Play / pause');
    pathKeyRow(pathKeysPage, '← / →', 'Skip ±15s');
    pathKeyRow(pathKeysPage, 'J', 'Arm the path  →  Space plays it');
)EDJS"
R"EDJS(
    // ===================== the per-frame sync ===========================
    // One function rather than a block inside render(): the fragment that builds these controls
    // is the fragment that knows how to fill them, and render() then has nothing to say about
    // what the PATH tab contains.
    function syncPathInspector() {
      // Keep this a real BOOLEAN. Assigning the st.sel OBJECT to panel.visible throws in Panorama
      // and aborts the whole render, which previously blanked Transform, froze the lens tiles and
      // desynced the Path card.
      var has = (st.selected >= 0 && !!st.sel);

      selLbl.text = has ? ('Key #' + (st.selected + 1) + ' / ' + st.count)
                        : (st.count > 0 ? '— / ' + st.count : 'No cameras');
      var tickTxt = has ? ('tick ' + Math.round(num(st.sel.tick))) : (st.count > 0 ? 'No key selected' : 'Press K to place one');
      if (selTickLbl.text !== tickTxt) selTickLbl.text = tickTxt;
      selSec.__badge(String(st.count || 0), 'count');
      // DISABLED, not hidden: hiding these changed the action grid's width budget under the user
      // every time the selection changed, so the whole row re-flowed.
      btnEnable(delBtn, has);
      btnEnable(retimeBtn, has);
      btnEnable(clearAllBtn, st.count > 0);
      btnEnable(reposBtn, has);

      pathFovTile.sync(has ? num(st.sel.fov) : 1);
      pathRollTile.sync(has ? num(st.sel.roll) : 0);
      lensHint.text = has ? 'FOV and roll belong to the selected camera and are keyed with it.'
                          : 'Select a camera to edit its lens.';

      var interpName = st.interp || 'Linear';
      pathInterpRow.sync(interpName);
      var easeIdx = (has && st.sel.ease) || 0;
      pathEaseRow.sync(has ? EASE[easeIdx] : '');
      for (var ek = 0; ek < EASE.length; ek++) btnEnable(pathEaseRow.btns[EASE[ek]], has);

      var speedName = st.speedMode || 'Manual';
      pathSpeedRow.sync(speedName);
      var timingName = (st.timing === 'Freeze') ? 'Freeze' : 'Live';
      pathTimingRow.sync(timingName);

      // The one multiplier stepper serves whichever speed mode is active, and is simply disabled
      // in Manual (where there is no multiplier to edit) rather than the row vanishing.
      var perSeg = (speedName === 'Per-Segment');
      var constMode = (speedName === 'Constant');
      var mulUsable = (perSeg && has && !st.sel.isLast) || constMode;
      btnEnable(mulMinus, mulUsable);
      btnEnable(mulPlus, mulUsable);
      var mulValue = perSeg ? (has ? st.sel.speedMul : null)
                            : (constMode ? (st.constSpeed != null ? st.constSpeed : 1) : null);
      mulLbl.text = perSeg
        ? (has ? ('Segment  x' + st.sel.speedMul.toFixed(2)) : 'select a key')
        : (constMode ? ('Constant  x' + (st.constSpeed != null ? st.constSpeed.toFixed(2) : '1.00'))
                     : 'not used in Manual');
      mulLbl.style.color = mulUsable ? S.value : S.inactive;
      pathHint.text = perSeg ? 'Per-Segment: each key carries its own speed into the next one.'
                    : (constMode ? 'Constant: one speed for the whole path.'
                                 : 'Manual: the path follows the demo timeline.');

      // ---- the card summary: what the path is doing, for the pane header and the rail ----
      selSec.__note((has ? ('Camera ' + (st.selected + 1) + ' of ' + st.count)
                         : (st.count > 0 ? (st.count + ' cameras') : 'No cameras'))
        + ' · ' + (interpName === 'Linear' ? 'linear' : 'smooth') + ' · '
        + (has ? ({ none: 'no ease', in: 'ease in', out: 'ease out', inout: 'ease in/out' }[EASE[easeIdx]])
               : 'no key') + ' · '
        + (perSeg ? ('per-segment ' + (mulValue != null ? mulValue.toFixed(2) : '1.00') + 'x')
           : constMode ? ('constant ' + (mulValue != null ? mulValue.toFixed(2) : '1.00') + 'x')
           : 'manual'));
    }
)EDJS"
