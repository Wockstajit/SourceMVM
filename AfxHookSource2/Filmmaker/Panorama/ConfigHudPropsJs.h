#pragma once

// SCENE OBJECTS > ASSETS & LIGHTS (Docs/features/filmmaker_config_panel.md; original "Scene objects pane redesign", option 2a): real
// CS2 models (trees, plants, props), effects, and your own meshes, placed into the scene.
// JS fragment concatenated into the kConfigHudJs body, spliced into ConfigHudEnvironmentTabJs.h
// after the Outliner, so this is the card's second page. Everything mirrors `st.props`
// (Props_AppendStateJson); every action is a `props` command (meshes: `environment add obj`).
//
// Top to bottom, in the order the work happens: find (search + category), look (the turning
// preview), place or paint, then the results -- one list that grows as it is scrolled (24 rows,
// then 20 more near its end), never paged.
//
// A paint brush can hold several models: Ctrl-click results to add or drop them (grass, ferns and
// rocks in one stroke; each copy picks one at random). While painting, a plain click on a result
// switches the brush to that model; Ctrl-click adds it to the brush.

R"CFJS(
    // ===================== SCENE OBJECTS > ASSETS & LIGHTS ====================
    var propsPage = envSceneSec.__page('Library');
    function propsCommand(t) { cmd('mirv_filmmaker props ' + t); }
    // A brush mix as console lines: verb (start|models) with the first models, then paint add for
    // the rest. One console line holds only a few model paths; a long line lost the last models.
    function propsSendBrush(verb, models) {
      var line = verb, sent = 0;
      for (var i = 0; i < models.length; ++i) {
        if (sent && line.length + models[i].length > 180) { propsCommand('paint ' + line); line = 'add'; sent = 0; }
        line += ' ' + models[i]; ++sent;
      }
      if (sent) propsCommand('paint ' + line);
    }
    var PROPS_FIRST = 24, PROPS_MORE = 20;
    var propsQuery = '', propsQueryFocus = false, propsCategory = 'all', propsLimit = PROPS_FIRST;
    var propsRequested = false, propsCtrl = false;
    // Models picked for the brush with Ctrl-click (besides the one in the preview).
    var propsBrushSet = [];
    function propsSearch() {
      propsCommand('search "' + propsQuery + '" ' + propsCategory + ' ' + propsLimit);
    }
    // A new search or category starts again at the first rows, at the top of the list.
    function propsNewSearch() {
      propsLimit = PROPS_FIRST;
      propsSearch();
      if (propsWell) propsWell.reset();
    }

    // ---- find ----------------------------------------------------------------------------------
    // Search field: the same window-level text capture the scene-name field uses, since HUD
    // TextEntry panels never get real keyboard focus here.
    var propsField = mk('Panel', propsPage);
    propsField.hittest = true;
    propsField.style.width = '100%'; propsField.style.height = '36px'; propsField.style.flowChildren = 'right';
    propsField.style.backgroundColor = '#1b2129ff';
    propsField.style.border = '1px solid ' + S.tileBorder;
    propsField.style.borderRadius = '4px';
    propsField.style.marginTop = '8px';
    endTileLine(propsPage);
    var propsSearchGlyph = lbl(propsField, '⌕', S.muted, 16);
    propsSearchGlyph.hittest = false; propsSearchGlyph.style.marginLeft = '10px'; propsSearchGlyph.style.verticalAlign = 'center';
    var propsFieldLabel = lbl(propsField, '', S.value, 14);
    propsFieldLabel.hittest = false; propsFieldLabel.style.width = 'fill-parent-flow(1.0)';
    propsFieldLabel.style.marginLeft = '8px'; propsFieldLabel.style.whiteSpace = 'nowrap';
    propsFieldLabel.style.textOverflow = 'ellipsis'; propsFieldLabel.style.verticalAlign = 'center';
    var propsClear = lbl(propsField, '×', S.muted, 16);
    propsClear.hittest = true; propsClear.style.verticalAlign = 'center'; propsClear.style.padding = '0px 10px';
    propsClear.SetPanelEvent('onactivate', function () { propsQuery = ''; propsFieldPaint(); propsNewSearch(); });
    function propsFieldPaint() {
      propsFieldLabel.text = (propsQuery + (propsQueryFocus ? ' |' : '')) || 'Search CS2 models: wood, tree, grass, rock, car…';
      propsFieldLabel.style.color = propsQuery ? S.value : S.muted;
      propsField.style.border = '1px solid ' + (propsQueryFocus ? ENV_ACCENT : S.tileBorder);
      propsClear.visible = !!propsQuery;
    }
    var propsPreviousBlur = configWindowBlur, propsPreviousChars = configWindowChars;
    configWindowBlur = function () { propsQueryFocus = false; propsFieldPaint(); propsPreviousBlur(); };
    configWindowChars = function (csv) {
      if (!propsQueryFocus) { propsPreviousChars(csv); return; }
      var before = propsQuery;
      String(csv || '').split(',').forEach(function (part) {
        var n = parseInt(part, 10);
        if (n === 13 || n === 27) configWindowBlur();
        else if (n === 8) propsQuery = propsQuery.slice(0, -1);
        else if (propsQuery.length < 40 && ((n >= 48 && n <= 57) || (n >= 65 && n <= 90)
                 || (n >= 97 && n <= 122) || n === 32 || n === 45 || n === 95)) {
          propsQuery += String.fromCharCode(n);
        }
      });
      if (propsQuery !== before) propsNewSearch();
      propsFieldPaint();
    };
    propsField.SetPanelEvent('onactivate', function () {
      configWindowBlur(); propsQueryFocus = true; cfgTextOwner = propsField;
      root.SetAttributeString('textcapture', '1');
      propsFieldPaint();
    });
    propsFieldPaint();

    // "Meshes" is not a catalog search: it lists the OBJ / GLB / glTF files in the Environment's
    // meshes folder (st.envAssets), placed as Environment meshes.
    var propsMine = false, propsMeshPick = '';
    var propsCats = segmented(propsPage, [
      ['all', 'All'], ['foliage', 'Foliage'], ['props', 'Props'], ['effects', 'Effects'], ['map', 'This map'], ['mine', 'Meshes']
    ], '100%', function (k) {
      propsMine = k === 'mine';
      if (propsWell) propsWell.reset();
      if (!propsMine) { propsCategory = k; propsNewSearch(); }
    }, 12);
    if (propsCats.panel) propsCats.panel.style.marginTop = '4px';
)CFJS"
R"CFJS(
    // ---- look: the preview -------------------------------------------------------------------
    // The model being browsed, turning in CS2's own player preview (the death-animation picker's
    // MapPlayerPreviewPanel). Native preview scenes only spawn a model while the demo plays, so the
    // panel wears a marker agent its pooled pawn takes paused or not, and Filmmaker/Props/PropPreview
    // swaps that pawn to the asset, scales it to fill the frame, keeps it drawing while paused and
    // turns it. A preview panel only builds its scene if its parent is visible and laid out when it
    // is created, so the clip is placed first; it composites only while marked ready, so that is
    // re-asserted on every tick, as the Customize modal does.
    var propsPreviewModel = '', propsPreviewName = '', propsPreviewFolder = '', propsSpinning = false;
    var PROPS_PREVIEW_MARKER = 'agents/models/tm_jungle_raider/tm_jungle_raider_variantb2.vmdl'; // PropPreview.h
    var propsPreviewSerial = 0;
    var propsPreviewBox = mk('Panel', propsPage);
    propsPreviewBox.hittest = false; propsPreviewBox.hittestchildren = true;
    propsPreviewBox.style.width = '100%'; propsPreviewBox.style.height = '230px';
    propsPreviewBox.style.marginTop = '10px';
    propsPreviewBox.style.backgroundColor = '#0b0e12ff';
    propsPreviewBox.style.border = '1px solid ' + S.tileBorder;
    propsPreviewBox.style.borderRadius = '4px';
    propsPreviewBox.style.overflow = 'clip';
    endTileLine(propsPage);
    var propsPreview3d = null;
    // The 3D panel lives in a clip panel on the root, laid over the box every frame, as the
    // death-animation picker does: inside the scrolling inspector its scene drew off to one corner.
    // The clip is the part of the box the inspector actually shows, so scrolling crops it.
    var propsClip = mk('Panel', root);
    propsClip.hittest = false; propsClip.visible = false;
    propsClip.style.overflow = 'clip'; propsClip.style.zIndex = '95';
    var propsClipSig = '';
    function propsPlace3d() {
      var show = !!propsPreviewModel && !propsMine && activeTab === 'environment' && root.visible && propsPreviewBox.visible;
      var sx = root.actualuiscale_x || 1, sy = root.actualuiscale_y || 1;
      var b = show ? propsPreviewBox.GetPositionWithinWindow() : null, r0 = root.GetPositionWithinWindow();
      if (!b || !r0) show = false;
      var x0 = 0, y0 = 0, x1 = 0, y1 = 0, vx0 = 0, vy0 = 0, vx1 = 0, vy1 = 0;
      if (show) {
        x0 = b.x; y0 = b.y; x1 = x0 + propsPreviewBox.actuallayoutwidth; y1 = y0 + propsPreviewBox.actuallayoutheight;
        vx0 = x0; vy0 = y0; vx1 = x1; vy1 = y1;
        for (var a = propsPreviewBox.GetParent(); a && a !== root; a = a.GetParent()) {
          if (!a.visible) { show = false; break; }
          var q = a.GetPositionWithinWindow();
          if (!q) continue;
          vx0 = Math.max(vx0, q.x); vy0 = Math.max(vy0, q.y);
          vx1 = Math.min(vx1, q.x + a.actuallayoutwidth); vy1 = Math.min(vy1, q.y + a.actuallayoutheight);
        }
        show = show && vx1 - vx0 > 4 && vy1 - vy0 > 4;
      }
      propsClip.visible = show;
      if (!show) return;
      var sig = [x0, y0, x1, y1, vx0, vy0, vx1, vy1].map(Math.round).join(',');
      if (sig === propsClipSig) return;
      propsClipSig = sig;
      propsClip.style.position = ((vx0 - r0.x) / sx).toFixed(1) + 'px ' + ((vy0 - r0.y) / sy).toFixed(1) + 'px 0px';
      propsClip.style.width = ((vx1 - vx0) / sx).toFixed(1) + 'px'; propsClip.style.height = ((vy1 - vy0) / sy).toFixed(1) + 'px';
      if (!propsPreview3d) { propsClipSig = ''; return; }
      propsPreview3d.style.position = ((x0 - vx0) / sx).toFixed(1) + 'px ' + ((y0 - vy0) / sy).toFixed(1) + 'px 0px';
      propsPreview3d.style.width = ((x1 - x0) / sx).toFixed(1) + 'px'; propsPreview3d.style.height = ((y1 - y0) / sy).toFixed(1) + 'px';
    }
    var propsPreviewHint = lbl(propsPreviewBox, 'Pick a model below to see it here', S.muted, 13);
    propsPreviewHint.hittest = false;
    propsPreviewHint.style.horizontalAlign = 'center'; propsPreviewHint.style.verticalAlign = 'center';
    // Name and source · folder, top-left over the model.
    var propsPreviewCap = mk('Panel', propsPreviewBox);
    propsPreviewCap.hittest = false; propsPreviewCap.style.flowChildren = 'down';
    propsPreviewCap.style.margin = '10px 12px 0px 12px'; propsPreviewCap.style.verticalAlign = 'top';
    var propsPreviewTitle = lbl(propsPreviewCap, '', S.value, 14);
    propsPreviewTitle.hittest = false; propsPreviewTitle.style.fontWeight = 'bold';
    var propsPreviewSub = lbl(propsPreviewCap, '', S.muted, 11);
    propsPreviewSub.hittest = false; propsPreviewSub.style.marginTop = '2px';

    // ---- place or paint ------------------------------------------------------------------------
    var propsActs = mk('Panel', propsPage);
    propsActs.style.width = '100%'; propsActs.style.flowChildren = 'right'; propsActs.style.marginTop = '8px';
    var propsPlaceBtn = btn(propsActs, 'Place in scene', function () {
      if (propsPlacing) propsCommand('cancel');
      else if (propsMine && propsMeshPick) envCommand('add obj "' + propsMeshPick + '"');
      else if (propsPreviewModel) propsCommand('place ' + propsPreviewModel);
    }, S.accent);
    propsPlaceBtn.style.width = 'fill-parent-flow(1.0)'; propsPlaceBtn.style.padding = '8px 11px';
    propsPlaceBtn.__lbl.style.letterSpacing = '1px'; propsPlaceBtn.__lbl.style.textOverflow = 'ellipsis';
    propsPlaceBtn.__lbl.style.horizontalAlign = 'center';
    propsPlaceBtn.__bgBase = '#80bdff14'; propsPlaceBtn.__brBase = '#80bdff66'; paintBtn(propsPlaceBtn);
    var propsPaintBtn = btn(propsActs, 'Paint', function () {
      if (propsPainting) { propsCommand('paint stop'); return; }
      var models = propsBrushModels();
      if (models.length) propsSendBrush('start', models);
    }, S.label);
    propsPaintBtn.style.marginRight = '0px'; propsPaintBtn.style.marginLeft = '6px'; propsPaintBtn.style.padding = '8px 12px';
    var propsPlacing = false, propsPainting = false;
    // What the brush paints: the Ctrl-picked set, or the model in the preview.
    function propsBrushModels() {
      var list = propsBrushSet.slice();
      if (!list.length && propsPreviewModel && !/\.vpcf$/i.test(propsPreviewModel)) list.push(propsPreviewModel);
      return list;
    }
)CFJS"
R"CFJS(
    // ---- the paint panel ------------------------------------------------------------------------
    // Hold the mouse over the view to spray copies inside the circle; Shift (or Erase) removes this
    // brush's copies under it. Every copy goes into one group, the "folder" the outliner shows.
    var propsPaintBox = mk('Panel', propsPage);
    propsPaintBox.style.width = '100%'; propsPaintBox.style.flowChildren = 'down'; propsPaintBox.style.marginTop = '8px';
    propsPaintBox.style.padding = '8px 0px 10px 0px'; propsPaintBox.style.backgroundColor = '#7fe0d00d';
    propsPaintBox.style.border = '1px solid #7fe0d033'; propsPaintBox.style.borderRadius = '4px';
    var propsPaintHead = mk('Panel', propsPaintBox);
    propsPaintHead.style.width = '100%'; propsPaintHead.style.flowChildren = 'right'; propsPaintHead.style.padding = '0px 10px 4px 12px';
    var propsPaintTitle = lbl(propsPaintHead, '', ENV_ACCENT, 12);
    propsPaintTitle.style.fontWeight = 'bold'; propsPaintTitle.style.width = 'fill-parent-flow(1.0)';
    propsPaintTitle.style.verticalAlign = 'center'; propsPaintTitle.style.whiteSpace = 'nowrap'; propsPaintTitle.style.textOverflow = 'ellipsis';
    actionBtn(propsPaintHead, 'Done painting', function () { propsCommand('paint stop'); });
    var propsPaintModels = lbl(propsPaintBox, '', S.muted, 11);
    propsPaintModels.style.margin = '0px 12px 4px 12px'; propsPaintModels.style.whiteSpace = 'normal';
    var propsPaintSliders = [
      ['radius', 'Brush size', 16, 1024, 0, 'u', 'How wide the circle paints.'],
      ['spacing', 'Spacing', 4, 512, 0, 'u', 'The least distance between two copies: small packs them tight, large leaves gaps.'],
      ['density', 'Density', 5, 400, 0, '%', 'How full a held brush makes the circle: 100% fills every gap the spacing allows, 50% half of them, and above 100% packs copies closer than the spacing (200% is twice as many), so painting over a patch adds to it. Hold the mouse to keep spraying until it is that full.'],
      ['size', 'Size', 0.1, 5, 2, '×', 'The copies' + "'" + ' base size. 1× is the model' + "'" + 's size in the game.'],
      ['jitter', 'Size variation', 0, 0.9, 2, '', 'How much each copy' + "'" + 's size differs from the base, either way.'],
      ['turn', 'Turn variation', 0, 180, 0, '°', 'How far each copy is turned at random: 180° faces any way, 0 all the same.']
    ].map(function (d) {
      var row = sliderRow(propsPaintBox, { label: d[1], min: d[2], max: d[3], decimals: d[4], unit: d[5], tip: d[6],
        onChange: function (v) { propsCommand('paint set ' + d[0] + ' ' + v); } });
      return { key: d[0], row: row };
    });
    var propsPaintErase = modeRow(propsPaintBox, 'Brush', [['0', 'Paint'], ['1', 'Erase']], '100%', function (k) {
      propsCommand('paint set erase ' + k);
    }, 'Erase removes this brush' + "'" + 's copies under the circle. Holding Shift erases too.');
    var propsPaintNote = lbl(propsPaintBox, 'Hold the mouse over the view to paint; Shift erases; G flies the camera; Esc is done.', S.hint, 11);
    propsPaintNote.style.margin = '6px 12px 0px 12px'; propsPaintNote.style.whiteSpace = 'normal';

    // ---- results: one list that grows as it scrolls --------------------------------------------
    var propsResultHead = mk('Panel', propsPage);
    propsResultHead.style.width = '100%'; propsResultHead.style.flowChildren = 'right'; propsResultHead.style.marginTop = '12px';
    var propsResultLabel = lbl(propsResultHead, 'RESULTS', S.muted, 11);
    propsResultLabel.style.fontWeight = 'bold'; propsResultLabel.style.letterSpacing = '1.5px';
    propsResultLabel.style.verticalAlign = 'center'; propsResultLabel.style.width = 'fill-parent-flow(1.0)';
    var propsResultCount = lbl(propsResultHead, '', S.muted, 11); propsResultCount.style.verticalAlign = 'center';
    var propsMineBtns = mk('Panel', propsResultHead); propsMineBtns.style.flowChildren = 'right';
    actionBtn(propsMineBtns, 'Open folder', function () { envCommand('folder'); });
    actionBtn(propsMineBtns, 'Refresh', function () { envCommand('rescan'); });
    var propsList = mk('Panel', propsPage);
    propsList.style.width = '100%'; propsList.style.flowChildren = 'down'; propsList.style.marginTop = '6px';
    propsList.style.backgroundColor = '#0f1318ff'; propsList.style.border = '1px solid ' + S.tileBorder;
    propsList.style.borderRadius = '4px'; propsList.style.padding = '3px';
    // Eight rows (256px) at most; the rest scrolls inside, with no scrollbar. Near its end it asks
    // for 20 more (the backend returns the first N matches).
    var propsWell = scrollWell(propsList, 256);
    var propsResults = propsWell.inner;
    var propsTotal = 0;
    propsWell.onNearEnd = function () {
      if (propsMine || propsLimit >= propsTotal || propsLimit >= 4000) return;
      propsLimit = Math.min(4000, propsLimit + PROPS_MORE);
      propsSearch();
    };
    var propsListHint = lbl(propsPage, 'Click to preview · double-click to place. It follows your mouse over the view until you click.', S.hint, 11);
    propsListHint.style.marginTop = '6px'; propsListHint.style.whiteSpace = 'normal';
    var propsMessage = envNote(propsPage, '');
    propsMessage.style.color = '#e8b339ff';
    // Saved objects held back by the crash-loop guard (Props.cpp): load them again or drop them.
    var propsHeldRow = mk('Panel', propsPage);
    propsHeldRow.style.flowChildren = 'right'; propsHeldRow.style.marginTop = '4px';
    propsHeldRow.visible = false;
    actionBtn(propsHeldRow, 'Load saved objects', function () { propsCommand('restore'); });
    actionBtn(propsHeldRow, 'Clear saved objects', function () { propsCommand('clear'); });

    var PROPS_KIND_DOT = { asset: '#80bdff', effect: '#f0a070', mesh: '#b9a3ff' };
    // A result row: kind dot, name, and source · last folder on the right.
    function propsRow(r) {
      var b = mk('Panel', propsResults);
      b.hittest = true; b.style.width = '100%'; b.style.height = '29px'; b.style.flowChildren = 'right';
      b.style.padding = '0px 9px'; b.style.marginBottom = '1px'; b.style.borderRadius = '3px';
      b.style.border = '1px solid ' + (r.picked ? '#80bdffff' : S.clear);
      b.style.backgroundColor = r.picked ? '#80bdff2e' : (r.inBrush ? '#7fe0d014' : S.clear);
      b.SetPanelEvent('onmouseover', function () { if (!r.picked) b.style.backgroundColor = '#212833ff'; });
      b.SetPanelEvent('onmouseout', function () { b.style.backgroundColor = r.picked ? '#80bdff2e' : (r.inBrush ? '#7fe0d014' : S.clear); });
      b.SetPanelEvent('onactivate', r.click);
      if (r.place) b.SetPanelEvent('ondblclick', r.place);
      var kindIcon = olIcon(b, r.kind === 'light' ? 'lightbulb' : (r.kind === 'mesh' ? 'shapes' : 'box'), 13,
        PROPS_KIND_DOT[r.kind] + 'ff');
      kindIcon.style.marginRight = '8px';
      var n = lbl(b, (r.inBrush ? '✓ ' : '') + r.name, r.picked || r.inBrush ? ENV_ACCENT : S.label, 13);
      n.hittest = false; n.style.verticalAlign = 'center'; n.style.width = 'fill-parent-flow(1.0)';
      n.style.whiteSpace = 'nowrap'; n.style.textOverflow = 'ellipsis';
      var d = lbl(b, r.detail || '', S.muted, 11);
      d.hittest = false; d.style.verticalAlign = 'center'; d.style.maxWidth = '44%';
      d.style.whiteSpace = 'nowrap'; d.style.textOverflow = 'ellipsis'; d.style.marginLeft = '8px';
    }
)CFJS"
R"CFJS(
    function propsSpinStep() {
      if (!propsSpinning) return;
      if (!propsPreview3d || !propsPreview3d.IsValid() || !propsPreviewModel) { propsSpinning = false; return; }
      try { propsPreview3d.SetReadyForDisplay(true); } catch (e) {}
      propsPlace3d();
      $.Schedule(1 / 30, propsSpinStep);
    }
    // ONE panel, reused for every model (PropPreview swaps the asset on its pawn). A fresh panel per
    // model left each old one's scene lights behind, and they added up: after twenty models the
    // preview was blown out to white.
    function propsBuild3d(model) {
      try { if (propsPreview3d && propsPreview3d.IsValid()) propsPreview3d.DeleteAsync(0); } catch (e) {}
      propsPreview3d = null;
      propsPreviewIsFx = false;
      propsClipSig = '';
      propsPlace3d();
      try {
        var tex = 'PropsPreviewTex' + (++propsPreviewSerial);
        propsPreview3d = $.CreatePanel('MapPlayerPreviewPanel', propsClip, tex, {
          'require-composition-layer': 'true', 'composition-layer-texture-name': tex,
          'pin-fov': 'vertical', 'transparent-background': 'true', 'class': 'mvp_map',
          map: 'ui/buy_menu', camera: 'cam_loadoutmenu_ct', animgraphcharactermode: 'buy-menu',
          playername: 'vanity_character', player: 'true', mouse_rotate: 'false', sync_spawn_addons: 'true',
          hide_while_waiting_for_composite_materials: 'false', playermodel: PROPS_PREVIEW_MARKER
        });
        if (!(propsPreview3d && propsPreview3d.IsValid())) propsPreview3d = null;
      } catch (e2) { propsPreview3d = null; }
      if (!propsPreview3d) return;
      propsPreview3d.hittest = false;
      propsClipSig = '';
      propsPlace3d();
      try { propsPreview3d.SetPlayerModel(PROPS_PREVIEW_MARKER); } catch (e3) {}
      try { propsPreview3d.SetReadyForDisplay(true); } catch (e4) {}
    }
    // The particle viewer for an effect. One panel, reused: a new effect is swapped in with
    // SetParticleNameAndRefresh. The camera stands off at a three-quarter view of a ~150-unit
    // stage, which frames everything from a candle to a large fire.
    var propsPreviewIsFx = false;
    function propsBuildFx(effect) {
      if (propsPreviewIsFx && propsPreview3d && propsPreview3d.IsValid()) {
        try { propsPreview3d.SetParticleNameAndRefresh(effect); propsPreview3d.StartParticles(); return; } catch (e) {}
      }
      try { if (propsPreview3d && propsPreview3d.IsValid()) propsPreview3d.DeleteAsync(0); } catch (e1) {}
      propsPreview3d = null;
      propsPreviewIsFx = false;
      try {
        var tex = 'PropsFxTex' + (++propsPreviewSerial);
        // Set up the way CS2's own particle panels are (add_major_tokens_anim.xml): no composition
        // layer. Rendered through one, the view landed off-centre, the effect in the box's corner.
        propsPreview3d = $.CreatePanel('ParticleScenePanel', propsClip, tex, {
          'transparent-background': 'true',
          particleName: effect, cameraOrigin: '170 -110 110', lookAt: '0 0 40', fov: '50', farPlane: '4000',
          startActive: 'true', syncSpawn: 'true'
        });
        if (!(propsPreview3d && propsPreview3d.IsValid())) propsPreview3d = null;
      } catch (e2) { propsPreview3d = null; }
      if (!propsPreview3d) return;
      propsPreviewIsFx = true;
      propsPreview3d.hittest = false;
      propsClipSig = '';
      propsPlace3d();
      try { propsPreview3d.StartParticles(); } catch (e3) {}
    }
    function propsShow(model, name, folder) {
      // Trimmed: names that arrive through `config eval` come padded by the console tokenizer.
      model = evalArg(model); name = evalArg(name); folder = evalArg(folder);
      if (!model) return;
      propsPreviewName = name || model;
      propsPreviewFolder = folder || '';
      // An effect (fire, water, steam) is a particle system, not a model: CS2's own particle viewer
      // (ParticleScenePanel, CCSGO_Particle3dPanel in client.dll) plays it in the same box.
      if (/\.vpcf$/i.test(model)) {
        if (model !== propsPreviewModel) {
          propsPreviewModel = model;
          propsCommand('preview off'); // the model turntable's pawn is not needed
          propsBuildFx(model);
          propsResultSig = '';
        }
        propsPreviewTitle.text = propsPreviewName;
        propsPreviewSub.text = propsPreviewFolder;
        propsPreviewHint.visible = !propsPreview3d;
        propsPreviewHint.text = 'This effect has no preview: place it to see it play';
        if (!propsSpinning) { propsSpinning = true; $.Schedule(1 / 30, propsSpinStep); }
        return;
      }
      propsPreviewHint.text = 'Pick a model below to see it here';
      // Back from an effect: the box holds the particle viewer, so the model turntable is rebuilt.
      if (propsPreviewIsFx) { propsPreviewModel = ''; propsBuild3d(model); }
      if (model !== propsPreviewModel) {
        propsPreviewModel = model;
        if (!(propsPreview3d && propsPreview3d.IsValid())) propsBuild3d(model);
        propsCommand('preview ' + model);
        propsResultSig = ''; // repaint the highlighted row
      }
      propsPreviewTitle.text = propsPreviewName;
      propsPreviewSub.text = propsPreviewFolder;
      propsPreviewHint.visible = false;
      if (!propsSpinning) { propsSpinning = true; $.Schedule(1 / 30, propsSpinStep); }
    }
    // A click on a catalog result. Ctrl-click puts it into (or takes it out of) the brush; while
    // painting, a plain click switches the brush to it and Ctrl-click changes the brush's mix live.
    var propsPrevPick = '';
    function propsPickResult(r, detail) {
      var ctrl = propsCtrl;
      propsPrevPick = propsPreviewModel; // what was picked before this click
      propsShow(r.model, r.name, detail);
      var paintable = !/\.vpcf$/i.test(r.model);
      if (ctrl && paintable) {
        // Mixing into a brush that is already painting starts from what it paints now.
        if (propsPainting && !propsBrushSet.length) propsBrushSet = propsPaintNow.slice();
        // The model clicked before the first Ctrl-click (the one in the preview) is part of the pick:
        // click A, Ctrl-click B and C, Paint -- all three paint, not just B and C.
        else if (!propsPainting && !propsBrushSet.length && propsPrevPick && propsPrevPick !== r.model && !/\.vpcf$/i.test(propsPrevPick))
          propsBrushSet.push(propsPrevPick);
        var at = propsBrushSet.indexOf(r.model);
        if (at >= 0) propsBrushSet.splice(at, 1); else propsBrushSet.push(r.model);
        if (propsPainting && propsBrushSet.length) propsSendBrush('models', propsBrushSet);
      } else if (propsPainting && paintable) {
        propsBrushSet = [];
        propsCommand('paint models ' + r.model);
      } else if (!ctrl) {
        propsBrushSet = [];
      }
      propsResultSig = '';
    }

    // Automation / netcon: preview a model without a click (synthetic clicks never reach Panorama).
    $.ConfigHudProps = {
      show: propsShow,
      // The Meshes view on or off (its chip is not a catalog search).
      mine: function (on) { propsMine = !!on; return propsMine; },
      // The brush's models: $.ConfigHudProps.brush('a.vmdl,b.vmdl') (empty clears).
      brush: function (csv) { csv = evalArg(csv || ''); propsBrushSet = csv ? csv.split(',') : []; propsResultSig = ''; return propsBrushSet.length; },
      more: function () { propsWell.onNearEnd(); return propsLimit; },
      // Triage: the panel alone, wearing its marker agent, with no asset swapped on.
      bare: function () { propsPreviewModel = 'bare'; propsBuild3d(''); propsPreviewHint.visible = false;
        if (!propsSpinning) { propsSpinning = true; $.Schedule(1 / 30, propsSpinStep); } },
      // Netcon triage: the preview panel's state, printed to the console.
      info: function () {
        var p = propsPreview3d;
        $.Msg('props preview: ' + (p ? p.type + ' valid=' + p.IsValid() + ' ' + p.actuallayoutwidth + 'x' +
          p.actuallayoutheight + ' visible=' + p.visible : 'no panel') + ' model=' + propsPreviewModel +
          ' spinning=' + propsSpinning + ' clip=' + propsClip.visible + ' ' + propsClipSig +
          ' limit=' + propsLimit + ' total=' + propsTotal + ' brush=' + propsBrushSet.join(','));
      }
    };
)CFJS"
R"CFJS(
    var propsResultSig = '';
    renderHooks.push(function (st) {
      var p = st && st.props;
      if (!p) return;
      propsCtrl = !!st.ctrl;
      // Turn only while the page is on screen; reopening it picks the loop back up.
      var onScreen = propsPage.visible && root.visible;
      propsPlace3d();
      if (!onScreen) propsSpinning = false;
      else if (propsPreviewModel && !propsSpinning) { propsSpinning = true; $.Schedule(1 / 30, propsSpinStep); }
      if (p.total === -1 && !propsRequested) { propsRequested = true; propsSearch(); }
      // The query can also be set by a command; mirror it while the box is not being typed in.
      if (!propsQueryFocus && (p.query || '') !== propsQuery) {
        propsQuery = p.query || ''; propsCategory = p.category || 'all'; propsLimit = p.limit || PROPS_FIRST;
        propsFieldPaint();
      }
      propsCats.sync(propsMine ? 'mine' : (p.category || 'all'));
      propsTotal = p.total > 0 ? p.total : 0;

      var paint = p.paint || {};
      propsPainting = !!paint.active;
      propsPaintBox.visible = propsPainting;
      if (propsPainting) {
        propsPaintTitle.text = 'Painting ' + (paint.name || '');
        var pm = paint.models || [];
        propsPaintNow = pm;
        propsPaintModels.text = pm.length > 1
          ? pm.map(function (m) { return m.split('/').pop().replace(/\.vmdl$/, '').replace(/_/g, ' '); }).join(' · ')
            + '   ·   Ctrl-click a result to add or drop one'
          : 'Click a result to paint it instead, Ctrl-click to mix several';
        propsPaintSliders.forEach(function (sl) { sl.row.sync(sl.key === 'density' ? Math.round((paint.density || 1) * 100) : paint[sl.key]); });
        propsPaintErase.sync(paint.erasing ? '1' : '0');
        // The brush's models are the ticked rows while it paints.
        var brushSig = pm.join(',');
        if (brushSig !== propsBrushSig) { propsBrushSig = brushSig; propsBrushSet = pm.length > 1 ? pm.slice() : []; propsResultSig = ''; }
      }

      propsPlacing = !!p.placing;
      var isFx = /\.vpcf$/i.test(propsPreviewModel);
      var picked = propsMine ? !!propsMeshPick : !!propsPreviewModel;
      var pickName = propsMine ? propsMeshPick : propsPreviewName;
      // Dimmed, not disabled: the design keeps their outline at 45% (the click handlers check).
      propsPlaceBtn.style.opacity = (propsPlacing || picked) ? '1' : '0.45';
      propsPlaceBtn.__lbl.text = propsPlacing ? 'Cancel placing' : (picked ? 'Place ' + pickName + ' in scene' : 'Place in scene');
      var brush = propsBrushModels();
      propsPaintBtn.visible = !propsMine && !isFx;
      propsPaintBtn.style.opacity = (propsPainting || brush.length) ? '1' : '0.45';
      propsPaintBtn.__lbl.text = propsPainting ? 'Done' : (propsBrushSet.length > 1 ? 'Paint ' + propsBrushSet.length : 'Paint');
      setBtnOn(propsPaintBtn, propsPainting, ENV_ACCENT);
      if (propsMine) {
        propsPreviewTitle.text = propsMeshPick; propsPreviewSub.text = propsMeshPick ? 'Your mesh  ·  meshes folder' : '';
        propsPreviewHint.visible = !propsMeshPick;
      }

      var results = p.results || [], meshes = st.envAssets || [];
      propsMineBtns.visible = propsMine;
      if (propsMine) {
        propsResultCount.text = meshes.length + (meshes.length === 1 ? ' mesh' : ' meshes') + ' in the meshes folder';
      } else {
        propsResultCount.text = p.total < 0 ? 'Loading the game’s models…'
          : results.length < p.total ? 'Showing ' + results.length + ' of ' + p.total + ' · scroll for more'
          : p.total + (p.total === 1 ? ' model' : ' models');
      }
      var rsig = (propsMine ? 'mine|' + meshes.join(',') + '|' + propsMeshPick
        : p.query + '|' + p.category + '|' + p.limit + '|' + p.total + '|' + results.length + '|' + propsPreviewModel)
        + '|' + propsBrushSet.join(',');
      if (rsig !== propsResultSig) {
        propsResultSig = rsig;
        propsResults.RemoveAndDeleteChildren();
        if (propsMine) {
          meshes.forEach(function (name) {
            propsRow({ kind: 'mesh', name: name, detail: 'meshes', picked: name === propsMeshPick,
              click: function () { propsMeshPick = name; propsResultSig = ''; },
              place: function () { propsMeshPick = name; envCommand('add obj "' + name + '"'); } });
          });
          if (!meshes.length) envNote(propsResults, 'No meshes yet. Put an OBJ, GLB or glTF in the meshes folder, then Refresh.');
        } else {
          results.forEach(function (r) {
            // Where it comes from: the map first ("Cache"), then its own folder.
            var from = r.source ? r.source + '  ·  ' : '';
            var detail = from + String(r.folder || '').split('/').pop();
            propsRow({ kind: /\.vpcf$/i.test(r.model) ? 'effect' : 'asset', name: r.name, detail: detail,
              picked: r.model === propsPreviewModel, inBrush: propsBrushSet.indexOf(r.model) >= 0,
              click: function () { propsPickResult(r, from + r.folder); },
              place: function () { propsShow(r.model, r.name, from + r.folder); propsCommand('place ' + r.model); } });
          });
          if (!results.length && p.total === 0) envNote(propsResults, 'No matches');
        }
      }

      propsMessage.text = p.message || '';
      propsMessage.visible = !!p.message;
      propsHeldRow.visible = !!p.held;
    });
    var propsBrushSig = '', propsPaintNow = [];
)CFJS"
