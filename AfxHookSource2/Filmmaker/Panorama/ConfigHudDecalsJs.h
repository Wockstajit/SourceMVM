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

    actionGroup(decalsPage, T('Add projection'), [
      [T('+ Text'), function () { decalCmd('add text wall'); }],
      [T('+ Image / Logo'), function () { decalCmd('add image wall'); }],
      [T('+ Wall Video'), function () { decalCmd('add video wall'); }],
      [T('+ Arrow'), function () { decalCmd('add arrow floor'); }],
      [T('+ Ground Route'), function () { decalCmd('add route floor'); }]
    ], 3, T('Projects text, logos, images, videos, arrows, or animated routes onto walls and floors.'));

    var decalListHost = mk('Panel', decalsPage);
    decalListHost.style.width = '100%'; decalListHost.style.flowChildren = 'down';
    decalListHost.style.marginTop = '8px';
    var decalEmptyNote = envNote(decalsPage, T('No projections yet. Add text, a wall image/video, an arrow, or an animated floor route above.'));

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
    var decalDupBtn = btn(decalInspHead, T('Dup'), function () {
      var d = decalSelected(); if (d) decalCmd('duplicate ' + d.id);
    }, S.label);
    decalDupBtn.style.marginRight = '4px';
    var decalDelBtn = btn(decalInspHead, T('Remove'), function () {
      var d = decalSelected(); if (d) decalCmd('remove ' + d.id);
    }, '#ff8a8aff');
    decalDelBtn.style.marginRight = '0px';

    function dSet(field, v) { var d = decalSelected(); if (d) decalCmd('set ' + d.id + ' ' + field + ' ' + v); }
    function dSlider(parent, label, field, min, max, dec, unit, tip) {
      return sliderRow(parent, {
        label: label, min: min, max: max, decimals: dec || 0, unit: unit || '', tip: tip,
        onChange: function (v) { dSet(field, v); }
      });
    }

    actionGroup(decalInsp, T('Place at camera'), [
      [T('Project on Wall'), function () { var d = decalSelected(); if (d) decalCmd('place ' + d.id + ' wall'); }],
      [T('Project on Floor'), function () { var d = decalSelected(); if (d) decalCmd('place ' + d.id + ' floor'); }]
    ], 2, T('Moves and aligns the projection onto the wall or floor in front of the camera.'));

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
    var dcFontSize = dSlider(dcTextBox, T('Font size'), 'fontSize', 18, 160, 0, 'px', T('Rendered text sharpness and scale.'));
    var dcOutline = togglePill(dcTextBox, T('Dark outline'), T('Adds a dark border around letters so text stays readable on bright walls.'),
      function () { var d = decalSelected(); if (d) dSet('outline', d.outline ? 0 : 1); });
)CFJS"
R"CFJS(
    // ---- Image / Logo / Wall Video section ----
    var dcMediaBox = mk('Panel', decalInsp); dcMediaBox.style.width = '100%'; dcMediaBox.style.flowChildren = 'down';
    subHead(dcMediaBox, T('Wall image / video'));
    actionGroup(dcMediaBox, T('Media file'), [
      [T('Choose Image / Logo…'), function () { var d = decalSelected(); if (d) decalCmd('pickmedia ' + d.id + ' image'); }],
      [T('Choose Video…'), function () { var d = decalSelected(); if (d) decalCmd('pickmedia ' + d.id + ' video'); }]
    ], 2, T('Pick a PNG/JPG logo or an MP4/MOV/WMV video file to project onto walls and floors.'));
    var dcMediaNote = envNote(dcMediaBox, '');
    var dcVideoOpts = mk('Panel', dcMediaBox); dcVideoOpts.style.width = '100%'; dcVideoOpts.style.flowChildren = 'down';
    var dcVidSpeed = dSlider(dcVideoOpts, T('Video speed'), 'videoSpeed', 0.1, 3.0, 2, 'x', T('Playback rate synced to the demo clock.'));
    var dcVidLoop = togglePill(dcVideoOpts, T('Loop video'), T('Loops the wall video continuously.'),
      function () { var d = decalSelected(); if (d) dSet('videoLoop', d.videoLoop ? 0 : 1); });

    // ---- Arrow section ----
    var dcArrowBox = mk('Panel', decalInsp); dcArrowBox.style.width = '100%'; dcArrowBox.style.flowChildren = 'down';
    subHead(dcArrowBox, T('Arrow'));
    var dcArrowHead = dSlider(dcArrowBox, T('Head size'), 'arrowHead', 0.15, 0.7, 2, '', T('Length of the arrowhead.'));
    var dcArrowShaft = dSlider(dcArrowBox, T('Shaft width'), 'arrowShaft', 0.08, 0.5, 2, '', T('Thickness of the arrow stem.'));
    var dcArrowDash = togglePill(dcArrowBox, T('Dashed shaft'), T('Splits the arrow shaft into animated dashes.'),
      function () { var d = decalSelected(); if (d) dSet('arrowDashed', d.arrowDashed ? 0 : 1); });
    var dcArrowPulse = dSlider(dcArrowBox, T('Pulse speed'), 'arrowPulse', 0, 8, 1, '', T('Animates a traveling glow along the arrow.'));

    // ---- Animated Ground Route section ----
    var dcRouteBox = mk('Panel', decalInsp); dcRouteBox.style.width = '100%'; dcRouteBox.style.flowChildren = 'down';
    subHead(dcRouteBox, T('Animated ground route'));
    actionGroup(dcRouteBox, T('Waypoints'), [
      [T('+ Point here'), function () { var d = decalSelected(); if (d) decalCmd('route ' + d.id + ' addpoint'); }],
      [T('Undo point'), function () { var d = decalSelected(); if (d) decalCmd('route ' + d.id + ' undo'); }],
      [T('Start draw now'), function () { var d = decalSelected(); if (d) decalCmd('route ' + d.id + ' startnow'); }]
    ], 3, T('Adds waypoints along the floor where the camera aims, and starts the route drawing from the current demo time.'));
    var dcRouteNote = envNote(dcRouteBox, '');
    var dcRouteAnim = togglePill(dcRouteBox, T('Animate draw-on'), T('Draws the route smoothly across the ground over Duration seconds.'),
      function () { var d = decalSelected(); if (d) dSet('routeAnimate', d.routeAnimate ? 0 : 1); });
    var dcRouteProg = sliderRow(dcRouteBox, {
      label: T('Draw progress'), min: 0, max: 100, decimals: 0, unit: '%',
      tip: T('Manual draw progress when Animate draw-on is off.'),
      onChange: function (v) { dSet('routeProgress', (v / 100).toFixed(3)); }
    });
    var dcRouteDur = dSlider(dcRouteBox, T('Draw duration'), 'routeDuration', 0.3, 20, 1, 's', T('Seconds it takes the route to draw from start to finish.'));
    var dcRouteWidth = dSlider(dcRouteBox, T('Line width'), 'routeWidth', 4, 64, 0, 'u', T('Width of the painted line on the ground.'));
    var dcRouteDash = togglePill(dcRouteBox, T('Dashed line'), T('Paints the route as tactical dashes.'),
      function () { var d = decalSelected(); if (d) dSet('routeDashed', d.routeDashed ? 0 : 1); });
    var dcRouteCap = togglePill(dcRouteBox, T('Arrowhead tip'), T('Draws a directional arrow at the leading tip as the route draws.'),
      function () { var d = decalSelected(); if (d) dSet('routeArrowCap', d.routeArrowCap ? 0 : 1); });

    // ---- Transform & Appearance ----
    var dcXformBox = mk('Panel', decalInsp); dcXformBox.style.width = '100%'; dcXformBox.style.flowChildren = 'down';
    subHead(dcXformBox, T('Transform & projection'));
    var dcWidth = dSlider(dcXformBox, T('Width'), 'width', 8, 512, 0, 'u', T('Projection width in world units.'));
    var dcHeight = dSlider(dcXformBox, T('Height'), 'height', 8, 512, 0, 'u', T('Projection height in world units.'));
    var dcDepth = dSlider(dcXformBox, T('Depth'), 'depth', 8, 256, 0, 'u', T('How deep the projection reaches into walls/floors.'));
    var dcPitch = dSlider(dcXformBox, T('Pitch'), 'pitch', -180, 180, 0, '°', T('Tilt up or down (90° points straight into the floor).'));
    var dcYaw = dSlider(dcXformBox, T('Yaw'), 'yaw', -180, 180, 0, '°', T('Horizontal facing angle.'));
    var dcRoll = dSlider(dcXformBox, T('Roll'), 'roll', -180, 180, 0, '°', T('Rotates the decal in the wall/floor plane.'));

    subHead(decalInsp, T('Appearance'));
    var dcBlend = modeRow(decalInsp, T('Blend'), [
      ['alpha', T('Alpha')], ['lit', T('Lit Paint')], ['additive', T('Glow')], ['multiply', T('Multiply')]
    ], '240px', function (m) { dSet('blend', m); },
      T('Lit Paint adopts the map wall lighting so posters and painted lines sit naturally in shadows.'), 12);
    var dcOpacity = sliderRow(decalInsp, {
      label: T('Opacity'), min: 0, max: 100, decimals: 0, unit: '%',
      tip: T('Overall visibility of the projection.'),
      onChange: function (v) { dSet('opacity', (v / 100).toFixed(3)); }
    });
    var DC_COLORS = [
      ['1 1 1', '#ffffffff', T('White')], ['1 0.8 0.2', '#ffcc33ff', T('Gold')],
      ['0.25 0.9 0.72', '#40e6b8ff', T('Teal')], ['1 0.32 0.28', '#ff5247ff', T('Red')],
      ['0.35 0.72 1', '#59b8ffff', T('Blue')], ['0.85 0.45 1', '#d973ffff', T('Violet')]
    ];
    swatchGrid(decalInsp, DC_COLORS, function (rgb) { dSet('color', rgb); },
      { label: T('Tint colour'), perRow: 6, chipW: 40, marginTop: '4px' });
    var dcSkipPlayers = togglePill(decalInsp, T('Skip players'), T('Prevents floor routes and wall posters from projecting onto players walking past.'),
      function () { var d = decalSelected(); if (d) dSet('skipPlayers', d.skipPlayers ? 0 : 1); });

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

    var dcListSig = '';
    renderHooks.push(function (st) {
      var ds = decalsState(); if (!ds) return;
      var items = ds.items || [];
      var sig = ds.selected + '|' + items.map(function (d) { return d.id + ':' + d.kind + ':' + d.name + ':' + (d.enabled ? 1 : 0); }).join('|');
      if (sig !== dcListSig) {
        dcListSig = sig;
        decalListHost.RemoveAndDeleteChildren();
        decalEmptyNote.visible = items.length === 0;
        for (var i = 0; i < items.length; i++) (function (d) {
          var on = d.id === ds.selected;
          var r = mk('Panel', decalListHost); r.hittest = true;
          r.style.width = '100%'; r.style.height = '30px'; r.style.flowChildren = 'right';
          r.style.paddingLeft = '8px'; r.style.paddingRight = '6px'; r.style.marginBottom = '3px';
          r.style.borderRadius = '3px';
          r.style.backgroundColor = on ? '#5fd0c028' : '#ffffff08';
          r.style.border = '1px solid ' + (on ? ENV_ACCENT : '#ffffff12');
          var tag = lbl(r, d.kind.toUpperCase(), ENV_ACCENT, 10); tag.hittest = false;
          tag.style.width = '48px'; tag.style.fontWeight = 'bold'; tag.style.verticalAlign = 'center';
          var nm = lbl(r, d.name, on ? S.value : S.label, 12); nm.hittest = false;
          nm.style.width = 'fill-parent-flow(1.0)'; nm.style.verticalAlign = 'center';
          var tog = btn(r, d.enabled ? T('On') : T('Off'), function () { decalCmd('set ' + d.id + ' enabled ' + (d.enabled ? 0 : 1)); }, d.enabled ? ENV_ACCENT : S.muted);
          tog.style.marginRight = '0px';
          r.SetPanelEvent('onactivate', function () { decalCmd('select ' + d.id); });
        })(items[i]);
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
