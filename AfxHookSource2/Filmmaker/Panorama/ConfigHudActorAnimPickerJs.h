#pragma once

// ACTOR ANIMATION PICKER: compact inline card (`bots.anims`) inside the BOTS tab of the
// Filmmaker Config sidebar (replacing the old fullscreen modal overlay so the live CS2 viewport
// stays visible beside the panel). Uses the ConfigHud window-level keyboard bridge
// (`configWindowChars` / `cfgTextOwner`) because HUD TextEntry panels never receive keyboard focus.

R"CFJS(
    var AAP_ROWS = 8, AAP_ROW_H = 40;
    var AAP_AC = '#80bdff', AAP_CAP = '#9ea9b8ff', AAP_SUB = '#8b95a3ff';
    var AAP_CATS = [
      [-1, T('All categories')], [0, T('Locomotion')], [1, T('Crouch')], [2, T('Weapon')],
      [3, T('Gestures')], [4, T('Idle')], [5, T('Reactions')], [6, T('Cinematic')]
    ];
    var AAP_SOURCES = [
      ['all', T('All characters')], ['grunt', T('Grunt')], ['heavy', T('Heavy')],
      ['suppressor', T('Suppressor')], ['metrocop', T('Metrocop')]
    ];
    function aapCatName(idx) {
      if (idx === -1) return T('All categories');
      for (var i = 0; i < AAP_CATS.length; i++) if (AAP_CATS[i][0] === idx) return AAP_CATS[i][1];
      return T('Other');
    }
    function aapWeaponName(w) {
      return w === 1 ? T('Rifle') : (w === 3 ? T('Grenade') : (w === 2 ? T('Pistol') : T('Unarmed')));
    }
    function aapSourceOf(row) {
      var id = '' + row[0], nm = '' + row[1];
      if (id.indexOf('hla_hea_') === 0 || id.indexOf('hla_heavy_') === 0 || nm.indexOf('Heavy:') === 0) return 'heavy';
      if (id.indexOf('hla_sup_') === 0 || nm.indexOf('Suppressor:') === 0) return 'suppressor';
      if (id.indexOf('hla_mc_') === 0 || id.indexOf('hla_met_') === 0 || id.indexOf('hla_run_') === 0 ||
          id.indexOf('hla_crouch_walk_') === 0 || nm.indexOf('Metrocop:') === 0) return 'metrocop';
      return 'grunt';
    }
    function aapDurationLabel(row) {
      var dur = (row && typeof row[3] === 'number') ? row[3] : 0;
      var spd = (row && typeof row[7] === 'number') ? row[7] : 0;
      return dur.toFixed(1) + 's' + (spd > 5 ? (' · ' + Math.round(spd) + ' u/s') : '');
    }
    function aapSpeedLabel(row) {
      var spd = (row && typeof row[7] === 'number') ? row[7] : 0;
      return spd > 5 ? (Math.round(spd) + ' u/s') : T('In-place');
    }

    var AAP_WEAPONS = [
      ['auto', T('Auto')], ['weapon_ak47', T('AK-47')], ['weapon_m4a1_silencer', T('M4A1-S')],
      ['weapon_awp', T('AWP')], ['weapon_deagle', T('Deagle')], ['keep', T('Keep mine')]
    ];
    var aapWeapon = 'auto';

    // [id, displayName, catIdx, durSec, loopable, rootMotion, weaponClass, rootSpeed]
    var aapCatalog = null;
    var aapCatalogAsked = false;
    var aapFiltered = [];
    var aapTop = 0;
    var aapCat = -1;
    var aapSource = 'all';
    var aapQuery = '';
    var aapQueryFocus = false;
    var aapClipId = '';
    var aapTargetTrack = -1;
    var aapSpeed = 1.0, aapLoop = true, aapUpperBody = false, aapStart = 0.0, aapDur = 0.0, aapBlendIn = 0.14, aapBlendOut = 0.22;
    var aapTickOn = false, aapViewLive = false;

    var aapSec = section(pageBots, T('ANIMATIONS'), true, false, 'bots.anims', '#80bdffff');

    // Live playing status bar + Stop button (always accessible without scrolling)
    var aapLiveBar = mk('Panel', aapSec); aapLiveBar.hittest = false; aapLiveBar.hittestchildren = true;
    aapLiveBar.style.width = '100%'; aapLiveBar.style.flowChildren = 'right';
    aapLiveBar.style.padding = '5px 8px'; aapLiveBar.style.marginTop = '4px'; aapLiveBar.style.borderRadius = '3px';
    aapLiveBar.style.backgroundColor = '#26446640'; aapLiveBar.style.border = '1px solid #80bdff55';
    aapLiveBar.visible = false;
    var aapLiveLbl = lbl(aapLiveBar, '', S.value, 12); aapLiveLbl.hittest = false;
    aapLiveLbl.style.width = 'fill-parent-flow(1.0)'; aapLiveLbl.style.verticalAlign = 'center';
    aapLiveLbl.style.whiteSpace = 'nowrap'; aapLiveLbl.style.textOverflow = 'ellipsis';
    var aapLiveStop = btn(aapLiveBar, T('Stop'), function () { cmd('mirv_filmmaker takes anim stop'); }, '#ff9e9eff');
    aapLiveStop.style.marginRight = '0px'; aapLiveStop.style.padding = '3px 10px';

    // Compact 3D preview box inside the inline card, with clip overlay parented to root
    var aapPreviewBox = mk('Panel', aapSec); aapPreviewBox.hittest = false; aapPreviewBox.hittestchildren = true;
    aapPreviewBox.style.width = '100%'; aapPreviewBox.style.height = '176px'; aapPreviewBox.style.marginTop = '6px';
    aapPreviewBox.style.backgroundColor = '#0b0e12ff'; aapPreviewBox.style.border = '1px solid ' + S.tileBorder;
    aapPreviewBox.style.borderRadius = '4px'; aapPreviewBox.style.overflow = 'clip';
    var aapViewMsg = lbl(aapPreviewBox, T('Pick an animation below to preview'), S.hint, 12); aapViewMsg.hittest = false;
    aapViewMsg.style.horizontalAlign = 'center'; aapViewMsg.style.verticalAlign = 'center';
    var aapPreviewCap = mk('Panel', aapPreviewBox); aapPreviewCap.hittest = false;
    aapPreviewCap.style.flowChildren = 'down'; aapPreviewCap.style.margin = '8px 10px 0px 10px';
    var aapPreviewTitle = lbl(aapPreviewCap, '', S.value, 13); aapPreviewTitle.hittest = false;
    aapPreviewTitle.style.fontWeight = 'bold'; aapPreviewTitle.style.whiteSpace = 'nowrap'; aapPreviewTitle.style.textOverflow = 'ellipsis';
    var aapPreviewMeta = lbl(aapPreviewCap, '', S.muted, 11); aapPreviewMeta.hittest = false;
    aapPreviewMeta.style.marginTop = '1px';

    var aap3d = null, aap3dClip = null, aap3dSerial = 0, aap3dReadyTicks = 0, aapClipSig = '';
    function aapVisibleOnScreen() {
      if (!root || (root.IsValid && !root.IsValid())) return false;
      return !!(root.visible && typeof activeTab !== 'undefined' && activeTab === 'bots' &&
        (typeof cfgSearchActive === 'undefined' || !cfgSearchActive) &&
        cfgRails && cfgRails.bots && cfgRails.bots.sel === 'bots.anims');
    }
    function aapDestroy3d() {
      try { if (aap3d && aap3d.IsValid && aap3d.IsValid()) aap3d.DeleteAsync(0); } catch (e) {}
      try { if (aap3dClip && aap3dClip.IsValid && aap3dClip.IsValid()) aap3dClip.DeleteAsync(0); } catch (e2) {}
      aap3d = null; aap3dClip = null; aapClipSig = '';
    }
    function aapPlace3d() {
      var show = !!(aapClipId && aapVisibleOnScreen());
      var sx = root.actualuiscale_x || 1, sy = root.actualuiscale_y || 1;
      var b = (show && aapPreviewBox.GetPositionWithinWindow) ? aapPreviewBox.GetPositionWithinWindow() : null;
      var r0 = root.GetPositionWithinWindow ? root.GetPositionWithinWindow() : null;
      if (!b || !r0 || !aapPreviewBox.actuallayoutwidth) show = false;
      var x0 = 0, y0 = 0, x1 = 0, y1 = 0, vx0 = 0, vy0 = 0, vx1 = 0, vy1 = 0;
      if (show) {
        x0 = b.x; y0 = b.y; x1 = x0 + aapPreviewBox.actuallayoutwidth; y1 = y0 + aapPreviewBox.actuallayoutheight;
        vx0 = x0; vy0 = y0; vx1 = x1; vy1 = y1;
        for (var a = aapPreviewBox.GetParent ? aapPreviewBox.GetParent() : null; a && a !== root; a = a.GetParent ? a.GetParent() : null) {
          if (a.visible === false) { show = false; break; }
          var q = a.GetPositionWithinWindow ? a.GetPositionWithinWindow() : null;
          if (!q || !a.actuallayoutwidth) continue;
          vx0 = Math.max(vx0, q.x); vy0 = Math.max(vy0, q.y);
          vx1 = Math.min(vx1, q.x + a.actuallayoutwidth); vy1 = Math.min(vy1, q.y + a.actuallayoutheight);
        }
        show = show && (vx1 - vx0 > 4) && (vy1 - vy0 > 4);
      }
      if (aap3dClip) aap3dClip.visible = show;
      if (!show || !aap3dClip) return;
      var sig = [x0, y0, x1, y1, vx0, vy0, vx1, vy1].map(Math.round).join(',');
      if (sig === aapClipSig) return;
      aapClipSig = sig;
      aap3dClip.style.position = ((vx0 - r0.x) / sx).toFixed(1) + 'px ' + ((vy0 - r0.y) / sy).toFixed(1) + 'px 0px';
      aap3dClip.style.width = ((vx1 - vx0) / sx).toFixed(1) + 'px'; aap3dClip.style.height = ((vy1 - vy0) / sy).toFixed(1) + 'px';
      if (!aap3d) { aapClipSig = ''; return; }
      aap3d.style.position = ((x0 - vx0) / sx).toFixed(1) + 'px ' + ((y0 - vy0) / sy).toFixed(1) + 'px 0px';
      aap3d.style.width = ((x1 - x0) / sx).toFixed(1) + 'px'; aap3d.style.height = ((y1 - y0) / sy).toFixed(1) + 'px';
    }
    function aapEnsure3d() {
      if (aap3d && aap3d.IsValid && aap3d.IsValid()) { aapPlace3d(); return; }
      aapDestroy3d();
      aap3dClip = mk('Panel', root); aap3dClip.hittest = false;
      aap3dClip.style.overflow = 'clip'; aap3dClip.style.borderRadius = '4px'; aap3dClip.style.zIndex = '95';
      var tex = 'AapPreviewTex' + (++aap3dSerial);
      var model = 'agents/models/ctm_sas/ctm_sas.vmdl';
      try {
        aap3d = $.CreatePanel('MapPlayerPreviewPanel', aap3dClip, tex, {
          'require-composition-layer': 'true', 'composition-layer-texture-name': tex,
          'pin-fov': 'vertical', 'transparent-background': 'true', 'class': 'mvp_map',
          map: 'ui/buy_menu', camera: 'cam_loadoutmenu_ct', animgraphcharactermode: 'buy-menu',
          playername: 'vanity_character', player: 'true', mouse_rotate: 'true', sync_spawn_addons: 'true',
          csm_split_plane0_distance_override: '250.0', hide_while_waiting_for_composite_materials: 'false',
          hittest: 'true', panzoom_enabled: 'true', playermodel: model
        });
        if (!(aap3d && aap3d.IsValid && aap3d.IsValid())) aap3d = null;
      } catch (e) { aap3d = null; }
      if (aap3d) {
        aap3d.style.width = '100%'; aap3d.style.height = '100%';
        try { aap3d.SetPlayerModel(model); aap3d.SetReadyForDisplay(true); } catch (e2) {}
        aap3dReadyTicks = 20;
      }
      aapClipSig = '';
      aapPlace3d();
    }
    function aapTick() {
      if (!aapVisibleOnScreen()) {
        aapTickOn = false;
        if (aap3dClip && (!aap3dClip.IsValid || aap3dClip.IsValid())) aap3dClip.visible = false;
        if (aapViewLive) { aapViewLive = false; cmd('mirv_filmmaker takes anim view off'); }
        return;
      }
      aapPlace3d();
      if (aap3dReadyTicks > 0 && aap3d) {
        --aap3dReadyTicks;
        try { aap3d.SetReadyForDisplay(true); } catch (e) {}
      }
      $.Schedule(1 / 30, aapTick);
    }
)CFJS"
R"CFJS(
    function aapArgsStr() {
      return aapClipId + ' ' + aapSpeed.toFixed(2) + ' ' + (aapLoop ? 1 : 0) + ' ' +
        aapStart.toFixed(2) + ' ' + aapDur.toFixed(2) + ' ' + aapBlendIn.toFixed(2) + ' ' + aapBlendOut.toFixed(2) + ' ' + (aapUpperBody ? 1 : 0);
    }
    function aapRefreshView() {
      if (!aapClipId || !aapVisibleOnScreen()) return;
      aapViewLive = true;
      cmd('mirv_filmmaker takes anim view ' + aapClipId + ' ' + aapSpeed.toFixed(2) + ' ' +
        (aapLoop ? 1 : 0) + ' ' + aapStart.toFixed(2) + ' ' + aapDur.toFixed(2));
    }

    // Full-width 50/50 split action buttons (matching btBigButtons visual balance)
    function aapSplitRow(parent, items, accentFirst) {
      endTileLine(parent);
      var line = mk('Panel', parent); line.hittest = false; line.hittestchildren = true;
      line.style.flowChildren = 'right'; line.style.width = '100%'; line.style.marginTop = '6px';
      var out = [];
      for (var i = 0; i < items.length; i++) (function (caption, onClick, idx) {
        var b = mk('Panel', line);
        b.style.width = 'fill-parent-flow(1.0)'; b.style.height = '30px'; b.style.borderRadius = '3px';
        if (idx > 0) b.style.marginLeft = '6px';
        var l = lbl(b, caption, S.label, 12);
        l.hittest = false; l.style.fontWeight = 'bold'; l.style.whiteSpace = 'nowrap';
        l.style.horizontalAlign = 'center'; l.style.verticalAlign = 'center';
        makeInteractive(b, l, onClick, { fg: S.label });
        if (idx === 0 && accentFirst) setBtnOn(b, true, AAP_AC);
        out.push(b);
      })(items[i][0], items[i][1], i);
      return { row: line, btns: out };
    }
    function aapPlayOnMe() { if (aapClipId) cmd('mirv_filmmaker takes anim play ' + aapArgsStr()); }
    function aapAssignToBot() {
      if (!aapClipId) return;
      var idx = aapTargetTrack >= 0 ? aapTargetTrack : btOpen;
      if (idx < 0) idx = 0;
      cmd('mirv_filmmaker takes anim assign ' + (idx + 1) + ' ' + aapArgsStr());
    }
    var aapAct1 = aapSplitRow(aapSec, [
      [T('Play on me'), aapPlayOnMe],
      [T('Stop'), function () { cmd('mirv_filmmaker takes anim stop'); }]
    ], true);
    var aapAct2 = aapSplitRow(aapSec, [
      [T('Place where looking'), function () { if (aapClipId) cmd('mirv_filmmaker takes actor place ' + aapArgsStr()); }],
      [T('Assign to bot'), aapAssignToBot]
    ], false);

    var aapUpperPill = togglePill(aapSec, T('Blend with CS walk'),
      T('Plays this animation on the upper body only (Half-Life: Alyx spine bone mask) while you walk, run, crouch and jump normally with CS2 WASD.'),
      function () {
        aapUpperBody = !aapUpperBody;
        aapUpperPill.set(aapUpperBody);
        cmd('mirv_filmmaker takes anim walkblend ' + (aapUpperBody ? 'on' : 'off'));
      });
    var aapLoopPill = togglePill(aapSec, T('Loop'),
      T('Repeats looping clips continuously. One-shot reactions and signals default to Loop Off when selected.'),
      function () { aapLoop = !aapLoop; aapLoopPill.set(aapLoop); aapRefreshView(); });

    // Working window-level keyboard search box (HUD TextEntry never receives focus in CS2)
    var aapSearchField = mk('Panel', aapSec);
    aapSearchField.hittest = true;
    aapSearchField.style.width = '100%'; aapSearchField.style.height = '32px'; aapSearchField.style.flowChildren = 'right';
    aapSearchField.style.backgroundColor = '#1b2129ff'; aapSearchField.style.border = '1px solid ' + S.tileBorder;
    aapSearchField.style.borderRadius = '4px'; aapSearchField.style.marginTop = '8px';
    endTileLine(aapSec);
    var aapSearchGlyph = lbl(aapSearchField, '⌕', S.muted, 15); aapSearchGlyph.hittest = false;
    aapSearchGlyph.style.marginLeft = '10px'; aapSearchGlyph.style.verticalAlign = 'center';
    var aapSearchLbl = lbl(aapSearchField, '', S.value, 13); aapSearchLbl.hittest = false;
    aapSearchLbl.style.width = 'fill-parent-flow(1.0)'; aapSearchLbl.style.marginLeft = '8px';
    aapSearchLbl.style.whiteSpace = 'nowrap'; aapSearchLbl.style.textOverflow = 'ellipsis'; aapSearchLbl.style.verticalAlign = 'center';
    var aapSearchClear = lbl(aapSearchField, '×', S.muted, 16); aapSearchClear.hittest = true;
    aapSearchClear.style.verticalAlign = 'center'; aapSearchClear.style.padding = '0px 10px';
    function aapSearchPaint() {
      aapSearchLbl.text = (aapQuery + (aapQueryFocus ? ' |' : '')) || T('Search animations: shoot, signal, walk, explosion…');
      aapSearchLbl.style.color = aapQuery ? S.value : S.muted;
      aapSearchField.style.border = '1px solid ' + (aapQueryFocus ? (AAP_AC + 'ff') : S.tileBorder);
      aapSearchClear.visible = !!aapQuery;
    }
    aapSearchClear.SetPanelEvent('onactivate', function () {
      aapQuery = ''; aapTop = 0; aapSearchPaint(); aapApplyFilter();
      if (aapQueryFocus) configWindowBlur();
    });
    var aapPrevBlur = configWindowBlur, aapPrevChars = configWindowChars;
    configWindowBlur = function () { aapQueryFocus = false; aapSearchPaint(); aapPrevBlur(); };
    configWindowChars = function (csv) {
      if (!aapQueryFocus) { aapPrevChars(csv); return; }
      var before = aapQuery;
      String(csv || '').split(',').forEach(function (part) {
        var n = parseInt(part, 10);
        if (n === 13 || n === 27) configWindowBlur();
        else if (n === 8) aapQuery = aapQuery.slice(0, -1);
        else if (aapQuery.length < 40 && ((n >= 48 && n <= 57) || (n >= 65 && n <= 90) ||
                 (n >= 97 && n <= 122) || n === 32 || n === 45 || n === 95)) {
          aapQuery += String.fromCharCode(n);
        }
      });
      if (aapQuery !== before) { aapTop = 0; aapApplyFilter(); }
      aapSearchPaint();
    };
    function aapSearchFocusOn() {
      configWindowBlur(); aapQueryFocus = true; cfgTextOwner = aapSearchField;
      root.SetAttributeString('textcapture', '1');
      aapSearchPaint();
    }
    aapSearchField.SetPanelEvent('onactivate', aapSearchFocusOn);
    aapSearchPaint();

    // Clean labeled dropdown filters (replaces the cluttered two-row chip wall)
    var aapCatDrop = dropdownRow(aapSec, T('Category'), AAP_CATS, function (k) {
      aapCat = k; aapTop = 0; aapApplyFilter();
    });
    var aapSrcDrop = dropdownRow(aapSec, T('Character'), AAP_SOURCES, function (k) {
      aapSource = k; aapTop = 0; aapApplyFilter();
    });
    aapCatDrop.sync(aapCat);
    aapSrcDrop.sync(aapSource);
)CFJS"
R"CFJS(
    // Paginated single-column clip list
    var aapListHead = mk('Panel', aapSec); aapListHead.hittest = false;
    aapListHead.style.width = '100%'; aapListHead.style.flowChildren = 'right'; aapListHead.style.marginTop = '8px';
    var aapCount = lbl(aapListHead, T('LOADING...'), AAP_SUB, 11); aapCount.hittest = false;
    aapCount.style.width = 'fill-parent-flow(1.0)'; aapCount.style.fontWeight = 'bold'; aapCount.style.letterSpacing = '1.2px';
    aapCount.style.verticalAlign = 'center';
    var aapPageLbl = lbl(aapListHead, '', AAP_CAP, 11); aapPageLbl.hittest = false;
    aapPageLbl.style.verticalAlign = 'center'; aapPageLbl.style.marginRight = '6px';
    function aapPageBtn(glyph, delta) {
      var b = btn(aapListHead, glyph, function () { aapScroll(delta); }, S.label);
      b.style.width = '24px'; b.style.height = '20px'; b.style.marginRight = '0px'; b.style.marginLeft = '3px';
      b.style.padding = '0px'; b.__lbl.style.horizontalAlign = 'center'; b.__lbl.style.verticalAlign = 'center';
      return b;
    }
    var aapPrev = aapPageBtn('‹', -AAP_ROWS);
    var aapNext = aapPageBtn('›', AAP_ROWS);

    var aapListBox = mk('Panel', aapSec); aapListBox.hittest = false;
    aapListBox.style.width = '100%'; aapListBox.style.flowChildren = 'down'; aapListBox.style.marginTop = '4px';
    aapListBox.style.backgroundColor = '#0f1318ff'; aapListBox.style.border = '1px solid ' + S.tileBorder;
    aapListBox.style.borderRadius = '4px'; aapListBox.style.padding = '4px'; aapListBox.style.overflow = 'clip';
    var aapEmptyLbl = lbl(aapListBox, T('No animations match your search.'), S.muted, 12); aapEmptyLbl.hittest = false;
    aapEmptyLbl.style.margin = '12px 10px'; aapEmptyLbl.visible = false;
    var aapCards = [], aapLastClickId = '', aapLastClickMs = 0;
    for (var aki = 0; aki < AAP_ROWS; aki++) (function (i) {
      var c = mk('Panel', aapListBox); c.style.width = '100%'; c.style.height = AAP_ROW_H + 'px';
      c.style.marginBottom = '2px'; c.style.borderRadius = '3px'; c.style.padding = '4px 8px'; c.style.overflow = 'clip';
      var inner = mk('Panel', c); inner.hittest = false; inner.style.width = '100%'; inner.style.flowChildren = 'down';
      inner.style.verticalAlign = 'center';
      var top = mk('Panel', inner); top.hittest = false; top.style.width = '100%'; top.style.flowChildren = 'right';
      var n = lbl(top, '', S.label, 12); n.hittest = false; n.style.width = 'fill-parent-flow(1.0)';
      n.style.fontWeight = 'bold'; n.style.whiteSpace = 'nowrap'; n.style.textOverflow = 'ellipsis';
      n.style.marginRight = '8px';
      var dur = lbl(top, '', AAP_SUB, 11); dur.hittest = false; dur.style.fontFamily = S.fontMonodigit;
      dur.style.width = 'fit-children'; dur.style.whiteSpace = 'nowrap'; dur.style.textAlign = 'right';
      dur.style.verticalAlign = 'center';
      var sub = lbl(inner, '', S.muted, 11); sub.hittest = false; sub.style.width = '100%';
      sub.style.marginTop = '1px'; sub.style.whiteSpace = 'nowrap'; sub.style.textOverflow = 'ellipsis';
      c.__name = n; c.__dur = dur; c.__sub = sub;
      hoverable(c, aapPaintCard);
      c.SetPanelEvent('onactivate', function () {
        var row = aapFiltered[aapTop + i];
        if (!row) return;
        var now = Date.now();
        var isDbl = aapLastClickId === row[0] && (now - aapLastClickMs) <= 450;
        aapSelectClip(row[0]);
        if (isDbl) {
          aapLastClickId = ''; aapLastClickMs = 0;
          if (aapTargetTrack >= 0) aapAssignToBot(); else aapPlayOnMe();
        } else {
          aapLastClickId = row[0]; aapLastClickMs = now;
        }
      });
      aapCards.push(c);
    })(aki);
    function aapPaintCard(c) {
      var on = !!c.__clip && c.__clip === aapClipId;
      c.style.backgroundColor = on ? (AAP_AC + '24') : (c.__hot ? '#1b2027ff' : S.clear);
      c.style.border = '1px solid ' + (on ? (AAP_AC + '80') : S.clear);
      c.__name.style.color = on ? S.value : S.label;
    }

    // Playback timing & weapon selector below clip list
    var aapPlayGrp = aapSec.__group ? aapSec.__group(T('TIMING AND WEAPON')) : aapSec;
    var aapWpnDrop = dropdownRow(aapPlayGrp, T('Weapon'), AAP_WEAPONS, function (key) {
      aapWeapon = key; cmd('mirv_filmmaker takes anim weapon ' + key);
    });
    aapWpnDrop.sync(aapWeapon);
    var aapSpeedRow = sliderRow(aapPlayGrp, { label: T('Speed'), min: 0.1, max: 3.0, decimals: 2, unit: 'x',
      onChange: function (v) { aapSpeed = v; aapRefreshView(); cmd('mirv_filmmaker takes anim speed ' + v.toFixed(2)); } });
    var aapStartRow = sliderRow(aapPlayGrp, { label: T('Start time'), min: 0, max: 15, decimals: 2, unit: 's',
      onChange: function (v) { aapStart = v; aapRefreshView(); } });
    var aapDurRow = sliderRow(aapPlayGrp, { label: T('Duration'), min: 0, max: 30, decimals: 2, unit: 's',
      tip: T('0 plays the full animation length.'),
      onChange: function (v) { aapDur = v; aapRefreshView(); } });
    var aapBlendInRow = sliderRow(aapPlayGrp, { label: T('Blend in'), min: 0, max: 1.5, decimals: 2, unit: 's',
      onChange: function (v) { aapBlendIn = v; } });
    var aapBlendOutRow = sliderRow(aapPlayGrp, { label: T('Blend out'), min: 0, max: 1.5, decimals: 2, unit: 's',
      onChange: function (v) { aapBlendOut = v; } });
    aapSpeedRow.sync(aapSpeed); aapLoopPill.set(aapLoop);
    aapStartRow.sync(aapStart); aapDurRow.sync(aapDur);
    aapBlendInRow.sync(aapBlendIn); aapBlendOutRow.sync(aapBlendOut);

    function aapApplyFilter() {
      aapFiltered = [];
      var list = aapCatalog || [];
      var q = aapQuery.toLowerCase();
      for (var i = 0; i < list.length; i++) {
        var r = list[i];
        if (aapCat >= 0 && r[2] !== aapCat) continue;
        if (aapSource !== 'all' && aapSourceOf(r) !== aapSource) continue;
        if (q && ('' + r[1]).toLowerCase().indexOf(q) < 0 && ('' + r[0]).toLowerCase().indexOf(q) < 0) continue;
        aapFiltered.push(r);
      }
      if (aapTop >= aapFiltered.length) aapTop = 0;
      aapPaintClips();
    }
    function aapScroll(delta) {
      if (!aapFiltered.length) return;
      var maxTop = Math.max(0, Math.floor((aapFiltered.length - 1) / AAP_ROWS) * AAP_ROWS);
      aapTop = Math.max(0, Math.min(maxTop, aapTop + delta));
      aapPaintClips();
    }
    function aapPaintClips() {
      aapCount.text = aapCatalog ? TN(aapFiltered.length, '{0} CLIP', '{0} CLIPS') : T('LOADING...');
      var pages = Math.max(1, Math.ceil(aapFiltered.length / AAP_ROWS));
      var curPage = Math.floor(aapTop / AAP_ROWS) + 1;
      aapPageLbl.text = curPage + ' / ' + pages;
      aapEmptyLbl.visible = !!aapCatalog && !aapFiltered.length;
      for (var i = 0; i < AAP_ROWS; i++) {
        var c = aapCards[i], row = aapFiltered[aapTop + i];
        c.visible = !!row; c.__clip = row ? row[0] : '';
        if (!row) continue;
        c.__name.text = row[1];
        c.__dur.text = aapDurationLabel(row);
        c.__sub.text = aapCatName(row[2]) + ' · ' + (row[4] ? T('Loop') : T('One-shot')) + ' · ' + aapWeaponName(row[6]);
        aapPaintCard(c);
      }
    }
    function aapSelectClip(id) {
      aapClipId = id;
      var aapRow = null;
      for (var ai = 0; aapCatalog && ai < aapCatalog.length; ai++) if (aapCatalog[ai][0] === id) { aapRow = aapCatalog[ai]; break; }
      if (aapRow) {
        // Default Loop to the clip's own loopable flag so one-shots like Explosion Wince unlock movement cleanly.
        aapLoop = !!aapRow[4];
        aapLoopPill.set(aapLoop);
        aapPreviewTitle.text = aapRow[1];
        aapPreviewMeta.text = aapCatName(aapRow[2]) + ' · ' + aapRow[3].toFixed(2) + 's · ' + aapSpeedLabel(aapRow) + ' · ' + aapWeaponName(aapRow[6]);
      } else {
        aapPreviewTitle.text = ''; aapPreviewMeta.text = '';
      }
      aapViewMsg.visible = !id;
      for (var i = 0; i < aapCards.length; i++) aapPaintCard(aapCards[i]);
      if (id && aapVisibleOnScreen()) { aapEnsure3d(); aapRefreshView(); }
    }
    function aapRequestCatalog() {
      if (!aapCatalog && !aapCatalogAsked) { aapCatalogAsked = true; cmd('mirv_filmmaker takes anim catalog'); }
    }

    renderHooks.push(function (st) {
      var k = st && st.takes;
      if (k && typeof k.localAnimUpper === 'boolean') {
        aapUpperBody = !!k.localAnimUpper;
        aapUpperPill.set(aapUpperBody);
      }
      if (k && typeof k.localAnimSpeed === 'number') {
        aapSpeed = k.localAnimSpeed;
        aapSpeedRow.sync(aapSpeed);
      }
      var playingClip = (k && k.localAnim) ? k.localAnim : '';
      aapLiveBar.visible = !!playingClip;
      if (playingClip) aapLiveLbl.text = T('Playing: {0}', playingClip);
      var noteTxt = playingClip ? playingClip : (aapCatalog ? TN(aapCatalog.length, '{0} clip', '{0} clips') : T('Browse'));
      aapSec.__note(noteTxt);

      var onScreen = aapVisibleOnScreen();
      if (onScreen) {
        aapRequestCatalog();
        if (aapClipId) {
          aapEnsure3d();
          if (!aapViewLive) aapRefreshView();
        }
        if (!aapTickOn) { aapTickOn = true; $.Schedule(1 / 30, aapTick); }
      } else {
        if (aap3dClip) aap3dClip.visible = false;
        if (aapViewLive) { aapViewLive = false; cmd('mirv_filmmaker takes anim view off'); }
        if (aapQueryFocus) configWindowBlur();
      }
    });

    $.ConfigHudAap = {
      setCatalog: function (rows) {
        aapCatalog = rows || []; aapTop = 0; aapApplyFilter();
        if (!aapClipId && aapFiltered.length) aapSelectClip(aapFiltered[0][0]);
      },
      isOpen: function () { return aapVisibleOnScreen(); },
      open: function (trackIdx) {
        if (typeof trackIdx === 'number') aapTargetTrack = trackIdx;
        if (typeof cfgSearchActive !== 'undefined') cfgSearchActive = false;
        if (typeof activeTab !== 'undefined') activeTab = 'bots';
        if (typeof applyTabs === 'function') applyTabs();
        cmd('mirv_filmmaker settings ui config.tab bots');
        if (typeof cfgSelectCard === 'function') cfgSelectCard('bots', 'bots.anims', true);
        aapRequestCatalog();
        aapPaintClips();
        if (aapClipId && aapVisibleOnScreen()) { aapEnsure3d(); aapRefreshView(); }
        if (!aapTickOn && aapVisibleOnScreen()) { aapTickOn = true; $.Schedule(1 / 30, aapTick); }
      },
      close: function () {
        aapDestroy3d();
        if (aapViewLive) { aapViewLive = false; cmd('mirv_filmmaker takes anim view off'); }
        if (aapQueryFocus) configWindowBlur();
      },
      focusSearch: function () { aapSearchFocusOn(); return 'ok'; },
      pick: function (idx) { var r = aapFiltered[aapTop + (idx | 0)]; if (r) aapSelectClip(r[0]); return aapClipId + ' loop=' + (aapLoop ? 1 : 0); },
      logState: function () { $.Msg('[aap_state] ' + JSON.stringify({ open: aapVisibleOnScreen(), total: aapCatalog ? aapCatalog.length : 0, filtered: aapFiltered.length, query: aapQuery, focus: aapQueryFocus, clip: aapClipId, loop: aapLoop, upper: aapUpperBody }) + '\n'); },
      wheel: function (notches) { aapScroll(-notches * 2); }
    };
)CFJS"
