#pragma once

// PER-PLAYER DEATH ANIMATIONS: the picker the Advanced Ragdolls card opens (RAGDOLLS tab). JS
// fragment concatenated into kConfigHudJs (ConfigHudJs.h), sharing the root IIFE's scope -- every
// name here is prefixed `dap` because a bare name would collide with one in another fragment.
//
// Layout (Docs/features/filmmaker_config_panel.md; original design 8c, "body map"): three columns over the Config panel.
//   left   the filters: a body map for the hit area (the player faces you, so their right arm is
//          on your left), chips for the non-body causes, and the source / stance / weapon strips;
//   middle the clips, ten 2-up cards per page;
//   right  a looping preview of the selected clip, the players to assign it to -- T always on the
//          left, CT always on the right, never interleaved -- and ASSIGN / USE AUTOMATIC PICK.
// "Assign" makes every death of the selected player play that clip (Movie/DeathAnimations.h,
// g_playerClips).
//
// LAYOUT IS FIXED. Nothing in the card changes size when a value changes: every list row and card
// has a fixed height, the filter strips take their widths from the column, the text lines have
// fixed heights and the assign block is pinned to the column's foot. CLEAR sits on the HIT AREA
// rule, which absorbs its width. A picker whose layout reflows on every click reads as broken, so
// keep it that way when adding controls.
//
// Data: the player list rides the state JSON (daPlayers). The 449-clip catalog is too big for the
// per-frame state, so the picker asks for it (`deathanim catalog`) and ConfigHud runs the answer,
// which calls $.ConfigHudDap.setCatalog below.
//
// The preview is the selected player's own agent (a MapPlayerPreviewPanel, the Customize Player
// modal's panel) playing the clip: `deathanim view <clip>` hands the clip to C++, which poses the
// panel's character from it every frame (Panorama/DeathAnimPreview.h).

R"CFJS(
    var DAP_ROWS = 10, DAP_TEAM_ROWS = 5, DAP_CARD_H = 62, DAP_GAP = 6;
    var DAP_T = '#e0b861ff', DAP_CT = '#7fb2e5ff';
    var DAP_AC = '#80bdff', DAP_RULE = '#ffffff10', DAP_CAP = '#6b7480ff', DAP_SUB = '#8b95a3ff';
    var DAP_LOOP_HOLD = 0.8;          // DeathAnimPreview.cpp kLoopHold: the last frame holds this long
    var dapCatalog = null;           // [[name, source, stance, area, side, weapon, frames], ...]
    var dapFiltered = [];
    var dapTop = 0;
    var dapPlayers = [];
    var dapPlayerKey = '';
    var dapClipName = '';
    var dapViewStart = 0;            // when the preview last (re)started the clip, for the loop line
    var dapOpen = false, dapTickOn = false;
    var dapFilter = { source: 'All', stance: 'All', weapon: 'All', area: 'All' };
    var dapPlayersSig = '';
    var DAP_AREAS = [['All', 'All areas'], ['head', 'Head'], ['neck', 'Neck'], ['torso', 'Torso'],
      ['leftarm', 'Left arm'], ['rightarm', 'Right arm'], ['leftleg', 'Left leg'], ['rightleg', 'Right leg'],
      ['explosion', 'Explosion'], ['fire', 'Fire'], ['melee', 'Melee'], ['generic', 'Generic'], ['manual', 'Manual']];
    function dapAreaName(k) {
      for (var i = 0; i < DAP_AREAS.length; i++) if (DAP_AREAS[i][0] === k) return DAP_AREAS[i][1];
      return k;
    }

    var dapOverlay = mk('Panel', root);
    dapOverlay.visible = false; dapOverlay.hittest = true;
    dapOverlay.style.width = '100%'; dapOverlay.style.height = '100%';
    dapOverlay.style.backgroundColor = '#000000c8'; dapOverlay.style.zIndex = '300';

    var dapCard = mk('Panel', dapOverlay); dapCard.hittest = true;
    dapCard.style.width = '1180px'; dapCard.style.height = '700px';
    dapCard.style.horizontalAlign = 'center'; dapCard.style.verticalAlign = 'center';
    dapCard.style.backgroundColor = S.bg; dapCard.style.border = '1px solid ' + S.tileBorder;
    dapCard.style.borderRadius = '8px'; dapCard.style.flowChildren = 'down';
    dapCard.style.boxShadow = S.cardShadow;
    dapCard.style.paddingTop = '18px'; dapCard.style.paddingBottom = '18px';
    dapCard.style.paddingLeft = '20px'; dapCard.style.paddingRight = '20px';

    // Header: the title on the left, CLOSE on the right. Nothing in here depends on picker state.
    var dapHead = mk('Panel', dapCard); dapHead.hittest = false;
    dapHead.style.width = '100%'; dapHead.style.height = '30px';
    dapHead.style.flowChildren = 'right'; dapHead.style.marginBottom = '14px';
    var dapTitle = lbl(dapHead, 'PER-PLAYER DEATH ANIMATIONS', S.value, 18);
    dapTitle.style.fontWeight = 'bold'; dapTitle.style.letterSpacing = '1px';
    dapTitle.style.verticalAlign = 'center'; dapTitle.hittest = false;
    var dapHeadSpace = mk('Panel', dapHead); dapHeadSpace.hittest = false;
    dapHeadSpace.style.width = 'fill-parent-flow(1.0)';
    var dapClose = btn(dapHead, 'CLOSE', function () { dapShow(false); }, S.label);
    dapClose.style.marginRight = '0px'; dapClose.style.height = '30px';
    dapClose.style.paddingTop = '0px'; dapClose.style.paddingBottom = '0px';
    dapClose.style.paddingLeft = '14px'; dapClose.style.paddingRight = '14px';
    dapClose.__lbl.style.verticalAlign = 'center';

    var dapBody = mk('Panel', dapCard); dapBody.hittest = false;
    dapBody.style.width = '100%'; dapBody.style.height = 'fill-parent-flow(1.0)'; dapBody.style.flowChildren = 'right';
    function dapColumn(width, gapRight) {
      var col = mk('Panel', dapBody); col.hittest = false;
      col.style.width = width; col.style.height = '100%'; col.style.flowChildren = 'down';
      col.style.marginRight = gapRight + 'px';
      return col;
    }
    var dapColFilters = dapColumn('250px', 18);
    var dapColClips = dapColumn('fill-parent-flow(1.0)', 18);
    var dapColPreview = dapColumn('310px', 0);

    // A caption on a rule: the column headings (HIT AREA, the clip count).
    function dapRuleRow(parent, text, color) {
      var r = mk('Panel', parent); r.hittest = false;
      r.style.width = '100%'; r.style.height = '20px'; r.style.flowChildren = 'right';
      var l = lbl(r, text, color || DAP_SUB, 11); l.hittest = false;
      l.style.fontWeight = 'bold'; l.style.letterSpacing = '1.6px'; l.style.verticalAlign = 'center';
      l.style.whiteSpace = 'nowrap';
      var rule = mk('Panel', r); rule.hittest = false;
      rule.style.width = 'fill-parent-flow(1.0)'; rule.style.height = '1px';
      rule.style.backgroundColor = DAP_RULE; rule.style.verticalAlign = 'center';
      rule.style.marginLeft = '8px'; rule.style.marginRight = '8px';
      r.__lbl = l;
      return r;
    }
    // A small letterspaced caption above a control.
    function dapCaption(parent, text, color, top) {
      var l = lbl(parent, text, color || DAP_CAP, 10); l.hittest = false;
      l.style.fontWeight = 'bold'; l.style.letterSpacing = '1.6px';
      l.style.marginTop = (top || 0) + 'px'; l.style.height = '14px';
      return l;
    }
    // A panel painted by its own function on hover / press / state, with a click handler. The
    // paint receives the panel; p.__hot / p.__down are kept by hoverable().
    function dapPressable(p, paint, onClick) {
      hoverable(p, paint);
      p.SetPanelEvent('onactivate', function () {
        if (p.__off) return;
        pressFlash(p, paint);
        if (onClick) onClick();
      });
      return p;
    }
)CFJS"
R"CFJS(
    // ---- filters: hit area ---------------------------------------------------------------------
    var dapAreaHead = dapRuleRow(dapColFilters, 'HIT AREA');
    // CLEAR replaces the old CLEAR FILTERS button. It sits on the rule, which absorbs its width.
    var dapClear = mk('Panel', dapAreaHead);
    var dapClearL = lbl(dapClear, 'CLEAR', DAP_AC, 11); dapClearL.hittest = false;
    dapClearL.style.fontWeight = 'bold'; dapClearL.style.letterSpacing = '1px';
    dapClear.style.verticalAlign = 'center';
    dapPressable(dapClear, function (p) { dapClearL.style.color = p.__hot ? S.value : DAP_AC; }, function () {
      dapFilter = { source: 'All', stance: 'All', weapon: 'All', area: 'All' };
      dapTop = 0; dapApplyFilter();
    });

    // The body map. Seven regions on a 4 x 4 grid (columns 30/34/34/30, rows 38/12/84/92, 4px
    // gaps), placed by position because Panorama has no grid layout. Regions never overlap.
    var DAP_COLX = [0, 34, 72, 110], DAP_ROWY = [0, 42, 58, 146];
    var DAP_BODY = [
      // key, x, y, w, h, radii (tl tr br bl)
      ['head', 51, DAP_ROWY[0], 38, 38, ['19px', '19px', '19px', '19px']],
      ['neck', 58, DAP_ROWY[1], 24, 12, ['3px', '3px', '3px', '3px']],
      ['torso', DAP_COLX[1], DAP_ROWY[2], 72, 84, ['12px', '12px', '6px', '6px']],
      ['rightarm', DAP_COLX[0], DAP_ROWY[2], 30, 84, ['10px', '4px', '8px', '8px']],
      ['leftarm', DAP_COLX[3], DAP_ROWY[2], 30, 84, ['4px', '10px', '8px', '8px']],
      ['rightleg', DAP_COLX[1], DAP_ROWY[3], 34, 92, ['4px', '4px', '8px', '8px']],
      ['leftleg', DAP_COLX[2], DAP_ROWY[3], 34, 92, ['4px', '4px', '8px', '8px']]];
    var dapMapBox = mk('Panel', dapColFilters); dapMapBox.hittest = false;
    dapMapBox.style.width = '100%'; dapMapBox.style.marginTop = '10px'; dapMapBox.style.flowChildren = 'down';
    dapMapBox.style.paddingTop = '14px'; dapMapBox.style.paddingBottom = '10px';
    dapMapBox.style.borderRadius = '4px'; dapMapBox.style.border = '1px solid ' + DAP_RULE;
    dapMapBox.style.backgroundColor = 'gradient( radial, 50% 40%, 0% 0%, 75% 75%, from( #18202bff ), to( #11151bff ) )';
    var dapMapRow = mk('Panel', dapMapBox); dapMapRow.hittest = false;
    dapMapRow.style.flowChildren = 'right'; dapMapRow.style.horizontalAlign = 'center';
    function dapSideTag(text) {
      var l = lbl(dapMapRow, text, DAP_CAP, 10); l.hittest = false;
      l.style.fontWeight = 'bold'; l.style.letterSpacing = '1px';
      l.style.width = '12px'; l.style.textAlign = 'center'; l.style.verticalAlign = 'center';
      return l;
    }
    dapSideTag('R');                 // the player faces you: their right is on your left
    var dapMap = mk('Panel', dapMapRow); dapMap.hittest = false;
    dapMap.style.width = '140px'; dapMap.style.height = '238px';
    dapMap.style.marginLeft = '10px'; dapMap.style.marginRight = '10px';
    dapSideTag('L');
    var dapRegions = [];
    for (var dbi = 0; dbi < DAP_BODY.length; dbi++) (function (spec) {
      var p = mk('Panel', dapMap);
      p.style.position = spec[1] + 'px ' + spec[2] + 'px 0px';
      p.style.width = spec[3] + 'px'; p.style.height = spec[4] + 'px';
      p.style.borderTopLeftRadius = spec[5][0]; p.style.borderTopRightRadius = spec[5][1];
      p.style.borderBottomRightRadius = spec[5][2]; p.style.borderBottomLeftRadius = spec[5][3];
      var n = lbl(p, '', DAP_SUB, 11); n.hittest = false;
      n.style.fontFamily = S.fontMonodigit; n.style.fontWeight = 'bold';
      n.style.horizontalAlign = 'center'; n.style.verticalAlign = 'center';
      p.__key = spec[0]; p.__count = n;
      dapPressable(p, dapPaintRegion, function () { dapSetFilter('area', dapFilter.area === spec[0] ? 'All' : spec[0]); });
      dapRegions.push(p);
    })(DAP_BODY[dbi]);
    function dapPaintRegion(p) {
      var on = dapFilter.area === p.__key;
      p.style.backgroundColor = on ? (DAP_AC + '55') : (p.__hot ? '#2a3544ff' : '#1f2732ff');
      p.style.border = '1px solid ' + ((on || p.__hot) ? DAP_AC : '#ffffff1f');
      p.__count.style.color = on ? S.value : DAP_SUB;
    }
    var dapAreaLine = mk('Panel', dapMapBox); dapAreaLine.hittest = false;
    dapAreaLine.style.flowChildren = 'right'; dapAreaLine.style.horizontalAlign = 'center';
    dapAreaLine.style.height = '18px'; dapAreaLine.style.marginTop = '8px';
    var dapAreaVal = lbl(dapAreaLine, '', S.label, 12); dapAreaVal.hittest = false;
    dapAreaVal.style.fontWeight = 'bold'; dapAreaVal.style.verticalAlign = 'center';
    var dapAreaHint = lbl(dapAreaLine, '· player facing you', DAP_CAP, 11); dapAreaHint.hittest = false;
    dapAreaHint.style.marginLeft = '6px'; dapAreaHint.style.verticalAlign = 'center';

    // The causes that are not a place on the body: a wrapping row of chips with counts. Same area
    // keys the 13-step stepper used.
    var dapCauses = mk('Panel', dapColFilters); dapCauses.hittest = false;
    dapCauses.style.width = '100%'; dapCauses.style.flowChildren = 'right-wrap'; dapCauses.style.marginTop = '10px';
    var dapCauseChips = [];
    for (var dci = 8; dci < DAP_AREAS.length; dci++) (function (key, caption) {
      var c = mk('Panel', dapCauses);
      c.style.height = '26px'; c.style.flowChildren = 'right';
      c.style.paddingLeft = '9px'; c.style.paddingRight = '9px';
      c.style.marginRight = '4px'; c.style.marginBottom = '4px'; c.style.borderRadius = '3px';
      var l = lbl(c, caption, S.label, 12); l.hittest = false;
      l.style.fontWeight = 'bold'; l.style.verticalAlign = 'center';
      var n = lbl(c, '', DAP_CAP, 10); n.hittest = false;
      n.style.fontFamily = S.fontMonodigit; n.style.marginLeft = '5px'; n.style.verticalAlign = 'center';
      c.__key = key; c.__lbl2 = l; c.__count = n;
      dapPressable(c, dapPaintCause, function () { dapSetFilter('area', dapFilter.area === key ? 'All' : key); });
      dapCauseChips.push(c);
    })(DAP_AREAS[dci][0], DAP_AREAS[dci][1]);
    function dapPaintCause(c) {
      var on = dapFilter.area === c.__key;
      c.style.backgroundColor = on ? (DAP_AC + '2e') : (c.__hot ? '#ffffff12' : '#ffffff08');
      c.style.border = '1px solid ' + (on ? (DAP_AC + '80') : (c.__hot ? '#ffffff40' : '#ffffff1a'));
      c.__lbl2.style.color = (on || c.__hot) ? S.value : S.label;
    }

    // Source / stance / weapon: stacked segmented strips with small captions. Only tags the
    // library actually carries -- no clip is prone, and none is sniper-only (sniper kills are served
    // by the `heavy` clips), so those two would be filters that always empty the list.
    var dapSegs = {};
    function dapSegFilter(caption, key, items) {
      dapCaption(dapColFilters, caption, DAP_CAP, 10);
      var seg = segmented(dapColFilters, items, '100%', function (v) { dapSetFilter(key, v); }, 11);
      // `segmented` centres its strip vertically; as the last child of this full-height column that
      // centred the WEAPON strip in the space left below it, far under its caption, in game.
      seg.panel.style.marginTop = '4px'; seg.panel.style.verticalAlign = 'top';
      for (var k in seg.btns) { seg.btns[k].style.paddingTop = '5px'; seg.btns[k].style.paddingBottom = '5px'; }
      dapSegs[key] = seg;
    }
    dapSegFilter('SOURCE', 'source', [['All', 'All'], ['MW2019', 'MW2019'], ['Classic CoD', 'Classic CoD'], ['Extra', 'Extra']]);
    dapSegFilter('STANCE', 'stance', [['All', 'All'], ['standing', 'Standing'], ['crouched', 'Crouched'], ['running', 'Running']]);
    dapSegFilter('WEAPON', 'weapon', [['All', 'All'], ['any', 'Universal'], ['shotgun', 'Shotgun'], ['heavy', 'Heavy']]);

    function dapSetFilter(key, value) {
      if (dapFilter[key] === value) return;
      dapFilter[key] = value;
      dapTop = 0; dapApplyFilter();
    }
    function dapFiltering() {
      return dapFilter.source !== 'All' || dapFilter.stance !== 'All' || dapFilter.weapon !== 'All' || dapFilter.area !== 'All';
    }
    // A clip passes every filter except `skip`: the counts on the map and the chips are "what this
    // choice would show under the other filters", so they ignore the area filter itself.
    function dapMatch(c, skip) {
      return (skip === 'source' || dapFilter.source === 'All' || c[1] === dapFilter.source)
        && (skip === 'stance' || dapFilter.stance === 'All' || c[2] === dapFilter.stance)
        && (skip === 'area' || dapFilter.area === 'All' || c[3] === dapFilter.area)
        && (skip === 'weapon' || dapFilter.weapon === 'All' || c[5] === dapFilter.weapon);
    }
    function dapAreaCounts() {
      var counts = {};
      if (dapCatalog) for (var i = 0; i < dapCatalog.length; i++) {
        var c = dapCatalog[i];
        if (dapMatch(c, 'area')) counts[c[3]] = (counts[c[3]] || 0) + 1;
      }
      return counts;
    }
    function dapPaintFilters() {
      for (var k in dapSegs) dapSegs[k].sync(dapFilter[k]);
      var counts = dapAreaCounts(), i;
      for (i = 0; i < dapRegions.length; i++) {
        var r = dapRegions[i], n = counts[r.__key] || 0;
        r.__count.text = dapCatalog ? '' + n : '';
        r.__tipText = dapAreaName(r.__key) + ' · ' + n + (n === 1 ? ' clip' : ' clips');
        dapPaintRegion(r);
      }
      for (i = 0; i < dapCauseChips.length; i++) {
        var c = dapCauseChips[i];
        c.__count.text = dapCatalog ? '' + (counts[c.__key] || 0) : '';
        dapPaintCause(c);
      }
      dapAreaVal.text = dapAreaName(dapFilter.area);
      dapAreaVal.style.color = dapFilter.area === 'All' ? S.label : DAP_AC;
      dapClear.visible = dapFiltering();
    }
)CFJS"
R"CFJS(
    // ---- clips ---------------------------------------------------------------------------------
    var dapClipHead = dapRuleRow(dapColClips, '');
    var dapCount = dapClipHead.__lbl;
    var dapPage = lbl(dapClipHead, '', DAP_CAP, 11); dapPage.hittest = false;
    dapPage.style.verticalAlign = 'center'; dapPage.style.marginRight = '8px';
    function dapPageBtn(glyph, delta) {
      var b = btn(dapClipHead, glyph, function () { dapScroll(delta); }, S.label);
      b.style.width = '26px'; b.style.height = '22px'; b.style.marginRight = '0px'; b.style.marginLeft = '4px';
      b.style.paddingLeft = '0px'; b.style.paddingRight = '0px'; b.style.paddingTop = '0px'; b.style.paddingBottom = '0px';
      b.__lbl.style.horizontalAlign = 'center'; b.__lbl.style.verticalAlign = 'center'; b.__lbl.style.fontSize = '14px';
      return b;
    }
    var dapPrev = dapPageBtn('‹', -DAP_ROWS);
    var dapNext = dapPageBtn('›', DAP_ROWS);

    // Ten cards, two up. The middle column is a fixed width (the card is a fixed 1180), so the card
    // width is too: inside the 1px border and the padding the body is 1138, which leaves the column
    // 542 -- (542 - 6) / 2 = 268. Rounding up to 269 wraps every card onto its own row.
    var DAP_CARD_W = 267;
    var dapGrid = mk('Panel', dapColClips); dapGrid.hittest = false;
    dapGrid.style.width = '100%'; dapGrid.style.flowChildren = 'right-wrap'; dapGrid.style.marginTop = '6px';
    dapGrid.style.height = (DAP_ROWS / 2 * (DAP_CARD_H + DAP_GAP)) + 'px';
    var dapCards = [];
    for (var dki = 0; dki < DAP_ROWS; dki++) (function (i) {
      var c = mk('Panel', dapGrid);
      c.style.width = DAP_CARD_W + 'px'; c.style.height = DAP_CARD_H + 'px';
      c.style.marginRight = (i % 2 === 0) ? (DAP_GAP + 'px') : '0px'; c.style.marginBottom = DAP_GAP + 'px';
      c.style.borderRadius = '3px'; c.style.paddingLeft = '10px'; c.style.paddingRight = '10px';
      c.style.overflow = 'clip';
      var inner = mk('Panel', c); inner.hittest = false;
      inner.style.width = '100%'; inner.style.flowChildren = 'down'; inner.style.verticalAlign = 'center';
      var top = mk('Panel', inner); top.hittest = false;
      top.style.width = '100%'; top.style.flowChildren = 'right';
      var n = lbl(top, '', S.label, 13); n.hittest = false;
      n.style.width = 'fill-parent-flow(1.0)'; n.style.fontWeight = 'bold';
      n.style.whiteSpace = 'nowrap'; n.style.textOverflow = 'ellipsis';
      var dur = lbl(top, '', DAP_SUB, 11); dur.hittest = false;
      dur.style.fontFamily = S.fontMonodigit; dur.style.marginLeft = '8px'; dur.style.verticalAlign = 'center';
      var tags = mk('Panel', inner); tags.hittest = false;
      tags.style.width = '100%'; tags.style.flowChildren = 'right'; tags.style.marginTop = '5px';
      tags.style.overflow = 'clip';
      var tagL = [];
      for (var t = 0; t < 4; t++) {
        var tg = mk('Panel', tags); tg.hittest = false;
        tg.style.paddingLeft = '6px'; tg.style.paddingRight = '6px'; tg.style.paddingTop = '2px'; tg.style.paddingBottom = '2px';
        tg.style.borderRadius = '2px'; tg.style.backgroundColor = '#ffffff0d'; tg.style.marginRight = '4px';
        var tl = lbl(tg, '', S.muted, 10); tl.hittest = false; tl.style.fontWeight = 'bold'; tl.style.whiteSpace = 'nowrap';
        tagL.push({ box: tg, l: tl });
      }
      c.__name = n; c.__dur = dur; c.__tags = tagL;
      dapPressable(c, dapPaintCard, function () {
        var clip = dapFiltered[dapTop + i];
        if (clip) dapSelectClip(clip[0]);
      });
      dapCards.push(c);
    })(dki);
    function dapPaintCard(c) {
      var on = !!c.__clip && c.__clip === dapClipName;
      c.style.backgroundColor = on ? (DAP_AC + '24') : (c.__hot ? '#1b2027ff' : '#151a21ff');
      c.style.border = '1px solid ' + (on ? (DAP_AC + '80') : S.clear);
      c.__name.style.color = on ? S.value : S.label;
    }
    var dapListEmpty = lbl(dapColClips, '', DAP_SUB, 13); dapListEmpty.hittest = false;
    dapListEmpty.style.marginTop = '4px'; dapListEmpty.style.whiteSpace = 'normal';
    dapListEmpty.style.paddingLeft = '2px';

    function dapApplyFilter() {
      dapFiltered = [];
      if (dapCatalog) for (var i = 0; i < dapCatalog.length; i++) if (dapMatch(dapCatalog[i])) dapFiltered.push(dapCatalog[i]);
      dapPaintClips();
    }
    function dapScroll(delta) {
      var n = dapFiltered.length;
      var maxTop = n > 0 ? Math.floor((n - 1) / DAP_ROWS) * DAP_ROWS : 0;
      dapTop = Math.max(0, Math.min(maxTop, dapTop + delta));
      dapPaintClips();
    }
    function dapSeconds(frames) { return ((frames - 1) / 30).toFixed(2); }
    function dapPaintClips() {
      dapPaintFilters();
      for (var i = 0; i < DAP_ROWS; i++) {
        var card = dapCards[i], c = dapFiltered[dapTop + i];
        card.visible = !!c;
        card.__clip = c ? c[0] : '';
        if (!c) continue;
        card.__name.text = c[0];
        card.__dur.text = dapSeconds(c[6]) + 's';
        var tags = [c[2], dapAreaName(c[3]), 'from ' + c[4]];
        if (c[5] !== 'any') tags.push(c[5]);
        for (var t = 0; t < card.__tags.length; t++) {
          card.__tags[t].box.visible = t < tags.length;
          card.__tags[t].l.text = tags[t] || '';
        }
        dapPaintCard(card);
      }
      var n = dapFiltered.length;
      dapListEmpty.visible = !n;
      dapListEmpty.text = !dapCatalog ? 'Loading clips…' : 'No clips match these filters. Clear filters to see all ' + dapCatalog.length + '.';
      dapCount.text = !dapCatalog ? 'CLIPS' : n ? (dapTop + 1) + '–' + Math.min(n, dapTop + DAP_ROWS) + ' OF ' + n + (n === 1 ? ' CLIP' : ' CLIPS') : '0 CLIPS';
      dapPage.text = n ? 'Page ' + (Math.floor(dapTop / DAP_ROWS) + 1) + ' / ' + Math.ceil(n / DAP_ROWS) : '';
      setBtnDisabled(dapPrev, dapTop <= 0);
      setBtnDisabled(dapNext, dapTop + DAP_ROWS >= n);
    }
)CFJS"
R"CFJS(
    // ---- preview -------------------------------------------------------------------------------
    // The box is only a frame, a hint and the loop line; the character is a MapPlayerPreviewPanel
    // laid over it on the overlay (a full-screen root-level panel). Nested inside the flowing card
    // its composition target came up blank -- the same trap CameraEditorCustomizePreviewJs.h
    // documents.
    var dapView = mk('Panel', dapColPreview); dapView.hittest = false;
    dapView.style.width = '310px'; dapView.style.height = '260px';
    dapView.style.backgroundColor = 'gradient( radial, 50% 80%, 0% 0%, 70% 70%, from( #1c2534ff ), to( #11151bff ) )';
    dapView.style.border = '1px solid ' + S.tileBorder;
    dapView.style.borderRadius = '4px'; dapView.style.overflow = 'clip';
    var dapViewMsg = lbl(dapView, 'Select a clip to preview it', S.hint, 13); dapViewMsg.hittest = false;
    dapViewMsg.style.horizontalAlign = 'center'; dapViewMsg.style.verticalAlign = 'center';
    // Loop progress along the bottom edge: how far through clip + hold the preview is. The C++
    // restarts the clip on every `deathanim view`, so dapViewStart is reset whenever one is sent.
    var dapLoop = mk('Panel', dapView); dapLoop.hittest = false;
    dapLoop.style.width = '100%'; dapLoop.style.height = '3px'; dapLoop.style.verticalAlign = 'bottom';
    dapLoop.style.backgroundColor = '#ffffff14'; dapLoop.style.zIndex = '5';
    var dapLoopFill = mk('Panel', dapLoop); dapLoopFill.hittest = false;
    dapLoopFill.style.height = '100%'; dapLoopFill.style.width = '0%'; dapLoopFill.style.backgroundColor = DAP_AC;

    var dap3d = null, dap3dClip = null, dap3dModel = '', dap3dSerial = 0, dap3dRect = '', dap3dReadyTicks = 0;
    // SetReadyForDisplay is what lets the panel draw at all (the paused-demo render bypass arms on
    // it), and a call made before the panel has laid out is lost -- so it is repeated for the first
    // ticks after the panel is built, as the Customize modal does (pokePreviewSoon).
    function dapReady3d() {
      if (!dap3d) return;
      try { dap3d.SetPlayerModel(dap3dModel); } catch (e) {}
      try { dap3d.SetReadyForDisplay(true); } catch (e2) {}
    }
    function dapModelFor(p) {
      if (p && p.m) return p.m;
      return (p && p.t === 2) ? 'agents/models/tm_phoenix/tm_phoenix.vmdl' : 'agents/models/ctm_sas/ctm_sas.vmdl';
    }
    function dapDestroy3d() {
      try { if (dap3dClip && dap3dClip.IsValid && dap3dClip.IsValid()) dap3dClip.DeleteAsync(0); } catch (e) {}
      dap3dClip = null; dap3d = null; dap3dModel = ''; dap3dRect = '';
    }
    function dapView3d(name) {
      dapViewStart = Date.now();
      cmd('mirv_filmmaker deathanim view ' + name);
    }
    // One panel per model: switching to a player with another agent builds a new one, and the clip
    // is re-sent so C++ captures the new character's idle pose before posing it.
    function dapEnsure3d() {
      var model = dapModelFor(dapPlayer());
      if (dap3d && dap3d.IsValid && dap3d.IsValid() && dap3dModel === model) return;
      dapDestroy3d();
      dap3dClip = mk('Panel', dapOverlay); dap3dClip.hittest = false;
      dap3dClip.style.overflow = 'clip'; dap3dClip.style.borderRadius = '4px';
      var tex = 'DapPreviewTex' + (++dap3dSerial);
      try {
        dap3d = $.CreatePanel('MapPlayerPreviewPanel', dap3dClip, tex, {
          'require-composition-layer': 'true', 'composition-layer-texture-name': tex,
          'pin-fov': 'vertical', 'transparent-background': 'true', 'class': 'mvp_map',
          map: 'ui/buy_menu', camera: 'cam_loadoutmenu_ct', animgraphcharactermode: 'buy-menu',
          playername: 'vanity_character', player: 'true', mouse_rotate: 'true', sync_spawn_addons: 'true',
          csm_split_plane0_distance_override: '250.0', hide_while_waiting_for_composite_materials: 'false',
          hittest: 'true', panzoom_enabled: 'true', playermodel: model
        });
        if (!(dap3d && dap3d.IsValid && dap3d.IsValid())) dap3d = null;
      } catch (e) { dap3d = null; }
      dap3dModel = model;
      if (dap3d) { dap3d.style.width = '100%'; dap3d.style.height = '100%'; dapReady3d(); dap3dReadyTicks = 20; }
      dapPlace3d();
      if (dapClipName) dapView3d(dapClipName);
    }
    // The 3D clip stops 3px short of the box's bottom so the loop line stays visible under it.
    function dapPlace3d() {
      if (!dap3dClip || !dapView.GetPositionWithinWindow) return;
      var sx = root.actualuiscale_x || 1, sy = root.actualuiscale_y || 1;
      var a = dapView.GetPositionWithinWindow();
      if (!a) return;
      var r = Math.round(a.x / sx) + ',' + Math.round(a.y / sy) + ','
        + Math.round((dapView.actuallayoutwidth || 0) / sx) + ',' + Math.max(0, Math.round((dapView.actuallayoutheight || 0) / sy) - 4);
      if (r === dap3dRect) return;
      dap3dRect = r;
      var v = r.split(',');
      dap3dClip.style.position = v[0] + 'px ' + v[1] + 'px 0px';
      dap3dClip.style.width = v[2] + 'px'; dap3dClip.style.height = v[3] + 'px';
    }

    // Fixed-height text lines, so a long clip name or a two-line meta line never moves the assign
    // block.
    var dapClipTitle = lbl(dapColPreview, 'Select a clip', S.value, 14); dapClipTitle.hittest = false;
    dapClipTitle.style.width = '100%'; dapClipTitle.style.height = '19px'; dapClipTitle.style.marginTop = '8px';
    dapClipTitle.style.fontWeight = 'bold'; dapClipTitle.style.whiteSpace = 'nowrap'; dapClipTitle.style.textOverflow = 'ellipsis';
    var dapClipMeta = lbl(dapColPreview, '', S.muted, 11); dapClipMeta.hittest = false;
    dapClipMeta.style.width = '100%'; dapClipMeta.style.height = '28px'; dapClipMeta.style.marginTop = '4px';
    dapClipMeta.style.whiteSpace = 'normal';
)CFJS"
R"CFJS(
    // ---- assign: T on the left, CT on the right -------------------------------------------------
    dapCaption(dapColPreview, 'ASSIGN TO', DAP_CAP, 6);
    var dapTeams = mk('Panel', dapColPreview); dapTeams.hittest = false;
    dapTeams.style.width = '100%'; dapTeams.style.flowChildren = 'right'; dapTeams.style.marginTop = '6px';
    function dapTeamColumn(caption, color, gapRight) {
      var col = mk('Panel', dapTeams); col.hittest = false;
      col.style.width = '152px'; col.style.flowChildren = 'down'; col.style.marginRight = gapRight + 'px';
      var cap = lbl(col, caption, color, 10); cap.hittest = false;
      cap.style.fontWeight = 'bold'; cap.style.letterSpacing = '1.4px';
      cap.style.paddingLeft = '2px'; cap.style.height = '15px';
      var rows = [];
      for (var i = 0; i < DAP_TEAM_ROWS; i++) {
        var r = mk('Panel', col);
        r.style.width = '100%'; r.style.height = '26px'; r.style.marginTop = '3px';
        r.style.flowChildren = 'right'; r.style.borderRadius = '3px'; r.style.overflow = 'clip';
        var edge = mk('Panel', r); edge.hittest = false;
        edge.style.width = '3px'; edge.style.height = '100%';
        var n = lbl(r, '', S.label, 12); n.hittest = false;
        n.style.width = 'fill-parent-flow(1.0)'; n.style.marginLeft = '6px'; n.style.fontWeight = 'bold';
        n.style.verticalAlign = 'center'; n.style.whiteSpace = 'nowrap'; n.style.textOverflow = 'ellipsis';
        var dot = mk('Panel', r); dot.hittest = false;
        dot.style.width = '6px'; dot.style.height = '6px'; dot.style.borderRadius = '1px';
        dot.style.backgroundColor = DAP_AC; dot.style.verticalAlign = 'center';
        dot.style.marginLeft = '6px'; dot.style.marginRight = '7px';
        r.__edge = edge; r.__name = n; r.__dot = dot; r.__teamColor = color;
        (function (row) {
          dapPressable(row, dapPaintPlayerRow, function () {
            var p = row.__player;
            if (!p) return;
            dapPlayerKey = p.k;
            dapPaintPlayers();
            if (p.c) dapSelectClip(p.c);
          });
        })(r);
        rows.push(r);
      }
      return { col: col, rows: rows };
    }
    var dapTeamT = dapTeamColumn('T', DAP_T, 6);
    var dapTeamCT = dapTeamColumn('CT', DAP_CT, 0);
    function dapPaintPlayerRow(r) {
      var p = r.__player, on = !!p && p.k === dapPlayerKey;
      r.style.backgroundColor = on ? '#1a2029ff' : (r.__hot ? '#1b2027ff' : S.clear);
      r.style.border = '1px solid ' + (on ? (DAP_AC + '80') : S.clear);
      r.__edge.style.backgroundColor = on ? DAP_AC : (r.__teamColor.substring(0, 7) + '66');
      r.__name.style.color = on ? S.value : S.label;
    }
    var dapPlayersEmpty = lbl(dapColPreview, 'Players appear here once the demo is playing.', S.hint, 12);
    dapPlayersEmpty.hittest = false; dapPlayersEmpty.style.marginTop = '4px'; dapPlayersEmpty.style.whiteSpace = 'normal';

    function dapPlayer() {
      for (var i = 0; i < dapPlayers.length; i++) if (dapPlayers[i].k === dapPlayerKey) return dapPlayers[i];
      return null;
    }
    function dapPaintTeam(team, list) {
      for (var i = 0; i < team.rows.length; i++) {
        var r = team.rows[i], p = list[i];
        r.visible = !!p;
        r.__player = p || null;
        if (!p) continue;
        r.__name.text = p.n;
        r.__dot.visible = !!p.c;
        dapPaintPlayerRow(r);
      }
    }
    function dapPaintPlayers() {
      if (!dapPlayer() && dapPlayers.length) dapPlayerKey = dapPlayers[0].k;
      var ts = [], cts = [];
      for (var i = 0; i < dapPlayers.length; i++) (dapPlayers[i].t === 2 ? ts : cts).push(dapPlayers[i]);
      dapPaintTeam(dapTeamT, ts);
      dapPaintTeam(dapTeamCT, cts);
      dapTeams.visible = !!dapPlayers.length;
      dapPlayersEmpty.visible = !dapPlayers.length;
      dapPaintAssign();
    }

    // The note, ASSIGN TO <NAME> and USE AUTOMATIC PICK, pinned to the foot of the column.
    var dapSpacer = mk('Panel', dapColPreview); dapSpacer.hittest = false;
    dapSpacer.style.width = '100%'; dapSpacer.style.height = 'fill-parent-flow(1.0)';
    var dapNote = lbl(dapColPreview, '', S.muted, 12); dapNote.hittest = false;
    dapNote.style.width = '100%'; dapNote.style.height = '34px'; dapNote.style.whiteSpace = 'normal';
    dapNote.style.marginBottom = '8px';
    // ASSIGN is the picker's one solid action: blue with dark text, grey when there is nothing to do.
    var dapAssign = mk('Panel', dapColPreview);
    dapAssign.style.width = '100%'; dapAssign.style.height = '40px'; dapAssign.style.borderRadius = '3px';
    var dapAssignL = lbl(dapAssign, 'ASSIGN', S.value, 12); dapAssignL.hittest = false;
    dapAssignL.style.fontWeight = 'bold'; dapAssignL.style.letterSpacing = '1px';
    dapAssignL.style.horizontalAlign = 'center'; dapAssignL.style.verticalAlign = 'center';
    dapAssignL.style.whiteSpace = 'nowrap'; dapAssignL.style.textOverflow = 'ellipsis'; dapAssignL.style.maxWidth = '96%';
    function dapPaintAssignBtn(b) {
      var off = !!b.__off;
      b.style.backgroundColor = off ? '#ffffff14' : (b.__down ? '#6aa6e6ff' : (b.__hot ? '#9ccbffff' : DAP_AC));
      dapAssignL.style.color = off ? DAP_CAP : '#0c0e12ff';
    }
    dapPressable(dapAssign, dapPaintAssignBtn, function () {
      var p = dapPlayer();
      if (!p || !dapClipName) return;
      cmd('mirv_filmmaker deathanim player ' + p.k + ' ' + dapClipName);
    });
    var dapAuto = wideBtn(dapColPreview, 'USE AUTOMATIC PICK', function () {
      var p = dapPlayer();
      if (p) cmd('mirv_filmmaker deathanim player ' + p.k + ' auto');
    }, S.label);
    dapAuto.style.marginTop = '6px'; dapAuto.style.height = '30px';
    dapAuto.style.paddingTop = '0px'; dapAuto.style.paddingBottom = '0px';
    dapAuto.__lbl.style.fontSize = '11px'; dapAuto.__lbl.style.letterSpacing = '1px';
    dapAuto.__lbl.style.verticalAlign = 'center';

    function dapPaintAssign() {
      var p = dapPlayer();
      dapAssignL.text = p && dapClipName ? 'ASSIGN TO ' + p.n.toUpperCase() : 'ASSIGN';
      var off = !(p && dapClipName) || !!(p && p.c === dapClipName);
      dapAssign.__off = off; dapAssign.hittest = !off;
      if (off) { dapAssign.__hot = false; dapAssign.__down = false; }
      dapPaintAssignBtn(dapAssign);
      setBtnDisabled(dapAuto, !(p && p.c));
      dapNote.text = !p ? 'Choose a player.'
        : p.c === dapClipName && p.c ? p.n + ' plays this clip on every death.'
        : p.c ? p.n + ' plays ' + p.c + '. Stance and weapon rules do not apply.'
        : !dapClipName ? 'Choose a clip for ' + p.n + '.'
        : p.n + ' uses the automatic pick.';
    }
)CFJS"
R"CFJS(
    function dapClipRow(name) {
      if (dapCatalog) for (var i = 0; i < dapCatalog.length; i++) if (dapCatalog[i][0] === name) return dapCatalog[i];
      return null;
    }
    function dapSelectClip(name) {
      name = ('' + name).replace(/^\s+|\s+$/g, '');   // console-driven calls arrive padded
      dapClipName = name;
      dapClipTitle.text = name;
      var c = dapClipRow(name), meta = '';
      if (c) meta = c[1] + ' · ' + c[2] + ' · ' + dapAreaName(c[3]) + ' · hit from ' + c[4]
        + (c[5] !== 'any' ? ' · ' + c[5] + ' kills' : '') + ' · ' + dapSeconds(c[6]) + ' s';
      dapClipMeta.text = meta;
      dapViewMsg.visible = false;
      var had = !!dap3d;
      dapEnsure3d();
      if (had) dapView3d(name);   // a new panel already sent it
      dapPaintClips(); dapPaintAssign();
    }

    // Keeps the character over the box (the card can move when the window resizes), follows the
    // selected player's agent and advances the loop line.
    function dapTick() {
      if (!dapOpen) { dapTickOn = false; return; }
      if (dapClipName) { dapEnsure3d(); dapPlace3d(); }
      if (dap3dReadyTicks > 0) { dap3dReadyTicks--; if (dap3dReadyTicks % 4 === 0) dapReady3d(); }
      var c = dapClipName ? dapClipRow(dapClipName) : null;
      var frac = 0;
      if (c) {
        var cycle = (c[6] - 1) / 30 + DAP_LOOP_HOLD, t = (Date.now() - dapViewStart) / 1000;
        if (cycle > 0) frac = (t % cycle) / cycle;
      }
      dapLoopFill.style.width = (frac * 100).toFixed(1) + '%';
      $.Schedule(1 / 30, dapTick);
    }

    // THE PICKER OWNS THE MOUSE. A click on the card's empty space or the dimmed backdrop went on
    // to the game, where the first-person spectator's attack bind switched the demo player. The
    // full-screen overlay's hit-test does not stop that in this context, so the picker takes the
    // mouse from the game the way CS2's own modal menus do (scoreboard.js, CaptureMouse) for as
    // long as it is open. Panorama still gets every click.
    var dapDenyInput = null;
    function dapCaptureMouse(want) {
      try {
        if (want && !dapDenyInput) dapDenyInput = UiToolkitAPI.AddDenyInputFlagsToGame(dapOverlay, 'MvmDeathAnimPicker', 'CaptureMouse');
        else if (!want && dapDenyInput) { UiToolkitAPI.ReleaseDenyInputFlagsToGame(dapDenyInput); dapDenyInput = null; }
      } catch (e) { dapDenyInput = null; }
    }

    function dapShow(on) {
      dapOpen = !!on;
      dapOverlay.visible = dapOpen;
      dapCaptureMouse(dapOpen);
      if (!dapOpen) {
        dapDestroy3d();
        cmd('mirv_filmmaker deathanim view off');
        return;
      }
      if (!dapCatalog) cmd('mirv_filmmaker deathanim catalog');
      dapPaintPlayers(); dapPaintClips();
      if (dapClipName) { dapViewMsg.visible = false; dapEnsure3d(); }
      if (!dapTickOn) { dapTickOn = true; $.Schedule(1 / 30, dapTick); }
    }

    $.ConfigHudDap = {
      setCatalog: function (rows) { dapCatalog = rows; dapTop = 0; dapApplyFilter(); },
      isOpen: function () { return dapOpen; },
      open: function () { dapShow(true); },
      close: function () { dapShow(false); },
      select: function (name) { dapSelectClip(name); },
      // Automation: select the i-th listed player, as a click on its row would.
      player: function (i) {
        var p = dapPlayers[parseInt(i, 10) || 0];
        if (!p) return 'none';
        dapPlayerKey = p.k; dapPaintPlayers();
        return p.n;
      },
      // Automation: the body map / cause chips, as a click on that region would.
      area: function (key) {
        key = ('' + key).replace(/^\s+|\s+$/g, '');
        dapSetFilter('area', dapFilter.area === key ? 'All' : key);
        return dapFilter.area;
      },
      // One wheel notch moves one row of two cards.
      wheel: function (notches) { dapScroll(-notches * 2); },
      // Automation / netcon: the 3D preview's panel state (the eval path cannot carry commas).
      dbg: function () {
        var q = dap3d, c = dap3dClip, out = 'none';
        try {
          if (q) out = [q.paneltype, q.IsValid ? q.IsValid() : '?', q.visible, q.actuallayoutwidth, q.actuallayoutheight,
            c ? c.actuallayoutwidth : -1, c ? c.actuallayoutheight : -1, dap3dRect, dap3dModel, dap3dReadyTicks,
            root.actualuiscale_x].join('|');
        } catch (e) { out = 'err ' + e; }
        $.Msg('DAPDBG ' + out);
        return out;
      }
    };

    renderHooks.push(function (st) {
      var players = st.daPlayers || [];
      var sig = JSON.stringify(players);
      if (sig !== dapPlayersSig) { dapPlayersSig = sig; dapPlayers = players; if (dapOpen) dapPaintPlayers(); }
    });
)CFJS"
