#pragma once

// GLOBAL SEARCH for the CONFIG panel (Docs/features/filmmaker_config_panel.md). JS fragment
// spliced into kConfigHudJs after every tab fragment, so the index (cfgSearchIndex, filled by
// sliderRow / togglePill / modeRow as they build) is complete when this runs.
//
// The box sits on the tab rule (built in ConfigHudJs.h). Typing goes through ConfigHud's WM_CHAR
// bridge, the same path the recoil preset and scene name fields use, by decorating
// configWindowBlur / configWindowChars. At 2+ characters the rail and the pane are replaced by up
// to 24 results: a crumb (TAB > CARD > PAGE) that jumps to the control, and a LIVE copy of the
// control itself -- built by the same builder with the same command, so editing a result is
// editing the setting.

R"CFJS(
    var cfgQuery = '', cfgSearchFocus = false;
    var CFG_TAB_NAMES = { camera: 'CAMERA', effects: 'EFFECTS', environment: 'ENVIRONMENT', ragdolls: 'RAGDOLLS' };
    function cfgSearchPaint() {
      var hot = searchBox.__hot;
      var active = cfgSearchFocus || cfgSearchActive;
      searchBox.style.border = '1px solid ' + (active ? '#80bdff80' : (hot ? '#ffffff40' : '#ffffff1f'));
      var t = cfgQuery ? (cfgQuery + (cfgSearchFocus ? '|' : '')) : (cfgSearchFocus ? '|' : 'Search all tabs');
      if (searchText.text !== t) searchText.text = t;
      searchText.style.color = (cfgQuery || cfgSearchFocus) ? S.value : S.muted;
      searchClear.visible = cfgQuery.length > 0;
    }
    hoverable(searchBox, cfgSearchPaint);
    function cfgSearchFocusOn() {
      configWindowBlur();
      cfgSearchFocus = true; cfgTextOwner = searchBox;
      root.SetAttributeString('textcapture', '1');
      cfgSearchPaint();
    }
    searchBox.SetPanelEvent('onactivate', cfgSearchFocusOn);
    searchClear.SetPanelEvent('onactivate', function () { cfgSetQuery(''); configWindowBlur(); });
    hoverable(searchClear, function () { searchClear.style.color = searchClear.__hot ? S.value : S.muted; });
    attachTip(searchClear, 'Clear the search (Esc)');

    var cfgSearchPrevBlur = configWindowBlur, cfgSearchPrevChars = configWindowChars;
    configWindowBlur = function () { cfgSearchFocus = false; cfgSearchPaint(); cfgSearchPrevBlur(); };
    configWindowChars = function (csv) {
      if (!cfgSearchFocus) { cfgSearchPrevChars(csv); return; }
      var q = cfgQuery, parts = String(csv || '').split(',');
      for (var i = 0; i < parts.length; i++) {
        var n = parseInt(parts[i], 10);
        if (n === 13) { configWindowBlur(); }
        else if (n === 27) { q = ''; configWindowBlur(); }
        else if (n === 8) q = q.slice(0, -1);
        else if (n >= 32 && n < 127 && q.length < 32) q += String.fromCharCode(n);
      }
      if (q !== cfgQuery) cfgSetQuery(q); else cfgSearchPaint();
    };

    // Where a control lives: the card (and page) it was built in, found by walking up its panels.
    function cfgCrumbFor(rowPanel) {
      var page = null, pivot = null;
      try {
        for (var p = rowPanel, g = 0; p && g < 48; g++, p = p.GetParent()) {
          if (!page && p.__cfgPage) page = p.__cfgPage;
          if (!pivot && p.__pivotOwner) pivot = p.__pivotOwner;
          if (p.__cfgCard) return { card: p.__cfgCard, page: page, pivot: pivot };
        }
      } catch (e) {}
      return null;
    }
    function cfgJump(card, page, pivot) {
      cfgSetQuery('');
      configWindowBlur();
      if (pages[card.tab]) {
        activeTab = card.tab; applyTabs();
        cmd('mirv_filmmaker settings ui config.tab ' + card.tab);
      }
      cfgSelectCard(card.tab, card.key, true);
      if (page && card.body.__setPage) {
        card.body.__setPage(page.idx);
        cmd('mirv_filmmaker settings ui ' + card.key + '.page ' + page.idx);
      }
      if (pivot && cfgPivotDrawers[pivot]) cfgPivotDrawers[pivot].setOpen(true, true);
    }
)CFJS"
R"CFJS(
    function cfgSearchCrumb(host, text, accent, onJump) {
      var c = mk('Panel', host); c.hittest = true;
      c.style.flowChildren = 'right'; c.style.width = '100%'; c.style.marginBottom = '4px';
      var dot = mk('Panel', c); dot.hittest = false;
      dot.style.width = '6px'; dot.style.height = '6px'; dot.style.borderRadius = '1px';
      dot.style.backgroundColor = accent; dot.style.verticalAlign = 'center'; dot.style.marginRight = '6px';
      var t = lbl(c, text + '  →', S.group, 11); t.hittest = false;
      t.style.fontWeight = 'bold'; t.style.letterSpacing = '1.4px'; t.style.whiteSpace = 'nowrap';
      t.style.textOverflow = 'ellipsis'; t.style.width = 'fill-parent-flow(1.0)';
      hoverable(c, function () { t.style.color = c.__hot ? S.accent : S.group; });
      c.SetPanelEvent('onactivate', onJump);
      return c;
    }
    function cfgRenderResults() {
      // Proxies are about to go: mark them so the drag pump forgets their records.
      for (var i = 0; i < cfgSearchIndex.length; i++) {
        var px = cfgSearchIndex[i].proxies;
        for (var j = 0; j < px.length; j++) if (px[j] && px[j].row) px[j].row.__dead = true;
        cfgSearchIndex[i].proxies = [];
      }
      pageSearch.RemoveAndDeleteChildren();
      cfgPruneDrags();
      if (!cfgSearchActive) return;
      var q = cfgQuery.replace(/^\s+|\s+$/g, '').toLowerCase();
      var results = [];
      for (var tk in cfgRails) {
        var items = cfgRails[tk].items;
        for (var key in items) {
          var cc = items[key].card.__cfgCard;
          if (cc.title.toLowerCase().indexOf(q) >= 0) results.push({ card: cc });
        }
      }
      for (var k = 0; k < cfgSearchIndex.length; k++) {
        var e = cfgSearchIndex[k];
        if (e.words.indexOf(q) < 0) continue;
        var crumb = cfgCrumbFor(e.row);
        if (crumb) results.push({ e: e, crumb: crumb });
      }
      // Count EVERY match, then show the first 24 -- and say so, rather than reporting the cap
      // as if it were the total.
      var total = results.length;
      if (results.length > 24) results.length = 24;
      var countText = (total > results.length)
        ? ('Showing ' + results.length + ' of ' + total + ' matches across all tabs')
        : (total + (total === 1 ? ' match' : ' matches') + ' across all tabs');
      var count = lbl(pageSearch, countText, S.muted, 11);
      count.hittest = false; count.style.fontWeight = 'bold'; count.style.letterSpacing = '1.5px';
      count.style.textTransform = 'uppercase';
      if (!results.length) {
        var none = lbl(pageSearch, 'Nothing matches \u201c' + cfgQuery.replace(/^\s+|\s+$/g, '')
          + '\u201d. Try a shorter word or clear the search.', S.muted, 13);
        none.hittest = false; none.style.whiteSpace = 'normal'; none.style.width = '100%';
        none.style.textAlign = 'center'; none.style.marginTop = '24px';
        return;
      }
      for (var r = 0; r < results.length; r++) (function (res) {
        var w = mk('Panel', pageSearch); w.hittest = false; w.hittestchildren = true;
        w.style.flowChildren = 'down'; w.style.width = '100%'; w.style.marginTop = '10px';
        if (res.card) {
          var cd = res.card;
          cfgSearchCrumb(w, CFG_TAB_NAMES[cd.tab] || cd.tab.toUpperCase(), cd.accent, function () { cfgJump(cd, null, null); });
          var tile = mk('Panel', w); tile.hittest = true;
          tile.style.flowChildren = 'right'; tile.style.width = '100%'; tile.style.height = '34px';
          tile.style.paddingLeft = '10px'; tile.style.paddingRight = '10px'; tile.style.borderRadius = '3px';
          tile.style.backgroundColor = '#161b21ff';
          var tt = lbl(tile, cd.title, S.value, 13); tt.hittest = false;
          tt.style.fontWeight = 'bold'; tt.style.letterSpacing = '0.8px';
          tt.style.width = 'fill-parent-flow(1.0)'; tt.style.verticalAlign = 'center';
          tt.style.whiteSpace = 'nowrap'; tt.style.textOverflow = 'ellipsis';
          var ts = lbl(tile, cd.body.__summary ? cd.body.__summary() : '', S.muted, 12); ts.hittest = false;
          ts.style.verticalAlign = 'center'; ts.style.whiteSpace = 'nowrap'; ts.style.marginLeft = '10px';
          hoverable(tile, function () { tile.style.border = '1px solid ' + (tile.__hot ? '#80bdff66' : '#ffffff14'); });
          tile.SetPanelEvent('onactivate', function () { cfgJump(cd, null, null); });
          return;
        }
        var c = res.crumb.card, pg = res.crumb.page;
        var text = (CFG_TAB_NAMES[c.tab] || c.tab.toUpperCase()) + ' › ' + c.title
          + (res.crumb.pivot ? ' › PIVOT' : (pg && c.body.__pageCount && c.body.__pageCount() > 1 ? ' › ' + pg.name : ''));
        cfgSearchCrumb(w, text, c.accent, function () { cfgJump(c, pg, res.crumb.pivot); });
        var proxy = res.e.make(w);
        if (proxy && proxy.row) proxy.row.style.marginTop = '0px';
        if (proxy && res.e.has) { if (proxy.sync) proxy.sync(res.e.last); else if (proxy.set) proxy.set(res.e.last); }
        res.e.proxies.push(proxy);
      })(results[r]);
    }
    function cfgSetQuery(q) {
      cfgQuery = q || '';
      var want = cfgQuery.replace(/^\s+|\s+$/g, '').length >= 2;
      var changed = want !== cfgSearchActive;
      cfgSearchActive = want;
      if (changed) applyTabs();
      try { cfgScrollY = 0; } catch (e) {}
      cfgRenderResults();
      cfgSearchPaint();
    }
    cfgSearchPaint();
)CFJS"
