#pragma once

// CUSTOMIZE PLAYER: the player menu behind the name in the title bar, and the modal's per-frame
// target pass. JS fragment spliced into the Customize modal (CameraEditorCustomizeJs.h) between the
// header and the body. Not a standalone script: shares the root IIFE's scope (mk, clbl, S, F8, st,
// custSurface, custPlayerBtn, custTitleName, ...).

R"EDJS(
    // ===================== PLAYERS =====================
    // Every T and CT player of the demo, alive or dead, from Cosmetics/CustomizeTarget.cpp
    // (st.customize). That module reads CS2's own demo bar, so a dead player keeps their place -- their
    // body stays on screen with death animations -- and is drawn at lower opacity. Picking a player
    // makes them the target and puts the view in first person on them ("cosmetics pick"): CS2's own
    // spectate for the living, the free cam at a dead player's eyes. CONFIG's CUSTOMIZE PLAYER opens
    // this same modal, so both panels pick and edit the same way.
    //
    // The menu hangs off the name in the title bar (custPlayerBtn), in the editor's dropdown look
    // (customDrop: #0e1620 panel, gold border, the rotating caret), T and CT side by side. It is a
    // child of the backdrop, not of the window, so it can drop over the body -- and over the game
    // when the window is collapsed to its title bar.
    var CUST_T = '#e0b861ff', CUST_CT = '#7fb2e5ff';   // the death-animation picker's team colours
    var CUST_DEAD_OPACITY = '0.45';
    var custTargetController = -1, custTargetSource = 'none', custTargetAlive = true, custTargetLastSeen = false;
    var custTargetOnScreen = false;    // the HUD is showing this player (the HUD paint read is theirs)
    var custPickPending = -1, custPickPendingFrames = 0;
    var custPlayerMenuOpen = false;

    // In compact size the pickup notice drops out of the header (no room) into this band.
    var custPickupStrip = clbl(custSurface, '', '#ffdba6', 11); custPickupStrip.hittest = false;
    custPickupStrip.visible = false; custPickupStrip.style.width = '100%';
    custPickupStrip.style.fontWeight = 'bold'; custPickupStrip.style.letterSpacing = '1px';
    custPickupStrip.style.textTransform = 'uppercase'; custPickupStrip.style.whiteSpace = 'normal';
    custPickupStrip.style.paddingLeft = '18px'; custPickupStrip.style.paddingRight = '18px';
    custPickupStrip.style.paddingTop = '7px'; custPickupStrip.style.paddingBottom = '7px';
    custPickupStrip.style.backgroundColor = '#3a2a10cc'; custPickupStrip.style.borderBottom = '1px solid #ffb34755';

    // No target yet (free cam, nobody watched or picked this demo): the body asks for a pick.
    var custNoTarget = mk('Panel', custSurface); custNoTarget.hittest = true; custNoTarget.visible = false;
    custNoTarget.style.width = '100%'; custNoTarget.style.height = 'fill-parent-flow(1.0)';
    var custNoTargetLbl = clbl(custNoTarget, 'Choose a player from the menu on their name to see and change their loadout.', S.hint, 14);
    custNoTargetLbl.hittest = false; custNoTargetLbl.style.horizontalAlign = 'center'; custNoTargetLbl.style.verticalAlign = 'center';

    var custPlayerMenu = mk('Panel', custOverlay); custPlayerMenu.visible = false; custPlayerMenu.hittest = true;
    custPlayerMenu.style.zIndex = '236'; custPlayerMenu.style.flowChildren = 'right';
    custPlayerMenu.style.backgroundColor = '#0e1620fa'; custPlayerMenu.style.border = '1px solid ' + S.accent;
    custPlayerMenu.style.borderRadius = '4px'; custPlayerMenu.style.boxShadow = '#000000d0 0px 4px 14px 2px';
    custPlayerMenu.style.paddingTop = '8px'; custPlayerMenu.style.paddingBottom = '8px';
    custPlayerMenu.style.paddingLeft = '8px'; custPlayerMenu.style.paddingRight = '8px';
    custPlayerMenu.SetPanelEvent('onactivate', function () { /* a click between rows keeps it open */ });
    function custTeamColumn(caption, color, gapRight) {
      var col = mk('Panel', custPlayerMenu); col.hittest = false;
      col.style.flowChildren = 'down'; col.style.minWidth = '200px'; col.style.marginRight = gapRight + 'px';
      var cap = clbl(col, caption, color, 10); cap.hittest = false;
      cap.style.fontWeight = 'bold'; cap.style.letterSpacing = '1.4px';
      cap.style.paddingLeft = '4px'; cap.style.height = '16px'; cap.style.marginBottom = '2px';
      var rows = mk('Panel', col); rows.hittest = false;
      rows.style.width = '100%'; rows.style.flowChildren = 'down';
      return { col: col, rows: rows, color: color, list: [] };
    }
    var custTeamT = custTeamColumn('T', CUST_T, 10), custTeamCT = custTeamColumn('CT', CUST_CT, 0);
    var custPlayersEmpty = clbl(custPlayerMenu, 'Players appear here once the demo is playing.', S.hint, 12);
    custPlayersEmpty.hittest = false; custPlayersEmpty.style.margin = '6px 8px';

    // The highlighted player: a click shows at once, before C++ reports the new target.
    function custShownController() { return custPickPending >= 0 ? custPickPending : custTargetController; }
    function custTeamColor(team) { return team === 3 ? CUST_CT : CUST_T; }
    function paintPlayerBtn() {
      var on = custPlayerMenuOpen, hot = !!custPlayerBtn.__hot;
      custPlayerBtn.style.backgroundColor = on ? '#1a2230ff' : (hot ? '#ffffff14' : '#ffffff0a');
      custPlayerBtn.style.border = '1px solid ' + (on ? S.accent : (hot ? '#f0b32366' : '#ffffff1f'));
      custPlayerEdge.style.backgroundColor = custTargetController >= 0 ? custTeamColor(custTargetTeam === 3 || custTargetTeam === '3' ? 3 : 2) : '#ffffff1f';
      custPlayerBtn.style.opacity = (custTargetController >= 0 && !custTargetAlive) ? '0.75' : '1';
    }
    function paintPlayerRow(r) {
      var p = r.__p, on = !!p && p.controller === custShownController();
      r.style.opacity = (p && p.alive) ? '1' : CUST_DEAD_OPACITY;
      r.style.backgroundColor = on ? '#1a2230ff' : (r.__hot ? '#1b2027ff' : 'rgba(0,0,0,0)');
      r.style.border = '1px solid ' + (on ? '#f0b32380' : 'rgba(0,0,0,0)');
      r.__edge.style.backgroundColor = on ? S.accent : (r.__team.substring(0, 7) + 'aa');
      r.__name.style.color = on ? S.value : S.label;
    }
    function custPlayerRow(team) {
      var r = mk('Panel', team.rows);
      r.style.width = '100%'; r.style.height = '30px'; r.style.marginTop = '2px';
      r.style.flowChildren = 'right'; r.style.borderRadius = '3px'; r.style.overflow = 'clip';
      var edge = mk('Panel', r); edge.hittest = false;
      edge.style.width = '3px'; edge.style.height = '100%';
      // The full name: the column grows to the longest one rather than cutting any off.
      var n = clbl(r, '', S.label, 13); n.hittest = false;
      n.style.fontWeight = 'bold'; n.style.verticalAlign = 'center'; n.style.whiteSpace = 'nowrap';
      n.style.marginLeft = '9px'; n.style.marginRight = '12px';
      r.__edge = edge; r.__name = n; r.__team = team.color; r.__p = null; r.__hot = false;
      r.hittest = true;
      r.SetPanelEvent('onmouseover', function () {
        r.__hot = true; paintPlayerRow(r);
        if (r.__p && !r.__p.alive) showTip(r, 'Dead. Picking them shows the view from where they died.');
      });
      r.SetPanelEvent('onmouseout', function () { r.__hot = false; paintPlayerRow(r); hideTip(); });
      r.SetPanelEvent('onactivate', function () { hideTip(); if (r.__p) custPickPlayer(r.__p.controller); });
      return r;
    }
    // A pick from the menu: target them and switch the view (CustomizeTarget_Pick). Unapplied picks for
    // the previous player are dropped by the reseed, the same as when the spectated player changes.
    function custPickPlayer(controller) {
      custPickPending = controller; custPickPendingFrames = 30;
      custPaintSig = '';
      custSetPlayerMenu(false);
      cmd('mirv_filmmaker cosmetics pick ' + controller);
    }
    function custSetPlayerMenu(open) {
      custPlayerMenuOpen = !!open && custOverlay.visible;
      custPlayerMenu.visible = custPlayerMenuOpen;
      caretSpin(custPlayerCaret, custPlayerMenuOpen);
      paintPlayerBtn();
      if (custPlayerMenuOpen) { custPaintSig = ''; renderPlayers(((st && st.customize && st.customize.players) || [])); custPositionPlayerMenu(); }
    }
    // Under the name button, kept on screen; re-placed every frame while open (the window can move).
    function custPositionPlayerMenu() {
      if (!custPlayerMenuOpen) return;
      var a = custPlayerBtn.GetPositionWithinWindow ? custPlayerBtn.GetPositionWithinWindow() : null;
      if (!a) return;
      var sx = custOverlay.actualuiscale_x || ctx.actualuiscale_x || 1, sy = custOverlay.actualuiscale_y || ctx.actualuiscale_y || 1;
      var x = a.x / sx, y = (a.y + (custPlayerBtn.actuallayoutheight || 0)) / sy + 6;
      var w = (custPlayerMenu.actuallayoutwidth || 0) / sx, sw = (custOverlay.actuallayoutwidth || 0) / sx;
      if (sw > 0 && w > 0 && x + w > sw - 8) x = Math.max(8, sw - 8 - w);
      custPlayerMenu.style.position = Math.round(x) + 'px ' + Math.round(y) + 'px 0px';
    }
    var custRosterSig = '', custPaintSig = '';
    function renderPlayers(list) {
      var sig = '';
      for (var i = 0; i < list.length; i++) sig += list[i].controller + ':' + list[i].team + ':' + list[i].name + '|';
      if (sig !== custRosterSig) {
        custRosterSig = sig; custPaintSig = '';
        var teams = [custTeamT, custTeamCT];
        for (var t = 0; t < teams.length; t++) { teams[t].rows.RemoveAndDeleteChildren(); teams[t].list = []; }
        for (var j = 0; j < list.length; j++) {
          var team = list[j].team === 3 ? custTeamCT : custTeamT;
          var row = custPlayerRow(team);
          row.__name.text = list[j].name;
          team.list.push(row);
        }
        custTeamT.col.visible = custTeamT.list.length > 0;
        custTeamCT.col.visible = custTeamCT.list.length > 0;
        custTeamT.col.style.marginRight = (custTeamT.list.length && custTeamCT.list.length) ? '10px' : '0px';
        custPlayersEmpty.visible = list.length === 0;
      }
      // Life and selection change without the set changing: repaint, do not rebuild.
      var paint = custShownController() + '/';
      for (var k = 0; k < list.length; k++) paint += list[k].alive ? '1' : '0';
      if (paint === custPaintSig) return;
      custPaintSig = paint;
      var ti = 0, ci = 0;
      for (var m = 0; m < list.length; m++) {
        var r = list[m].team === 3 ? custTeamCT.list[ci++] : custTeamT.list[ti++];
        if (!r) continue;
        r.__p = list[m];
        paintPlayerRow(r);
      }
    }

    // ONE per-frame pass for the modal. The Camera Editor's render() calls it, and so does the
    // CONFIG-hosted modal (st.customizeHost 'config', editor workspace closed): the target, its
    // loadout and the roster all come from st.customize, the same answer CONFIG's button reads.
    // A target change rebuilds the lists on one frame and refreshes the 3D preview on the next, so
    // the game's own work for the new preview scene never lands on the frame of the list rebuild.
    var custPreviewDue = false;
    function customizeFrame() {
      if (custPreviewDue) {
        custPreviewDue = false;
        if (custOverlay.visible) { updatePreview(); pokePreviewSoon(); }
      }
      var cs = (st && st.customize) || null;
      var list = (cs && cs.players) || [];
      var prevTargetKey = custTargetKey;
      var p = null;
      for (var i = 0; cs && i < list.length; i++) if (list[i].controller === cs.target) { p = list[i]; break; }
      var ct = (p && p.loadout) ? p.loadout : null;
      custTargetController = p ? p.controller : -1;
      custTargetSource = (cs && cs.source) || 'none';
      custTargetAlive = p ? !!p.alive : true;
      custTargetLastSeen = p ? !!p.lastSeen : false;
      custTargetOnScreen = !!(p && cs && cs.spectated === p.controller);
      custTargetIndex = p ? p.pawn : -1;
      custTargetKey = p ? p.key : 'none';
      custTargetName = p ? p.name : 'Player';
      custTargetTeam = p ? p.team : '';
      custActiveWeaponDef = ct ? (parseInt(ct.activeWeaponDefIndex || 0, 10) || 0) : 0;
      custActiveWeaponSlot = WEAPON_SLOT_BY_DEF[custActiveWeaponDef] || '';
      custActiveWeaponPickup = !!(ct && ct.activeWeaponPickup);
      custActiveWeaponOwnerSteam = (ct && ct.activeWeaponOwnerSteamId) ? ('' + ct.activeWeaponOwnerSteamId) : '';
      custActiveWeaponOwnerName = (ct && ct.activeWeaponOwnerName) ? ('' + ct.activeWeaponOwnerName) : '';
      applyCustomizeTargetLoadout(ct);
      updatePickupBanner();
      var nextLoadoutSig = customizeLoadoutSignature();
      if (custOverlay.visible && (custTargetKey !== prevTargetKey || custDisplayedLoadoutSig !== nextLoadoutSig)) {
        // Target/loadout changed under the open modal (a pick, a player switch, or the demo's own
        // state moved on) -- discard any unstaged picks and reseed from the loadout, as on opening.
        previewLog('reseed', (custTargetKey !== prevTargetKey ? 'target' : 'loadout') + ' dirty=' + (custDirty ? 1 : 0)
          + ' was=' + custDisplayedLoadoutSig + ' now=' + nextLoadoutSig);
        custTouched = { agent: false, primary: false, secondary: false, knife: false, gloves: false };
        custDirty = false;
        var t0 = Date.now();
        resetSelectionsFromLoadout();
        populateCustomize();
        var t1 = Date.now();
        // The reseed picked the slot the new loadout holds; the always-open picker follows it.
        selectSlot(custActiveCategory);
        custPreviewDue = true;
        custDisplayedLoadoutSig = nextLoadoutSig;
        if (mvmDebugOn()) $.Msg('[mvmpreview] reseedCost populate=' + (t1 - t0) + 'ms select=' + (Date.now() - t1) + 'ms [' + custPopulateCost + '] target=' + custTargetKey);
      }
      if (custPickPending >= 0 && (custPickPending === custTargetController || --custPickPendingFrames <= 0)) custPickPending = -1;
      if (custOverlay.visible) {
        updateTargetChrome();
        if (custPlayerMenuOpen) { renderPlayers(list); custPositionPlayerMenu(); }
      } else if (custPlayerMenuOpen) {
        custSetPlayerMenu(false);
      }
      maintainPreview(); // self-heals the 3D preview for a few frames after it (re)opens
      custWindowFrame(); // the title-bar drag (CameraEditorCustomizeWindowJs.h)
    }
    // The header and body for the current target: the name, the dead tag, the empty state.
    var custChromeSig = '';
    function updateTargetChrome() {
      var sig = custTargetController + '|' + custTargetName + '|' + custTargetTeam + '|' + (custTargetAlive ? 1 : 0) + '|' + custSize;
      if (sig === custChromeSig) return;
      custChromeSig = sig;
      custTitleName.text = custTargetController >= 0 ? custTargetName : 'Choose a player';
      custDeadTag.visible = custTargetController >= 0 && !custTargetAlive;
      btnEnable(custResetBtn, custTargetController >= 0);
      paintPlayerBtn();
      custApplyBodyVisibility();
    }
)EDJS"
