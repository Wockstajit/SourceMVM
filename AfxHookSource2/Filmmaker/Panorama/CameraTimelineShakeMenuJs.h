#pragma once

// The keyframe SHAKE menu: right-click a diamond on the camera timeline and this opens over the
// bar, Rockstar Editor style -- one style, one intensity, one speed.
//
// Fragment only: this file is concatenated into kCameraTimelineJs as adjacent raw string
// literals, exactly like CameraTimelineRangeJs.h. It shares that IIFE's scope (S / UI / mk / lbl /
// btn / btnSel / cmd / root / panel / ctx / st).
//
// Why right-click and not a button: the timeline already spends its footer on keyframe verbs
// (add / delete / retime / ease). Shake is a per-keyframe PROPERTY, and the graph editor next
// door already teaches right-click-a-key-for-its-properties.
//
// It stays open until you click somewhere else -- a full-screen catcher sits under it and
// dismisses on activate, which is the only way to get click-outside in this Panorama context
// (there is no focus-out event on the in-game HUD).
//
// DESIGN NOTE -- why intensity and speed are METERS, not "5 / 10" text. This is a dial you set by
// feel while watching the shot, not a form field: a number has to be read and compared, a filled
// bar is a quantity you take in at a glance and can nudge with the arrows without looking away
// from the viewport. It is also what the Rockstar Editor's own stepped controls read like. The
// number stays above the bar for when you do want the exact value.
//
// The style list is NOT hardcoded here. CameraTimelineHud pushes it once from the C++ table
// (setShakeStyles), so the index the menu sends is always the index the engine means.

R"TLJS(
    // ---- keyframe shake menu -------------------------------------------
    // SHAKE_MENU: the curated camera styles, [{i: engine style index, l: 'Hand'}, ...].
    // SHAKE_LABELS: engine style index -> label, for anything set from the console that is not
    // on the menu. SHAKE_RSE_COUNT: how many leading entries are the real Rockstar Editor set.
    // All pushed by C++ (setShakeStyles), so there is no name list here to drift out of step.
    var SHAKE_MENU = [], SHAKE_LABELS = {}, SHAKE_RSE_COUNT = 0;
    var shakeMenu = null, shakeCatcher = null, shakeList = null, shakeListInner = null;
    var shakeListY = 0;          // manual scroll offset, css px, 0 = top
    var shakeKey = -1;           // marker index the menu is editing; < 0 = closed
    var shakeListOpen = false;
    var shakeRefs = {};

    function shakeSettingFor(i) {
      if (!st || !st.shakes) return null;
      for (var k = 0; k < st.shakes.length; k++) if (st.shakes[k].i === i) return st.shakes[k];
      return null;
    }
    function shakeCount() { return (st && st.shakes) ? st.shakes.length : 0; }
    function shakeStyleName(idx) {
      if (idx == null || idx < 0) return 'None';
      return SHAKE_LABELS[idx] || ('#' + idx);
    }
    function shakeCmd(tail) { cmd('mirv_filmmaker pathshake ' + tail); }

    function closeShakeMenu() {
      shakeKey = -1; shakeListOpen = false; shakeListY = 0;
      publishMenuOpen();
      if (shakeMenu) shakeMenu.visible = false;
      if (shakeCatcher) { shakeCatcher.visible = false; shakeCatcher.hittest = false; }
    }

    // A segmented magnitude bar. Segments rather than a continuous fill because both values ARE
    // discrete (1-10, and speed in steps of 10) -- a smooth bar would imply precision the control
    // does not have, and you could not tell 6 from 7 without reading the number.
    function shakeMeter(parent, count) {
      var m = mk('Panel', parent);
      m.hittest = false;
      m.style.flowChildren = 'right';
      m.style.width = '100%'; m.style.height = '6px';
      m.__cells = [];
      for (var i = 0; i < count; i++) {
        var c = mk('Panel', m);
        c.style.width = 'fill-parent-flow(1.0)'; c.style.height = '100%';
        c.style.marginRight = (i === count - 1) ? '0px' : '3px';
        c.style.borderRadius = '1px';
        c.style.backgroundColor = '#ffffff14';
        m.__cells.push(c);
      }
      return m;
    }
    function setShakeMeter(m, filled, color) {
      for (var i = 0; i < m.__cells.length; i++)
        m.__cells[i].style.backgroundColor = (i < filled) ? color : '#ffffff14';
    }

    // ◀ [ value + meter ] ▶ . `steps` > 0 adds the meter; the style row passes 0 and gets a
    // clickable value instead (it opens the full list).
    function shakeRow(parent, label, steps, onStep, onPick) {
      var row = mk('Panel', parent);
      row.style.flowChildren = 'right'; row.style.width = '100%';
      row.style.marginBottom = '8px';
      row.style.height = steps > 0 ? '34px' : '30px';

      var l = lbl(row, label, S.label, 12);
      l.style.width = '68px'; l.style.verticalAlign = 'center';

      var dec = btn(row, '◀', function () { onStep(-1); }, S.accent);
      dec.style.width = '26px'; dec.style.paddingLeft = '0px'; dec.style.paddingRight = '0px';
      dec.style.marginRight = '0px'; dec.style.verticalAlign = 'center';
      dec.__lbl.style.width = '100%'; dec.__lbl.style.textAlign = 'center';

      var box = mk('Panel', row);
      box.style.width = '212px'; box.style.verticalAlign = 'center';
      box.style.marginLeft = '6px'; box.style.marginRight = '6px';
      box.style.flowChildren = 'down';

      var vl = lbl(box, '', S.value, 12);
      vl.style.width = '100%'; vl.style.textAlign = 'center';
      vl.style.fontWeight = 'bold';
      // Fixed-width value area, so the arrows never shift as the text changes. `shrink` is the
      // backstop for an off-menu style set from the console -- those raw names run 40+ chars and
      // would otherwise be clipped mid-word.
      vl.style.textOverflow = 'shrink'; vl.style.whiteSpace = 'nowrap';

      if (steps > 0) {
        vl.style.marginBottom = '6px';
        row.__meter = shakeMeter(box, steps);
      } else {
        // The style value is the only one that opens something, so it is the only one that gets
        // a button's affordance. Meter rows must not look clickable.
        box.style.height = '26px';
        box.style.borderRadius = UI.btn.radius + 'px';
        box.style.backgroundColor = S.btnBg;
        vl.style.verticalAlign = 'center'; vl.style.height = '100%';
        if (onPick) {
          box.hittest = true;
          box.SetPanelEvent('onmouseover', function () { box.style.backgroundColor = S.btnHover; });
          box.SetPanelEvent('onmouseout', function () { box.style.backgroundColor = S.btnBg; });
          box.SetPanelEvent('onactivate', onPick);
        }
      }

      var inc = btn(row, '▶', function () { onStep(1); }, S.accent);
      inc.style.width = '26px'; inc.style.paddingLeft = '0px'; inc.style.paddingRight = '0px';
      inc.style.marginRight = '0px'; inc.style.verticalAlign = 'center';
      inc.__lbl.style.width = '100%'; inc.__lbl.style.textAlign = 'center';

      row.__val = vl;
      return row;
    }

    // 1px hairline. Separation between groups comes from space; this marks the one place the
    // menu changes register -- from editing THIS key to acting on the whole path.
    function shakeRule(parent) {
      var r = mk('Panel', parent);
      r.hittest = false;
      r.style.width = '100%'; r.style.height = '1px';
      r.style.backgroundColor = S.panelBorder;
      r.style.marginBottom = '10px';
      return r;
    }
)TLJS"
R"TLJS(
    function buildShakeMenu() {
      if (shakeMenu) return;

      // Dismiss layer. Sits UNDER the menu and over everything else in this root, so the first
      // click anywhere outside closes the menu (and is swallowed rather than also hitting the
      // control underneath -- one click, one action).
      shakeCatcher = mk('Panel', root);
      shakeCatcher.style.width = '100%'; shakeCatcher.style.height = '100%';
      shakeCatcher.style.backgroundColor = 'rgba(0,0,0,0)';
      shakeCatcher.style.zIndex = '60';
      shakeCatcher.visible = false; shakeCatcher.hittest = false;
      shakeCatcher.SetPanelEvent('onactivate', function () { closeShakeMenu(); });

      shakeMenu = mk('Panel', root);
      shakeMenu.hittest = true;
      shakeMenu.style.zIndex = '61';
      shakeMenu.style.width = '390px';
      shakeMenu.style.height = 'fit-children';
      shakeMenu.style.horizontalAlign = 'left'; shakeMenu.style.verticalAlign = 'bottom';
      shakeMenu.style.backgroundColor = 'rgba(16,20,26,0.99)';
      shakeMenu.style.border = '1px solid ' + S.panelBorder;
      shakeMenu.style.borderRadius = '6px';
      // Offset + soft blur: the menu floats above an opaque bar, so it needs real depth rather
      // than a halo.
      shakeMenu.style.boxShadow = '#000000ee 0px 6px 26px 4px';
      shakeMenu.style.flowChildren = 'down';
      shakeMenu.style.paddingTop = '14px'; shakeMenu.style.paddingBottom = '14px';
      shakeMenu.style.paddingLeft = '16px'; shakeMenu.style.paddingRight = '16px';
      shakeMenu.style.fontFamily = S.font;
      // Clicks inside must not reach the catcher behind it.
      shakeMenu.SetPanelEvent('onactivate', function () { });
      shakeMenu.visible = false;

      // --- header: what this is, which key, and the way out ---
      var head = mk('Panel', shakeMenu);
      head.style.flowChildren = 'right'; head.style.width = '100%'; head.style.marginBottom = '14px';
      var ht = lbl(head, 'KEYFRAME', S.value, 13);
      ht.style.fontWeight = 'bold'; ht.style.letterSpacing = '1.5px'; ht.style.verticalAlign = 'center';

      // The key number as a chip, not a sentence: it is an identifier, and the eye should find it
      // without reading. Fixed height so the header row cannot change height with its text.
      var chip = mk('Panel', head);
      chip.hittest = false;
      chip.style.marginLeft = '10px'; chip.style.verticalAlign = 'center';
      chip.style.height = '18px'; chip.style.borderRadius = '9px';
      chip.style.paddingLeft = '9px'; chip.style.paddingRight = '9px';
      chip.style.backgroundColor = S.btnOn;
      shakeRefs.key = lbl(chip, '', S.accent, 11);
      shakeRefs.key.style.verticalAlign = 'center'; shakeRefs.key.style.fontWeight = 'bold';

      var spacer = mk('Panel', head);
      spacer.hittest = false; spacer.style.width = 'fill-parent-flow(1.0)'; spacer.style.height = '1px';

      var x = btn(head, '✕', function () { closeShakeMenu(); }, S.label);
      x.style.marginRight = '0px';

      // --- EASE: the third surface that writes this key's ease (Docs/features/camera-editor-feature.md) ------------
      // Reads the ease of the key the menu was opened ON (shakeKey), not of the selected key --
      // right-clicking a key you have not clicked first would otherwise show, and edit, a
      // different key's ease.
      var easeHead = mk('Panel', shakeMenu);
      easeHead.style.flowChildren = 'right'; easeHead.style.width = '100%';
      easeHead.style.marginBottom = '7px';
      var easeCap = lbl(easeHead, 'EASE', S.dim, 10);
      easeCap.style.fontWeight = 'bold'; easeCap.style.letterSpacing = '1.6px';
      easeCap.style.verticalAlign = 'center';
      var easeSpacer = mk('Panel', easeHead); easeSpacer.hittest = false;
      easeSpacer.style.width = 'fill-parent-flow(1.0)'; easeSpacer.style.height = '1px';
      var easeHint = lbl(easeHead, 'this key only', S.dim, 10);
      easeHint.style.verticalAlign = 'center'; easeHint.style.whiteSpace = 'nowrap';

      var easeRow = mk('Panel', shakeMenu);
      easeRow.style.flowChildren = 'right'; easeRow.style.width = '100%';
      easeRow.style.marginBottom = '13px';
      shakeRefs.easeBtns = {};
      for (var ei = 0; ei < 4; ei++) (function (idx, cap, first) {
        var b = btn(easeRow, cap, function () {
          if (shakeKey < 0) return;
          cmd('mirv_filmmaker camtl ease ' + shakeKey + ' ' + EASE[idx]);
        }, S.value);
        b.style.marginLeft = first ? '0px' : '0px';
        b.style.marginRight = '2px';
        shakeRefs.easeBtns[idx] = b;
      })(ei, ['None', 'In', 'Out', 'In-Out'][ei], ei === 0);

      var easeDiv = mk('Panel', shakeMenu); easeDiv.hittest = false;
      easeDiv.style.width = '100%'; easeDiv.style.height = '1px';
      easeDiv.style.marginBottom = '13px';
      easeDiv.style.backgroundColor = '#ffffff14';

      // --- the three shake properties ---
      shakeRefs.style = shakeRow(shakeMenu, 'Style', 0,
        function (d) { shakeCmd('style ' + shakeKey + (d > 0 ? ' next' : ' prev')); },
        function () { shakeListOpen = !shakeListOpen; renderShakeMenu(); });
      shakeRefs.intensity = shakeRow(shakeMenu, 'Intensity', 10,
        function (d) { shakeCmd('intensity ' + shakeKey + (d > 0 ? ' next' : ' prev')); }, null);
      shakeRefs.speed = shakeRow(shakeMenu, 'Speed', 10,
        function (d) { shakeCmd('speed ' + shakeKey + (d > 0 ? ' next' : ' prev')); }, null);

      // Scrollable list of every style. Hidden until the style value is clicked -- the arrows are
      // the fast path; this is for jumping straight to one.
      // A CLIPPED VIEWPORT with an inner column, not `overflow: scroll`. Native scrolling is
      // dead here: the wheel never reaches Panorama in this HUD context, so a scroll container
      // can only be dragged. shakeListInner is moved by marginTop from api.wheel() below.
      shakeList = mk('Panel', shakeMenu);
      shakeList.style.width = '100%'; shakeList.style.height = '250px';
      shakeList.style.overflow = 'clip';
      shakeList.style.flowChildren = 'down';
      shakeList.style.marginBottom = '10px';
      shakeList.style.backgroundColor = '#00000055';
      shakeList.style.borderRadius = '4px';
      shakeList.visible = false;
      shakeListInner = mk('Panel', shakeList);
      shakeListInner.style.width = '100%';
      shakeListInner.style.height = 'fit-children';
      shakeListInner.style.flowChildren = 'down';
      shakeListInner.style.transitionDuration = '0s';
)TLJS"
R"TLJS(
      // --- actions: this key, then the whole path ---
      shakeRefs.rule = shakeRule(shakeMenu);

      var foot = mk('Panel', shakeMenu);
      foot.style.flowChildren = 'right'; foot.style.width = '100%';

      // Apply to All leads: dialling one keyframe in and pushing it across is the normal way to
      // shake a shot, and it is the only action here that is not destructive.
      shakeRefs.applyAll = btn(foot, 'Apply to All', function () {
        shakeCmd('applyall ' + shakeKey);
      }, S.accent);
      shakeRefs.applyAll.style.backgroundColor = S.btnOn;

      shakeRefs.remove = btn(foot, 'Remove', function () {
        shakeCmd('clear ' + shakeKey);
        closeShakeMenu();
      }, S.value);

      var footSpacer = mk('Panel', foot);
      footSpacer.hittest = false;
      footSpacer.style.width = 'fill-parent-flow(1.0)'; footSpacer.style.height = '1px';

      // Remove All is the one irreversible action in the menu, so it sits apart from the pair
      // above, carries the danger colour, and confirms in place rather than firing on one click.
      shakeRefs.removeAllArmed = false;
      shakeRefs.removeAll = btn(foot, 'Remove All', function () {
        if (!shakeRefs.removeAllArmed) { shakeRefs.removeAllArmed = true; renderShakeMenu(); return; }
        shakeRefs.removeAllArmed = false;
        shakeCmd('clearall');
        closeShakeMenu();
      }, '#ff8a8aff');
      shakeRefs.removeAll.style.marginRight = '0px';
    }


    // Published on the panel root so MovieMode's wheel router can see it: while this menu is up
    // the wheel belongs to it, not to the workspace behind it.
    function publishMenuOpen() {
      try { root.SetAttributeString('menuopen', (shakeKey >= 0) ? '1' : '0'); } catch (e) {}
    }
    // Called from C++ (CameraTimelineHud::DispatchWheel) with the notches captured this frame.
    // Only the STYLE LIST scrolls -- the menu itself always fits.
    function shakeMenuWheel(notches) {
      if (shakeKey < 0 || !shakeListOpen || !shakeList || !shakeListInner) return false;
      var viewH = shakeList.actuallayoutheight / (shakeList.actualuiscale_y || 1);
      var contH = shakeListInner.actuallayoutheight / (shakeListInner.actualuiscale_y || 1);
      var maxY = Math.max(0, contH - viewH);
      if (maxY <= 0.5) return true;   // it all fits; still ours, just nothing to move
      shakeListY -= notches * 60;
      if (shakeListY < 0) shakeListY = 0;
      if (shakeListY > maxY) shakeListY = maxY;
      shakeListInner.style.marginTop = (-Math.round(shakeListY)) + 'px';
      return true;
    }
    var shakeListBuiltFor = -1;
    function rebuildShakeList() {
      shakeListInner.RemoveAndDeleteChildren();
      for (var i = 0; i < SHAKE_MENU.length; i++) (function (i) {
        var e = SHAKE_MENU[i];
        // Section headers. The first block is the five-shake reference set (see the C++ style
        // table for where that list comes from); everything after is ours. The break is worth
        // labelling because the top five cover almost every shot -- but the labels are written
        // for a director choosing a look, so they name the CONTENT, not its provenance. No
        // third-party product names anywhere in the in-game UI.
        if (i === 0 || i === SHAKE_RSE_COUNT) {
          var h = mk('Panel', shakeListInner);
          h.hittest = false; h.style.width = '100%'; h.style.height = '24px';
          h.style.paddingLeft = '12px'; h.style.marginTop = (i === 0 ? 4 : 10) + 'px';
          var hd = lbl(h, i === 0 ? 'ESSENTIALS' : 'MORE CAMERA SHAKES', S.hint, 10);
          hd.style.verticalAlign = 'center'; hd.style.letterSpacing = '1.2px';
          hd.style.fontWeight = 'bold';
        }
        var r = mk('Panel', shakeListInner);
        r.hittest = true; r.style.width = '100%'; r.style.height = '26px';
        r.style.paddingLeft = '12px'; r.style.paddingRight = '12px';
        var t = lbl(r, e.l, S.label, 12);
        t.style.verticalAlign = 'center';
        t.style.whiteSpace = 'nowrap'; t.style.textOverflow = 'shrink';
        r.__t = t; r.__i = e.i;   // __i is the ENGINE style index, not the row number
        r.SetPanelEvent('onmouseover', function () { r.style.backgroundColor = S.btnHover; });
        r.SetPanelEvent('onmouseout', function () { r.style.backgroundColor = 'rgba(0,0,0,0)'; });
        r.SetPanelEvent('onactivate', function () {
          // Send the numeric style index, not the label -- labels have spaces and would arrive at
          // the console command as several arguments.
          shakeCmd('style ' + shakeKey + ' ' + e.i);
          shakeListOpen = false; renderShakeMenu();
        });
      })(i);
      shakeListBuiltFor = SHAKE_MENU.length;
    }

    // Position the menu above the timeline bar, horizontally centred on the diamond it belongs
    // to. The diamond's x is the SAME frac math rebuildTimelineDiamonds uses, so the two cannot
    // drift apart; the bar's own left edge and height come from its live layout, so this stays
    // right in the hosted (editor) bar, the standalone card, and after a resolution change.
    function positionShakeMenu(diamondX) {
      var uis = root.actualuiscale_x || ctx.actualuiscale_x || 1;
      var screenW = (ctx.actuallayoutwidth || 0) / uis;
      var panelLeft = (panel.actualxoffset || 0) / uis;
      var barH = (panel.actuallayoutheight || 0) / ((panel.actualuiscale_y || uis) || 1);
      var menuW = 390; // keep in sync with shakeMenu.style.width in buildShakeMenu
      var left = panelLeft + 14 + diamondX - menuW / 2;
      if (left < 8) left = 8;
      if (screenW > menuW && left > screenW - menuW - 8) left = screenW - menuW - 8;
      shakeMenu.style.marginLeft = Math.round(left) + 'px';
      shakeMenu.style.marginBottom = Math.round((barH > 8 ? barH : 120) + 10) + 'px';
    }

    function openShakeMenu(i, diamondX) {
      buildShakeMenu();
      shakeKey = i;
      shakeListOpen = false;
      shakeListY = 0;      // a fresh menu always opens its list at the top
      publishMenuOpen();   // the wheel is ours from here until this closes
      shakeRefs.removeAllArmed = false; // never open pre-armed on a destructive action
      positionShakeMenu(diamondX);
      shakeCatcher.visible = true; shakeCatcher.hittest = true;
      shakeMenu.visible = true;
      renderShakeMenu();
    }
)TLJS"
R"TLJS(
    function renderShakeMenu() {
      if (shakeKey < 0 || !shakeMenu) return;
      // The keyframe went away (deleted, cleared, or the demo changed): close rather than keep
      // editing an index that now means a different key.
      if (!st || !st.markers || shakeKey >= st.markers.length) { closeShakeMenu(); return; }

      var s = shakeSettingFor(shakeKey);
      shakeRefs.key.text = 'KEY ' + (shakeKey + 1);
      // Light the ease this key actually has. Per-key, straight off the marker -- the selected
      // key's ease is a different value and would be wrong for any key you right-click without
      // selecting first.
      var kEase = (st.markers[shakeKey] && st.markers[shakeKey].ease) || 0;
      for (var eb in shakeRefs.easeBtns) btnSel(shakeRefs.easeBtns[eb], Number(eb) === kEase);

      shakeRefs.style.__val.text = s ? shakeStyleName(s.s) : 'None — press ▶';
      shakeRefs.style.__val.style.color = s ? S.value : S.hint;

      // Meters and numbers. Speed is 10-100 in steps of 10, so it fills the same ten segments as
      // intensity -- the two dials read as the same kind of control because they are.
      shakeRefs.intensity.__val.text = s ? String(s.n) : '–';
      shakeRefs.intensity.__val.style.color = s ? S.value : S.hint;
      setShakeMeter(shakeRefs.intensity.__meter, s ? s.n : 0, S.accent);

      shakeRefs.speed.__val.text = s ? String(s.p) : '–';
      shakeRefs.speed.__val.style.color = s ? S.value : S.hint;
      setShakeMeter(shakeRefs.speed.__meter, s ? Math.round(s.p / 10) : 0, S.freeze);

      // Both per-key actions need a shake on THIS key to act on.
      shakeRefs.applyAll.visible = !!s;
      shakeRefs.remove.visible = !!s;
      // Remove All acts on the path, so it stands on its own count.
      var total = shakeCount();
      shakeRefs.removeAll.visible = total > 0;
      shakeRefs.rule.visible = !!s || total > 0;
      if (shakeRefs.removeAllArmed && total > 0) {
        shakeRefs.removeAll.__lbl.text = 'Remove All — sure?';
        shakeRefs.removeAll.__lbl.style.color = '#ffb3b3ff';
        shakeRefs.removeAll.style.backgroundColor = '#c92a2a33';
      } else {
        shakeRefs.removeAllArmed = false;
        shakeRefs.removeAll.__lbl.text = 'Remove All (' + total + ')';
        shakeRefs.removeAll.__lbl.style.color = '#ff8a8aff';
        shakeRefs.removeAll.style.backgroundColor = S.btnBg;
      }

      if (shakeListOpen && SHAKE_MENU.length) {
        if (shakeListBuiltFor !== SHAKE_MENU.length) rebuildShakeList();
        shakeList.visible = true;
        var sel = s ? s.s : -1;
        for (var c = 0; c < shakeList.GetChildCount(); c++) {
          var ch = shakeList.GetChild(c);
          if (ch.__t) ch.__t.style.color = (ch.__i === sel) ? S.accent : S.label;
        }
      } else {
        shakeList.visible = false;
      }
    }

    // Pushed once from C++ so the index the menu sends always matches the engine's style table.
    function setShakeStylesImpl(menu, rseCount, labels) {
      SHAKE_MENU = menu || [];
      SHAKE_RSE_COUNT = rseCount || 0;
      SHAKE_LABELS = labels || {};
      shakeListBuiltFor = -1;
      if (shakeMenu && shakeKey >= 0) renderShakeMenu();
    }
)TLJS"
