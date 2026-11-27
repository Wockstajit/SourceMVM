#pragma once

// NODES tab of the CONFIG panel and its graph editor window (EffectNodes.h,
// Docs/features/effect-nodes.md). Spliced into ConfigHudJs.h; shares scope with the root IIFE.
//
// The tab: starter presets, a searchable list of every node type to add, the graph's nodes in a
// list that scrolls on its own, and the inspector of the selected node.
//
// The graph editor is its own window in Config's overlay layer (above the TIMELINE dock and the
// demo bar), moved by its title bar and sized by its corner. Everything in it runs from the
// cursor pump (st.mx / st.my / st.lmb / st.rmb), the way Blender and Nuke work:
//   press a node and drag            select and move it (no double click)
//   drag from an output pin          a wire; drop it on an input to connect, on empty space to
//                                    add a node that takes it
//   drag from a connected input      pick the wire up again
//   drag empty space / wheel         pan
//   right-click                      node: bypass, duplicate, disconnect, delete
//                                    wire: disconnect      empty: the searchable add menu
//   Delete / Ctrl+D                  delete / duplicate the selected node
// Nothing in the window is rebuilt on a click: cards and rows are kept and restyled, so a button
// never blinks back to its unhovered ground for a frame.

R"CFJS(
    // ===================== NODES TAB =====================================
    var NODES_ACCENT = '#b98cff';
    var NG_MASK = '#5fd0c0', NG_FX = '#f0b323', NG_OUT = '#80bdff';
    var nodesSec = section(pageNodes, T('EFFECT NODES'), true, true, 'nodes.main', NODES_ACCENT + 'ff');
    var nodesReset = nodesSec.__headerReset(['mirv_filmmaker nodes reset'], 'nodes.main');
    var nodesPill = nodesSec.__headerPill(function () {
      cmd('mirv_filmmaker nodes toggle');
    }, T('Connects depth ranges, sky masks, fog masks, and player mattes to grades, glows, outlines, and distortions.'));

    function nodesCmd(t) { cmd('mirv_filmmaker nodes ' + t); }
    function nodesState() { return (st && st.effectNodes) ? st.effectNodes : null; }
    function nodesSelected() {
      var ns = nodesState();
      if (!ns || !ns.nodes) return null;
      for (var i = 0; i < ns.nodes.length; i++) if (ns.nodes[i].id === ns.selected) return ns.nodes[i];
      return null;
    }
    function ngAccentOf(n) { return n.type === 'output' ? NG_OUT : (n.mask ? NG_MASK : NG_FX); }

    // Every node type the graph can add, with the words the add search matches besides its name.
    var NODE_TYPES = [
      { t: 'depth_range', name: T('Depth Range'), mask: true, keys: 'depth distance near far z' },
      { t: 'sky_mask', name: T('Sky Mask'), mask: true, keys: 'sky skybox horizon' },
      { t: 'player_matte', name: T('Player Matte'), mask: true, keys: 'player subject person character' },
      { t: 'fog_mask', name: T('Fog Mask'), mask: true, keys: 'fog haze atmosphere' },
      { t: 'luma_key', name: T('Luma Key'), mask: true, keys: 'luma brightness highlights shadows key' },
      { t: 'mask_combine', name: T('Mask Combine'), mask: true, keys: 'combine merge union intersect subtract invert' },
      { t: 'mask_feather', name: T('Mask Feather'), mask: true, keys: 'feather blur soften grow shrink expand choke' },
      { t: 'height_mask', name: T('Height Mask'), mask: true, keys: 'height altitude water ground floor level z' },
      { t: 'hue_key', name: T('Hue Key'), mask: true, keys: 'hue color colour key chroma select' },
      { t: 'radial_mask', name: T('Radial Mask'), mask: true, keys: 'radial circle ellipse spot center shape' },
      { t: 'box_mask', name: T('Box Mask'), mask: true, keys: 'box rectangle square shape window letterbox' },
      { t: 'gradient_mask', name: T('Gradient Mask'), mask: true, keys: 'gradient ramp linear graduated top bottom' },
      { t: 'edge_mask', name: T('Edge Mask'), mask: true, keys: 'edge silhouette contour depth lines' },
      { t: 'noise_mask', name: T('Noise Mask'), mask: true, keys: 'noise clouds random flicker smoke animated' },
      { t: 'player_radius', name: T('Around Player'), mask: true, keys: 'around player sphere radius near proximity focus' },
      { t: 'grade', name: T('Color Grade'), mask: false, keys: 'grade color colour saturation contrast exposure temperature' },
      { t: 'glow', name: T('Glow'), mask: false, keys: 'glow bloom light' },
      { t: 'outline', name: T('Outline'), mask: false, keys: 'outline edge stroke' },
      { t: 'distortion', name: T('Distortion'), mask: false, keys: 'distortion distort ripple heat haze chromatic warp' },
      { t: 'lift_gamma_gain', name: T('Lift Gamma Gain'), mask: false, keys: 'lift gamma gain wheels shadows midtones highlights resolve' },
      { t: 'hue_shift', name: T('Hue Shift'), mask: false, keys: 'hue shift rotate vibrance saturation colour color' },
      { t: 'duotone', name: T('Duotone'), mask: false, keys: 'duotone gradient map two tone tint' },
      { t: 'color_fill', name: T('Color Fill'), mask: false, keys: 'color colour fill tint overlay multiply screen solid' },
      { t: 'blur', name: T('Blur'), mask: false, keys: 'blur soft defocus smooth' },
      { t: 'depth_blur', name: T('Depth Blur'), mask: false, keys: 'depth blur focus dof bokeh lens' },
      { t: 'sharpen', name: T('Sharpen'), mask: false, keys: 'sharpen crisp detail unsharp' },
      { t: 'vignette', name: T('Vignette'), mask: false, keys: 'vignette corners dark edges frame' },
      { t: 'film_grain', name: T('Film Grain'), mask: false, keys: 'film grain noise texture analog' },
      { t: 'light_rays', name: T('Light Rays'), mask: false, keys: 'light rays god rays sun shafts volumetric' },
      { t: 'depth_fog', name: T('Depth Fog'), mask: false, keys: 'depth fog haze mist atmosphere distance' },
      { t: 'posterize', name: T('Posterize'), mask: false, keys: 'posterize bands levels cel toon' },
      { t: 'pixelate', name: T('Pixelate'), mask: false, keys: 'pixelate mosaic censor blocks retro' },
      { t: 'halftone', name: T('Halftone'), mask: false, keys: 'halftone dots comic print' },
      { t: 'ink_lines', name: T('Ink Lines'), mask: false, keys: 'ink lines sketch toon edges comic drawing' },
      { t: 'scanlines', name: T('Scanlines'), mask: false, keys: 'scanlines crt tv retro monitor vhs' },
      { t: 'glitch', name: T('Glitch'), mask: false, keys: 'glitch datamosh digital rgb split error' },
      { t: 'thermal', name: T('Thermal'), mask: false, keys: 'thermal heat infrared predator false color' },
      { t: 'night_vision', name: T('Night Vision'), mask: false, keys: 'night vision nvg green goggles' },
      { t: 'invert', name: T('Invert'), mask: false, keys: 'invert negative' },
      { t: 'tonemap', name: T('Tonemap'), mask: false, keys: 'tonemap filmic aces exposure contrast curve' },
      { t: 'split_tone', name: T('Split Tone'), mask: false, keys: 'split tone teal orange shadows highlights tint grade' },
      { t: 'glare', name: T('Glare'), mask: false, keys: 'glare streaks star anamorphic flare ghosts fog glow bloom' },
      { t: 'halation', name: T('Halation'), mask: false, keys: 'halation film red glow highlights bleed' },
      { t: 'lens_distortion', name: T('Lens Distortion'), mask: false, keys: 'lens distortion barrel pincushion fisheye dispersion' },
      { t: 'chromatic_aberration', name: T('Chromatic Aberration'), mask: false, keys: 'chromatic aberration fringe rgb lens' },
      { t: 'directional_blur', name: T('Directional Blur'), mask: false, keys: 'directional motion blur streak speed' },
      { t: 'radial_blur', name: T('Radial Blur'), mask: false, keys: 'radial zoom spin blur speed' },
      { t: 'tilt_shift', name: T('Tilt Shift'), mask: false, keys: 'tilt shift miniature blur band' },
      { t: 'refraction', name: T('Refraction'), mask: false, keys: 'refraction glass water heat distort cloak predator' },
      { t: 'kuwahara', name: T('Kuwahara'), mask: false, keys: 'kuwahara paint painterly oil watercolor' },
      { t: 'cel_shade', name: T('Cel Shade'), mask: false, keys: 'cel shade toon comic borderlands outline hatching ink' },
      { t: 'mirror', name: T('Mirror'), mask: false, keys: 'mirror kaleidoscope symmetry flip' },
      { t: 'film_damage', name: T('Film Damage'), mask: false, keys: 'film damage scratches dust flicker old weave' },
      { t: 'analog_damage', name: T('Analog Damage'), mask: false, keys: 'analog damage vhs tape tracking retro' }
    ];
    function ngTypeMatches(ty, q) {
      if (!q) return true;
      q = q.toLowerCase();
      return ty.name.toLowerCase().indexOf(q) >= 0 || ty.keys.indexOf(q) >= 0 || ty.t.indexOf(q) >= 0;
    }
    // The input a new node takes from a wire dropped on empty space (DefaultLinkPort in
    // EffectNodesMath.h): a mask lands on the mask input, an image on the image input.
    function ngIsMaskProcessor(t) { return t === 'mask_combine' || t === 'mask_feather'; }
    function ngLinkPort(fromNode, ty) {
      if (!fromNode) return 0;
      if (ngIsMaskProcessor(ty.t)) return fromNode.mask ? 0 : -1;
      if (ty.mask) return -1;
      return fromNode.mask ? 1 : 0;
    }

)CFJS"
R"CFJS(
    var NODE_PRESETS = [
      ['color_subject_desat_world', T('Color subject, desaturate world')],
      ['glow_through_fog', T('Glow through the fog')],
      ['player_neon_outline', T('Player neon outline')],
      ['sky_grade_heat_haze', T('Sky grade & heat haze')],
      ['depth_spotlight', T('Depth spotlight')]
    ];
    // A dropdown like the other preset pickers; it shows the preset the graph was last loaded from.
    var nodesPresetDrop = dropdownRow(nodesSec, T('Starter presets'), NODE_PRESETS,
      function (k) { nodesCmd('preset ' + k); }, { placeholder: T('Choose…') });
    var nodesPresetGrid = { sync: function (key) {
      var known = NODE_PRESETS.some(function (it) { return it[0] === key; });
      if (known) nodesPresetDrop.sync(key); else nodesPresetDrop.setText(T('Choose…'), true);
    } };

    var nodesGraphBtn = wideBtn(nodesSec, T('Open Graph Editor'), function () {
      nodesCmd('canvas toggle');
    }, NODES_ACCENT + 'ff');
    nodesGraphBtn.__bgBase = '#b98cff22'; nodesGraphBtn.__brBase = '#b98cff88'; paintBtn(nodesGraphBtn);

    // ---- Add node: one searchable list (After Effects' Effects & Presets) --------------------
    subHead(nodesSec, T('Add node'));
    var nodesAddBox = mk('Panel', nodesSec); nodesAddBox.hittest = true;
    nodesAddBox.style.width = '100%'; nodesAddBox.style.height = '30px'; nodesAddBox.style.flowChildren = 'right';
    nodesAddBox.style.marginTop = '4px'; nodesAddBox.style.paddingLeft = '10px'; nodesAddBox.style.paddingRight = '8px';
    nodesAddBox.style.backgroundColor = '#0d1015ff'; nodesAddBox.style.borderRadius = '3px';
    var nodesAddIcon = lbl(nodesAddBox, '⌕', S.muted, 13); nodesAddIcon.hittest = false;
    nodesAddIcon.style.verticalAlign = 'center'; nodesAddIcon.style.marginRight = '8px';
    var nodesAddText = lbl(nodesAddBox, '', S.muted, 12); nodesAddText.hittest = false;
    nodesAddText.style.verticalAlign = 'center'; nodesAddText.style.width = 'fill-parent-flow(1.0)';
    var nodesAddQuery = '', nodesAddFocus = false;
    function nodesAddPaint() {
      var hot = nodesAddBox.__hot;
      nodesAddBox.style.border = '1px solid ' + (nodesAddFocus ? (NODES_ACCENT + '99') : (hot ? '#ffffff40' : '#ffffff1f'));
      var t = nodesAddQuery ? (nodesAddQuery + (nodesAddFocus ? '|' : '')) : (nodesAddFocus ? '|' : T('Search'));
      if (nodesAddText.text !== t) nodesAddText.text = t;
      nodesAddText.style.color = (nodesAddQuery || nodesAddFocus) ? S.value : S.muted;
    }
    hoverable(nodesAddBox, nodesAddPaint);
    nodesAddBox.SetPanelEvent('onactivate', function () {
      configWindowBlur();
      nodesAddFocus = true; cfgTextOwner = nodesAddBox;
      root.SetAttributeString('textcapture', '1');
      nodesAddPaint();
    });
    // Two to a line, each line split by fill-parent-flow so no cell can wrap under its neighbour.
    // About ten buttons show at once; the rest scroll inside the box (the wheel over it), or the
    // search narrows them.
    var nodesAddWell = scrollWell(nodesSec, 5 * 35 + 22);
    var nodesAddGrid = nodesAddWell.inner;
    nodesAddWell.clip.style.marginTop = '4px';
    function nodesAddFill() {
      nodesAddGrid.RemoveAndDeleteChildren();
      var shown = NODE_TYPES.filter(function (ty) { return ngTypeMatches(ty, nodesAddQuery); });
      var line = null, lastMask = null, col = 0;
      for (var i = 0; i < shown.length; i++) (function (ty, i) {
        if (!nodesAddQuery && lastMask !== ty.mask) {
          lastMask = ty.mask; col = 0;
          var h = lbl(nodesAddGrid, ty.mask ? T('Masks') : T('Effects'), S.muted, 10); h.hittest = false;
          h.style.fontWeight = 'bold'; h.style.letterSpacing = '1px'; h.style.margin = '4px 0px 3px 2px';
        }
        var nextSame = i + 1 < shown.length && (nodesAddQuery || shown[i + 1].mask === ty.mask);
        if (col % 2 === 0) {
          line = mk('Panel', nodesAddGrid); line.hittest = false; line.hittestchildren = true;
          line.style.width = '100%'; line.style.flowChildren = 'right'; line.style.marginBottom = '4px';
        }
        var b = btn(line, '+ ' + ty.name, function () { nodesCmd('add ' + ty.t); }, (ty.mask ? NG_MASK : NG_FX) + 'ff');
        b.style.width = 'fill-parent-flow(1.0)'; b.style.marginRight = (col % 2 === 0 && nextSame) ? '4px' : '0px';
        b.__lbl.style.horizontalAlign = 'center';
        col++;
      })(shown[i], i);
      if (!shown.length) { var none = lbl(nodesAddGrid, T('No results'), S.muted, 12); none.style.marginTop = '4px'; }
      nodesAddWell.reset();
      $.Schedule(0, function () { try { nodesAddWell.apply(); } catch (e) {} });
    }
    nodesAddFill(); nodesAddPaint();

    // ---- The graph's nodes: a list that scrolls inside a fixed box (like Scene Objects) -------
    subHead(nodesSec, T('Graph nodes'));
    var nodesWell = scrollWell(nodesSec, 6 * 32);
    var nodesRows = {}, nodesRowSig = '';
    function nodesRowFor(n) {
      var r = mk('Panel', nodesWell.inner); r.hittest = true;
      r.style.width = '100%'; r.style.height = '28px'; r.style.flowChildren = 'right';
      r.style.paddingLeft = '8px'; r.style.paddingRight = '4px'; r.style.marginBottom = '3px';
      r.style.borderRadius = '3px';
      var tag = lbl(r, '', '#ffffffff', 10); tag.hittest = false;
      tag.style.width = '40px'; tag.style.fontWeight = 'bold'; tag.style.verticalAlign = 'center';
      var nm = lbl(r, '', S.label, 12); nm.hittest = false;
      nm.style.width = 'fill-parent-flow(1.0)'; nm.style.verticalAlign = 'center';
      nm.style.whiteSpace = 'nowrap'; nm.style.textOverflow = 'ellipsis';
      var row = { r: r, tag: tag, nm: nm, tg: null, id: n.id, sig: '' };
      if (n.type !== 'output') {
        row.tg = btn(r, T('On'), function () {
          var cur = ngNodeById(row.id); if (cur) nodesCmd('enable ' + cur.id + ' ' + (cur.enabled ? 0 : 1));
        }, S.label);
        row.tg.style.marginRight = '0px'; row.tg.style.width = '48px'; row.tg.__lbl.style.horizontalAlign = 'center';
        row.tg.style.paddingTop = '2px'; row.tg.style.paddingBottom = '2px'; row.tg.style.height = '22px';
      }
      hoverable(r, function () { row.sig = ''; });
      r.SetPanelEvent('onactivate', function () { nodesCmd('select ' + row.id); });
      return row;
    }
    function nodesListSync(ns) {
      var nodes = ns.nodes || [];
      var setSig = nodes.map(function (n) { return n.id; }).join(',');
      if (setSig !== nodesRowSig) {
        nodesRowSig = setSig;
        nodesWell.inner.RemoveAndDeleteChildren(); nodesRows = {};
        for (var i = 0; i < nodes.length; i++) nodesRows[nodes[i].id] = nodesRowFor(nodes[i]);
        nodesWell.apply();
      }
      for (var j = 0; j < nodes.length; j++) {
        var n = nodes[j], row = nodesRows[n.id]; if (!row) continue;
        var on = n.id === ns.selected, acc = ngAccentOf(n), hot = !!row.r.__hot;
        var sig = [on, hot, n.label, n.enabled, n.type].join('|');
        if (sig === row.sig) continue;
        row.sig = sig;
        row.r.style.backgroundColor = on ? (acc + '28') : (hot ? '#ffffff12' : '#ffffff08');
        row.r.style.border = '1px solid ' + (on ? (acc + 'ff') : '#ffffff12');
        row.tag.text = n.mask ? 'MASK' : (n.type === 'output' ? 'OUT' : 'FX');
        row.tag.style.color = acc + 'ff';
        row.nm.text = n.label; row.nm.style.color = on ? S.value : S.label;
        row.r.style.opacity = n.enabled ? '1' : '0.6';
        if (row.tg) { row.tg.__lbl.text = n.enabled ? T('On') : T('Off'); setBtnOn(row.tg, n.enabled, acc + 'ff'); }
      }
    }
)CFJS"
R"CFJS(
    // ---- Selected node inspector -----------------------------------------------------------
    var nodeInspSec = section(pageNodes, T('NODE INSPECTOR'), true, false, 'nodes.insp', NODES_ACCENT + 'ff');
    var nodeInspHead = mk('Panel', nodeInspSec);
    nodeInspHead.style.width = '100%'; nodeInspHead.style.flowChildren = 'right';
    var nodeInspTitle = lbl(nodeInspHead, T('Select a node'), S.value, 13);
    nodeInspTitle.style.fontWeight = 'bold'; nodeInspTitle.style.width = 'fill-parent-flow(1.0)';
    nodeInspTitle.style.verticalAlign = 'center';
    var nodeInspDup = btn(nodeInspHead, T('Duplicate'), function () {
      var n = nodesSelected(); if (n && n.type !== 'output') nodesCmd('duplicate ' + n.id);
    }, S.label);
    var nodeInspDel = btn(nodeInspHead, T('Delete'), function () {
      var n = nodesSelected(); if (n && n.type !== 'output') nodesCmd('remove ' + n.id);
    }, '#ff8a8aff');
    nodeInspDel.style.marginRight = '0px';

    var nodeParamHost = mk('Panel', nodeInspSec);
    nodeParamHost.style.width = '100%'; nodeParamHost.style.flowChildren = 'down';
    nodeParamHost.style.marginTop = '6px';

    var nodeWireHead = subHead(nodeInspSec, T('Mask input connection'));
    var nodeWireRow = mk('Panel', nodeInspSec);
    nodeWireRow.style.width = '100%'; nodeWireRow.style.flowChildren = 'right-wrap';

    // Parameter schemas per node type: [paramIdx, label, min, max, dec, unit, isBool]
    var NODE_SCHEMAS = {
      depth_range:  [[0, T('Near'), 0, 4000, 0, 'u'], [1, T('Far'), 50, 8000, 0, 'u'], [2, T('Softness'), 1, 1000, 0, 'u'], [3, T('Invert mask'), 0, 1, 0, '', true]],
      sky_mask:     [[0, T('Sky cutoff'), 2000, 28000, 0, 'u'], [1, T('Softness'), 10, 4000, 0, 'u'], [2, T('Invert mask'), 0, 1, 0, '', true]],
      player_matte: [[0, T('Picked player only'), 0, 1, 0, '', true], [2, T('Invert (select world)'), 0, 1, 0, '', true]],
      fog_mask:     [[0, T('Fog start'), 0, 3000, 0, 'u'], [1, T('Fog end'), 100, 8000, 0, 'u'], [2, T('Density'), 0.1, 4, 2, 'x'], [3, T('Invert mask'), 0, 1, 0, '', true]],
      luma_key:     [[0, T('Threshold'), 0, 1, 2, ''], [1, T('Softness'), 0.01, 0.5, 2, ''], [2, T('Invert mask'), 0, 1, 0, '', true]],
      mask_combine: [[0, T('Op (0 Union, 1 Intersect, 2 Sub, 3 Inv)'), 0, 3, 0, '']],
      grade:        [[0, T('Saturation'), 0, 2.5, 2, 'x'], [1, T('Contrast'), 0.2, 2.5, 2, 'x'], [2, T('Exposure'), -2, 2, 2, 'EV'], [3, T('Temperature'), -1, 1, 2, ''], [7, T('Mix'), 0, 1, 2, '']],
      glow:         [[0, T('Threshold'), 0, 1, 2, ''], [1, T('Intensity'), 0, 4, 2, 'x'], [2, T('Radius'), 2, 36, 0, 'px'], [6, T('Mix'), 0, 1, 2, '']],
      outline:      [[0, T('Thickness'), 1, 10, 1, 'px'], [1, T('Edge threshold'), 0.05, 0.8, 2, ''], [5, T('Opacity'), 0, 1, 2, '']],
      distortion:   [[0, T('Mode (0 Rip, 1 Haze, 2 Rad, 3 Chr)'), 0, 3, 0, ''], [1, T('Strength'), 0, 2, 2, ''], [2, T('Scale'), 2, 40, 0, ''], [3, T('Speed'), 0, 10, 1, ''], [4, T('Chromatic'), 0, 2, 2, ''], [5, T('Mix'), 0, 1, 2, '']],
      output:       [[0, T('Master mix'), 0, 1, 2, '']],
      height_mask:  [[0, T('Min height'), -2000, 4000, 0, 'u'], [1, T('Max height'), -2000, 4000, 0, 'u'], [2, T('Softness'), 1, 500, 0, 'u'], [3, T('Invert mask'), 0, 1, 0, '', true]],
      hue_key:      [[0, T('Hue'), 0, 360, 0, '°'], [1, T('Hue range'), 1, 180, 0, '°'], [2, T('Softness'), 0, 90, 0, '°'], [3, T('Min saturation'), 0, 1, 2, ''], [4, T('Invert mask'), 0, 1, 0, '', true]],
      radial_mask:  [[0, T('Center X'), 0, 1, 2, ''], [1, T('Center Y'), 0, 1, 2, ''], [2, T('Radius'), 0, 1.5, 2, ''], [3, T('Softness'), 0, 1, 2, ''], [4, T('Aspect'), 0.2, 4, 2, 'x'], [5, T('Invert mask'), 0, 1, 0, '', true]],
      gradient_mask:[[0, T('Angle'), 0, 360, 0, '°'], [1, T('Position'), 0, 1, 2, ''], [2, T('Softness'), 0, 1, 2, ''], [3, T('Invert mask'), 0, 1, 0, '', true]],
      edge_mask:    [[0, T('Threshold'), 0.005, 0.5, 3, ''], [1, T('Thickness'), 0.5, 6, 1, 'px'], [2, T('Invert mask'), 0, 1, 0, '', true]],
      noise_mask:   [[0, T('Scale'), 0.5, 40, 1, ''], [1, T('Speed'), 0, 5, 2, ''], [2, T('Threshold'), 0, 1, 2, ''], [3, T('Softness'), 0, 1, 2, ''], [4, T('Invert mask'), 0, 1, 0, '', true]],
      player_radius:[[0, T('Radius'), 10, 3000, 0, 'u'], [1, T('Softness'), 1, 2000, 0, 'u'], [2, T('Invert mask'), 0, 1, 0, '', true]],
      mask_feather: [[0, T('Radius'), 0, 40, 1, 'px'], [1, T('Grow'), -1, 1, 2, ''], [2, T('Invert mask'), 0, 1, 0, '', true]],
      blur:         [[0, T('Radius'), 0, 40, 1, 'px'], [1, T('Mix'), 0, 1, 2, '']],
      sharpen:      [[0, T('Amount'), 0, 3, 2, ''], [1, T('Radius'), 0.5, 6, 1, 'px'], [2, T('Mix'), 0, 1, 2, '']],
      vignette:     [[0, T('Strength'), 0, 1, 2, ''], [1, T('Radius'), 0, 1.5, 2, ''], [2, T('Softness'), 0, 1.5, 2, ''], [3, T('Red'), 0, 1, 2, ''], [4, T('Green'), 0, 1, 2, ''], [5, T('Blue'), 0, 1, 2, ''], [6, T('Mix'), 0, 1, 2, '']],
      film_grain:   [[0, T('Amount'), 0, 0.5, 2, ''], [1, T('Size'), 0.5, 6, 1, 'px'], [2, T('Color'), 0, 1, 2, ''], [3, T('Mix'), 0, 1, 2, '']],
      posterize:    [[0, T('Levels'), 2, 16, 0, ''], [1, T('Mix'), 0, 1, 2, '']],
      pixelate:     [[0, T('Size'), 1, 64, 0, 'px'], [1, T('Mix'), 0, 1, 2, '']],
      duotone:      [[0, T('Shadow red'), 0, 1, 2, ''], [1, T('Shadow green'), 0, 1, 2, ''], [2, T('Shadow blue'), 0, 1, 2, ''], [3, T('Highlight red'), 0, 1, 2, ''], [4, T('Highlight green'), 0, 1, 2, ''], [5, T('Highlight blue'), 0, 1, 2, ''], [6, T('Mix'), 0, 1, 2, '']],
      lift_gamma_gain: [[0, T('Lift'), -0.5, 0.5, 2, ''], [1, T('Gamma'), 0.2, 3, 2, ''], [2, T('Gain'), 0, 3, 2, ''], [3, T('Saturation'), 0, 2.5, 2, 'x'], [4, T('Mix'), 0, 1, 2, '']],
      hue_shift:    [[0, T('Hue'), -180, 180, 0, '°'], [1, T('Saturation'), 0, 2.5, 2, 'x'], [2, T('Vibrance'), -1, 1, 2, ''], [3, T('Mix'), 0, 1, 2, '']],
      depth_fog:    [[0, T('Fog start'), 0, 5000, 0, 'u'], [1, T('Fog end'), 50, 20000, 0, 'u'], [2, T('Density'), 0.1, 4, 2, 'x'], [3, T('Red'), 0, 1, 2, ''], [4, T('Green'), 0, 1, 2, ''], [5, T('Blue'), 0, 1, 2, ''], [6, T('Mix'), 0, 1, 2, '']],
      light_rays:   [[0, T('Threshold'), 0, 1, 2, ''], [1, T('Strength'), 0, 3, 2, ''], [2, T('Length'), 0, 1, 2, ''], [3, T('Center X'), 0, 1, 2, ''], [4, T('Center Y'), 0, 1, 2, ''], [5, T('Mix'), 0, 1, 2, '']],
      scanlines:    [[0, T('Lines'), 60, 1200, 0, ''], [1, T('Strength'), 0, 1, 2, ''], [2, T('Curvature'), 0, 1, 2, ''], [3, T('Speed'), 0, 10, 1, ''], [4, T('Mix'), 0, 1, 2, '']],
      glitch:       [[0, T('Strength'), 0, 1, 2, ''], [1, T('Size'), 4, 120, 0, 'px'], [2, T('Speed'), 0.1, 30, 1, ''], [3, T('RGB split'), 0, 2, 2, ''], [4, T('Mix'), 0, 1, 2, '']],
      thermal:      [[0, T('Palette (0 White hot, 1 Black hot, 2 Ironbow, 3 Sepia)'), 0, 3, 0, ''], [1, T('Contrast'), 0.2, 3, 2, 'x'],
                     [2, T('People heat'), 0, 1, 2, ''], [3, T('Noise'), 0, 1, 2, ''], [4, T('Blur'), 0, 6, 1, 'px'],
                     [5, T('Scope (0 Off, 1 Circle, 2 Reticle)'), 0, 2, 0, ''], [6, T('Sway'), 0, 2, 2, ''], [7, T('Mix'), 0, 1, 2, '']],
      night_vision: [[0, T('Gain'), 0.5, 12, 2, 'x'], [1, T('Scintillation'), 0, 1, 2, ''], [2, T('Halo'), 0, 2, 2, ''],
                     [3, T('Tube (0 Full frame, 1 One tube, 2 Two tubes)'), 0, 2, 0, ''], [4, T('White phosphor'), 0, 1, 0, '', true],
                     [5, T('Sway'), 0, 2, 2, ''], [6, T('Mix'), 0, 1, 2, '']],
      ink_lines:    [[0, T('Threshold'), 0.01, 0.6, 2, ''], [1, T('Strength'), 0, 2, 2, ''], [2, T('Paper'), 0, 1, 2, ''], [3, T('Mix'), 0, 1, 2, '']],
      invert:       [[0, T('Mix'), 0, 1, 2, '']],
      halftone:     [[0, T('Size'), 2, 40, 0, 'px'], [1, T('Angle'), 0, 90, 0, '°'], [2, T('Mix'), 0, 1, 2, '']],
      depth_blur:   [[0, T('Focus distance'), 10, 5000, 0, 'u'], [1, T('Focus range'), 10, 3000, 0, 'u'], [2, T('Radius'), 0, 24, 1, 'px'], [3, T('Mix'), 0, 1, 2, '']],
      box_mask:     [[0, T('Center X'), 0, 1, 2, ''], [1, T('Center Y'), 0, 1, 2, ''], [2, T('Width'), 0, 1.5, 2, ''], [3, T('Height'), 0, 1.5, 2, ''], [4, T('Angle'), -180, 180, 0, '°'], [5, T('Softness'), 0, 0.5, 3, ''], [6, T('Invert mask'), 0, 1, 0, '', true]],
      lens_distortion: [[0, T('Distortion'), -1, 1, 2, ''], [1, T('Dispersion'), 0, 0.3, 3, ''], [2, T('Fit to frame'), 0, 1, 0, '', true], [3, T('Mix'), 0, 1, 2, '']],
      directional_blur: [[0, T('Angle'), 0, 360, 0, '°'], [1, T('Length'), 0, 200, 0, 'px'], [2, T('Mix'), 0, 1, 2, '']],
      radial_blur:  [[0, T('Center X'), 0, 1, 2, ''], [1, T('Center Y'), 0, 1, 2, ''], [2, T('Amount'), 0, 1, 2, ''], [3, T('Spin'), 0, 1, 0, '', true], [4, T('Mix'), 0, 1, 2, '']],
      glare:        [[0, T('Type (0 Streaks, 1 Star 4, 2 Star 6, 3 Ghosts, 4 Fog glow)'), 0, 4, 0, ''], [1, T('Threshold'), 0, 1, 2, ''], [2, T('Length'), 0, 0.6, 2, ''], [3, T('Intensity'), 0, 4, 2, 'x'], [4, T('Warmth'), -1, 1, 2, ''], [5, T('Mix'), 0, 1, 2, '']],
      halation:     [[0, T('Threshold'), 0, 1, 2, ''], [1, T('Radius'), 2, 60, 0, 'px'], [2, T('Intensity'), 0, 3, 2, 'x'], [3, T('Red'), 0, 1, 2, ''], [4, T('Green'), 0, 1, 2, ''], [5, T('Blue'), 0, 1, 2, ''], [6, T('Mix'), 0, 1, 2, '']],
      chromatic_aberration: [[0, T('Amount'), 0, 30, 1, 'px'], [1, T('Lateral'), 0, 1, 0, '', true], [2, T('Clear centre'), 0, 1, 0, '', true], [3, T('Mix'), 0, 1, 2, '']],
      kuwahara:     [[0, T('Radius'), 1, 12, 1, 'px'], [1, T('Mix'), 0, 1, 2, '']],
      cel_shade:    [[0, T('Bands'), 2, 8, 0, ''], [1, T('Outline'), 0.5, 6, 1, 'px'], [2, T('Outline strength'), 0, 1, 2, ''], [3, T('Depth edges'), 0.005, 0.3, 3, ''], [4, T('Crease edges'), 0.05, 1, 2, ''], [5, T('Hatching'), 0, 1, 2, ''], [6, T('Saturation'), 0, 2.5, 2, 'x'], [7, T('Mix'), 0, 1, 2, '']],
      refraction:   [[0, T('Strength'), 0, 3, 2, ''], [1, T('Scale'), 0.5, 40, 1, ''], [2, T('Speed'), 0, 5, 2, ''], [3, T('Chromatic'), 0, 2, 2, ''], [4, T('Bend at mask edges'), 0, 1, 0, '', true], [5, T('Mix'), 0, 1, 2, '']],
      film_damage:  [[0, T('Scratches'), 0, 1, 2, ''], [1, T('Dust'), 0, 1, 2, ''], [2, T('Flicker'), 0, 1, 2, ''], [3, T('Gate weave'), 0, 1, 2, ''], [4, T('Age'), 0, 1, 2, ''], [5, T('Mix'), 0, 1, 2, '']],
      tilt_shift:   [[0, T('Focus'), 0, 1, 2, ''], [1, T('Band'), 0, 1, 2, ''], [2, T('Blur'), 0, 30, 1, 'px'], [3, T('Saturation'), 0, 2.5, 2, 'x'], [4, T('Mix'), 0, 1, 2, '']],
      tonemap:      [[0, T('Exposure'), -3, 3, 2, 'EV'], [1, T('Contrast'), 0.2, 2.5, 2, 'x'], [2, T('Curve (0 Filmic, 1 Soft, 2 Punchy)'), 0, 2, 0, ''], [3, T('Mix'), 0, 1, 2, '']],
      split_tone:   [[0, T('Shadow hue'), 0, 360, 0, '°'], [1, T('Shadow amount'), 0, 1, 2, ''], [2, T('Highlight hue'), 0, 360, 0, '°'], [3, T('Highlight amount'), 0, 1, 2, ''], [4, T('Balance'), -1, 1, 2, ''], [5, T('Mix'), 0, 1, 2, '']],
      mirror:       [[0, T('Mode (0 Left to right, 1 Top to bottom, 2 Kaleidoscope)'), 0, 2, 0, ''], [1, T('Segments'), 2, 16, 0, ''], [2, T('Angle'), 0, 360, 0, '°'], [3, T('Mix'), 0, 1, 2, '']],
      analog_damage: [[0, T('Tracking'), 0, 1, 2, ''], [1, T('Noise'), 0, 1, 2, ''], [2, T('Color bleed'), 0, 2, 2, ''], [3, T('Wobble'), 0, 2, 2, ''], [4, T('Mix'), 0, 1, 2, '']],
      color_fill:   [[0, T('Red'), 0, 1, 2, ''], [1, T('Green'), 0, 1, 2, ''], [2, T('Blue'), 0, 1, 2, ''], [3, T('Blend (0 Normal, 1 Multiply, 2 Screen, 3 Overlay, 4 Add)'), 0, 4, 0, ''], [4, T('Mix'), 0, 1, 2, '']]
    };

)CFJS"
R"CFJS(
    var nodeInspBuiltId = -1, nodeInspBuiltType = '', nodeInspControls = [];
    function rebuildNodeInspector(n) {
      nodeParamHost.RemoveAndDeleteChildren();
      nodeInspControls = [];
      nodeInspBuiltId = n ? n.id : -1;
      nodeInspBuiltType = n ? n.type : '';
      if (!n) return;
      var schema = NODE_SCHEMAS[n.type] || [];
      for (var i = 0; i < schema.length; i++) (function (sc) {
        var idx = sc[0], lblText = sc[1], mn = sc[2], mx = sc[3], dec = sc[4], unit = sc[5], isBool = !!sc[6];
        if (isBool) {
          var tp = togglePill(nodeParamHost, lblText, '', function () {
            var cur = nodesSelected();
            if (!cur) return;
            nodesCmd('param ' + cur.id + ' ' + idx + ' ' + ((cur.p[idx] > 0.5) ? 0 : 1));
          });
          nodeInspControls.push({ idx: idx, boolCtrl: tp });
        } else {
          var sr = sliderRow(nodeParamHost, {
            label: lblText, min: mn, max: mx, decimals: dec, unit: unit,
            onChange: function (v) {
              var cur = nodesSelected();
              if (cur) nodesCmd('param ' + cur.id + ' ' + idx + ' ' + v);
            }
          });
          nodeInspControls.push({ idx: idx, sliderCtrl: sr });
        }
      })(schema[i]);
    }

    // Mask input: Full frame plus one button per mask node. Built when the selection or the set of
    // masks changes, and only restyled when the wire changes -- rebuilding on the click itself made
    // every button blink to its plain ground for a frame (user report).
    var nodeWireSig = '', nodeWireBtns = [];
    function nodeWireSync(ns, sel) {
      var nodes = ns.nodes || [], edges = ns.edges || [];
      var wants = !!(sel && !sel.mask && sel.type !== 'output');
      var masks = nodes.filter(function (q) { return q.mask; });
      var sig = (wants ? sel.id : -1) + '|' + masks.map(function (q) { return q.id + ':' + q.label; }).join(',');
      if (sig !== nodeWireSig) {
        nodeWireSig = sig;
        nodeWireRow.RemoveAndDeleteChildren(); nodeWireBtns = [];
        if (wants) {
          var bNone = btn(nodeWireRow, T('Full frame'), function () {
            var cur = nodesSelected(), e = cur && ngEdgeInto(cur.id, 1);
            if (e) nodesCmd('disconnect ' + e.id);
          }, S.label);
          bNone.style.marginBottom = '4px'; bNone.__maskId = -1; nodeWireBtns.push(bNone);
          masks.forEach(function (mn) {
            var bm = btn(nodeWireRow, mn.label, function () {
              var cur = nodesSelected(); if (cur) nodesCmd('connect ' + mn.id + ' ' + cur.id + ' 1');
            }, S.label);
            bm.style.marginBottom = '4px'; bm.__maskId = mn.id; nodeWireBtns.push(bm);
          });
        }
      }
      nodeWireHead.visible = wants; nodeWireRow.visible = wants;
      if (!wants) return;
      var cur = null;
      for (var i = 0; i < edges.length; i++) if (edges[i].to === sel.id && edges[i].toPort === 1) cur = edges[i];
      nodeWireBtns.forEach(function (b) { setBtnOn(b, cur ? b.__maskId === cur.from : b.__maskId === -1, NG_MASK + 'ff'); });
    }
)CFJS"
R"CFJS(
    // ===================== GRAPH EDITOR WINDOW =====================================
    var NG_TITLE_H = 30, NG_GRIP = 18, NODE_W = 176, NODE_HDR = 24, NODE_H = 80, NG_PIN = 12;
    var ngWin = null, ngTitleBar = null, ngCanvas = null, ngWireLayer = null, ngNodeLayer = null, ngLiveLayer = null;
    var ngGrip = null, ngNoteLbl = null, ngTitleBtns = [];
    var ngRect = null, ngRectSig = '', ngStoredSeen = null, ngPendingStore = null, ngPendingFrames = 0, ngPubRect = null;
    // The graph's view: pan in graph units, and a zoom applied to the wire, node and live layers
    // (scale about their top-left), so a canvas point is graph = point / zoom - pan.
    var ngPan = { x: 0, y: 0 }, ngFramed = false, ngZoom = 1, ngZoomApplied = -1;
    var ngCards = {}, ngNodes = [], ngEdges = [], ngSelected = -1;
    var ngGesture = null, ngLastLmb = false, ngLastRmb = false, ngPendingFrom = -1;
    var ngDragPos = {};               // id -> {x, y, until}: where a dropped node shows until the state agrees
    var ngHotPin = '', ngWireSig = '', ngWirePolys = [];
    var ngNoteSeq = -1, ngNoteUntil = 0, ngLiveSegs = [];

    function ngNodeById(id) {
      for (var i = 0; i < ngNodes.length; i++) if (ngNodes[i].id === id) return ngNodes[i];
      return null;
    }
    function ngEdgeInto(id, port) {
      for (var i = 0; i < ngEdges.length; i++) if (ngEdges[i].to === id && ngEdges[i].toPort === port) return ngEdges[i];
      return null;
    }
    function ngPosOf(n) {
      var d = ngDragPos[n.id];
      if (ngGesture && ngGesture.kind === 'node' && ngGesture.id === n.id) return { x: ngGesture.nx, y: ngGesture.ny };
      return d ? d : { x: n.x, y: n.y };
    }
    // Ports a node takes, and where its pins sit (graph coordinates).
    function ngInputs(n) {
      if (n.type === 'output' || n.type === 'mask_feather') return [0];
      if (n.type === 'mask_combine') return [0, 1];
      return n.mask ? [] : [0, 1];
    }
    function ngPortLabel(n, port) {
      if (n.type === 'mask_combine') return port === 0 ? T('Mask A') : T('Mask B');
      if (n.type === 'mask_feather') return T('Mask');
      return port === 0 ? T('Image') : T('Mask');
    }
    function ngPortIsMask(n, port) { return ngIsMaskProcessor(n.type) || (n.type !== 'output' && port === 1); }
    function ngPinAt(n, which) {
      var p = ngPosOf(n);
      if (which === 'out') return { x: p.x + NODE_W, y: p.y + NODE_HDR + 18 };
      return { x: p.x, y: p.y + NODE_HDR + (which === 1 ? 40 : 18) };
    }

    function ngBuild() {
      if (ngWin && cfgValid(ngWin)) return;
      ngCards = {}; ngWireSig = ''; ngRectSig = ''; ngLiveSegs = [];
      ngWin = mk('Panel', cfgOverlay()); ngWin.hittest = true; ngWin.hittestchildren = true;
      ngWin.visible = false; ngWin.style.zIndex = '0';
      // See-through, so the shot under the graph stays visible; the cards and title bar stay solid.
      ngWin.style.backgroundColor = 'rgba(11,14,19,0.45)'; ngWin.style.border = '1px solid ' + NODES_ACCENT + '66';
      ngWin.style.borderRadius = '6px'; ngWin.style.flowChildren = 'down'; ngWin.style.overflow = 'clip';
      ngWin.style.boxShadow = '#000000cc 0px 10px 30px 0px';
      ngWin.SetPanelEvent('onactivate', function () {});

      ngTitleBar = mk('Panel', ngWin); ngTitleBar.hittest = true; ngTitleBar.hittestchildren = true;
      ngTitleBar.style.width = '100%'; ngTitleBar.style.height = NG_TITLE_H + 'px'; ngTitleBar.style.flowChildren = 'right';
      ngTitleBar.style.paddingLeft = '12px'; ngTitleBar.style.paddingRight = '6px';
      ngTitleBar.style.backgroundColor = '#141820ff'; ngTitleBar.style.borderBottom = '1px solid #ffffff1a';
      var tl = lbl(ngTitleBar, T('EFFECT NODE GRAPH'), NODES_ACCENT + 'ff', 12); tl.hittest = false;
      tl.style.fontWeight = 'bold'; tl.style.letterSpacing = '1.6px'; tl.style.verticalAlign = 'center';
      ngNoteLbl = lbl(ngTitleBar, '', '#ff9a8aff', 12); ngNoteLbl.hittest = false;
      ngNoteLbl.style.verticalAlign = 'center'; ngNoteLbl.style.marginLeft = '14px';
      ngNoteLbl.style.width = 'fill-parent-flow(1.0)'; ngNoteLbl.style.whiteSpace = 'nowrap'; ngNoteLbl.style.textOverflow = 'ellipsis';
      function tbtn(text, fn) {
        var b = btn(ngTitleBar, text, fn, S.label);
        b.style.paddingTop = '3px'; b.style.paddingBottom = '3px'; b.style.marginRight = '4px';
        ngTitleBtns.push(b); return b;
      }
      ngTitleBtns = [];
      tbtn('+ ' + T('Add node'), function () { ngOpenAddMenu(ngCanvasCentre(), -1); });
      tbtn(T('Frame all'), function () { ngFrameAll(); });
      tbtn('✕', function () { nodesCmd('canvas close'); }).style.marginRight = '0px';

      ngCanvas = mk('Panel', ngWin); ngCanvas.hittest = true; ngCanvas.hittestchildren = false;
      ngCanvas.style.width = '100%'; ngCanvas.style.height = 'fill-parent-flow(1.0)'; ngCanvas.style.overflow = 'clip';
      ngCanvas.style.backgroundColor = 'rgba(8,10,14,0.35)';
      ngWireLayer = mk('Panel', ngCanvas); ngWireLayer.hittest = false;
      ngWireLayer.style.width = '100%'; ngWireLayer.style.height = '100%';
      ngNodeLayer = mk('Panel', ngCanvas); ngNodeLayer.hittest = false;
      ngNodeLayer.style.width = '100%'; ngNodeLayer.style.height = '100%';
      ngLiveLayer = mk('Panel', ngCanvas); ngLiveLayer.hittest = false;
      ngLiveLayer.style.width = '100%'; ngLiveLayer.style.height = '100%';
      // Scaled about the top-left, and never clipped to their own (scaled) box: the canvas clips.
      // Panorama clips children to a panel's own layout box before its transform applies, so a
      // layer the size of the canvas, scaled down, cut the graph off at the scaled edge. The
      // layers are far larger than any graph; the canvas does the clipping.
      [ngWireLayer, ngNodeLayer, ngLiveLayer].forEach(function (l) {
        l.style.transformOrigin = '0% 0%'; l.style.overflow = 'noclip noclip';
        l.style.width = '12000px'; l.style.height = '12000px';
      });
      ngZoomApplied = -1;
      ngGrip = mk('Panel', ngCanvas); ngGrip.hittest = false;
      ngGrip.style.width = NG_GRIP + 'px'; ngGrip.style.height = NG_GRIP + 'px';
      ngGrip.style.horizontalAlign = 'right'; ngGrip.style.verticalAlign = 'bottom';
      var gl = lbl(ngGrip, '◢', '#ffffff40', 13); gl.hittest = false;
      gl.style.horizontalAlign = 'right'; gl.style.verticalAlign = 'bottom';
    }

    // ---- window rect: default, stored, applied -----------------------------------------------
    function ngDefaultRect() {
      var sc = cfgScreen();
      var w = Math.max(520, Math.min(1100, Math.round(sc.rw - INSPECTOR_W - 60)));
      var h = Math.max(320, Math.min(620, Math.round(sc.rh - 300)));
      return { x: 24, y: 60, w: w, h: h };
    }
    function ngClampRect(r) {
      var sc = cfgScreen();
      r.w = Math.max(420, Math.min(r.w, sc.rw - 20)); r.h = Math.max(260, Math.min(r.h, sc.rh - 20));
      r.x = Math.max(-r.w + 120, Math.min(r.x, sc.rw - 120)); r.y = Math.max(0, Math.min(r.y, sc.rh - NG_TITLE_H));
      return r;
    }
    function ngReadStored(s) {
      var v = s && s.ui && s.ui['nodes.graph'];
      // Our own write is in flight: until it arrives the state still holds the rect from before
      // the drag, and reading that back made the window jump back and forth on release.
      if (ngPendingStore !== null) {
        if (v === ngPendingStore || ++ngPendingFrames > 240) { ngStoredSeen = ngPendingStore; ngPendingStore = null; }
        return null;
      }
      if (!v || v === ngStoredSeen) return null;
      ngStoredSeen = v;
      var p = String(v).split('_').map(function (q) { return parseFloat(q); });
      if (p.length < 4 || p.some(function (q) { return !isFinite(q); })) return null;
      return { x: p[0], y: p[1], w: p[2], h: p[3] };
    }
    function ngPersistRect() {
      var v = [ngRect.x, ngRect.y, ngRect.w, ngRect.h].map(function (q) { return Math.round(q); }).join('_');
      ngStoredSeen = v; ngPendingStore = v; ngPendingFrames = 0;
      cmd('mirv_filmmaker settings ui nodes.graph ' + v);
    }
    function ngApplyRect() {
      var sig = [ngRect.x, ngRect.y, ngRect.w, ngRect.h].map(Math.round).join(',');
      if (sig === ngRectSig) return;
      ngRectSig = sig;
      ngWin.style.position = Math.round(ngRect.x) + 'px ' + Math.round(ngRect.y) + 'px 0px';
      ngWin.style.width = Math.round(ngRect.w) + 'px'; ngWin.style.height = Math.round(ngRect.h) + 'px';
    }
)CFJS"
R"CFJS(
    // ---- coordinates -------------------------------------------------------------------------
    // st.mx / st.my are window pixels; the window and canvas are laid out in overlay px.
    function ngScale() { return (ngWin && ngWin.actualuiscale_x) || 1; }
    function ngCanvasSize() {
      var s = ngScale();
      return { w: (ngCanvas.actuallayoutwidth || 0) / s, h: (ngCanvas.actuallayoutheight || 0) / s };
    }
    function ngToCanvas(mx, my) {
      var p = ngCanvas.GetPositionWithinWindow ? ngCanvas.GetPositionWithinWindow() : null;
      if (!p) return null;
      var s = ngScale();
      return { x: (mx - p.x) / s, y: (my - p.y) / s };
    }
    function ngCanvasCentre() {
      var c = ngCanvasSize();
      return { x: Math.round(c.w / 2 / ngZoom - ngPan.x - NODE_W / 2), y: Math.round(c.h / 2 / ngZoom - ngPan.y - NODE_H / 2) };
    }
    function ngInPanel(p, mx, my) {
      if (!p || !cfgValid(p) || !p.visible || !p.GetPositionWithinWindow) return false;
      var q = p.GetPositionWithinWindow();
      return !!q && mx >= q.x && mx < q.x + (p.actuallayoutwidth || 0) && my >= q.y && my < q.y + (p.actuallayoutheight || 0);
    }
    function ngFrameAll() {
      if (!ngNodes.length || !ngCanvas) return;
      var c = ngCanvasSize(); if (!(c.w > 0)) return;
      var x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
      ngNodes.forEach(function (n) { var p = ngPosOf(n); x0 = Math.min(x0, p.x); y0 = Math.min(y0, p.y); x1 = Math.max(x1, p.x + NODE_W); y1 = Math.max(y1, p.y + NODE_H); });
      // Zoom out until the whole graph fits with a margin (never in past 100%), then centre it.
      var margin = 40;
      ngZoom = Math.max(0.3, Math.min(1, (c.w - margin) / Math.max(x1 - x0, 1), (c.h - margin) / Math.max(y1 - y0, 1)));
      ngPan.x = Math.round((c.w / ngZoom - (x1 - x0)) / 2 - x0);
      ngPan.y = Math.round((c.h / ngZoom - (y1 - y0)) / 2 - y0);
      ngFramed = true;
      ngApplyZoom();
    }
    function ngApplyZoom() {
      if (!ngNodeLayer || ngZoomApplied === ngZoom) return;
      ngZoomApplied = ngZoom;
      var t = ngZoom === 1 ? 'none' : ('scale3d(' + ngZoom.toFixed(4) + ',' + ngZoom.toFixed(4) + ',1)');
      [ngWireLayer, ngNodeLayer, ngLiveLayer].forEach(function (l) { if (l && cfgValid(l)) l.style.transform = t; });
    }

    // What lies under a canvas point (graph coordinates): a pin, a node, a wire, or nothing.
    function ngHit(g) {
      for (var i = ngNodes.length - 1; i >= 0; i--) {
        var n = ngNodes[i];
        if (n.type !== 'output') {
          var o = ngPinAt(n, 'out');
          if (Math.abs(g.x - o.x) <= NG_PIN / ngZoom && Math.abs(g.y - o.y) <= NG_PIN / ngZoom) return { kind: 'out', n: n };
        }
        var ins = ngInputs(n);
        for (var k = 0; k < ins.length; k++) {
          var ip = ngPinAt(n, ins[k]);
          if (Math.abs(g.x - ip.x) <= NG_PIN / ngZoom && Math.abs(g.y - ip.y) <= NG_PIN / ngZoom) return { kind: 'in', n: n, port: ins[k] };
        }
      }
      for (var j = ngNodes.length - 1; j >= 0; j--) {
        var m = ngNodes[j], p = ngPosOf(m);
        if (g.x >= p.x && g.x <= p.x + NODE_W && g.y >= p.y && g.y <= p.y + NODE_H) return { kind: 'node', n: m };
      }
      for (var w = 0; w < ngWirePolys.length; w++) {
        var pts = ngWirePolys[w].pts;
        for (var s2 = 0; s2 + 1 < pts.length; s2++) if (ngSegDist(g, pts[s2], pts[s2 + 1]) < 6 / ngZoom) return { kind: 'wire', e: ngWirePolys[w].e };
      }
      return null;
    }
    function ngSegDist(p, a, b) {
      var dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
      var t = l2 > 0 ? Math.max(0, Math.min(1, ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2)) : 0;
      var qx = a.x + t * dx - p.x, qy = a.y + t * dy - p.y;
      return Math.sqrt(qx * qx + qy * qy);
    }
    // A wire is a horizontal-tangent bezier, drawn as short rotated bars.
    function ngCurve(a, b) {
      var k = Math.max(40, Math.abs(b.x - a.x) * 0.5), pts = [];
      for (var i = 0; i <= 16; i++) {
        var t = i / 16, u = 1 - t;
        pts.push({ x: u * u * u * a.x + 3 * u * u * t * (a.x + k) + 3 * u * t * t * (b.x - k) + t * t * t * b.x,
                   y: u * u * u * a.y + 3 * u * u * t * a.y + 3 * u * t * t * b.y + t * t * t * b.y });
      }
      return pts;
    }
    function ngBar(p, a, b, color, w) {
      var dx = b.x - a.x, dy = b.y - a.y, len = Math.sqrt(dx * dx + dy * dy);
      p.style.position = (a.x + ngPan.x).toFixed(1) + 'px ' + (a.y + ngPan.y - w / 2).toFixed(1) + 'px 0px';
      p.style.width = (len + 1).toFixed(1) + 'px'; p.style.height = w + 'px';
      p.style.transform = 'rotateZ(' + (Math.atan2(dy, dx) * 180 / Math.PI).toFixed(2) + 'deg)';
      p.style.backgroundColor = color;
    }
    function ngDrawWires() {
      var parts = [ngPan.x, ngPan.y, ngHotPin];
      ngEdges.forEach(function (e) {
        var a = ngNodeById(e.from), b = ngNodeById(e.to);
        if (a && b) { var pa = ngPosOf(a), pb = ngPosOf(b); parts.push(e.id + ':' + pa.x + ',' + pa.y + '>' + pb.x + ',' + pb.y + ':' + e.toPort + ':' + a.enabled); }
      });
      var sig = parts.join('|');
      if (sig === ngWireSig) return;
      ngWireSig = sig;
      ngWireLayer.RemoveAndDeleteChildren(); ngWirePolys = [];
      ngEdges.forEach(function (e) {
        var a = ngNodeById(e.from), b = ngNodeById(e.to);
        if (!a || !b) return;
        var pts = ngCurve(ngPinAt(a, 'out'), ngPinAt(b, e.toPort));
        var col = (a.mask ? NG_MASK : NG_FX) + (a.enabled ? 'dd' : '66');
        for (var i = 0; i + 1 < pts.length; i++) {
          var seg = mk('Panel', ngWireLayer); seg.hittest = false; seg.style.transformOrigin = '0% 50%';
          ngBar(seg, pts[i], pts[i + 1], col, 2);
        }
        ngWirePolys.push({ e: e, pts: pts });
      });
    }
    function ngDrawLive(a, b, color) {
      var pts = a ? ngCurve(a, b) : [];
      while (ngLiveSegs.length < 16) {
        var seg = mk('Panel', ngLiveLayer); seg.hittest = false; seg.style.transformOrigin = '0% 50%';
        ngLiveSegs.push(seg);
      }
      for (var i = 0; i < ngLiveSegs.length; i++) {
        var on = i + 1 < pts.length;
        ngLiveSegs[i].visible = on;
        if (on) ngBar(ngLiveSegs[i], pts[i], pts[i + 1], color, 2);
      }
    }
)CFJS"
R"CFJS(
    // ---- node cards: built once per node, restyled and moved in place ------------------------
    function ngCardFor(n) {
      var c = ngCards[n.id];
      if (c && cfgValid(c.card)) return c;
      var card = mk('Panel', ngNodeLayer); card.hittest = false;
      card.style.width = NODE_W + 'px'; card.style.height = NODE_H + 'px';
      card.style.borderRadius = '5px'; card.style.overflow = 'noclip';
      var hdr = mk('Panel', card); hdr.hittest = false; hdr.style.width = '100%'; hdr.style.height = NODE_HDR + 'px';
      hdr.style.flowChildren = 'right'; hdr.style.paddingLeft = '8px'; hdr.style.paddingRight = '8px';
      hdr.style.borderRadius = '5px 5px 0px 0px';
      var title = lbl(hdr, '', S.value, 12); title.hittest = false; title.style.fontWeight = 'bold';
      title.style.verticalAlign = 'center'; title.style.width = 'fill-parent-flow(1.0)';
      title.style.whiteSpace = 'nowrap'; title.style.textOverflow = 'ellipsis';
      var kind = lbl(hdr, '', S.muted, 9); kind.hittest = false; kind.style.verticalAlign = 'center'; kind.style.fontWeight = 'bold';
      function pinPanel(x, y) {
        var p = mk('Panel', card); p.hittest = false;
        p.style.width = '11px'; p.style.height = '11px'; p.style.borderRadius = '6px';
        p.style.position = (x - 5.5) + 'px ' + (y - 5.5) + 'px 0px';
        return p;
      }
      function portLbl(x, y, right) {
        var l = lbl(card, '', S.label, 11); l.hittest = false;
        l.style.width = (NODE_W / 2 - 14) + 'px'; l.style.height = '16px';
        l.style.horizontalAlign = 'left'; l.style.verticalAlign = 'top';
        l.style.position = x + 'px ' + (y - 8) + 'px 0px';
        if (right) l.style.textAlign = 'right';
        return l;
      }
      c = { card: card, hdr: hdr, title: title, kind: kind, sig: '', pos: '',
            inL: [portLbl(11, NODE_HDR + 18), portLbl(11, NODE_HDR + 40)],
            inP: [pinPanel(0, NODE_HDR + 18), pinPanel(0, NODE_HDR + 40)],
            outL: portLbl(NODE_W / 2 + 3, NODE_HDR + 18, true), outP: pinPanel(NODE_W, NODE_HDR + 18) };
      ngCards[n.id] = c;
      return c;
    }
    function ngPaintCard(n) {
      var c = ngCardFor(n), acc = ngAccentOf(n), sel = n.id === ngSelected;
      var pending = ngPendingFrom === n.id || (ngGesture && ngGesture.kind === 'wire' && ngGesture.from === n.id);
      // The inputs' wired state is part of it: a connect from the inspector or a disconnect from a
      // wire's menu changes nothing else on the card, and the pin has to fill or empty with it.
      var wired = (ngEdgeInto(n.id, 0) ? 1 : 0) + (ngEdgeInto(n.id, 1) ? 2 : 0);
      var sig = [n.label, n.type, n.enabled, sel, pending, wired, ngHotPin.indexOf(n.id + ':') === 0 ? ngHotPin : ''].join('|');
      if (sig !== c.sig) {
        c.sig = sig;
        c.card.style.backgroundColor = sel ? '#1c2230ff' : '#141922ff';
        c.card.style.border = (sel ? 2 : 1) + 'px solid ' + acc + (sel ? 'ff' : '66');
        c.card.style.opacity = n.enabled ? '1.0' : '0.5';
        c.hdr.style.backgroundColor = acc + (sel ? '40' : '26');
        c.title.text = n.label;
        c.kind.text = n.type === 'output' ? 'OUT' : (n.mask ? 'MASK' : 'FX');
        c.kind.style.color = acc + 'ff';
        var ins = ngInputs(n);
        for (var k = 0; k < 2; k++) {
          var has = ins.indexOf(k) >= 0, hot = ngHotPin === n.id + ':in:' + k;
          c.inL[k].visible = has; c.inP[k].visible = has;
          if (!has) continue;
          var pc = ngPortIsMask(n, k) ? NG_MASK : NG_FX;
          c.inL[k].text = ngPortLabel(n, k);
          c.inP[k].style.backgroundColor = hot ? '#ffffffff' : (ngEdgeInto(n.id, k) ? pc + 'ff' : '#0b0e13ff');
          c.inP[k].style.border = '2px solid ' + pc + 'ff';
        }
        var hasOut = n.type !== 'output', hotOut = ngHotPin === n.id + ':out';
        c.outL.visible = hasOut; c.outP.visible = hasOut;
        if (hasOut) {
          c.outL.text = T('Out');
          c.outP.style.backgroundColor = (hotOut || pending) ? '#ffffffff' : acc + 'ff';
          c.outP.style.border = '2px solid ' + acc + 'ff';
        }
      }
      var p = ngPosOf(n), pos = Math.round(p.x + ngPan.x) + 'px ' + Math.round(p.y + ngPan.y) + 'px 0px';
      if (pos !== c.pos) { c.pos = pos; c.card.style.position = pos; }
    }
    function ngSyncCards() {
      var live = {};
      ngNodes.forEach(function (n) { live[n.id] = 1; ngPaintCard(n); });
      Object.keys(ngCards).forEach(function (id) {
        if (live[id]) return;
        try { ngCards[id].card.DeleteAsync(0); } catch (e) {}
        delete ngCards[id];
      });
    }
)CFJS"
R"CFJS(
    // ---- menus -------------------------------------------------------------------------------
    function ngNodeMenu(n) {
      if (n.type === 'output') {
        cfgMenu([[T('Disconnect'), function () { nodesCmd('unlink ' + n.id); }]]);
        return;
      }
      cfgMenu([
        [n.enabled ? T('Off') : T('On'), function () { nodesCmd('enable ' + n.id + ' ' + (n.enabled ? 0 : 1)); }],
        [T('Duplicate'), function () { nodesCmd('duplicate ' + n.id); }, 'Ctrl+D'],
        [T('Disconnect'), function () { nodesCmd('unlink ' + n.id); }],
        null,
        [T('Delete'), function () { nodesCmd('remove ' + n.id); }, 'Del', true]
      ]);
    }
    // The add menu: a search line over every node type that fits, at the cursor (Nuke's Tab menu,
    // Blender's Shift+A search). `at` is where the node goes (graph coordinates); `linkFrom` the
    // node whose wire was dropped there, or -1.
    var ngAdd = null;
    function ngOpenAddMenu(at, linkFrom) {
      cfgDropdownClose(); configWindowBlur();
      var list = mk('Panel', cfgDropLayer()); list.hittest = true; list.hittestchildren = true;
      list.style.flowChildren = 'down'; list.style.width = '236px';
      list.style.backgroundColor = '#11151bfa'; list.style.border = '1px solid ' + NODES_ACCENT + '66';
      list.style.borderRadius = '4px'; list.style.padding = '4px';
      list.style.boxShadow = '#000000b3 0px 6px 18px 0px';
      list.SetPanelEvent('onactivate', function () {});
      var sbox = mk('Panel', list); sbox.hittest = false; sbox.style.width = '100%'; sbox.style.height = '28px';
      sbox.style.flowChildren = 'right'; sbox.style.paddingLeft = '8px'; sbox.style.marginBottom = '4px';
      sbox.style.backgroundColor = '#0d1015ff'; sbox.style.borderRadius = '3px';
      var si = lbl(sbox, '⌕', S.muted, 13); si.hittest = false; si.style.verticalAlign = 'center'; si.style.marginRight = '8px';
      var sl = lbl(sbox, '', S.value, 12); sl.hittest = false; sl.style.verticalAlign = 'center';
      // Ten rows show; the rest scroll with the wheel over the menu (ngWheel), or typing narrows them.
      var clip = mk('Panel', list); clip.hittest = false; clip.hittestchildren = true;
      clip.style.width = '100%'; clip.style.overflow = 'clip'; clip.style.flowChildren = 'down';
      var host = mk('Panel', clip); host.hittest = false; host.hittestchildren = true;
      host.style.width = '100%'; host.style.flowChildren = 'down';
      host.style.height = 'fit-children'; host.style.overflow = 'noclip noclip';
      var menu = { field: list, list: list, __menu: true, __lastLmb: true,
        __setOpen: function (open) {
          if (open) return;
          try { list.DeleteAsync(0); } catch (e) {}
          if (ngAdd && ngAdd.list === list) { ngAdd = null; root.SetAttributeString('textcapture', '0'); }
        } };
      ngAdd = { list: list, sl: sl, clip: clip, host: host, q: '', at: at, link: linkFrom, first: null, y: 0, h: 0 };
      cfgDropOpen = menu;
      cfgTextOwner = list; root.SetAttributeString('textcapture', '1');
      ngAddFill();
      try {
        var rp = root.GetPositionWithinWindow();
        var sx = root.actualuiscale_x || 1, sy = root.actualuiscale_y || 1;
        var x = (cfgCursorX - rp.x) / sx + 2, y = (cfgCursorY - rp.y) / sy + 2;
        var rw = (root.actuallayoutwidth || 0) / sx, rh = (root.actuallayoutheight || 0) / sy;
        if (rw > 0 && x + 240 > rw) x = rw - 240;
        if (rh > 0 && y + 360 > rh) y = rh - 360;
        list.style.position = Math.round(Math.max(0, x)) + 'px ' + Math.round(Math.max(0, y)) + 'px 0px';
      } catch (e) {}
    }
    function ngAddFill() {
      var a = ngAdd; if (!a) return;
      a.sl.text = a.q ? (a.q + '|') : '|';
      a.host.RemoveAndDeleteChildren(); a.first = null; a.y = 0; a.h = 0;
      var from = a.link > 0 ? ngNodeById(a.link) : null;
      var shown = NODE_TYPES.filter(function (ty) { return ngTypeMatches(ty, a.q) && (!from || ngLinkPort(from, ty) >= 0); });
      var lastMask = null;
      shown.forEach(function (ty) {
        if (!a.q && lastMask !== ty.mask) {
          lastMask = ty.mask;
          var h = lbl(a.host, ty.mask ? T('Masks') : T('Effects'), S.muted, 10); h.hittest = false;
          h.style.fontWeight = 'bold'; h.style.letterSpacing = '1px'; h.style.margin = '4px 0px 2px 8px';
          h.style.height = '14px'; a.h += 20;
        }
        var o = mk('Panel', a.host); o.hittest = true;
        o.style.flowChildren = 'right'; o.style.width = '100%'; o.style.height = '26px';
        a.h += 26;
        o.style.paddingLeft = '8px'; o.style.borderRadius = '3px';
        var dot = mk('Panel', o); dot.hittest = false; dot.style.width = '8px'; dot.style.height = '8px';
        dot.style.borderRadius = '4px'; dot.style.verticalAlign = 'center'; dot.style.marginRight = '9px';
        dot.style.backgroundColor = (ty.mask ? NG_MASK : NG_FX) + 'ff';
        var cap = lbl(o, ty.name, S.label, 13); cap.hittest = false; cap.style.verticalAlign = 'center';
        if (!a.first) { a.first = ty; o.style.backgroundColor = '#ffffff10'; }
        o.SetPanelEvent('onmouseover', function () { o.style.backgroundColor = '#ffffff1a'; });
        o.SetPanelEvent('onmouseout', function () { o.style.backgroundColor = (a.first === ty) ? '#ffffff10' : S.clear; });
        o.SetPanelEvent('onactivate', function () { ngAddPick(ty); });
      });
      if (!shown.length) { var none = lbl(a.host, T('No results'), S.muted, 12); none.hittest = false; none.style.margin = '4px 8px'; a.h += 26; }
      ngAddScroll(0);
    }
    var NG_ADD_ROWS = 10;
    function ngAddScroll(dy) {
      var a = ngAdd; if (!a) return;
      var view = Math.min(a.h, NG_ADD_ROWS * 26);
      a.y = Math.max(0, Math.min(a.y + dy, a.h - view));
      a.clip.style.height = view + 'px';
      a.host.style.marginTop = (-Math.round(a.y)) + 'px';
    }
    function ngAddPick(ty) {
      var a = ngAdd; if (!a) return;
      cfgDropdownClose();
      nodesCmd('add ' + ty.t + ' ' + Math.round(a.at.x) + ' ' + Math.round(a.at.y) + (a.link > 0 ? ' link ' + a.link : ''));
    }
    // Typing: the open add menu takes the keys first, then the tab's own search box.
    var ngPrevChars = configWindowChars, ngPrevBlur = configWindowBlur;
    function ngTypeInto(q, csv, onEnter, onEsc) {
      var parts = String(csv || '').split(',');
      for (var i = 0; i < parts.length; i++) {
        var c = parseInt(parts[i], 10);
        if (c === 13) { onEnter(); return null; }
        if (c === 27) { onEsc(); return null; }
        if (c === 8) q = q.slice(0, -1);
        else if (c >= 32 && c !== 127 && q.length < 24) q += String.fromCharCode(c);
      }
      return q;
    }
    configWindowChars = function (csv) {
      if (ngAdd) {
        var q = ngTypeInto(ngAdd.q, csv, function () { if (ngAdd && ngAdd.first) ngAddPick(ngAdd.first); }, cfgDropdownClose);
        if (q !== null && ngAdd) { ngAdd.q = q; ngAddFill(); }
        return;
      }
      if (nodesAddFocus) {
        var q2 = ngTypeInto(nodesAddQuery, csv, configWindowBlur, function () { nodesAddQuery = ''; configWindowBlur(); nodesAddFill(); });
        if (q2 !== null && q2 !== nodesAddQuery) { nodesAddQuery = q2; nodesAddFill(); }
        nodesAddPaint();
        return;
      }
      ngPrevChars(csv);
    };
    configWindowBlur = function () {
      if (ngAdd) cfgDropdownClose();
      if (nodesAddFocus) { nodesAddFocus = false; nodesAddPaint(); }
      ngPrevBlur();
    };
    // Delete removes and Ctrl+D duplicates the selected node while NODES is the tab on screen.
    keyHooks.push(function (name) {
      if (activeTab !== 'nodes') return;
      var n = nodesSelected(); if (!n || n.type === 'output') return;
      if (name === 'delete') nodesCmd('remove ' + n.id);
      else if (name === 'duplicate') nodesCmd('duplicate ' + n.id);
    });
    // The wheel over the graph, as in Blender and Nuke: it zooms about the pointer (25% to 200%);
    // Shift+wheel pans sideways and Ctrl+wheel up and down. It never scrolls the column under the
    // window. Over the open add menu it scrolls the menu. From api.wheel.
    function ngWheel(notches) {
      if (!st) return false;
      var mx = ngSim ? ngSim.mx : st.mx, my = ngSim ? ngSim.my : st.my;
      if (ngAdd && ngInPanel(ngAdd.list, mx, my)) { ngAddScroll(-notches * 78); return true; }
      if (!ngWin || !ngWin.visible || !ngInPanel(ngWin, mx, my)) return false;
      if (st.shift) { ngPan.x += notches * 60 / ngZoom; return true; }
      if (st.ctrl) { ngPan.y += notches * 60 / ngZoom; return true; }
      var cp = ngToCanvas(mx, my); if (!cp) return true;
      var gx = cp.x / ngZoom - ngPan.x, gy = cp.y / ngZoom - ngPan.y;
      ngZoom = Math.max(0.25, Math.min(2, ngZoom * Math.pow(1.15, notches)));
      // The graph point under the pointer stays under it.
      ngPan.x = cp.x / ngZoom - gx; ngPan.y = cp.y / ngZoom - gy;
      ngApplyZoom();
      return true;
    }
)CFJS"
R"CFJS(
    // ---- the pump: every press, drag and release in the window -------------------------------
    // Automation: a pointer fed over netcon instead of the OS cursor, so the gestures can be driven
    // without moving the user's mouse. $.ConfigHud.nodesPointer(x, y, lmb, rmb); no arguments ends it.
    var ngSim = null;
    // True when (x, y) is on the visible graph window: Config's own raw-cursor controls (slider
    // drags) must not start through it.
    function ngCovers(x, y) { return !!(ngWin && cfgValid(ngWin) && ngWin.visible && ngInPanel(ngWin, x, y)); }
    // The window's rect in client px for the C++ click router (UiLayout NodeGraph): a press or a
    // notch on it is the graph's, not the game's or Config's underneath. Empty while hidden.
    function ngPublishRect() {
      var r = '';
      if (ngWin && cfgValid(ngWin) && ngWin.visible && ngWin.GetPositionWithinWindow) {
        var q = ngWin.GetPositionWithinWindow();
        var w = ngWin.actuallayoutwidth || 0, h = ngWin.actuallayoutheight || 0;
        if (q && w > 0 && h > 0) r = Math.round(q.x) + ' ' + Math.round(q.y) + ' ' + Math.round(w) + ' ' + Math.round(h);
      }
      if (r !== ngPubRect) { ngPubRect = r; root.SetAttributeString('graphrect', r); }
    }
    function ngPump(s) {
      ngPublishRect();
      if (ngSim && s) s = { mx: ngSim.mx, my: ngSim.my, lmb: ngSim.lmb, rmb: ngSim.rmb, cursor: true, ui: s.ui };
      if (ngSim && s) { cfgCursorX = s.mx; cfgCursorY = s.my; }   // menus open at the fed pointer
      if (!ngWin || !cfgValid(ngWin) || !ngWin.visible || !s || typeof s.mx !== 'number') {
        ngGesture = null; ngLastLmb = !!(s && s.lmb); ngLastRmb = !!(s && s.rmb);
        return;
      }
      var lmb = !!s.lmb && !!s.cursor, rmb = !!s.rmb && !!s.cursor;
      var down = lmb && !ngLastLmb, up = !lmb && ngLastLmb, rdown = rmb && !ngLastRmb;
      ngLastLmb = lmb; ngLastRmb = rmb;
      var cp = ngToCanvas(s.mx, s.my); if (!cp) return;
      var g = { x: cp.x / ngZoom - ngPan.x, y: cp.y / ngZoom - ngPan.y };
      var inWin = ngInPanel(ngWin, s.mx, s.my), inCanvas = ngInPanel(ngCanvas, s.mx, s.my);
      var overPopover = !!(cfgDropOpen && cfgColorOverRect(cfgDropOpen.list, s.mx, s.my));
      var hit = (inCanvas && !overPopover) ? ngHit(g) : null;
      var sc = ngScale();

      if (down && inWin && !overPopover) {
        var cs = ngCanvasSize();
        var onTitleBtn = ngTitleBtns.some(function (b) { return ngInPanel(b, s.mx, s.my); });
        if (inCanvas && cp.x >= cs.w - NG_GRIP && cp.y >= cs.h - NG_GRIP) {
          ngGesture = { kind: 'size', mx: s.mx, my: s.my, r: { x: ngRect.x, y: ngRect.y, w: ngRect.w, h: ngRect.h } };
        } else if (!inCanvas && !onTitleBtn) {
          ngGesture = { kind: 'move', mx: s.mx, my: s.my, r: { x: ngRect.x, y: ngRect.y, w: ngRect.w, h: ngRect.h } };
        } else if (inCanvas && hit && hit.kind === 'out') {
          ngGesture = { kind: 'wire', from: hit.n.id, mx: s.mx, my: s.my, moved: false };
        } else if (inCanvas && hit && hit.kind === 'in') {
          var into = ngEdgeInto(hit.n.id, hit.port);
          if (ngPendingFrom > 0) { nodesCmd('connect ' + ngPendingFrom + ' ' + hit.n.id + ' ' + hit.port); ngPendingFrom = -1; }
          else if (into) {
            // Pick the wire up from its input, as in Blender and Nuke.
            nodesCmd('disconnect ' + into.id);
            ngGesture = { kind: 'wire', from: into.from, mx: s.mx, my: s.my, moved: true };
          }
        } else if (inCanvas && hit && hit.kind === 'node') {
          if (hit.n.id !== ngSelected) { ngSelected = hit.n.id; nodesCmd('select ' + hit.n.id); }
          var p0 = ngPosOf(hit.n);
          ngGesture = { kind: 'node', id: hit.n.id, ox: g.x - p0.x, oy: g.y - p0.y, nx: p0.x, ny: p0.y, mx: s.mx, my: s.my, moved: false };
        } else if (inCanvas) {
          ngGesture = { kind: 'pan', mx: s.mx, my: s.my, px: ngPan.x, py: ngPan.y, moved: false };
        }
      } else if (lmb && ngGesture) {
        var dx = (s.mx - ngGesture.mx) / sc, dy = (s.my - ngGesture.my) / sc;
        if (Math.abs(dx) + Math.abs(dy) > 3) ngGesture.moved = true;
        if (ngGesture.kind === 'node' && ngGesture.moved) { ngGesture.nx = Math.round(g.x - ngGesture.ox); ngGesture.ny = Math.round(g.y - ngGesture.oy); }
        else if (ngGesture.kind === 'pan') { ngPan.x = Math.round(ngGesture.px + dx / ngZoom); ngPan.y = Math.round(ngGesture.py + dy / ngZoom); }
        else if (ngGesture.kind === 'move') { ngRect.x = ngGesture.r.x + dx; ngRect.y = ngGesture.r.y + dy; ngClampRect(ngRect); }
        else if (ngGesture.kind === 'size') { ngRect.w = ngGesture.r.w + dx; ngRect.h = ngGesture.r.h + dy; ngClampRect(ngRect); }
      }
      if (up && ngGesture) {
        var ge = ngGesture; ngGesture = null;
        if (ge.kind === 'node' && ge.moved) {
          ngDragPos[ge.id] = { x: ge.nx, y: ge.ny, until: Date.now() + 1500 };
          nodesCmd('move ' + ge.id + ' ' + ge.nx + ' ' + ge.ny);
        } else if (ge.kind === 'pan' && !ge.moved && ngSelected > 0) {
          ngSelected = -1; ngPendingFrom = -1; nodesCmd('select -1');
        } else if (ge.kind === 'move' || ge.kind === 'size') {
          ngPersistRect();
        } else if (ge.kind === 'wire') {
          var tgt = inCanvas ? ngHit(g) : null;
          if (tgt && tgt.kind === 'in') nodesCmd('connect ' + ge.from + ' ' + tgt.n.id + ' ' + tgt.port);
          else if (tgt && tgt.kind === 'out' && tgt.n.id === ge.from && !ge.moved) ngPendingFrom = (ngPendingFrom === ge.from) ? -1 : ge.from;
          else if (ge.moved && inCanvas && (!tgt || tgt.kind !== 'node')) ngOpenAddMenu({ x: g.x, y: g.y - NODE_HDR - 18 }, ge.from);
        }
      }
      if (rdown && inCanvas && !overPopover) {
        if (hit && (hit.kind === 'node' || hit.kind === 'in' || hit.kind === 'out')) {
          if (hit.n.id !== ngSelected) { ngSelected = hit.n.id; nodesCmd('select ' + hit.n.id); }
          ngNodeMenu(hit.n);
        } else if (hit && hit.kind === 'wire') {
          var we = hit.e;
          cfgMenu([[T('Disconnect'), function () { nodesCmd('disconnect ' + we.id); }, '', true]]);
        } else {
          ngOpenAddMenu({ x: g.x - NODE_W / 2, y: g.y - 12 }, -1);
        }
      }
      var hp = '';
      if (hit && hit.kind === 'out') hp = hit.n.id + ':out';
      else if (hit && hit.kind === 'in') hp = hit.n.id + ':in:' + hit.port;
      ngHotPin = hp;

      ngApplyRect();
      ngApplyZoom();
      ngSyncCards();
      ngDrawWires();
      if (ngGesture && ngGesture.kind === 'wire') {
        var fromN = ngNodeById(ngGesture.from);
        ngDrawLive(fromN ? ngPinAt(fromN, 'out') : null, g, fromN && fromN.mask ? NG_MASK + 'ff' : NG_FX + 'ff');
      } else ngDrawLive(null, null, '');
    }
)CFJS"
R"CFJS(
    // ---- state -> tab, inspector and window ----------------------------------------------------
    renderHooks.push(function (st) {
      var ns = nodesState(); if (!ns) return;
      nodesPill.set(!!ns.on);
      nodesReset.setDisabled(!ns.on);
      nodesPresetGrid.sync(ns.preset || '');
      nodesSec.__note(ns.on ? T('{0} active steps', ns.steps || 0) : T('Off'));
      nodesGraphBtn.__lbl.text = ns.canvasOpen ? T('Close Graph Editor') : T('Open Graph Editor');

      ngNodes = ns.nodes || []; ngEdges = ns.edges || [];
      if (!(ngGesture && ngGesture.kind === 'node')) ngSelected = ns.selected;
      var now = Date.now();
      Object.keys(ngDragPos).forEach(function (id) {
        var d = ngDragPos[id], n = ngNodeById(+id);
        if (!n || (n.x === d.x && n.y === d.y) || now > d.until) delete ngDragPos[id];
      });

      var showCanvas = !!ns.canvasOpen && activeTab === 'nodes' && root.visible;
      if (showCanvas) ngBuild();
      if (ngWin && cfgValid(ngWin)) {
        if (ngWin.visible !== showCanvas) ngWin.visible = showCanvas;
        if (showCanvas) {
          var stored = ngReadStored(st);
          if (stored) ngRect = ngClampRect(stored);
          if (!ngRect) ngRect = ngDefaultRect();
          ngApplyRect();
          // The first time it opens, the whole graph -- Final Output included -- is framed.
          if (!ngFramed && ngCanvasSize().w > 0) ngFrameAll();
          if (typeof ns.noteSeq === 'number' && ns.noteSeq !== ngNoteSeq) {
            if (ngNoteSeq >= 0 && ns.note) ngNoteUntil = now + 3500;
            ngNoteSeq = ns.noteSeq;
          }
          var noteText = now < ngNoteUntil ? (ns.note || '') : '';
          if (ngNoteLbl.text !== noteText) ngNoteLbl.text = noteText;
        } else if (ngGesture) ngGesture = null;
      }

      nodesListSync(ns);
      var sel = nodesSelected();
      nodeInspTitle.text = sel ? sel.label : T('Select a node');
      nodeInspDel.visible = !!(sel && sel.type !== 'output');
      nodeInspDup.visible = !!(sel && sel.type !== 'output');
      if ((sel ? sel.id : -1) !== nodeInspBuiltId || (sel ? sel.type : '') !== nodeInspBuiltType)
        rebuildNodeInspector(sel);
      if (sel) {
        for (var ci = 0; ci < nodeInspControls.length; ci++) {
          var c = nodeInspControls[ci];
          if (c.boolCtrl) c.boolCtrl.set(sel.p[c.idx] > 0.5);
          else if (c.sliderCtrl) c.sliderCtrl.sync(sel.p[c.idx]);
        }
      }
      nodeWireSync(ns, sel);
    });
)CFJS"
