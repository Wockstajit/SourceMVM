#pragma once

// SCENE OBJECTS > DECALS & MEDIA (WorldDecals.h):
// Project text, logos, images, videos, arrows, or animated painted routes onto walls and floors,
// or attach a projection to any Scene Object so it moves with the shot.
// Spliced into ConfigHudEnvironmentTabJs.h inside envSceneSec (the Scene Builder card).

R"CFJS(
    // ===================== SCENE OBJECTS > DECALS & MEDIA =====================
    var decalsPage = envSceneSec.__page(T('Decals & Media'));
    function decalCmd(t) { cmd('mirv_filmmaker decals ' + t); }
    function decalsState() { return (st && st.decals) ? st.decals : null; }
    function decalSelected() {
      var ds = decalsState();
      if (!ds || !ds.items) return null;
      for (var i = 0; i < ds.items.length; i++) if (ds.items[i].id === ds.selected) return ds.items[i];
      return null;
    }

    // Explanations live on hover (Docs/panorama/panorama_ui_guide.md): the page shows controls only.
    var dcAddGroup = actionGroup(decalsPage, T('Add projection'), [
      [T('+ Text'), function () { decalCmd('add text wall'); }],
      [T('+ Image / Logo'), function () { decalCmd('add image wall'); }],
      [T('+ Wall Video'), function () { decalCmd('add video wall'); }],
      [T('+ Arrow'), function () { decalCmd('add arrow floor'); }],
      [T('+ Ground Route'), function () { decalCmd('add route floor'); }]
    ], 3, null);
    attachTip(dcAddGroup.row, T('Projects text, logos, images, videos, arrows, or animated routes onto walls and floors.'));

    // The list is a box like the outliner's (ConfigHudOutlinerJs.h): a NAME header, then six rows
    // at most, the rest scrolling inside it. Click a row to select it, right-click for its menu;
    // Delete and Ctrl+D act on the selected decal while this page shows.
    var DC_ROW_H = 26, DC_LIST_ROWS = 6;
    var dcTree = mk('Panel', decalsPage);
    dcTree.style.width = '100%'; dcTree.style.flowChildren = 'down'; dcTree.style.marginTop = '8px';
    dcTree.style.backgroundColor = '#0f1318ff'; dcTree.style.border = '1px solid ' + S.tileBorder;
    dcTree.style.borderRadius = '4px';
    var dcHead = mk('Panel', dcTree);
    dcHead.style.width = '100%'; dcHead.style.height = DC_ROW_H + 'px'; dcHead.style.flowChildren = 'right';
    dcHead.style.borderBottom = '1px solid #222832'; dcHead.style.paddingLeft = '12px';
    var dcHeadName = lbl(dcHead, T('NAME'), S.muted, 11);
    dcHeadName.style.width = 'fill-parent-flow(1.0)'; dcHeadName.style.verticalAlign = 'center';
    dcHeadName.style.fontWeight = 'bold'; dcHeadName.style.letterSpacing = '1.5px';
    attachTip(dcHead, T('Drag a decal in the view to move it. It sticks to the wall or floor under the cursor.'));
    var dcHeadEye = mk('Panel', dcHead); dcHeadEye.style.width = '30px'; dcHeadEye.style.height = DC_ROW_H + 'px';
    olIcon(dcHeadEye, 'eye', 14, '#8b95a3ff');
    attachTip(dcHeadEye, T('Shown in the view'));
    var dcWell = scrollWell(dcTree, DC_LIST_ROWS * (DC_ROW_H + 1));
    var decalListHost = dcWell.inner;
    var dcEmpty = lbl(dcTree, T('Nothing yet'), S.muted, 12);
    dcEmpty.style.padding = '7px 12px';

    function dcMenu(d) {
      cfgMenu([
        [T('Duplicate'), function () { decalCmd('duplicate ' + d.id); }, 'Ctrl+D'],
        [T('Place where looking'), function () { decalCmd('place ' + d.id + ' here'); }],
        [d.enabled ? T('Hide') : T('Show'), function () { decalCmd('set ' + d.id + ' enabled ' + (d.enabled ? 0 : 1)); }],
        null,
        [T('Remove'), function () { decalCmd('remove ' + d.id); }, 'Del', true]
      ]);
    }
    // Delete removes the selected decal and Ctrl+D duplicates it while this page shows. The
    // outliner's own Delete steps aside here (ConfigHudOutlinerJs.h), so it never removes the
    // scene object that happens to be selected on the other page.
    keyHooks.push(function (name) {
      if (!decalsViewActive()) return;
      var d = decalSelected(); if (!d) return;
      if (name === 'delete') decalCmd('remove ' + d.id);
      else if (name === 'duplicate') decalCmd('duplicate ' + d.id);
    });

    var decalInsp = mk('Panel', decalsPage);
    decalInsp.style.width = '100%'; decalInsp.style.flowChildren = 'down';
    decalInsp.style.marginTop = '10px'; decalInsp.style.padding = '10px';
    decalInsp.style.backgroundColor = S.drawer; decalInsp.style.borderRadius = '4px';
    decalInsp.style.border = '1px solid #ffffff14'; decalInsp.visible = false;

    var decalInspHead = mk('Panel', decalInsp);
    decalInspHead.style.width = '100%'; decalInspHead.style.flowChildren = 'right';
    var decalInspTitle = lbl(decalInspHead, '', S.value, 13);
    decalInspTitle.style.fontWeight = 'bold'; decalInspTitle.style.width = 'fill-parent-flow(1.0)';
    decalInspTitle.style.verticalAlign = 'center';
    var dcPlaceBtn = btn(decalInspHead, T('Place where looking'), function () {
      var d = decalSelected(); if (d) decalCmd('place ' + d.id + ' here');
    }, ENV_ACCENT);
    dcPlaceBtn.style.marginRight = '4px';
    attachTip(dcPlaceBtn, T('Moves and aligns the projection onto the wall or floor in front of the camera.'));
    var decalDupBtn = btn(decalInspHead, T('Dup'), function () {
      var d = decalSelected(); if (d) decalCmd('duplicate ' + d.id);
    }, S.label);
    decalDupBtn.style.marginRight = '4px';
    var decalDelBtn = btn(decalInspHead, T('Remove'), function () {
      var d = decalSelected(); if (d) decalCmd('remove ' + d.id);
    }, '#ff8a8aff');
    decalDelBtn.style.marginRight = '0px';

    function dSet(field, v) { var d = decalSelected(); if (d) decalCmd('set ' + d.id + ' ' + field + ' ' + v); }
    // Values sit in a grid of small cells, two or three to a line, instead of one long column.
    // A cell is the shared value row (sliderRow) or switch row (togglePill) restyled to stack:
    // the name and the number on top, the bar along the bottom. Dragging still works anywhere
    // in the cell and stays relative (Shift is finer).
    var DC_CELL_H = 40;
    // Each line of the grid is its own 'right' row of equal fill-width cells. Percent widths in a
    // right-wrap panel do not work here: Panorama adds a cell's padding or margin on top of its
    // percentage, so the last cell of every line wrapped (seen in game, not in the browser mockup).
    function dcGrid(parent, cols) {
      endTileLine(parent);
      var g = mk('Panel', parent); g.hittest = false; g.hittestchildren = true;
      g.style.width = '100%'; g.style.flowChildren = 'down'; g.style.marginTop = '2px';
      g.__cols = cols; g.__line = null; g.__inLine = 0;
      return g;
    }
    function dcCellHost(grid) {
      if (!grid.__line || grid.__inLine >= grid.__cols) {
        grid.__line = mk('Panel', grid); grid.__line.hittest = false; grid.__line.hittestchildren = true;
        grid.__line.style.width = '100%'; grid.__line.style.flowChildren = 'right';
        grid.__inLine = 0;
      }
      var c = mk('Panel', grid.__line); c.hittest = false; c.hittestchildren = true;
      c.style.width = 'fill-parent-flow(1.0)'; c.style.flowChildren = 'down';
      if (grid.__inLine > 0) c.style.marginLeft = '4px';
      grid.__inLine++;
      return c;
    }
    // Both rows put their name label first: sliderRow (name, bar, number), togglePill (name, state, switch).
    function dcCellRow(t) {
      var nameLbl = t.GetChild(0);
      t.style.minHeight = DC_CELL_H + 'px'; t.style.height = DC_CELL_H + 'px';
      t.style.marginTop = '3px'; t.style.paddingLeft = '8px'; t.style.paddingRight = '8px';
      nameLbl.style.width = 'fill-parent-flow(1.0)'; nameLbl.style.fontSize = '12px';
      nameLbl.style.verticalAlign = 'top'; nameLbl.style.paddingTop = '5px'; nameLbl.style.paddingBottom = '0px';
      nameLbl.style.whiteSpace = 'nowrap'; nameLbl.style.textOverflow = 'shrink';
    }
    function dSlider(grid, label, field, min, max, dec, unit, tip, onChange) {
      var h = sliderRow(dcCellHost(grid), {
        label: label, min: min, max: max, decimals: dec || 0, unit: unit || '', tip: tip,
        onChange: onChange || function (v) { dSet(field, v); }
      });
      var t = h.row;
      dcCellRow(t);
      var bar = t.GetChild(1), num = t.GetChild(2);
      bar.style.ignoreParentFlow = 'true'; bar.style.width = '100%';
      bar.style.marginLeft = '0px'; bar.style.marginRight = '0px';
      bar.style.verticalAlign = 'bottom'; bar.style.marginBottom = '5px';
      num.style.width = '54px'; num.style.fontSize = '12px';
      num.style.verticalAlign = 'top'; num.style.paddingTop = '5px';
      return h;
    }
    function dToggle(grid, label, tip, onClick) {
      var h = togglePill(dcCellHost(grid), label, null, onClick, tip);
      dcCellRow(h.row);
      var state = h.row.GetChild(1), sw = h.row.GetChild(2);
      state.visible = false;
      sw.style.width = '38px'; sw.style.verticalAlign = 'center';
      var nm = h.row.GetChild(0); nm.style.verticalAlign = 'center'; nm.style.paddingTop = '0px';
      return h;
    }

    // ---- Text section ----
    var dcTextBox = mk('Panel', decalInsp); dcTextBox.style.width = '100%'; dcTextBox.style.flowChildren = 'down';
    subHead(dcTextBox, T('Text'));
    var dcTextFocus = false, dcTextBuf = '';
    var dcTextField = mk('Panel', dcTextBox); dcTextField.hittest = true;
    dcTextField.style.width = '100%'; dcTextField.style.height = '32px'; dcTextField.style.flowChildren = 'right';
    dcTextField.style.backgroundColor = '#0c0e12ff'; dcTextField.style.borderRadius = '3px';
    dcTextField.style.border = '1px solid #ffffff1a'; dcTextField.style.paddingLeft = '9px'; dcTextField.style.paddingRight = '9px';
    var dcTextLbl = lbl(dcTextField, '', S.value, 13); dcTextLbl.hittest = false;
    dcTextLbl.style.verticalAlign = 'center'; dcTextLbl.style.width = 'fill-parent-flow(1.0)';
    function dcTextPaint() {
      var d = decalSelected();
      var shown = dcTextFocus ? (dcTextBuf + '|') : ((d && d.text) || T('Click to type decal text…'));
      dcTextLbl.text = shown;
      dcTextLbl.style.color = (dcTextFocus || (d && d.text)) ? S.value : S.muted;
      dcTextField.style.border = '1px solid ' + (dcTextFocus ? ENV_ACCENT : '#ffffff1a');
    }
    var dcPrevBlur = configWindowBlur, dcPrevChars = configWindowChars;
    configWindowBlur = function () {
      if (dcTextFocus) {
        dcTextFocus = false;
        if (dcTextBuf.length) dSet('text', dcTextBuf);
        dcTextPaint();
      }
      dcPrevBlur();
    };
    configWindowChars = function (csv) {
      if (!dcTextFocus) { dcPrevChars(csv); return; }
      String(csv || '').split(',').forEach(function (part) {
        var n = parseInt(part, 10);
        if (n === 13) { dcTextFocus = false; root.SetAttributeString('textcapture', '0'); dSet('text', dcTextBuf); }
        else if (n === 27) { dcTextFocus = false; root.SetAttributeString('textcapture', '0'); }
        else if (n === 8) dcTextBuf = dcTextBuf.slice(0, -1);
        else if (dcTextBuf.length < 64 && n >= 32 && n <= 126 && n !== 34 && n !== 59) dcTextBuf += String.fromCharCode(n);
      });
      dcTextPaint();
    };
    dcTextField.SetPanelEvent('onactivate', function () {
      var d = decalSelected();
      configWindowBlur();
      dcTextFocus = true;
      dcTextBuf = (d && d.text) ? d.text : '';
      cfgTextOwner = dcTextField;
      root.SetAttributeString('textcapture', '1');
      dcTextPaint();
    });
    var dcTextGrid = dcGrid(dcTextBox, 2);
    var dcFontSize = dSlider(dcTextGrid, T('Font size'), 'fontSize', 18, 160, 0, 'px', T('Rendered text sharpness and scale.'));
    var dcOutline = dToggle(dcTextGrid, T('Dark outline'), T('Adds a dark border around letters so text stays readable on bright walls.'),
      function () { var d = decalSelected(); if (d) dSet('outline', d.outline ? 0 : 1); });
)CFJS"
R"CFJS(
    // ---- Image / Logo / Wall Video section ----
    var dcMediaBox = mk('Panel', decalInsp); dcMediaBox.style.width = '100%'; dcMediaBox.style.flowChildren = 'down';
    subHead(dcMediaBox, T('Wall image / video'));
    var dcMediaGroup = actionGroup(dcMediaBox, T('Media file'), [
      [T('Choose Image / Logo…'), function () { var d = decalSelected(); if (d) decalCmd('pickmedia ' + d.id + ' image'); }],
      [T('Choose Video…'), function () { var d = decalSelected(); if (d) decalCmd('pickmedia ' + d.id + ' video'); }]
    ], 2, null);
    attachTip(dcMediaGroup.row, T('Pick a PNG/JPG logo or an MP4/MOV/WMV video file to project onto walls and floors.'));
    var dcMediaNote = envNote(dcMediaBox, '');
    var dcVideoOpts = mk('Panel', dcMediaBox); dcVideoOpts.style.width = '100%'; dcVideoOpts.style.flowChildren = 'down';
    var dcVideoGrid = dcGrid(dcVideoOpts, 2);
    var dcVidSpeed = dSlider(dcVideoGrid, T('Video speed'), 'videoSpeed', 0.1, 3.0, 2, 'x', T('Playback rate synced to the demo clock.'));
    var dcVidLoop = dToggle(dcVideoGrid, T('Loop video'), T('Loops the wall video continuously.'),
      function () { var d = decalSelected(); if (d) dSet('videoLoop', d.videoLoop ? 0 : 1); });

    // ---- Arrow section ----
    var dcArrowBox = mk('Panel', decalInsp); dcArrowBox.style.width = '100%'; dcArrowBox.style.flowChildren = 'down';
    subHead(dcArrowBox, T('Arrow'));
    var dcArrowGrid = dcGrid(dcArrowBox, 2);
    var dcArrowHead = dSlider(dcArrowGrid, T('Head size'), 'arrowHead', 0.15, 0.7, 2, '', T('Length of the arrowhead.'));
    var dcArrowShaft = dSlider(dcArrowGrid, T('Shaft width'), 'arrowShaft', 0.08, 0.5, 2, '', T('Thickness of the arrow stem.'));
    var dcArrowPulse = dSlider(dcArrowGrid, T('Pulse speed'), 'arrowPulse', 0, 8, 1, '', T('Animates a traveling glow along the arrow.'));
    var dcArrowDash = dToggle(dcArrowGrid, T('Dashed shaft'), T('Splits the arrow shaft into animated dashes.'),
      function () { var d = decalSelected(); if (d) dSet('arrowDashed', d.arrowDashed ? 0 : 1); });

    // ---- Animated Ground Route section ----
    var dcRouteBox = mk('Panel', decalInsp); dcRouteBox.style.width = '100%'; dcRouteBox.style.flowChildren = 'down';
    subHead(dcRouteBox, T('Animated ground route'));
    var dcRouteGroup = actionGroup(dcRouteBox, T('Waypoints'), [
      [T('+ Point here'), function () { var d = decalSelected(); if (d) decalCmd('route ' + d.id + ' addpoint'); }],
      [T('Undo point'), function () { var d = decalSelected(); if (d) decalCmd('route ' + d.id + ' undo'); }],
      [T('Start draw now'), function () { var d = decalSelected(); if (d) decalCmd('route ' + d.id + ' startnow'); }]
    ], 3, null);
    attachTip(dcRouteGroup.row, T('Adds waypoints along the floor where the camera aims, and starts the route drawing from the current demo time.'));
    var dcRouteNote = envNote(dcRouteBox, '');
    var dcRouteGrid = dcGrid(dcRouteBox, 2);
    var dcRouteProg = dSlider(dcRouteGrid, T('Draw progress'), 'routeProgress', 0, 100, 0, '%',
      T('Manual draw progress when Animate draw-on is off.'),
      function (v) { dSet('routeProgress', (v / 100).toFixed(3)); });
    var dcRouteDur = dSlider(dcRouteGrid, T('Draw duration'), 'routeDuration', 0.3, 20, 1, 's', T('Seconds it takes the route to draw from start to finish.'));
    var dcRouteWidth = dSlider(dcRouteGrid, T('Line width'), 'routeWidth', 4, 64, 0, 'u', T('Width of the painted line on the ground.'));
    var dcRouteAnim = dToggle(dcRouteGrid, T('Animate draw-on'), T('Draws the route smoothly across the ground over Duration seconds.'),
      function () { var d = decalSelected(); if (d) dSet('routeAnimate', d.routeAnimate ? 0 : 1); });
    var dcRouteDash = dToggle(dcRouteGrid, T('Dashed line'), T('Paints the route as tactical dashes.'),
      function () { var d = decalSelected(); if (d) dSet('routeDashed', d.routeDashed ? 0 : 1); });
    var dcRouteCap = dToggle(dcRouteGrid, T('Arrowhead tip'), T('Draws a directional arrow at the leading tip as the route draws.'),
      function () { var d = decalSelected(); if (d) dSet('routeArrowCap', d.routeArrowCap ? 0 : 1); });

    // ---- Transform & Appearance ----
    var dcXformBox = mk('Panel', decalInsp); dcXformBox.style.width = '100%'; dcXformBox.style.flowChildren = 'down';
    subHead(dcXformBox, T('Transform & projection'));
    var dcXformGrid = dcGrid(dcXformBox, 3);
    var dcWidth = dSlider(dcXformGrid, T('Width'), 'width', 8, 512, 0, 'u', T('Projection width in world units.'));
    var dcHeight = dSlider(dcXformGrid, T('Height'), 'height', 8, 512, 0, 'u', T('Projection height in world units.'));
    var dcDepth = dSlider(dcXformGrid, T('Depth'), 'depth', 8, 256, 0, 'u', T('How deep the projection reaches into walls/floors.'));
    var dcPitch = dSlider(dcXformGrid, T('Pitch'), 'pitch', -180, 180, 0, '°', T('Tilt up or down (90° points straight into the floor).'));
    var dcYaw = dSlider(dcXformGrid, T('Yaw'), 'yaw', -180, 180, 0, '°', T('Horizontal facing angle.'));
    var dcRoll = dSlider(dcXformGrid, T('Roll'), 'roll', -180, 180, 0, '°', T('Rotates the decal in the wall/floor plane.'));

    subHead(decalInsp, T('Appearance'));
    var dcBlend = modeRow(decalInsp, T('Blend'), [
      ['alpha', T('Alpha')], ['lit', T('Lit Paint')], ['additive', T('Glow')], ['multiply', T('Multiply')]
    ], '240px', function (m) { dSet('blend', m); },
      T('Lit Paint adopts the map wall lighting so posters and painted lines sit naturally in shadows.'), 12);
    var dcLookGrid = dcGrid(decalInsp, 2);
    var dcOpacity = dSlider(dcLookGrid, T('Opacity'), 'opacity', 0, 100, 0, '%', T('Overall visibility of the projection.'),
      function (v) { dSet('opacity', (v / 100).toFixed(3)); });
    var dcSkipPlayers = dToggle(dcLookGrid, T('Skip players'), T('Prevents floor routes and wall posters from projecting onto players walking past.'),
      function () { var d = decalSelected(); if (d) dSet('skipPlayers', d.skipPlayers ? 0 : 1); });
    var DC_COLORS = [
      ['1 1 1', '#ffffffff', T('White')], ['1 0.8 0.2', '#ffcc33ff', T('Gold')],
      ['0.25 0.9 0.72', '#40e6b8ff', T('Teal')], ['1 0.32 0.28', '#ff5247ff', T('Red')],
      ['0.35 0.72 1', '#59b8ffff', T('Blue')], ['0.85 0.45 1', '#d973ffff', T('Violet')]
    ];
    swatchGrid(decalInsp, DC_COLORS, function (rgb) { dSet('color', rgb); },
      { label: T('Tint colour'), perRow: 6, chipW: 40, marginTop: '4px' });

    // ---- Attach to Scene Object ----
    subHead(decalInsp, T('Attach to scene object'));
    var dcAttachRow = mk('Panel', decalInsp); dcAttachRow.style.width = '100%'; dcAttachRow.style.flowChildren = 'right-wrap';
    var dcAttachSig = '';
    function dcRebuildAttach(d) {
      var s = envState();
      var objs = (s && s.objects) || [];
      var cur = d ? d.attachObjectId : -1;
      var sig = cur + '|' + objs.map(function (o) { return o.id + ':' + o.kind; }).join(',');
      if (sig === dcAttachSig) return;
      dcAttachSig = sig;
      dcAttachRow.RemoveAndDeleteChildren();
      var choices = [[-1, T('World (none)')]];
      for (var i = 0; i < objs.length; i++) choices.push([objs[i].id, (objs[i].asset || (objs[i].kind + ' ' + objs[i].id))]);
      for (var c = 0; c < choices.length; c++) (function (oid, cap) {
        var b = btn(dcAttachRow, cap, function () {
          var sel = decalSelected(); if (sel) decalCmd('attach ' + sel.id + ' ' + (oid > 0 ? oid : 'off'));
        }, oid === cur ? S.value : S.label);
        b.style.marginBottom = '4px';
        setBtnOn(b, oid === cur, ENV_ACCENT);
      })(choices[c][0], choices[c][1]);
    }

    // Dragging in the view: while this page shows, a press over the preview grabs the decal under
    // the cursor (decals grab), moving drags it over the map's surfaces (decals drag: it turns to
    // face whatever wall or floor it is over) and the release keeps it. The props overlay hands the
    // view to this page (ConfigHudPropsGizmoJs.h: decalsViewActive / decalsViewInput).
    var dcGrabbing = false, dcLastU = -1, dcLastV = -1;
    function decalsViewActive() {
      return activeTab === 'environment' && decalsPage.visible && decalsPage.actuallayoutheight > 0 && !!decalsState();
    }
    function decalsViewInput(u, v, free, down, pressed) {
      if (pressed && free) {
        decalCmd('grab ' + u.toFixed(4) + ' ' + v.toFixed(4));
        dcGrabbing = true; dcLastU = u; dcLastV = v;
        return;
      }
      if (dcGrabbing && down) {
        if (Math.abs(u - dcLastU) + Math.abs(v - dcLastV) > 0.0015) {
          decalCmd('drag ' + u.toFixed(4) + ' ' + v.toFixed(4));
          dcLastU = u; dcLastV = v;
        }
        return;
      }
      if (dcGrabbing && !down) { decalCmd('release'); dcGrabbing = false; }
    }

    var dcListSig = '';
    renderHooks.push(function (st) {
      var ds = decalsState(); if (!ds) return;
      var items = ds.items || [];
      var sig = ds.selected + '|' + items.map(function (d) { return d.id + ':' + d.kind + ':' + d.name + ':' + (d.enabled ? 1 : 0); }).join('|');
      if (sig !== dcListSig) {
        dcListSig = sig;
        decalListHost.RemoveAndDeleteChildren();
        var selIndex = -1;
        for (var i = 0; i < items.length; i++) (function (d, index) {
          var on = d.id === ds.selected;
          if (on) selIndex = index;
          var r = mk('Panel', decalListHost);
          r.style.width = '100%'; r.style.height = DC_ROW_H + 'px'; r.style.flowChildren = 'right';
          r.style.backgroundColor = on ? '#7fe0d01a' : S.clear;
          r.style.opacity = d.enabled ? '1' : '0.45';
          var nb = mk('Panel', r); nb.hittest = true;
          nb.style.width = 'fill-parent-flow(1.0)'; nb.style.height = DC_ROW_H + 'px';
          nb.style.flowChildren = 'right'; nb.style.paddingLeft = '12px'; nb.style.borderRadius = '3px';
          nb.SetPanelEvent('onactivate', function () { decalCmd('select ' + d.id); });
          nb.SetPanelEvent('oncontextmenu', function () { decalCmd('select ' + d.id); dcMenu(d); });
          nb.SetPanelEvent('onmouseover', function () { nb.style.backgroundColor = '#1c222bff'; });
          nb.SetPanelEvent('onmouseout', function () { nb.style.backgroundColor = S.clear; });
          var tag = lbl(nb, d.kind.toUpperCase(), ENV_ACCENT, 10); tag.hittest = false;
          tag.style.width = '46px'; tag.style.fontWeight = 'bold'; tag.style.verticalAlign = 'center';
          var nm = lbl(nb, d.name, on ? ENV_ACCENT : S.value, 13); nm.hittest = false;
          nm.style.width = 'fill-parent-flow(1.0)'; nm.style.verticalAlign = 'center';
          nm.style.whiteSpace = 'nowrap'; nm.style.textOverflow = 'ellipsis';
          var eye = olIconBtn(r, function () { decalCmd('set ' + d.id + ' enabled ' + (d.enabled ? 0 : 1)); });
          eye.style.height = DC_ROW_H + 'px';
          olEye(eye).__paint(d.enabled, d.enabled ? '#9aa3b0ff' : '#e6e9eeff');
          attachTip(eye, d.enabled ? T('Hide in the view') : T('Show in the view'));
        })(items[i], i);
        dcEmpty.visible = items.length === 0;
        // A decal picked in the view scrolls its row into the box.
        if (selIndex >= 0) {
          var top = selIndex * (DC_ROW_H + 1), viewH = DC_LIST_ROWS * (DC_ROW_H + 1);
          if (top < dcWell.y) dcWell.y = top;
          else if (top + DC_ROW_H > dcWell.y + viewH) dcWell.y = top + DC_ROW_H + 1 - viewH;
        }
      }
      var sel = decalSelected();
      decalInsp.visible = !!sel;
      if (!sel) return;
      decalInspTitle.text = sel.name + '  (' + sel.kind + ')';
      dcTextBox.visible = sel.kind === 'text';
      dcMediaBox.visible = sel.kind === 'image' || sel.kind === 'video';
      dcVideoOpts.visible = sel.kind === 'video';
      dcArrowBox.visible = sel.kind === 'arrow';
      dcRouteBox.visible = sel.kind === 'route';
      dcXformBox.visible = sel.kind !== 'route';
      dcTextPaint();
      dcFontSize.sync(sel.fontSize); dcOutline.set(!!sel.outline);
      dcMediaNote.text = ds.pickerPending ? T('Choosing file…') : (sel.mediaPath || T('No file chosen yet.'));
      dcVidSpeed.sync(sel.videoSpeed); dcVidLoop.set(!!sel.videoLoop);
      dcArrowHead.sync(sel.arrowHead); dcArrowShaft.sync(sel.arrowShaft);
      dcArrowDash.set(!!sel.arrowDashed); dcArrowPulse.sync(sel.arrowPulse);
      dcRouteNote.text = T('{0} waypoints placed.', sel.routeCount || 0);
      dcRouteAnim.set(!!sel.routeAnimate); dcRouteProg.sync(Math.round((sel.routeProgress || 0) * 100));
      dcRouteDur.sync(sel.routeDuration); dcRouteWidth.sync(sel.routeWidth);
      dcRouteDash.set(!!sel.routeDashed); dcRouteCap.set(!!sel.routeArrowCap);
      dcWidth.sync(sel.width); dcHeight.sync(sel.height); dcDepth.sync(sel.depth);
      dcPitch.sync(sel.ang[0]); dcYaw.sync(sel.ang[1]); dcRoll.sync(sel.ang[2]);
      dcBlend.sync(sel.blend || 'alpha');
      dcOpacity.sync(Math.round((sel.color[3] || 0) * 100));
      dcSkipPlayers.set(!!sel.skipPlayers);
      dcRebuildAttach(sel);
    });
)CFJS"
