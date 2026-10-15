#pragma once

#include <string>

// Fragment 2 of the Play-page script: the workshop map sidebar.
//
// The native page gives the workshop list exactly one control -- the "Filter maps" text box
// (#WorkshopSearchTextEntry) -- and its handler, _UpdateWorkshopMapFilter(), filters purely by
// setting panel.visible on each tile. This fragment adds two things in the same currency:
//
//   * Sort: name / recently installed / recently played. Installed + played come from C++
//     (Workshop/WorkshopOrganizer) keyed by the tile's own "addon" attribute (= workshop id).
//     Reordering uses MoveChildBefore, the same API friendslist.js and context_menu_lobbies.js
//     use to reorder their listers.
//   * Hide training maps: one toggle over one editable keyword list.
//
// MATCH ON THE NAME, NOT THE DESCRIPTION. The first version matched keywords against the
// workshop DESCRIPTION and tags too, and it was wrong in-game in the most annoying possible
// way: ordinary maps disappeared because their blurb happened to say "great for practising your
// aim". The description is prose about a map, not a statement of what the map IS. Training
// matching therefore reads the map name and the workshop title ONLY. (The search box still
// searches everything -- that is native behaviour and the user typed those words on purpose.)
//
// ONE WRITER FOR panel.visible. Native writes it on every text change; we write it from our
// own recompute. So our predicate INCLUDES the native search test (replicated from
// _UpdateWorkshopMapFilter) rather than layering on top of whatever native last wrote --
// otherwise the two would fight and the result would depend on who ran last.
//
// NO ICU. Panorama's V8 ships without ICU data, so localeCompare / toLocale* / Intl throw
// RangeError at runtime and take the panel down with them. Sorting compares strings with < / >
// (codepoint order) and case-folding uses toLowerCase, which is not a locale API.

namespace Filmmaker {

// Two literals spliced back together at the bottom: MSVC caps one string literal at 16380 bytes
// and this fragment outgrew it. Splitting it into two SCRIPTS would mean hoisting every local
// onto $.MvmPlay just to satisfy the compiler; splicing keeps it one IIFE with one entry point.
inline const char* kWorkshopFilterJsHead = R"FMJS(
(function () {
  try {
    var M = $.MvmPlay;
    if (!M) return;

    var WS_PANEL = 'gameModeButtonContainer_workshop';

    // ---- state ---------------------------------------------------------------------------
    var data = { sort: 'name', hideTraining: false, words: [], items: {} };
    var bar = null;                 // our sidebar root
    var hideBtn = null, hideMark = null, sortDrop = null;
    var lastOrderSig = '';          // sort mode + tile count; reordering is not free
    var searchHooked = false;

    M.wsData = function () { return data; };

    // ---- payload from C++ ----------------------------------------------------------------
    M.renderWs = function () {
      try {
        var raw = M.host.GetAttributeString('ws', '');
        if (!raw) return;
        data = JSON.parse(raw);
        if (!data.words) data.words = [];
        if (!data.items) data.items = {};
      } catch (e) { return; }
      syncControls();
      if (M.wsSyncEditor) M.wsSyncEditor();
      lastOrderSig = '';   // a new sort mode must re-order even if the tile set did not change
      apply();
    };

    // ---- tile helpers --------------------------------------------------------------------
    function container() {
      var c = M.find(WS_PANEL);
      return (c && c.IsValid && c.IsValid()) ? c : null;
    }
    function tilesOf(c) {
      var out = [];
      try {
        var kids = c.Children();
        for (var i = 0; i < kids.length; ++i) {
          var p = kids[i];
          if (p && p.GetAttributeString('mapname', '') !== '') out.push(p);
        }
      } catch (e) {}
      return out;
    }
    function titleOf(p) {
      try {
        var lbl = p.FindChildTraverse('MapGroupName');
        if (lbl && lbl.text) return ('' + lbl.text).toLowerCase();
      } catch (e) {}
      try { return p.GetAttributeString('mapname', '').toLowerCase(); } catch (e) {}
      return '';
    }
    // What a KEYWORD may match: the @workshop/<id>/<map> path and the workshop title. Nothing
    // else -- see the header note on why the description is deliberately excluded.
    function nameHay(p) {
      var s = '';
      try { s += p.GetAttributeString('mapname', '') + ' '; } catch (e) {}
      return (s + titleOf(p)).toLowerCase();
    }
    // What the SEARCH BOX may match: everything native searches.
    function searchHay(p) {
      var s = nameHay(p) + ' ';
      try { s += p.GetAttributeString('data-workshop-modes', '') + ' '; } catch (e) {}
      try { s += p.GetAttributeString('data-tooltip', ''); } catch (e) {}
      return s.toLowerCase();
    }
    function metaOf(p) {
      var id = '';
      try { id = p.GetAttributeString('addon', ''); } catch (e) {}
      var m = id ? data.items[id] : null;
      return m || { installed: 0, played: 0 };
    }

    // ---- predicates ----------------------------------------------------------------------
    // Replicated from native _UpdateWorkshopMapFilter so our single visible write agrees with
    // what the search box means.
    function searchPass(p, filter) {
      if (filter === '') return true;
      return searchHay(p).indexOf(filter) >= 0;
    }
    function isTraining(p) {
      var hay = nameHay(p);
      var words = data.words || [];
      for (var i = 0; i < words.length; ++i) {
        if (words[i] && hay.indexOf(words[i]) >= 0) return true;
      }
      return false;
    }
    M.wsIsTraining = isTraining;
    function ourPass(p) {
      if (!data.hideTraining) return true;
      return !isTraining(p);
    }

    // ---- apply ---------------------------------------------------------------------------
    function currentFilterText() {
      try {
        var el = M.find('WorkshopSearchTextEntry');
        if (el && el.text) return ('' + el.text).toLowerCase();
      } catch (e) {}
      return '';
    }

    function sortTiles(c, tiles) {
      var mode = data.sort || 'name';
      var sig = mode + '|' + tiles.length;
      if (sig === lastOrderSig) return;
      lastOrderSig = sig;

      var keyed = [];
      for (var i = 0; i < tiles.length; ++i) {
        var p = tiles[i], m = metaOf(p);
        keyed.push({ p: p, name: titleOf(p), installed: m.installed || 0, played: m.played || 0, idx: i });
      }
      keyed.sort(function (a, b) {
        if (mode === 'installed' || mode === 'played') {
          var av = (mode === 'installed') ? a.installed : a.played;
          var bv = (mode === 'installed') ? b.installed : b.played;
          if (av !== bv) return bv - av;         // newest first
          // Never-played / unknown items keep a stable, readable order instead of shuffling.
          return (a.name < b.name) ? -1 : (a.name > b.name ? 1 : a.idx - b.idx);
        }
        return (a.name < b.name) ? -1 : (a.name > b.name ? 1 : a.idx - b.idx);
      });

      // MoveChildBefore(x, firstChild) puts x at the front, so walking the desired order
      // backwards leaves keyed[0] first. Guarded against moving a panel before itself.
      try {
        for (var k = keyed.length - 1; k >= 0; --k) {
          var first = c.GetChild(0);
          if (first && first !== keyed[k].p) c.MoveChildBefore(keyed[k].p, first);
        }
      } catch (e) {}
    }

    function apply() {
      var c = container();
      if (!c) return false;
      var tiles = tilesOf(c);
      if (!tiles.length) return false;
      var filter = currentFilterText();
      sortTiles(c, tiles);
      for (var i = 0; i < tiles.length; ++i) {
        var p = tiles[i];
        var vis = searchPass(p, filter) && ourPass(p);
        try { if (p.visible !== vis) p.visible = vis; } catch (e) {}
      }
      return true;
    }
    M.wsApply = apply;

    // ---- sidebar UI ----------------------------------------------------------------------
    // Deliberately four controls in a narrow column. An earlier version put seven category
    // toggles and a stack of keyword TextEntries in here; it stretched the sidebar and buried
    // the one control that matters.
    function syncControls() {
      try { if (sortDrop) sortDrop.SetSelected('mvmws_' + (data.sort || 'name')); } catch (e) {}
      try { if (hideBtn) hideBtn.checked = !!data.hideTraining; } catch (e) {}
      try { if (hideMark) hideMark.style.backgroundColor = data.hideTraining ? '#398aab' : '#ffffff08'; } catch (e) {}
    }

    function buildBar() {
      var search = M.find('WorkshopSearchBar');
      if (!search) return false;
      var parent = search.GetParent();
      if (!parent) return false;

      bar = $.CreatePanel('Panel', parent, 'MvmWorkshopFilters', { class: 'top-bottom-flow' });
      bar.style.flowChildren = 'down';
      bar.style.marginTop = '18px';
      bar.style.marginBottom = '22px';
      // Left, explicitly, and on every child below. The sidebar is a fixed-width column and the
      // native controls in it start at its left edge; a DropDown left to its class default sat
      // indented from them, which read as the whole block being pushed right.
      bar.style.width = '200px';
      bar.style.marginLeft = '22px';
      bar.style.horizontalAlign = 'left';
      // Sit directly under the native "Filter maps" box rather than at the bottom of the
      // sidebar, so the two map-narrowing controls read as one group.
      try { parent.MoveChildAfter(bar, search); } catch (e) {}

      var sortTitle = $.CreatePanel('Label', bar, 'MvmWsSortTitle', {
        text: 'SORT BY'
      });
      sortTitle.style.horizontalAlign = 'left';
      sortTitle.style.fontFamily = 'Stratum2';
      sortTitle.style.fontSize = '14px';
      sortTitle.style.color = '#c4c9cd';
      sortTitle.style.marginBottom = '8px';

      sortDrop = $.CreatePanel('DropDown', bar, 'MvmWsSort', {
        class: 'content-navbar__tabs__dropdown', menuclass: 'DropDownMenu'
      });
      sortDrop.style.horizontalAlign = 'left';
      sortDrop.style.marginLeft = '0px';
      sortDrop.style.width = '200px';
      sortDrop.style.height = '36px';
      sortDrop.style.marginBottom = '18px';
      sortDrop.style.backgroundColor = '#ffffff08';
      sortDrop.style.border = '1px solid #ffffff30';
      var sorts = [['name', 'Name'], ['installed', 'Recently installed'], ['played', 'Recently played']];
      for (var s = 0; s < sorts.length; ++s) {
        // Text set in the creation props, not afterwards: the DropDown shows the selected
        // option by cloning that panel, so it has to carry its caption from the start.
        var opt = $.CreatePanel('Label', sortDrop, 'mvmws_' + sorts[s][0], {
          class: 'DropDownMenu', text: sorts[s][1]
        });
        opt.style.fontFamily = 'Stratum2';
        opt.style.fontSize = '16px';
        sortDrop.AddOption(opt);
      }
      sortDrop.SetPanelEvent('oninputsubmit', function () {
        var sel = null;
        try { sel = sortDrop.GetSelected(); } catch (e) {}
        if (!sel) return;
        var mode = ('' + sel.id).replace('mvmws_', '');
        data.sort = mode;
        lastOrderSig = '';
        M.cmd('ws sort ' + mode);
        apply();
      });

      hideBtn = $.CreatePanel('Button', bar, 'MvmWsHide', {});
      hideBtn.style.width = '200px';
      hideBtn.style.height = '30px';
      var hideText = $.CreatePanel('Label', hideBtn, 'MvmWsHideLabel', { text: 'Hide training maps' });
      hideText.style.fontFamily = 'Stratum2';
      hideText.style.fontSize = '16px';
      hideText.style.color = '#e2e5e7';
      hideText.style.verticalAlign = 'center';
      hideMark = $.CreatePanel('Panel', hideBtn, 'MvmWsHideMark', {});
      hideMark.style.horizontalAlign = 'right';
      hideMark.style.verticalAlign = 'center';
      hideMark.style.width = '18px'; hideMark.style.height = '18px';
      hideMark.style.border = '1px solid #ffffff50';
      hideMark.style.borderRadius = '2px';
      hideBtn.checked = !!data.hideTraining;
      hideBtn.style.marginTop = '0px';
      hideBtn.style.horizontalAlign = 'left';
      hideBtn.SetPanelEvent('onactivate', function () {
        var on = false;
        try { on = !data.hideTraining; } catch (e) {}
        data.hideTraining = on;
        syncControls();
        M.cmd('ws hide ' + (on ? 1 : 0));
        apply();
      });

      var row = $.CreatePanel('Panel', bar, 'MvmWsButtons', {});
      row.style.flowChildren = 'right';
      row.style.marginTop = '14px';
      row.style.width = '200px';
      row.style.horizontalAlign = 'left';

      function toolButton(id, caption) {
        var b = $.CreatePanel('Button', row, id, {});
        b.style.width = '96px'; b.style.height = '32px';
        b.style.backgroundColor = '#ffffff08';
        b.style.border = '1px solid #ffffff20';
        b.style.borderRadius = '2px';
        var l = $.CreatePanel('Label', b, id + 'Label', { text: caption });
        l.style.fontFamily = 'Stratum2'; l.style.fontSize = '16px'; l.style.color = '#d0d6da';
        l.style.horizontalAlign = 'center'; l.style.verticalAlign = 'center';
        b.SetPanelEvent('onmouseover', function () { b.style.backgroundColor = '#ffffff18'; });
        b.SetPanelEvent('onmouseout', function () { b.style.backgroundColor = '#ffffff08'; });
        return b;
      }
      var editBtn = toolButton('MvmWsEdit', 'Keywords');
      editBtn.SetPanelEvent('onactivate', function () {
        if (M.wsToggleEditor) M.wsToggleEditor();
      });

      var rescanBtn = toolButton('MvmWsRescan', 'Rescan');
      rescanBtn.style.marginLeft = '8px';
      rescanBtn.SetPanelEvent('onactivate', function () { M.cmd('ws rescan'); });

      // The payload almost always arrives BEFORE these controls exist (C++ pushes it as soon as
      // the script is injected; the sidebar is not built until the play page appears), so the
      // syncControls() inside renderWs ran against nothing. Without this the sort dropdown
      // renders blank even though the sort itself is applied.
      syncControls();
      return true;
    }
    M.wsBar = function () { return bar; };

)FMJS";

inline const char* kWorkshopFilterJsTail = R"FMJS(
    // ---- every workshop map offers every mode ----------------------------------------------
    //
    // Pressing GO on the Workshop tab does not launch: native calls _DisplayWorkshopModePopup(),
    // which builds its list from each selected tile's `data-workshop-modes` attribute -- the
    // map's Steam Workshop TAGS, mapped through k_workshopModes (mainmenu_play.js). A map tagged
    // only "Custom" therefore carries one mode, and popup_workshop_mode_select.js has:
    //
    //     if (modes.length <= 1) { _Apply(modes[0]); return; }
    //
    // so it silently launches in that one mode with no choice offered at all. Which mode a map
    // was TAGGED with is the uploader's guess at how people would play it; it is not a property
    // of the map, and on an offline bot/movie build it is not a decision worth inheriting.
    //
    // So every tile is offered the full set. The map's own modes stay FIRST, because the popup
    // checks modes[0] by default -- open it and the tagged mode is still what is selected, with
    // the rest now available underneath.
    //
    // Attribute-only: native's launch path is untouched, and it resolves the chosen mode itself
    // through GameTypesAPI. Re-applied every tick because native rewrites the attribute whenever
    // it refreshes tile info.
    var ALL_MODES = [
      'casual', 'competitive', 'scrimcomp2v2', 'deathmatch',
      'gungameprogressive', 'retakes', 'flyingscoutsman', 'custom'
    ];

    function offerEveryMode() {
      var c = container();
      if (!c) return;
      var tiles = tilesOf(c);
      for (var i = 0; i < tiles.length; ++i) {
        try {
          var own = ('' + tiles[i].GetAttributeString('data-workshop-modes', '')).split(',');
          var list = [];
          for (var o = 0; o < own.length; ++o) {
            if (own[o] && list.indexOf(own[o]) < 0) list.push(own[o]);
          }
          var added = false;
          for (var m = 0; m < ALL_MODES.length; ++m) {
            if (list.indexOf(ALL_MODES[m]) < 0) { list.push(ALL_MODES[m]); added = true; }
          }
          // Only write when it actually changed: SetAttributeString every tick on every tile is
          // work native does not need to see.
          if (added) tiles[i].SetAttributeString('data-workshop-modes', list.join(','));
        } catch (e) {}
      }
    }

    // ---- tick hook -----------------------------------------------------------------------
    // The sidebar and the tile container are both created lazily by CS2 (the sidebar with the
    // play page, the tiles on the first visit to the Workshop tab), and both are destroyed
    // when the page unloads -- so every tick re-checks rather than assuming.
    M.hooks.push(function () {
      var search = M.find('WorkshopSearchBar');
      var alive = false;
      try { alive = !!(bar && bar.IsValid()); } catch (e) { alive = false; }
      if (!alive) {
        bar = null; hideBtn = null; hideMark = null; sortDrop = null; lastOrderSig = ''; searchHooked = false;
        if (search) buildBar();
      }
      // Our controls belong to the workshop tab only; native owns that decision for its own
      // search box, so just follow it.
      var onWorkshop = false;
      try { onWorkshop = !!(search && search.visible); } catch (e) {}
      if (bar && search) { try { bar.visible = onWorkshop; } catch (e) {} }

      // GAME MODES ON WORKSHOP MAPS.
      //
      // Native hides the game-mode row on the Workshop tab (a `gamemode-workshop` class switch)
      // and disables every radio in it:
      //   element.enabled = element.enabled && !isWorkshop && ...   (mainmenu_play.js)
      // The effect is that a workshop map launches in whatever mode was last selected, which for
      // most people is Wingman -- a 2-player mode with no room to add bots. Since bots are the
      // entire point of this build, put the row back: the mode buttons keep their native
      // handlers, so picking Casual or Deathmatch here goes through CS2's own bookkeeping.
      //
      // Re-asserted every tick rather than once, because native re-runs the disabling pass on
      // every tab switch and would take it away again.
      if (onWorkshop) {
        offerEveryMode();
        var row = M.find('id-navbar-tabs');
        if (row) { try { row.visible = true; } catch (e) {} }
        var radios = M.find('GameModeSelectionRadios');
        if (radios) {
          try { radios.visible = true; } catch (e) {}
          try {
            for (var r = 0; r < radios.GetChildCount(); ++r) {
              var kid = radios.GetChild(r);
              if (kid) kid.enabled = true;
            }
          } catch (e) {}
        }
      }

      // Re-apply as soon as the search text changes, instead of waiting for the next tick.
      // RegisterEventHandler runs ALONGSIDE the native ontextentrychange handler; replacing it
      // with SetPanelEvent would break the native filter and its clear button.
      if (!searchHooked) {
        var entry = M.find('WorkshopSearchTextEntry');
        if (entry) {
          try {
            $.RegisterEventHandler('TextEntryChanged', entry, function () { $.Schedule(0.0, apply); });
            searchHooked = true;
          } catch (e) {}
        }
      }
      apply();
    });

    // ---- netcon report -------------------------------------------------------------------
    //   mirv_filmmaker play eval $.Msg($.MvmPlay.wsReport())
    M.wsReport = function () {
      var c = container();
      var tiles = c ? tilesOf(c) : [];
      var shown = 0, withMeta = 0;
      for (var i = 0; i < tiles.length; ++i) {
        try { if (tiles[i].visible) shown++; } catch (e) {}
        if (metaOf(tiles[i]).installed) withMeta++;
      }
      return '[mvmws] bar=' + (bar ? 1 : 0) + ' tiles=' + tiles.length + ' shown=' + shown +
             ' meta=' + withMeta + ' sort=' + data.sort + ' hide=' + (data.hideTraining ? 1 : 0) +
             ' words=' + (data.words || []).length + '\n';
    };
    // What the GO popup will offer for the first N tiles. The popup builds itself from this
    // attribute, and it skips itself entirely when a tile carries one mode -- so "how many modes
    // does this tile have" is the whole question, and it is not visible anywhere on screen.
    M.wsModes = function (n) {
      var c = container();
      var tiles = c ? tilesOf(c) : [];
      var out = '[mvmmodes]';
      var count = Math.min(n || 3, tiles.length);
      for (var i = 0; i < count; ++i) {
        var modes = '';
        try { modes = '' + tiles[i].GetAttributeString('data-workshop-modes', ''); } catch (e) {}
        out += ' ' + titleOf(tiles[i]).substring(0, 10) + '=' + modes.split(',').length;
      }
      return out + ' tiles=' + tiles.length + '\n';
    };

    // First N tiles in display order, for spot-checking a sort against the folder mtimes.
    M.wsOrder = function (n) {
      var c = container();
      var tiles = c ? tilesOf(c) : [];
      var out = [];
      for (var i = 0; i < tiles.length && out.length < (n || 5); ++i) {
        if (!tiles[i].visible) continue;
        out.push(titleOf(tiles[i]) + '#' + metaOf(tiles[i]).installed);
      }
      return '[mvmws] ' + out.join(' | ') + '\n';
    };
    // Which tiles the keyword list currently classes as training, and which it does not.
    // This is the check that catches an over-broad keyword (an ordinary map in the hidden list)
    // or a missing one (a config/aim map in the kept list).
    M.wsHidden = function () {
      var c = container();
      var tiles = c ? tilesOf(c) : [];
      var hid = [], kept = [];
      for (var i = 0; i < tiles.length; ++i) {
        (isTraining(tiles[i]) ? hid : kept).push(titleOf(tiles[i]));
      }
      return '[mvmws] training(' + hid.length + '): ' + hid.join(', ') +
             '\n[mvmws] kept(' + kept.length + '): ' + kept.join(', ') + '\n';
    };
  } catch (e) {
    try { $.Msg('[mvmplay] workshop filter inject failed: ' + e + '\n'); } catch (e2) {}
  }
})();
)FMJS";

inline const std::string kWorkshopFilterJs =
	std::string(kWorkshopFilterJsHead) + kWorkshopFilterJsTail;

} // namespace Filmmaker
