#pragma once

// CUSTOMIZE PLAYER window: its three sizes, the title-bar drag and the remembered position.
// JS fragment spliced into the Customize modal (CameraEditorCustomizeJs.h) inside the header-action
// block, so the size buttons are built between Apply and the close button. Not a standalone script:
// shares the root IIFE's scope (mk, interactive, btnVisual, S, st, custWin, custOverlay, ...).

R"EDJS(
    // ===================== WINDOW: SIZE AND POSITION =====================
    // The modal used to be a fixed 1240 x 720 card in the middle of the screen over a dark scrim, so
    // the game behind it -- the thing being changed -- could not be seen. Now it is a tool window:
    //   full       1240 x 720, slot list | 3D preview | picker
    //   compact     800 x 720, no 3D preview (the game behind the window is the preview)
    //   collapsed   800 x 56, the title bar alone
    // Drag it by the title. The header always stays on screen; the body may hang off the bottom.
    // Position and size live in C++ for the game session (CameraEditorHud m_customizeLayout, echoed
    // back as st.customizeLayout), so they survive a HUD rebuild, a demo change and either host.
    var CUST_FULL_W = 1240, CUST_COMPACT_W = 800, CUST_FULL_H = 720, CUST_HEAD_H = 56;
    var custPos = null;               // {x, y} top-left in layout px once placed; null = centred
    var custDrag = null, custLmbWas = false;

    function custWinWidth() { return custSize === 'full' ? CUST_FULL_W : CUST_COMPACT_W; }
    function custWinHeight() { return custSize === 'collapsed' ? CUST_HEAD_H : CUST_FULL_H; }
    // The header is what you drag and what holds Apply and the close button, so it never leaves the
    // screen. The body can go below the bottom edge (more of the game showing above it).
    function custClampPos(x, y, w, sw, sh, headH) {
      if (!(sw > 0) || !(sh > 0)) return { x: x, y: y };
      return { x: Math.min(Math.max(0, x), Math.max(0, sw - w)), y: Math.min(Math.max(0, y), Math.max(0, sh - headH)) };
    }
    function custScreen() {
      var sx = custOverlay.actualuiscale_x || ctx.actualuiscale_x || 1, sy = custOverlay.actualuiscale_y || ctx.actualuiscale_y || 1;
      return { sx: sx, sy: sy, w: (custOverlay.actuallayoutwidth || 0) / sx, h: (custOverlay.actuallayoutheight || 0) / sy };
    }
    function custPlace(x, y) {
      var scr = custScreen();
      var c = custClampPos(x, y, custWinWidth(), scr.w, scr.h, CUST_HEAD_H);
      custPos = { x: Math.round(c.x), y: Math.round(c.y) };
      custWin.style.horizontalAlign = 'left'; custWin.style.verticalAlign = 'top';
      custWin.style.position = custPos.x + 'px ' + custPos.y + 'px 0px';
    }
    function custCentre() {
      custPos = null;
      custWin.style.position = '0px 0px 0px';
      custWin.style.horizontalAlign = 'center'; custWin.style.verticalAlign = 'center';
    }
    function custPublishLayout() {
      var v = (custPos ? (custPos.x + ' ' + custPos.y) : 'c c') + ' ' + custSize;
      try { root.SetAttributeString('custlayout', v); } catch (e) {}
    }
    // st.customizeLayout: "<x> <y> <size>" or "c c <size>"; empty before the first move this session.
    function custRestoreLayout() {
      var parts = ('' + ((st && st.customizeLayout) || '')).split(' ');
      if (parts.length === 3 && (parts[2] === 'full' || parts[2] === 'compact' || parts[2] === 'collapsed')) custSize = parts[2];
      custApplySize();
      var x = parseFloat(parts[0]), y = parseFloat(parts[1]);
      if (parts.length === 3 && isFinite(x) && isFinite(y)) custPlace(x, y); else custCentre();
    }
    function custApplyBodyVisibility() {
      var collapsed = custSize === 'collapsed', hasTarget = custTargetController >= 0;
      custBody.visible = !collapsed && hasTarget;
      custNoTarget.visible = !collapsed && !hasTarget;
    }
    // The pickup notice: in the header at full size, under the player list in compact (the header has
    // no room left for it there), nowhere while collapsed.
    function custPickupPlacement(on) {
      custPickupBanner.visible = !!on && custSize === 'full';
      custPickupStrip.visible = !!on && custSize === 'compact';
    }
    function custShowsPreview() { return custOverlay.visible && custSize === 'full'; }
    function custApplySize() {
      var full = custSize === 'full';
      custWin.style.width = custWinWidth() + 'px';
      custWin.style.height = custWinHeight() + 'px';
      prevWrap.visible = full;
      custTitlePrefix.visible = full;
      custTitleDot.visible = full;
      custApplyBodyVisibility();
      custPickupPlacement(pickupSlots().length > 0);
      if (!full) { destroyPreview3d(); custHoldChip.visible = false; }
      else if (custOverlay.visible && !preview3d) { try { $.Schedule(0.18, recreatePreview); } catch (e) { recreatePreview(); } }
      custChromeSig = '';
      custPaintSizeButtons();
      if (custPos) custPlace(custPos.x, custPos.y);
    }
    function custSetSize(size) {
      if (size !== 'full' && size !== 'compact' && size !== 'collapsed') return;
      if (size !== 'collapsed') custSizeBeforeCollapse = size;
      custSize = size;
      custApplySize();
      custPublishLayout();
    }
    var custSizeBeforeCollapse = 'full';

    // Six dots at the left of the title: the window moves when you drag the title.
    function custGripDots(parent) {
      var g = mk('Panel', parent); g.hittest = false;
      g.style.width = '8px'; g.style.height = '14px'; g.style.marginRight = '14px'; g.style.verticalAlign = 'center';
      g.style.flowChildren = 'right-wrap';
      for (var i = 0; i < 6; i++) {
        var d = mk('Panel', g); d.hittest = false;
        d.style.width = '2px'; d.style.height = '2px'; d.style.borderRadius = '1px';
        d.style.marginRight = (i % 2) ? '0px' : '4px'; d.style.marginBottom = '4px';
        d.style.backgroundColor = '#8b95a3ff';
      }
      return g;
    }
    // A 34px header button drawn from panels (no glyph a font might lack), like the close button.
    function custIconBtn(parent, onClick, tip) {
      var b = mk('Panel', parent); b.style.verticalAlign = 'center';
      b.style.width = '34px'; b.style.height = '34px'; b.style.marginLeft = '8px'; b.style.borderRadius = '4px';
      var icon = mk('Panel', b); icon.hittest = false;
      icon.style.horizontalAlign = 'center'; icon.style.verticalAlign = 'center';
      interactive(b, icon, onClick, { bg: '#ffffff14', border: '#ffffff1f', press: true, tip: tip });
      b.__icon = icon;
      return b;
    }
    // An outlined box of w x h: a window shape. A 2px bar along the bottom: "down to the title bar".
    function custDrawBox(icon, w, h) {
      icon.style.width = w + 'px'; icon.style.height = h + 'px'; icon.style.borderRadius = '1px';
      icon.style.border = '2px solid #eef2f6ff'; icon.style.backgroundColor = 'rgba(0,0,0,0)';
      icon.style.marginTop = '0px';
    }
    function custDrawBar(icon) {
      icon.style.width = '12px'; icon.style.height = '2px'; icon.style.borderRadius = '1px';
      icon.style.border = '0px solid #00000000'; icon.style.backgroundColor = '#eef2f6ff';
      icon.style.marginTop = '8px';
    }
    var custCompactBtn = null, custCollapseBtn = null;
    function custSizeButtons(parent) {
      custCompactBtn = custIconBtn(parent, function () { custSetSize(custSize === 'full' ? 'compact' : 'full'); },
        function () { return custSize === 'full' ? 'Compact: hide the 3D preview and watch the game instead' : 'Full size: show the 3D preview'; });
      custCollapseBtn = custIconBtn(parent, function () { custSetSize(custSize === 'collapsed' ? custSizeBeforeCollapse : 'collapsed'); },
        function () { return custSize === 'collapsed' ? 'Expand' : 'Collapse to the title bar'; });
      custPaintSizeButtons();
    }
    function custPaintSizeButtons() {
      if (!custCompactBtn || !custCollapseBtn) return;
      // Each icon shows what a click does.
      if (custSize === 'full') custDrawBox(custCompactBtn.__icon, 9, 12); else custDrawBox(custCompactBtn.__icon, 15, 12);
      custCompactBtn.visible = custSize !== 'collapsed';
      if (custSize === 'collapsed') custDrawBox(custCollapseBtn.__icon, 13, 11); else custDrawBar(custCollapseBtn.__icon);
    }

    // The title-bar drag, from the editor's OS-cursor pipe (st.mx / st.my / st.lmb, client px): this
    // HUD context has no mouse-move event. A press that starts on the title or the grip -- but not on
    // the player name, which is the player menu's button -- moves the window 1:1 with the pointer
    // until release; the position is published on release.
    function custInRect(p, mx, my) {
      if (!p || (p.IsValid && !p.IsValid()) || !p.GetPositionWithinWindow || !p.visible) return false;
      var a = p.GetPositionWithinWindow();
      var w = p.actuallayoutwidth || 0, h = p.actuallayoutheight || 0;
      return !!a && mx >= a.x && mx < a.x + w && my >= a.y && my < a.y + h;
    }
    function custWindowFrame() {
      if (!custOverlay.visible || !st || !st.cursor) { custDrag = null; custLmbWas = false; return; }
      var lmb = !!st.lmb, mx = st.mx || 0, my = st.my || 0;
      var scr = custScreen();
      if (lmb && !custLmbWas && !custInRect(custPlayerBtn, mx, my)
          && (custInRect(custTitleWrap, mx, my) || custInRect(custGrip, mx, my))) {
        custSetPlayerMenu(false);
        var a = custWin.GetPositionWithinWindow ? custWin.GetPositionWithinWindow() : null;
        custDrag = { mx: mx, my: my, x: a ? a.x / scr.sx : 0, y: a ? a.y / scr.sy : 0 };
      }
      if (custDrag && lmb) custPlace(custDrag.x + (mx - custDrag.mx) / scr.sx, custDrag.y + (my - custDrag.my) / scr.sy);
      if (custDrag && !lmb) { custDrag = null; custPublishLayout(); }
      custLmbWas = lmb;
    }
)EDJS"
