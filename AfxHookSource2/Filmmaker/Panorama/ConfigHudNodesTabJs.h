#pragma once

// NODES tab of the CONFIG panel + interactive Node Graph Canvas overlay (EffectNodes.h).
// Spliced into ConfigHudJs.h. Shares scope with the root IIFE; appends cards to `pageNodes`
// and renders the draggable visual graph editor when Open Graph Editor is toggled.

R"CFJS(
    // ===================== NODES TAB =====================================
    var NODES_ACCENT = '#b98cff';
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

    var NODE_PRESETS = [
      ['color_subject_desat_world', T('Color subject, desaturate world')],
      ['glow_through_fog', T('Glow through the fog')],
      ['player_neon_outline', T('Player neon outline')],
      ['sky_grade_heat_haze', T('Sky grade & heat haze')],
      ['depth_spotlight', T('Depth spotlight')]
    ];
    var nodesPresetGrid = presetDropdown(nodesSec, T('Starter presets'), NODE_PRESETS, 2,
      T('Loads a ready-made node graph and turns Effect Nodes on. Open the Graph Editor below to inspect and tweak every node and wire.'),
      function (k) { nodesCmd('preset ' + k); }, 12);

    var nodesGraphBtn = wideBtn(nodesSec, T('Open Graph Editor'), function () {
      nodesCmd('canvas toggle');
    }, NODES_ACCENT + 'ff');
    nodesGraphBtn.__bgBase = '#b98cff22'; nodesGraphBtn.__brBase = '#b98cff88'; paintBtn(nodesGraphBtn);
    attachTip(nodesGraphBtn, T('Opens the visual draggable node graph editor over the preview so you can drag nodes and wire masks to effects.'));

    subHead(nodesSec, T('Add mask node'));
    actionGroup(nodesSec, T('Masks'), [
      [T('+ Depth'), function () { nodesCmd('add depth_range'); }],
      [T('+ Sky'), function () { nodesCmd('add sky_mask'); }],
      [T('+ Player'), function () { nodesCmd('add player_matte'); }],
      [T('+ Fog'), function () { nodesCmd('add fog_mask'); }],
      [T('+ Luma'), function () { nodesCmd('add luma_key'); }],
      [T('+ Combine'), function () { nodesCmd('add mask_combine'); }]
    ], 3);

    subHead(nodesSec, T('Add effect node'));
    actionGroup(nodesSec, T('Effects'), [
      [T('+ Grade'), function () { nodesCmd('add grade'); }],
      [T('+ Glow'), function () { nodesCmd('add glow'); }],
      [T('+ Outline'), function () { nodesCmd('add outline'); }],
      [T('+ Distort'), function () { nodesCmd('add distortion'); }]
    ], 2);

    subHead(nodesSec, T('Graph nodes'));
    var nodesListHost = mk('Panel', nodesSec);
    nodesListHost.style.width = '100%'; nodesListHost.style.flowChildren = 'down';
    nodesListHost.style.marginTop = '4px';

    // ---- Selected Node Inspector Card ----
    var nodeInspSec = section(pageNodes, T('NODE INSPECTOR'), true, false, 'nodes.insp', NODES_ACCENT + 'ff');
    var nodeInspHead = mk('Panel', nodeInspSec);
    nodeInspHead.style.width = '100%'; nodeInspHead.style.flowChildren = 'right';
    var nodeInspTitle = lbl(nodeInspHead, T('Select a node'), S.value, 13);
    nodeInspTitle.style.fontWeight = 'bold'; nodeInspTitle.style.width = 'fill-parent-flow(1.0)';
    nodeInspTitle.style.verticalAlign = 'center';
    var nodeInspDel = btn(nodeInspHead, T('Delete'), function () {
      var n = nodesSelected(); if (n && n.type !== 'output') nodesCmd('remove ' + n.id);
    }, '#ff8a8aff');
    nodeInspDel.style.marginRight = '0px';

    var nodeParamHost = mk('Panel', nodeInspSec);
    nodeParamHost.style.width = '100%'; nodeParamHost.style.flowChildren = 'down';
    nodeParamHost.style.marginTop = '6px';

    subHead(nodeInspSec, T('Mask input connection'));
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
      output:       [[0, T('Master mix'), 0, 1, 2, '']]
    };

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
)CFJS"
R"CFJS(
    // ===================== INTERACTIVE NODE GRAPH CANVAS OVERLAY =====================
    // Opens over the preview area left of the inspector so editors can visually inspect, drag,
    // wire, and tweak the effect node graph live while watching CS2 respond underneath!
    var ngOverlay = mk('Panel', root);
    ngOverlay.id = 'NodesGraphOverlay';
    ngOverlay.hittest = true; ngOverlay.hittestchildren = true;
    ngOverlay.visible = false;
    ngOverlay.style.zIndex = '68';
    ngOverlay.style.backgroundColor = 'rgba(11,14,19,0.92)';
    ngOverlay.style.border = '1px solid #b98cff55';
    ngOverlay.style.borderRadius = '6px';
    ngOverlay.style.flowChildren = 'down';

    var ngTopBar = mk('Panel', ngOverlay); ngTopBar.hittest = true;
    ngTopBar.style.width = '100%'; ngTopBar.style.height = '38px'; ngTopBar.style.flowChildren = 'right';
    ngTopBar.style.paddingLeft = '12px'; ngTopBar.style.paddingRight = '10px';
    ngTopBar.style.backgroundColor = '#141820ff'; ngTopBar.style.borderBottom = '1px solid #ffffff1a';
    var ngTitle = lbl(ngTopBar, T('EFFECT NODE GRAPH'), NODES_ACCENT + 'ff', 13);
    ngTitle.style.fontWeight = 'bold'; ngTitle.style.letterSpacing = '1.8px'; ngTitle.style.verticalAlign = 'center';
    var ngHint = lbl(ngTopBar, T('Drag nodes · Click OUT then IN/MASK pin to connect · Click a wire badge to disconnect'), S.muted, 11);
    ngHint.style.verticalAlign = 'center'; ngHint.style.marginLeft = '14px'; ngHint.style.width = 'fill-parent-flow(1.0)';
    var ngCloseBtn = btn(ngTopBar, T('✕ Close Graph'), function () { nodesCmd('canvas close'); }, S.label);
    ngCloseBtn.style.verticalAlign = 'center'; ngCloseBtn.style.marginRight = '0px';

    var ngCanvas = mk('Panel', ngOverlay); ngCanvas.hittest = true; ngCanvas.hittestchildren = true;
    ngCanvas.style.width = '100%'; ngCanvas.style.height = 'fill-parent-flow(1.0)';
    ngCanvas.style.overflow = 'clip';
    ngCanvas.SetPanelEvent('onactivate', function () { ngPendingFrom = -1; });

    var ngWireLayer = mk('Panel', ngCanvas); ngWireLayer.hittest = false; ngWireLayer.hittestchildren = true;
    ngWireLayer.style.width = '100%'; ngWireLayer.style.height = '100%';
    var ngNodeLayer = mk('Panel', ngCanvas); ngNodeLayer.hittest = false; ngNodeLayer.hittestchildren = true;
    ngNodeLayer.style.width = '100%'; ngNodeLayer.style.height = '100%';

    var NODE_W = 166, NODE_H = 74;
    var ngPendingFrom = -1;
    var ngDrag = null; // { id, ox, oy, nx, ny }
    var ngLastLmb = false;
    var ngGraphSig = '';
    var ngNodePanels = {};

    function ngDrawWire(parent, x1, y1, x2, y2, color, edgeId) {
      var midX = Math.round((x1 + x2) * 0.5);
      function seg(ax, ay, bx, by) {
        var p = mk('Panel', parent); p.hittest = true;
        var lx = Math.min(ax, bx), ty = Math.min(ay, by);
        var w = Math.max(2, Math.abs(bx - ax)), h = Math.max(2, Math.abs(by - ay));
        p.style.position = lx + 'px ' + ty + 'px 0px';
        p.style.width = w + 'px'; p.style.height = h + 'px';
        p.style.backgroundColor = color;
        attachTip(p, T('Click wire to disconnect'));
        p.SetPanelEvent('onactivate', function () { nodesCmd('disconnect ' + edgeId); });
      }
      seg(x1, y1, midX, y1);
      seg(midX, y1, midX, y2);
      seg(midX, y2, x2, y2);
    }

    function ngRebuildCanvas(ns) {
      ngWireLayer.RemoveAndDeleteChildren();
      ngNodeLayer.RemoveAndDeleteChildren();
      ngNodePanels = {};
      var nodes = ns.nodes || [], edges = ns.edges || [];
      var byId = {};
      for (var i = 0; i < nodes.length; i++) byId[nodes[i].id] = nodes[i];

      // Draw wires first (under nodes)
      for (var ei = 0; ei < edges.length; ei++) {
        var e = edges[ei], a = byId[e.from], b = byId[e.to];
        if (!a || !b) continue;
        var x1 = Math.round(a.x + NODE_W), y1 = Math.round(a.y + 38);
        var x2 = Math.round(b.x), y2 = Math.round(b.y + (e.toPort === 1 ? 54 : 30));
        var col = a.mask ? '#5fd0c0cc' : '#f0b323cc';
        ngDrawWire(ngWireLayer, x1, y1, x2, y2, col, e.id);
      }

      // Draw node cards
      for (var ni = 0; ni < nodes.length; ni++) (function (n) {
        var sel = n.id === ns.selected;
        var accent = n.type === 'output' ? '#80bdff' : (n.mask ? '#5fd0c0' : '#f0b323');
        var card = mk('Panel', ngNodeLayer); card.hittest = true; card.hittestchildren = true;
        card.style.position = Math.round(n.x) + 'px ' + Math.round(n.y) + 'px 0px';
        card.style.width = NODE_W + 'px'; card.style.height = NODE_H + 'px';
        card.style.backgroundColor = sel ? '#1e2532ff' : '#151a22ff';
        card.style.border = '1px solid ' + (sel ? (accent + 'ff') : (accent + '66'));
        card.style.borderRadius = '4px'; card.style.flowChildren = 'down';
        card.style.opacity = n.enabled ? '1.0' : '0.45';

        var hdr = mk('Panel', card); hdr.hittest = true;
        hdr.style.width = '100%'; hdr.style.height = '24px'; hdr.style.flowChildren = 'right';
        hdr.style.paddingLeft = '7px'; hdr.style.paddingRight = '6px';
        hdr.style.backgroundColor = accent + '26';
        var hl = lbl(hdr, n.label, S.value, 11); hl.hittest = false;
        hl.style.fontWeight = 'bold'; hl.style.verticalAlign = 'center'; hl.style.width = 'fill-parent-flow(1.0)';
        hl.style.whiteSpace = 'nowrap'; hl.style.textOverflow = 'ellipsis';
        hdr.SetPanelEvent('onactivate', function () { nodesCmd('select ' + n.id); });

        var body = mk('Panel', card); body.hittest = false; body.hittestchildren = true;
        body.style.width = '100%'; body.style.height = 'fill-parent-flow(1.0)'; body.style.flowChildren = 'right';
        body.style.paddingLeft = '6px'; body.style.paddingRight = '6px';

        // Left pins (inputs)
        var leftCol = mk('Panel', body); leftCol.hittest = false; leftCol.hittestchildren = true;
        leftCol.style.flowChildren = 'down'; leftCol.style.width = 'fill-parent-flow(1.0)'; leftCol.style.verticalAlign = 'center';
        if (!n.mask || n.type === 'mask_combine') {
          var in0 = lbl(leftCol, n.type === 'mask_combine' ? '● MASK A' : '● IN', '#f0b323ff', 10); in0.hittest = true;
          in0.SetPanelEvent('onactivate', function () {
            if (ngPendingFrom > 0 && ngPendingFrom !== n.id) {
              nodesCmd('connect ' + ngPendingFrom + ' ' + n.id + ' 0'); ngPendingFrom = -1;
            }
          });
        }
        if ((!n.mask && n.type !== 'output') || n.type === 'mask_combine') {
          var in1 = lbl(leftCol, n.type === 'mask_combine' ? '● MASK B' : '● MASK', '#5fd0c0ff', 10); in1.hittest = true;
          in1.style.marginTop = '4px';
          in1.SetPanelEvent('onactivate', function () {
            if (ngPendingFrom > 0 && ngPendingFrom !== n.id) {
              nodesCmd('connect ' + ngPendingFrom + ' ' + n.id + ' 1'); ngPendingFrom = -1;
            }
          });
        }

        // Right pin (output)
        if (n.type !== 'output') {
          var outPin = lbl(body, (ngPendingFrom === n.id ? '◉ LINK' : 'OUT ●'), accent + 'ff', 10);
          outPin.hittest = true; outPin.style.verticalAlign = 'center'; outPin.style.fontWeight = 'bold';
          outPin.SetPanelEvent('onactivate', function () {
            ngPendingFrom = (ngPendingFrom === n.id) ? -1 : n.id;
            ngGraphSig = '';
          });
        }

        card.SetPanelEvent('onactivate', function () { nodesCmd('select ' + n.id); });
        ngNodePanels[n.id] = { card: card, node: n };
      })(nodes[ni]);
    }

    // Streamed mouse drag support for node cards on the canvas.
    function ngPumpDrag(st, ns) {
      if (!ngOverlay.visible) { ngDrag = null; ngLastLmb = false; return; }
      var lmb = !!(st && st.lmb);
      var mx = (st && typeof st.mx === 'number') ? st.mx : -1;
      var my = (st && typeof st.my === 'number') ? st.my : -1;
      var sx = root.actualuiscale_x || 1, sy = root.actualuiscale_y || 1;
      var cp = ngCanvas.GetPositionWithinWindow ? ngCanvas.GetPositionWithinWindow() : null;
      if (!cp) { ngLastLmb = lmb; return; }
      var lx = (mx - cp.x) / sx, ly = (my - cp.y) / sy;

      if (lmb && !ngLastLmb) {
        var nodes = (ns && ns.nodes) || [];
        for (var i = nodes.length - 1; i >= 0; i--) {
          var n = nodes[i];
          if (lx >= n.x && lx <= n.x + NODE_W && ly >= n.y && ly <= n.y + 26) {
            ngDrag = { id: n.id, ox: lx - n.x, oy: ly - n.y, nx: n.x, ny: n.y };
            nodesCmd('select ' + n.id);
            break;
          }
        }
      } else if (lmb && ngDrag) {
        var nx = Math.max(8, Math.round(lx - ngDrag.ox));
        var ny = Math.max(8, Math.round(ly - ngDrag.oy));
        ngDrag.nx = nx; ngDrag.ny = ny;
        var rec = ngNodePanels[ngDrag.id];
        if (rec && rec.card) rec.card.style.position = nx + 'px ' + ny + 'px 0px';
      } else if (!lmb && ngLastLmb && ngDrag) {
        nodesCmd('move ' + ngDrag.id + ' ' + ngDrag.nx + ' ' + ngDrag.ny);
        ngDrag = null;
      }
      ngLastLmb = lmb;
    }

    var nodesListSig = '', nodeWireSig = '';
    renderHooks.push(function (st) {
      var ns = nodesState(); if (!ns) return;
      nodesPill.set(!!ns.on);
      nodesReset.setDisabled(!ns.on);
      nodesPresetGrid.sync(ns.preset || '');
      nodesSec.__note(ns.on ? T('{0} active steps', ns.steps || 0) : T('Off'));
      nodesGraphBtn.__lbl.text = ns.canvasOpen ? T('Close Graph Editor') : T('Open Graph Editor');

      // Show/place the visual graph canvas over the preview area when open on the NODES tab.
      var showCanvas = !!ns.canvasOpen && activeTab === 'nodes' && root.visible;
      ngOverlay.visible = showCanvas;
      if (showCanvas) {
        var sc = cfgScreen();
        var cw = Math.max(420, Math.round(sc.rw - INSPECTOR_W - 28));
        var ch = Math.max(280, Math.round(sc.rh - 150));
        ngOverlay.style.position = '14px 48px 0px';
        ngOverlay.style.width = cw + 'px';
        ngOverlay.style.height = ch + 'px';
        var gsig = ngPendingFrom + '|' + ns.selected + '|'
          + (ns.nodes || []).map(function (n) { return n.id + ':' + n.x + ',' + n.y + ':' + (n.enabled ? 1 : 0); }).join(';') + '|'
          + (ns.edges || []).map(function (e) { return e.id + ':' + e.from + '>' + e.to + ':' + e.toPort; }).join(';');
        if (gsig !== ngGraphSig && !ngDrag) {
          ngGraphSig = gsig;
          ngRebuildCanvas(ns);
        }
        ngPumpDrag(st, ns);
      }

      // Sidebar node list
      var nodes = ns.nodes || [];
      var lsig = ns.selected + '|' + nodes.map(function (n) { return n.id + ':' + n.label + ':' + (n.enabled ? 1 : 0); }).join('|');
      if (lsig !== nodesListSig) {
        nodesListSig = lsig;
        nodesListHost.RemoveAndDeleteChildren();
        for (var i = 0; i < nodes.length; i++) (function (n) {
          var on = n.id === ns.selected;
          var acc = n.type === 'output' ? '#80bdff' : (n.mask ? '#5fd0c0' : '#f0b323');
          var r = mk('Panel', nodesListHost); r.hittest = true;
          r.style.width = '100%'; r.style.height = '28px'; r.style.flowChildren = 'right';
          r.style.paddingLeft = '8px'; r.style.paddingRight = '6px'; r.style.marginBottom = '3px';
          r.style.borderRadius = '3px';
          r.style.backgroundColor = on ? (acc + '28') : '#ffffff08';
          r.style.border = '1px solid ' + (on ? (acc + 'ff') : '#ffffff12');
          var tag = lbl(r, n.mask ? 'MASK' : (n.type === 'output' ? 'OUT' : 'FX'), acc + 'ff', 10);
          tag.hittest = false; tag.style.width = '40px'; tag.style.fontWeight = 'bold'; tag.style.verticalAlign = 'center';
          var nm = lbl(r, n.label, on ? S.value : S.label, 12); nm.hittest = false;
          nm.style.width = 'fill-parent-flow(1.0)'; nm.style.verticalAlign = 'center';
          if (n.type !== 'output') {
            var tg = btn(r, n.enabled ? T('On') : T('Off'), function () { nodesCmd('enable ' + n.id + ' ' + (n.enabled ? 0 : 1)); }, n.enabled ? (acc + 'ff') : S.muted);
            tg.style.marginRight = '0px';
          }
          r.SetPanelEvent('onactivate', function () { nodesCmd('select ' + n.id); });
        })(nodes[i]);
      }

      // Selected node inspector
      var sel = nodesSelected();
      nodeInspTitle.text = sel ? (sel.label + '  [' + sel.type + ']') : T('Select a node');
      nodeInspDel.visible = !!(sel && sel.type !== 'output');
      if ((sel ? sel.id : -1) !== nodeInspBuiltId || (sel ? sel.type : '') !== nodeInspBuiltType)
        rebuildNodeInspector(sel);
      if (sel) {
        for (var ci = 0; ci < nodeInspControls.length; ci++) {
          var c = nodeInspControls[ci];
          if (c.boolCtrl) c.boolCtrl.set(sel.p[c.idx] > 0.5);
          else if (c.sliderCtrl) c.sliderCtrl.sync(sel.p[c.idx]);
        }
        // Quick mask-input selector for effect nodes
        var wsig = sel.id + '|' + nodes.filter(function (q) { return q.mask; }).map(function (q) { return q.id; }).join(',')
          + '|' + (ns.edges || []).filter(function (e) { return e.to === sel.id && e.toPort === 1; }).map(function (e) { return e.from; }).join(',');
        if (wsig !== nodeWireSig) {
          nodeWireSig = wsig;
          nodeWireRow.RemoveAndDeleteChildren();
          if (!sel.mask && sel.type !== 'output') {
            var curMaskEdge = null;
            for (var ei2 = 0; ei2 < (ns.edges || []).length; ei2++)
              if (ns.edges[ei2].to === sel.id && ns.edges[ei2].toPort === 1) curMaskEdge = ns.edges[ei2];
            var bNone = btn(nodeWireRow, T('Full frame'), function () {
              if (curMaskEdge) nodesCmd('disconnect ' + curMaskEdge.id);
            }, !curMaskEdge ? S.value : S.label);
            bNone.style.marginBottom = '4px'; setBtnOn(bNone, !curMaskEdge, '#5fd0c0ff');
            for (var mi = 0; mi < nodes.length; mi++) if (nodes[mi].mask) (function (mn) {
              var active = curMaskEdge && curMaskEdge.from === mn.id;
              var bm = btn(nodeWireRow, mn.label, function () {
                nodesCmd('connect ' + mn.id + ' ' + sel.id + ' 1');
              }, active ? S.value : S.label);
              bm.style.marginBottom = '4px'; setBtnOn(bm, active, '#5fd0c0ff');
            })(nodes[mi]);
          }
        }
      }
    });
)CFJS"
