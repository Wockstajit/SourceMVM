#pragma once

// Demo list ORGANISER UI. JS fragment concatenated (via #include, as an adjacent raw string
// literal) into the kFilmmakerGuiJs script assembled in FilmmakerGuiJs.h -- not a standalone
// script. It shares that IIFE's scope and uses its helpers (demos, listPanel, mapDisplay,
// demoName, setMapIcon, api.addCommand, ...).
//
// What it owns, all of it living in the left rail ABOVE the demo tiles:
//   * the sort control          -> "org sort <mode>"
//   * the add control           -> "addfolder" | "adddemo" | "org newfolder <name>"
//   * collapsible folder headers-> "org collapse <id|-1> <0|1>", "org rename", "org delfolder"
//   * drag a tile onto a header -> "org assign <id|-1> <key>"
//
// C++ side: Demo/DemoOrganizer.{h,cpp} (command handling + payload) and
// Config/DemoOrganizerStore.{h,cpp} (persistence).
//
// THE JOIN KEY (the one thing to be careful with): C++ persists demos under
// NormalizeDemoKey(fullPath) -- lower-cased, '/' -> '\', no trailing separator. The UI has no
// full path in its payload, so orgKeyOf() rebuilds the same string from folder + '\' +
// fileName. If either side's normalisation changes, folder membership silently stops matching
// and every demo appears unsorted. Change both together.
//
// LAYOUT NOTE: tiles stay DIRECT children of FmMatchList. Grouping is done by interleaving
// header panels between them in the same flow and hiding a group's tiles when it is collapsed,
// rather than by nesting tiles in per-group containers -- native matchtile.vcss styles the
// tiles through that parent relationship, and re-parenting them loses the native look.

R"FMORG(
    // ---- organiser state -------------------------------------------------
    // Mirrors DemoOrganizer_BuildJson(): { sort, rootCollapsed, groups:[{id,name,collapsed}],
    // demos:{ "<key>": {g,played} } }. Defaults are the same as the C++ store's so the UI
    // renders sanely before the first push lands.
    var orgState = { sort: 'date', rootCollapsed: false, groups: [], demos: {} };
    var orgHeaders = [];        // header panels, in render order
    var orgTileGroup = [];      // parallel to tilePanels: which folder id each tile sits in
    var orgDragGhost = null;    // the floating card shown while dragging a tile
    var orgDragKey = null;      // key of the demo currently being dragged
    var renderedPlanSig = null; // structure the list was last built for
    var orgNewFolderRow = null, orgNewFolderInput = null;
    var orgSortDrop = null;     // native DropDown carrying the sort modes
    var orgSyncingSort = false; // guards SetSelected against re-entering oninputsubmit

    var ORG_SORTS = [
      ['played', 'Recently played'],
      ['date', 'Newest first'],
      ['dateasc', 'Oldest first'],
      ['map', 'Map name'],
      ['name', 'Demo name']
    ];
    var ORG_GOLD = '#f0b323';
    var ORG_ARROW = 's2r://panorama/images/icons/ui/arrowhead.vsvg';
    var ORG_SORT_OPT = 'fmsort_';   // option-panel id prefix; the suffix IS the sort mode

    // Pushes the persisted sort onto the DropDown without that write echoing back as a user
    // pick (SetSelected fires oninputsubmit).
    function orgSyncSortSelection() {
      if (!orgSortDrop) return;
      orgSyncingSort = true;
      try { orgSortDrop.SetSelected(ORG_SORT_OPT + (orgState.sort || 'date')); } catch (e) {}
      orgSyncingSort = false;
    }

    // Rebuilds the C++ NormalizeDemoKey() form from the fields the demo payload already has.
    // The trailing-separator strip matters for demos sitting at a drive root, where the
    // payload's folder is "C:\" and a naive join would produce a doubled separator.
    function orgKeyOf(demo) {
      var f = ('' + ((demo && demo.folder) || '')).replace(/\//g, '\\');
      while (f.length && f.charAt(f.length - 1) === '\\') f = f.substr(0, f.length - 1);
      var n = ('' + ((demo && demo.fileName) || '')).replace(/\//g, '\\');
      return (f + '\\' + n).toLowerCase();
    }
    function orgRecord(demo) { return orgState.demos[orgKeyOf(demo)] || null; }
    function orgPlayedAt(demo) { var r = orgRecord(demo); return (r && r.played) || 0; }
    // The sort already preserves the complete play history. The rail is a spatial shortcut,
    // so reserve it for exactly one tile: the newest played demo currently visible.
    function orgLatestPlayedKey() {
      var bestKey = '', bestTime = 0;
      for (var i = 0; i < demos.length; ++i) {
        var key = orgKeyOf(demos[i]);
        var played = orgPlayedAt(demos[i]);
        if (played > bestTime || (played === bestTime && played > 0 && (!bestKey || key < bestKey))) {
          bestKey = key; bestTime = played;
        }
      }
      return bestKey;
    }
    function orgIsLatestPlayed(demo) {
      var latest = orgLatestPlayedKey();
      return !!latest && orgKeyOf(demo) === latest;
    }
    function orgGroupOf(demo) { var r = orgRecord(demo); return (r && typeof r.g === 'number') ? r.g : -1; }

    function orgAgo(unix) {
      if (!unix) return 'never';
      var d = Math.floor(Date.now() / 1000) - unix;
      if (d < 90) return 'just now';
      if (d < 3600) return Math.floor(d / 60) + ' min ago';
      if (d < 172800) return Math.floor(d / 3600) + ' hr ago';
      if (d < 2592000) return Math.floor(d / 86400) + ' days ago';
      return Math.floor(d / 2592000) + ' mo ago';
    }

    function orgSound(name) { try { $.DispatchEvent('CSGOPlaySoundEffect', 'UIPanorama.' + name, 'MOUSE'); } catch (e) {} }

    // Opens a context menu ANCHORED to the panel that invoked it.
    //
    // The first argument is a panel id, and it matters. Native CS2 passes '' everywhere, but
    // native scripts run inside the panel's own layout context, so '' resolves to that panel.
    // This script is injected into the shared main-menu context instead, where '' resolves to
    // the root -- which is why the sort menu opened in the top-left corner of the screen rather
    // than under its button. Always pass the real id.
    function orgMenu(anchorPanel, items) {
      if (!items || !items.length) return;
      var id = '';
      try { id = (anchorPanel && anchorPanel.id) ? ('' + anchorPanel.id) : ''; } catch (e) {}
      try { UiToolkitAPI.ShowSimpleContextMenu(id, 'ControlLibSimpleContextMenu', items); }
      catch (e) { $.Msg('[filmmaker] context menu unavailable: ' + e + '\n'); }
    }
)FMORG"
// ---- ordering + the render plan ----
R"FMORG(
    // Plain codepoint compare on the lower-cased strings.
    //
    // NOT String.prototype.localeCompare: Panorama's V8 is built without full ICU data, and
    // calling it throws "RangeError: Internal error. Icu error." That exception propagates out
    // of orgBuildPlan -> reconcileList -> renderInto and takes the WHOLE demo list down, so the
    // Map-name and Demo-name sorts rendered nothing at all. Node has complete ICU, so this can
    // never be caught off-engine -- see the guard in demo-organizer-tests.mjs.
    function cmpText(x, y) { return x < y ? -1 : (x > y ? 1 : 0); }

    // One comparator, chosen by the persisted sort mode. Ties fall back to newest-first so the
    // order is total and a refresh never reshuffles equal rows.
    function orgCompare(a, b) {
      var mode = orgState.sort || 'date';
      if (mode === 'played') {
        var pa = orgPlayedAt(a), pb = orgPlayedAt(b);
        if (pa !== pb) return pb - pa;              // most recently played first
      } else if (mode === 'dateasc') {
        if ((a.date || 0) !== (b.date || 0)) return (a.date || 0) - (b.date || 0);
      } else if (mode === 'map') {
        var m = cmpText(mapDisplay(a.map).toLowerCase(), mapDisplay(b.map).toLowerCase());
        if (m) return m;
      } else if (mode === 'name') {
        var n = cmpText(demoName(a).toLowerCase(), demoName(b).toLowerCase());
        if (n) return n;
      }
      return (b.date || 0) - (a.date || 0);
    }

    // Builds the flat render order: for each folder a header followed by its members, then the
    // ungrouped bucket. Entries carry POSITIONS into `demos` (not sorted copies) because
    // tilePanels[] is indexed by that position and setCheckedTile()/selectDemo() rely on it.
    //
    // With no folders defined at all there are no headers -- a fresh install keeps exactly the
    // flat native list it had before this feature existed.
    function orgBuildPlan() {
      var groups = orgState.groups || [];
      var buckets = {}, loose = [];
      for (var g = 0; g < groups.length; ++g) buckets[groups[g].id] = [];
      for (var i = 0; i < demos.length; ++i) {
        var gid = orgGroupOf(demos[i]);
        if (gid >= 0 && buckets[gid]) buckets[gid].push(i); else loose.push(i);
      }
      function ordered(positions) {
        return positions.slice().sort(function (x, y) { return orgCompare(demos[x], demos[y]); });
      }
      var plan = [];
      for (var k = 0; k < groups.length; ++k) {
        var members = ordered(buckets[groups[k].id]);
        plan.push({ t: 'h', id: groups[k].id, name: groups[k].name, n: members.length, collapsed: !!groups[k].collapsed });
        for (var m = 0; m < members.length; ++m) plan.push({ t: 'd', pos: members[m], gid: groups[k].id });
      }
      var rest = ordered(loose);
      // "No folder", not "Unsorted": the sort control sits three rows above this header, and
      // calling the bucket "unsorted" reads as "the sort does not apply here".
      if (groups.length)
        plan.push({ t: 'h', id: -1, name: 'No folder', n: rest.length, collapsed: !!orgState.rootCollapsed });
      for (var r = 0; r < rest.length; ++r) plan.push({ t: 'd', pos: rest[r], gid: -1 });
      return plan;
    }

    // Structure only. Collapse state is deliberately NOT in here: collapsing must be a
    // visibility toggle, never a rebuild, or every expand/collapse would reset the scroll
    // position and the selection.
    function orgPlanSig(plan) {
      var s = [orgState.sort || ''];
      for (var i = 0; i < plan.length; ++i) {
        var e = plan[i];
        s.push(e.t === 'h' ? ('h' + e.id + ':' + e.name + ':' + e.n) : ('d' + e.pos));
      }
      return s.join('|');
    }

    // Which group ids are currently collapsed, keyed by id as a string ('-1' = No folder).
    // The ONE definition of "collapsed" -- orgApplyCollapse and orgIsCollapsed both read it.
    //
    // The No-folder bucket only gets a header when at least one folder exists (a folderless
    // install stays a plain flat list). Honouring a persisted rootCollapsed with no header on
    // screen would hide EVERY demo behind a control the user cannot see or click -- which is
    // exactly what happened after deleting the last folder while that bucket was collapsed: the
    // browser went empty with no way back. Never hide tiles whose header is not rendered.
    function orgCollapsedMap() {
      var groups = orgState.groups || [];
      var map = { '-1': groups.length > 0 && !!orgState.rootCollapsed };
      for (var g = 0; g < groups.length; ++g) map['' + groups[g].id] = !!groups[g].collapsed;
      return map;
    }

    // Collapse = hide the group's tiles. Panorama drops invisible panels out of flow, so the
    // header closes up against the next one with no gap. Expanding fades the first rows in
    // with a short stagger; the cap keeps a 200-demo folder from scheduling 200 callbacks.
    function orgApplyCollapse(animate) {
      var collapsedOf = orgCollapsedMap();
      var revealed = 0;
      for (var i = 0; i < tilePanels.length; ++i) {
        var tile = tilePanels[i];
        if (!tile) continue;
        var hide = !!collapsedOf['' + (orgTileGroup[i] != null ? orgTileGroup[i] : -1)];
        var was = tile.visible;
        tile.visible = !hide;
        if (!hide && animate && !was && revealed < 12) {
          (function (t, n) {
            try { t.style.opacity = '0'; } catch (e) {}
            $.Schedule(0.012 * n, function () { try { t.style.opacity = '1'; } catch (e) {} });
          })(tile, revealed);
          ++revealed;
        }
      }
      for (var h = 0; h < orgHeaders.length; ++h) {
        var hd = orgHeaders[h];
        if (!hd || !hd.__arrow) continue;
        var c = !!collapsedOf['' + hd.__gid];
        try { hd.__arrow.style.transform = 'rotateZ(' + (c ? 90 : 180) + 'deg)'; } catch (e) {}
      }
    }
)FMORG"
// ---- folder headers (collapse toggle, rename/delete menu, drop target) ----
R"FMORG(
    function orgResetHeaders() { orgHeaders = []; orgTileGroup = new Array(demos.length); }

    // Current collapse state for one folder id, via orgCollapsedMap (the single source of
    // truth). Header click handlers MUST go through this rather than capturing entry.collapsed
    // at build time: headers are only recreated on a structural rebuild, and collapsing
    // deliberately does not rebuild, so a captured value goes stale the moment the folder is
    // first closed. That stale capture is what made a shut folder impossible to reopen -- every
    // later click re-sent "collapse 1", which the store correctly treated as no change.
    function orgIsCollapsed(id) {
      return !!orgCollapsedMap()['' + id];
    }

    // Optimistic local write so the arrow turns on the same frame as the click; the C++
    // round-trip then confirms it.
    function orgSetCollapsedLocal(id, collapsed) {
      if (id < 0) { orgState.rootCollapsed = collapsed; return; }
      var gs = orgState.groups || [];
      for (var i = 0; i < gs.length; ++i) if (gs[i].id === id) gs[i].collapsed = collapsed;
    }

    function orgHeaderByGid(id) {
      for (var i = 0; i < orgHeaders.length; ++i)
        if (orgHeaders[i] && orgHeaders[i].__gid === id) return orgHeaders[i];
      return null;
    }

    // True when the headers already on screen are exactly the ones this plan wants (same ids,
    // same names). Counts may differ -- those are refreshed during the reorder.
    function orgHeaderSetMatches(plan) {
      var want = [];
      for (var i = 0; i < plan.length; ++i) if (plan[i].t === 'h') want.push(plan[i]);
      if (want.length !== orgHeaders.length) return false;
      for (var w = 0; w < want.length; ++w) {
        var head = orgHeaderByGid(want[w].id);
        if (!head || head.__hname !== want[w].name) return false;
      }
      return true;
    }

    // Re-orders the EXISTING children to match the plan instead of rebuilding the list.
    //
    // This is what keeps scroll position and selection alive. Sorting by a value that fills in
    // during a background scan (Recently played, and any date the .dem.info pass resolves late)
    // changes the running order mid-scan; rebuilding to express that destroyed and recreated
    // every row, so the list jumped under the user each time the scan published. Moving panels
    // costs nothing and preserves their identity.
    //
    // Returns false if any panel the plan needs is missing, in which case the caller rebuilds.
    function orgApplyPlanOrder(plan) {
      var prev = null;
      for (var i = 0; i < plan.length; ++i) {
        var e = plan[i];
        var panel = (e.t === 'h') ? orgHeaderByGid(e.id) : tilePanels[e.pos];
        if (!panel) return false;
        try {
          if (prev) {
            listPanel.MoveChildAfter(panel, prev);
          } else {
            // Reference the CURRENT first child, the way native does it
            // (mainmenu_play.js: MoveChildBefore(elParty, container.Children()[0])).
            // Guarded against moving a panel before itself, and against an empty list.
            var first = listPanel.Children()[0];
            if (first && first !== panel) listPanel.MoveChildBefore(panel, first);
          }
        } catch (err) { return false; }
        if (e.t === 'h') {
          // Membership may have moved even though the folder set did not; the count is the
          // only part of a header that reflects that.
          try { if (panel.__count) panel.__count.text = '' + e.n; } catch (err2) {}
        } else {
          orgTileGroup[e.pos] = e.gid;
        }
        prev = panel;
      }
      return true;
    }

    // Re-applies the "this demo has been played" edge to existing tiles. Needed on the in-place
    // refresh path: playing a demo bumps the organiser version without changing the render plan
    // (unless the sort IS "played"), so without this the accent would not appear until the next
    // structural rebuild.
    function orgRefreshAccents() {
      for (var i = 0; i < tilePanels.length && i < demos.length; ++i) {
        if (tilePanels[i]) orgPaintPlayedEdge(tilePanels[i], demos[i]);
      }
    }

    // A header is a Button so it gets keyboard/controller focus for free. It is also the drop
    // target for tiles dragged onto it, including while collapsed -- dropping onto a shut
    // folder is the fastest way to file something out of sight.
    function orgCreateHeader(entry) {
      var head = $.CreatePanel('Button', listPanel, 'FmOrgHead' + (entry.id < 0 ? 'Root' : entry.id), {});
      head.__gid = entry.id;
      head.style.flowChildren = 'right';
      head.style.width = '100%';
      head.style.height = '30px';
      // Extra right padding clears FmMatchList's vertical scrollbar, which overlays the right
      // edge of every row -- the folder count sat under it and rendered visibly cut in half.
      head.style.paddingLeft = '9px'; head.style.paddingRight = '20px';
      head.style.marginTop = '6px';
      // Flat band only. A folder header reads as a header from its chevron, uppercase label and
      // count -- an accent rail down its left edge would just be a second, louder signal for
      // something already legible, and CS2's own list rows carry no such rail.
      head.style.backgroundColor = 'rgba(255,255,255,0.04)';
      head.style.transitionProperty = 'background-color';
      head.style.transitionDuration = '0.12s';
      head.style.transitionTimingFunction = 'ease-out';

      var arrow = $.CreatePanel('Image', head, '', { src: ORG_ARROW, texturewidth: '10', textureheight: '10' });
      arrow.style.width = '10px'; arrow.style.height = '10px';
      arrow.style.verticalAlign = 'center'; arrow.style.marginRight = '9px';
      arrow.style.washColor = '#8b949c';
      // arrowhead.vsvg points UP at 0deg (see the native PaginationArrow rules), so an open
      // folder needs 180deg and a shut one 90deg.
      arrow.style.transform = 'rotateZ(' + (entry.collapsed ? 90 : 180) + 'deg)';
      arrow.style.transitionProperty = 'transform, wash-color';
      arrow.style.transitionDuration = '0.14s';
      arrow.style.transitionTimingFunction = 'ease-out';
      arrow.hittest = false;
      head.__arrow = arrow;

      // Weight/tracking follow CS2's own uppercase UI labels (.content-navbar__tabs__btn Label:
      // Stratum2, medium, no added tracking) and the navbar buttons this fork already ships.
      // Stratum2 is a wide squared face; the extra letter-spacing a header seems to want is a
      // habit from narrower typefaces and reads as drift next to the real navbar.
      var name = $.CreatePanel('Label', head, '', { text: ('' + entry.name).toUpperCase() });
      name.style.verticalAlign = 'center';
      // The name takes the slack and truncates; the count is never the thing that gets cut.
      // A right-aligned count in a right-flowing row was being pushed off the edge by a long
      // folder name instead of the name giving way.
      name.style.width = 'fill-parent-flow(1.0)';
      name.style.fontSize = '13px'; name.style.fontWeight = 'medium';
      name.style.letterSpacing = '0.3px';
      name.style.color = '#c7ced6ff';
      name.style.fontFamily = 'Stratum2, "Arial Unicode MS"';
      name.style.whiteSpace = 'nowrap';
      name.style.textOverflow = 'ellipsis';
      name.style.transitionProperty = 'color';
      name.style.transitionDuration = '0.12s';
      name.hittest = false;
      head.__name = name;

      // The count is real information (how many demos are in a shut folder), so it has to clear
      // 4.5:1 against the rail -- #8b949c does, the dimmer grey this started as did not.
      var count = $.CreatePanel('Label', head, '', { text: '' + entry.n });
      count.style.verticalAlign = 'center'; count.style.marginLeft = '8px';
      count.style.fontSize = '12px'; count.style.color = '#8b949cff';
      count.style.fontFamily = 'Stratum2, "Arial Unicode MS"';
      count.hittest = false;
      head.__count = count;
      head.__hname = entry.name;   // compared by orgHeaderSetMatches

      head.SetPanelEvent('onactivate', (function (id) {
        return function () {
          orgSound('sidemenu_select');
          var next = !orgIsCollapsed(id);   // read live state, never a build-time capture
          orgSetCollapsedLocal(id, next);
          orgApplyCollapse(true);
          api.addCommand('org collapse ' + id + ' ' + (next ? 1 : 0));
        };
      })(entry.id));

      head.SetPanelEvent('onmouseover', function () {
        try { head.style.backgroundColor = 'rgba(240,179,35,0.10)'; arrow.style.washColor = ORG_GOLD; } catch (e) {}
      });
      head.SetPanelEvent('onmouseout', function () {
        if (head.__dropActive) return;   // a drag is hovering this header; it owns the paint
        try { head.style.backgroundColor = 'rgba(255,255,255,0.04)'; arrow.style.washColor = '#8b949c'; } catch (e) {}
      });

      // The No-folder bucket is not a real folder, so it has nothing to rename or delete.
      if (entry.id >= 0) {
        head.SetPanelEvent('oncontextmenu', (function (id, current) {
          return function () {
            orgMenu(head, [
              { label: 'Rename folder', jsCallback: function () { orgBeginRename(id, current); } },
              { label: 'Delete folder', jsCallback: function () { orgConfirmDeleteFolder(id, current); } }
            ]);
          };
        })(entry.id, entry.name));
      }

      orgWireDropTarget(head, entry.id);
      orgHeaders.push(head);
      return head;
    }

    function orgConfirmDeleteFolder(id, name) {
      try {
        UiToolkitAPI.ShowGenericPopupOkCancel(
          'Delete folder?',
          'Delete "' + name + '"?\nIts demos stay in the list, just outside any folder. No demo files are touched.',
          '',
          function () { api.addCommand('org delfolder ' + id); orgSound('inventory_item_notequipped'); },
          function () {});
      } catch (e) {
        $.Msg('[filmmaker] confirmation popup unavailable; folder kept.\n');
      }
    }
)FMORG"
// ---- inline rename / new folder (a TextEntry in the rail, not a popup) ----
R"FMORG(
    // Renaming swaps the header's label for a TextEntry in place. Deliberately inline rather
    // than a modal: the rail is narrow and a popup for a 10-character name is heavier than the
    // action deserves. Enter commits, Esc/blur cancels, and either way the next payload push
    // rebuilds the header from the authoritative C++ state.
    function orgBeginRename(id, current) {
      var head = null;
      for (var i = 0; i < orgHeaders.length; ++i) if (orgHeaders[i] && orgHeaders[i].__gid === id) head = orgHeaders[i];
      if (!head || head.__renaming) return;
      head.__renaming = true;
      head.__name.visible = false;

      var entry = $.CreatePanel('TextEntry', head, '', { text: current, maxchars: '40' });
      entry.style.width = 'fill-parent-flow(1.0)';
      entry.style.height = '22px';
      entry.style.verticalAlign = 'center';
      entry.style.marginRight = '8px';
      entry.style.backgroundColor = '#0d0f12ff';
      entry.style.border = '1px solid ' + ORG_GOLD;
      entry.style.borderRadius = '3px';
      entry.style.color = '#eef2f6ff';
      entry.style.fontSize = '13px';
      entry.style.paddingLeft = '6px';

      function finish(commit) {
        if (!head.__renaming) return;
        head.__renaming = false;
        var text = '';
        try { text = ('' + (entry.text || '')).trim(); } catch (e) {}
        try { entry.DeleteAsync(0); } catch (e) {}
        try { head.__name.visible = true; } catch (e) {}
        if (commit && text && text !== current) {
          api.addCommand('org rename ' + id + ' ' + text);
          orgSound('sidemenu_select');
        }
      }
      // 'oninputsubmit' is Panorama's Enter event for a TextEntry (chat.js drives #ChatInput
      // with it). 'onactivate' is the BUTTON activation event and never fires here -- wiring
      // commit to it is why typing a name and pressing Enter did nothing.
      entry.SetPanelEvent('oninputsubmit', function () { finish(true); });  // Enter commits
      entry.SetPanelEvent('oncancel', function () { finish(false); });      // Esc cancels
      // Best-effort only: 'onblur' appears nowhere in CS2's own scripts, so treat it as
      // possibly-unsupported. Enter and Esc are the paths that are known to work, and the field
      // simply stays open until one of them is used. SetPanelEvent with an unknown event name
      // is inert, so this costs nothing if the engine does not raise it.
      entry.SetPanelEvent('onblur', function () { finish(true); });
      // Same deferred focus as orgOpenNewFolder: a just-created panel is not laid out yet, and
      // SetFocus on it is dropped.
      $.Schedule(0.0, function () {
        try { entry.SetFocus(); entry.SelectAll(); } catch (e) {}
      });
    }

    // New-folder block: hidden by default, revealed under the toolbar by the add menu.
    //
    // Two stacked lines rather than one cramped row. The rail is ~250px wide, and a single row
    // of [caption][field][ok][cancel] left the field about 90px -- too narrow to read a folder
    // name back while typing it. Giving the caption its own line hands the whole rail width to
    // the field, which is the part the user is actually looking at.
    function orgBuildNewFolderRow(parent) {
      // A shaded block attached under the toolbar, not a bare field floating on the rail: the
      // tint plus the bottom rule reads as "the toolbar has opened up to ask you something".
      orgNewFolderRow = $.CreatePanel('Panel', parent, 'FmOrgNewFolder', {});
      orgNewFolderRow.style.flowChildren = 'down';
      orgNewFolderRow.style.width = '100%';
      orgNewFolderRow.style.paddingLeft = '9px'; orgNewFolderRow.style.paddingRight = '7px';
      orgNewFolderRow.style.paddingTop = '7px'; orgNewFolderRow.style.paddingBottom = '8px';
      orgNewFolderRow.style.backgroundColor = 'rgba(240,179,35,0.07)';
      orgNewFolderRow.style.borderBottom = '1px solid rgba(255,255,255,0.07)';
      // Entrance: the block is already the right size, it just resolves into place. One short
      // ease-out, matching the header expand -- no slide, nothing to wait through.
      orgNewFolderRow.style.transitionProperty = 'opacity';
      orgNewFolderRow.style.transitionDuration = '0.12s';
      orgNewFolderRow.style.transitionTimingFunction = 'ease-out';
      orgNewFolderRow.visible = false;

      var caption = $.CreatePanel('Label', orgNewFolderRow, '', { text: 'NEW FOLDER' });
      caption.style.fontSize = '11px'; caption.style.fontWeight = 'medium';
      caption.style.letterSpacing = '0.5px';
      caption.style.color = '#8b949cff';
      caption.style.fontFamily = 'Stratum2, "Arial Unicode MS"';
      caption.style.marginBottom = '5px';
      caption.hittest = false;

      // 36px, not 32. A TextEntry lays its text out on the font's own line box and does not
      // shrink it to fit: at 14px Stratum2 a 32px box left no room under the baseline, so the
      // placeholder and anything typed had their descenders cut off by the field's bottom edge.
      // 36 is the height CS2's own search field in this page uses at a LARGER font, so it is the
      // proportion known to sit right rather than another guess.
      var FIELD_H = 36;
      var fieldRow = $.CreatePanel('Panel', orgNewFolderRow, '', {});
      fieldRow.style.flowChildren = 'right';
      fieldRow.style.width = '100%';
      fieldRow.style.height = FIELD_H + 'px';

      orgNewFolderInput = $.CreatePanel('TextEntry', fieldRow, 'FmOrgNewFolderInput', { placeholder: 'Name this folder', maxchars: '40' });
      orgNewFolderInput.style.width = 'fill-parent-flow(1.0)';
      orgNewFolderInput.style.height = FIELD_H + 'px';
      orgNewFolderInput.style.verticalAlign = 'center';
      orgNewFolderInput.style.backgroundColor = '#0d0f12ff';
      orgNewFolderInput.style.border = '1px solid rgba(240,179,35,0.75)';
      orgNewFolderInput.style.borderRadius = '3px';
      orgNewFolderInput.style.color = '#eef2f6ff';
      orgNewFolderInput.style.fontSize = '14px';
      orgNewFolderInput.style.fontFamily = 'Stratum2, "Arial Unicode MS"';
      orgNewFolderInput.style.paddingLeft = '8px'; orgNewFolderInput.style.paddingRight = '8px';

      function commit() {
        var text = '';
        try { text = ('' + (orgNewFolderInput.text || '')).trim(); } catch (e) {}
        if (text) { api.addCommand('org newfolder ' + text); orgSound('inventory_item_putdown'); }
        orgCloseNewFolder();
      }
      // Confirm is filled gold, cancel is a quiet ghost: the two actions are not equal weight,
      // and a pair of identical grey squares makes the user read icons to tell them apart.
      function mini(icon, rest, restTint, hover, hoverTint, onClick) {
        var b = $.CreatePanel('Button', fieldRow, '', {});
        b.style.width = FIELD_H + 'px'; b.style.height = FIELD_H + 'px';
        b.style.marginLeft = '5px'; b.style.verticalAlign = 'center';
        b.style.borderRadius = '3px';
        b.style.backgroundColor = rest;
        b.style.transitionProperty = 'background-color';
        b.style.transitionDuration = '0.12s';
        var im = $.CreatePanel('Image', b, '', { src: icon, texturewidth: '12', textureheight: '12' });
        im.style.width = '12px'; im.style.height = '12px';
        im.style.horizontalAlign = 'center'; im.style.verticalAlign = 'center';
        im.style.washColor = restTint; im.hittest = false;
        b.SetPanelEvent('onactivate', onClick);
        b.SetPanelEvent('onmouseover', function () { try { b.style.backgroundColor = hover; im.style.washColor = hoverTint; } catch (e) {} });
        b.SetPanelEvent('onmouseout', function () { try { b.style.backgroundColor = rest; im.style.washColor = restTint; } catch (e) {} });
        return b;
      }
      mini('s2r://panorama/images/icons/ui/check.vsvg',
        'rgba(240,179,35,0.85)', '#0d0f12', ORG_GOLD + 'ff', '#0d0f12', commit);
      mini('s2r://panorama/images/icons/ui/cancel.vsvg',
        'rgba(255,255,255,0.06)', '#8b949c', 'rgba(255,255,255,0.16)', '#eef2f6',
        function () { orgCloseNewFolder(); });

      // Enter is 'oninputsubmit' on a TextEntry, NOT 'onactivate' -- see orgBeginRename.
      orgNewFolderInput.SetPanelEvent('oninputsubmit', commit);
      orgNewFolderInput.SetPanelEvent('oncancel', function () { orgCloseNewFolder(); });
    }

    function orgOpenNewFolder() {
      if (!orgNewFolderRow) return;
      // Opacity 0 BEFORE visible, or the block paints at full opacity for a frame and then
      // blinks when the fade-in starts from 0.
      try { orgNewFolderRow.style.opacity = '0'; } catch (e) {}
      orgNewFolderRow.visible = true;
      try { orgNewFolderInput.text = ''; orgNewFolderInput.enabled = true; } catch (e) {}

      // Focus AFTER layout, not in the same frame the panel became visible.
      //
      // SetFocus on a panel Panorama has not laid out yet is dropped on the floor, which is why
      // the caret never appeared and you had to click the field before you could type. Two
      // attempts on purpose: the first lands on the very next frame in the common case, the
      // second covers a frame where the rail was still reflowing. Re-focusing an already-focused
      // TextEntry is harmless.
      $.Schedule(0.0, function () {
        try { orgNewFolderRow.style.opacity = '1'; } catch (e) {}
        try { orgNewFolderInput.SetFocus(); } catch (e) {}
      });
      $.Schedule(0.08, function () {
        try { if (orgNewFolderRow && orgNewFolderRow.visible) orgNewFolderInput.SetFocus(); } catch (e) {}
      });
    }
    function orgCloseNewFolder() {
      if (!orgNewFolderRow) return;
      try { orgNewFolderInput.text = ''; } catch (e) {}
      orgNewFolderRow.visible = false;
    }
)FMORG"
// ---- drag a demo tile into a folder ----
R"FMORG(
    function orgDeleteGhost() {
      if (orgDragGhost) { try { orgDragGhost.DeleteAsync(0); } catch (e) {} orgDragGhost = null; }
    }

    function orgClearDropHighlights() {
      for (var i = 0; i < orgHeaders.length; ++i) {
        var h = orgHeaders[i];
        if (!h || !h.__dropActive) continue;
        h.__dropActive = false;
        try {
          h.style.backgroundColor = 'rgba(255,255,255,0.04)';
          h.style.brightness = '1.0';
          if (h.__arrow) h.__arrow.style.washColor = '#8b949c';
        } catch (e) {}
      }
    }

    // Both folder headers and the No-folder header accept drops; dropping on No folder (id -1)
    // is how a demo gets taken back out of a folder.
    function orgWireDropTarget(head, groupId) {
      try {
        $.RegisterEventHandler('DragEnter', head, function () {
          head.__dropActive = true;
          try {
            head.style.backgroundColor = 'rgba(240,179,35,0.22)';
            head.style.brightness = '1.25';
            if (head.__arrow) head.__arrow.style.washColor = ORG_GOLD;
          } catch (e) {}
        });
        $.RegisterEventHandler('DragLeave', head, function () {
          head.__dropActive = false;
          try {
            head.style.backgroundColor = 'rgba(255,255,255,0.04)';
            head.style.brightness = '1.0';
            if (head.__arrow) head.__arrow.style.washColor = '#8b949c';
          } catch (e) {}
        });
        $.RegisterEventHandler('DragDrop', head, function (displayId, elDragImage) {
          var key = (elDragImage && elDragImage.Data && elDragImage.Data().fmKey) || orgDragKey;
          orgClearDropHighlights();
          if (!key) return;
          api.addCommand('org assign ' + groupId + ' ' + key);
          orgSound('inventory_item_putdown');
        });
      } catch (e) {}
    }

    // Makes a left-list tile pick-up-able. The ghost is a compact card (map icon + demo name)
    // rather than a clone of the tile: at cursor size a full tile is unreadable, and a small
    // card makes it obvious WHICH demo is in flight.
    // Folder membership is NOT taken here -- reconcileList owns orgTileGroup, and a copy of the
    // group id on the tile would be a second source of truth that goes stale on every reorder.
    function orgWireTile(tile, demo) {
      var key = orgKeyOf(demo);
      tile.__fmKey = key;
      try { tile.SetDraggable(true); } catch (e) {}

      try {
        $.RegisterEventHandler('DragStart', tile, function (panelId, drag) {
          orgDeleteGhost();
          orgDragKey = key;

          var ghost = $.CreatePanel('Panel', ctx, '', {});
          ghost.style.width = '190px'; ghost.style.height = '40px';
          ghost.style.flowChildren = 'right';
          ghost.style.paddingLeft = '9px'; ghost.style.paddingRight = '11px';
          ghost.style.backgroundColor = '#14171cee';
          ghost.style.border = '1px solid ' + ORG_GOLD;
          ghost.style.borderRadius = '3px';
          ghost.style.boxShadow = 'rgba(0,0,0,0.65) 0px 8px 20px 0px';
          ghost.style.opacity = '0.96';
          ghost.hittest = false;

          var gi = $.CreatePanel('Image', ghost, '', {});
          gi.style.width = '22px'; gi.style.height = '22px'; gi.style.verticalAlign = 'center';
          gi.style.marginRight = '9px';
          setMapIcon(gi, demo.map);

          var gl = $.CreatePanel('Label', ghost, '', { text: cropName(demoName(demo), 14, 6) });
          gl.style.verticalAlign = 'center';
          gl.style.color = '#eef2f6ff'; gl.style.fontSize = '13px';
          gl.style.fontFamily = 'Stratum2, "Arial Unicode MS"';
          gl.style.whiteSpace = 'nowrap'; gl.style.textOverflow = 'ellipsis';

          ghost.Data().fmKey = key;
          orgDragGhost = ghost;
          drag.displayPanel = ghost;
          drag.offsetX = 18;
          drag.offsetY = 20;
          // false, matching native predictions_group_stage.js: we delete the ghost ourselves on
          // DragEnd and never re-parent it into the drop target, so its position must survive
          // until then or the card visibly snaps to the origin mid-drop.
          drag.removePositionBeforeDrop = false;

          // The source tile visibly leaves the list while it is in flight. Opacity only --
          // 'saturation' is not a property CS2's own styles ever set, and a dim-out alone
          // reads the same without depending on an unverified one.
          try { tile.style.opacity = '0.32'; } catch (e) {}
          orgSound('inventory_item_pickup');
          try { UiToolkitAPI.HideTextTooltip(); } catch (e) {}
        });

        $.RegisterEventHandler('DragEnd', tile, function () {
          orgDeleteGhost();
          orgDragKey = null;
          orgClearDropHighlights();
          try { tile.style.opacity = '1'; } catch (e) {}
        });
      } catch (e) {}

      // Right-click is the keyboard-free path to the same thing the drag does, and the only
      // path when the target folder is scrolled off-screen.
      tile.SetPanelEvent('oncontextmenu', function () {
        var items = [];
        var groups = orgState.groups || [];
        var cur = orgGroupOf(demo);
        for (var i = 0; i < groups.length; ++i) {
          if (groups[i].id === cur) continue;
          items.push({
            label: 'Move to ' + groups[i].name,
            jsCallback: (function (gid) {
              return function () { api.addCommand('org assign ' + gid + ' ' + key); orgSound('inventory_item_putdown'); };
            })(groups[i].id)
          });
        }
        if (cur >= 0)
          items.push({ label: 'Remove from folder', jsCallback: function () { api.addCommand('org assign -1 ' + key); orgSound('inventory_item_notequipped'); } });
        if (!groups.length)
          items.push({ label: 'New folder\u2026', jsCallback: function () { orgOpenNewFolder(); } });
        // Renaming is a demo action, not a folder one, so it leads. It selects the demo first:
        // the field it opens is the TITLE of the detail pane on the right, which is only showing
        // this demo once it is selected.
        // tileKey(demo), NOT `key`. THE TWO KEY FORMATS ARE DIFFERENT STRINGS. This fragment
        // joins folder + '\' + fileName and lower-cases it (orgKeyOf, so it matches the C++
        // store), while the demo list indexes by folder + '|' + fileName in the original case
        // (tileKey). Handing the organiser's key to a lookup that compares against the list's
        // meant indexOfKey never matched, beginTitleRename returned on its very first line, and
        // "Rename demo" did nothing whatsoever -- including not selecting the demo.
        items.unshift({
          label: 'Rename demo\u2026',
          jsCallback: (function (dk) { return function () { beginTitleRename(dk); }; })(tileKey(demo))
        });
        var played = orgPlayedAt(demo);
        items.push({ label: 'Last played: ' + orgAgo(played), jsCallback: function () {} });
        orgMenu(tile, items);
      });

      // Opacity has to be declared transitionable on the tile itself or the expand fade in
      // orgApplyCollapse pops from 0 to 1 instead of fading, and the drag's dim-out snaps.
      try {
        tile.style.transitionProperty = 'opacity';
        tile.style.transitionDuration = '0.14s';
        tile.style.transitionTimingFunction = 'ease-out';
      } catch (e) {}
      orgPaintPlayedEdge(tile, demo);
    }

    // One visible gold rail marks the most recently played demo. The full history belongs to
    // Recently played sorting; repeating the rail on every old demo added noise without meaning.
    // Apply the same width to every tile (transparent otherwise) so content never shifts.
    function orgPaintPlayedEdge(tile, demo) {
      try {
        tile.style.borderLeft = orgIsLatestPlayed(demo)
          ? '3px solid rgba(240,179,35,0.9)'
          : '3px solid rgba(0,0,0,0)';
      } catch (e) {}
    }
)FMORG"
// ---- the rail toolbar (sort + add), sitting directly above the tiles ----
R"FMORG(
    // Built once into id-match-list-container, above FmMatchList. The container is forced to a
    // downward flow and the list below it to fill-parent-flow so the toolbar takes its 40px off
    // the top instead of overlapping the tiles.
    function buildListToolbar(listC) {
      if (listC.FindChildTraverse('FmListToolbar')) return;
      listC.style.flowChildren = 'down';
      // fill-parent-flow on the list below is only meaningful if this container has a definite
      // height; the native MatchList class does not guarantee one on a panel we created.
      listC.style.height = '100%';

      var bar = $.CreatePanel('Panel', listC, 'FmListToolbar', {});
      bar.style.flowChildren = 'right';
      bar.style.width = '100%';
      bar.style.height = '40px';
      bar.style.paddingLeft = '9px'; bar.style.paddingRight = '7px';
      bar.style.backgroundColor = 'rgba(0,0,0,0.22)';
      bar.style.borderBottom = '1px solid rgba(255,255,255,0.07)';

      // --- sort control ---
      //
      // A real Panorama DropDown, the same widget CS2's own inventory/crafting/leaderboard
      // sort controls use (`<DropDown class="content-navbar__tabs__dropdown" menuclass=
      // "DropDownMenu">`). It owns its popup and anchors it under the control.
      //
      // NOT a Button + ShowSimpleContextMenu: that API positions a menu like a RIGHT-CLICK menu
      // -- it opened up and to the right, on top of the button it came from, which is not how a
      // sort control behaves anywhere in this game. A context menu cannot be made to drop down;
      // the DropDown panel type is the widget for this.
      var sortIcon = $.CreatePanel('Image', bar, '', { src: 's2r://panorama/images/icons/ui/sort.vsvg', texturewidth: '12', textureheight: '12' });
      sortIcon.style.width = '12px'; sortIcon.style.height = '12px';
      sortIcon.style.verticalAlign = 'center'; sortIcon.style.marginRight = '7px';
      sortIcon.style.washColor = '#8b949c'; sortIcon.hittest = false;

      // Keeps the id the automation hooks and verifier already look for.
      orgSortDrop = $.CreatePanel('DropDown', bar, 'FmSortBtn', {
        class: 'content-navbar__tabs__dropdown',
        menuclass: 'DropDownMenu'
      });
      orgSortDrop.style.verticalAlign = 'center';
      orgSortDrop.style.height = '26px';
      orgSortDrop.style.minWidth = '150px';

      for (var s = 0; s < ORG_SORTS.length; ++s) {
        // Options are parented to the dropdown's container and handed over with AddOption,
        // exactly as mainmenu_inventory.js builds InvSortDropdown.
        var opt = $.CreatePanel('Label', bar, ORG_SORT_OPT + ORG_SORTS[s][0], { class: 'DropDownMenu' });
        opt.text = ORG_SORTS[s][1];
        try { orgSortDrop.AddOption(opt); } catch (e) {}
      }
      orgSyncSortSelection();

      orgSortDrop.SetPanelEvent('oninputsubmit', function () {
        if (orgSyncingSort) return;   // our own SetSelected, not a user pick
        var picked = null;
        try { picked = orgSortDrop.GetSelected(); } catch (e) {}
        var id = (picked && picked.id) ? ('' + picked.id) : '';
        if (id.indexOf(ORG_SORT_OPT) !== 0) return;
        var mode = id.substr(ORG_SORT_OPT.length);
        if (!mode || mode === orgState.sort) return;
        api.addCommand('org sort ' + mode);
        orgSound('sidemenu_select');
      });

      // --- add control: one button, three destinations ---
      var addBtn = $.CreatePanel('Button', bar, 'FmOrgAddBtn', {});
      addBtn.style.flowChildren = 'right';
      addBtn.style.width = '28px'; addBtn.style.height = '26px';
      addBtn.style.horizontalAlign = 'right'; addBtn.style.verticalAlign = 'center';
      addBtn.style.borderRadius = '3px';
      addBtn.style.backgroundColor = 'rgba(255,255,255,0.06)';
      addBtn.style.transitionProperty = 'background-color';
      addBtn.style.transitionDuration = '0.12s';

      var addIcon = $.CreatePanel('Image', addBtn, '', { src: 's2r://panorama/images/icons/ui/plus.vsvg', texturewidth: '12', textureheight: '12' });
      addIcon.style.width = '12px'; addIcon.style.height = '12px';
      addIcon.style.horizontalAlign = 'center'; addIcon.style.verticalAlign = 'center';
      addIcon.style.washColor = '#c7ced6'; addIcon.hittest = false;

      addBtn.SetPanelEvent('onactivate', function () {
        // Short and parallel: the rail's menu is narrow, and "Add demos from folder\u2026" was wide
        // enough to clip. Each label still names exactly what it makes.
        orgMenu(addBtn, [
          { label: 'New folder', jsCallback: function () { orgOpenNewFolder(); } },
          { label: 'Add demo file\u2026', jsCallback: function () { api.addCommand('adddemo'); } },
          { label: 'Add demo folder\u2026', jsCallback: function () { api.addCommand('addfolder'); } }
        ]);
      });
      addBtn.SetPanelEvent('onmouseover', function () {
        try { addBtn.style.backgroundColor = ORG_GOLD + 'ff'; addIcon.style.washColor = '#0d0f12'; } catch (e) {}
      });
      addBtn.SetPanelEvent('onmouseout', function () {
        try { addBtn.style.backgroundColor = 'rgba(255,255,255,0.06)'; addIcon.style.washColor = '#c7ced6'; } catch (e) {}
      });

      orgBuildNewFolderRow(listC);
    }

    // Reads the payload C++ pushed into the host's "org" attribute. Tolerant of a missing or
    // half-written value: a bad parse leaves the previous state rather than blanking the list.
    function orgParse() {
      var raw = host.GetAttributeString('org', '');
      if (!raw) return;
      var data = null;
      try { data = JSON.parse(raw); } catch (e) { return; }
      if (!data) return;
      orgState = {
        sort: data.sort || 'date',
        rootCollapsed: !!data.rootCollapsed,
        groups: data.groups || [],
        demos: data.demos || {}
      };
      orgSyncSortSelection();
    }

    // Automation hook: open the new-folder block, exactly as the "+" menu item does. Numeric
    // API surface only (see orgToggleHeader for why), so this takes no arguments.
    function orgOpenNewFolderUi() {
      orgOpenNewFolder();
      // Reports open state and layout only. Panorama exposes no key-focus getter to JS (the
      // HasKeyFocus() this used to call does not exist and just threw), so "did the caret land"
      // is confirmed from a screenshot, not from here -- better than printing a made-up number.
      $.Schedule(0.2, function () {
        var h = -1;
        try { h = Math.round(orgNewFolderRow.actuallayoutheight); } catch (e) {}
        $.Msg('[NEWFOLDER] open=' + ((orgNewFolderRow && orgNewFolderRow.visible) ? 1 : 0) +
          ' blockH=' + h + '\n');
      });
      return true;
    }

    // Automation hook: toggle the Nth folder header, by INDEX not by id or panel name.
    //
    // Numeric on purpose. The netcon console rewrites quoted string literals inside an
    // ui_eval payload (it re-tokenises and pads them, so 'Activated' arrives as ' Activated '),
    // which makes any string-argument helper unusable from automation. Every hook reachable
    // over netcon therefore takes numbers only -- same reason $.Filmmaker.gotoTab accepts one.
    function orgToggleHeader(index) {
      var head = orgHeaders[index | 0];
      if (!head) { $.Msg('[ORGTOGGLE] no header at ' + (index | 0) + '\n'); return false; }
      var id = head.__gid;
      var next = !orgIsCollapsed(id);
      orgSetCollapsedLocal(id, next);
      orgApplyCollapse(true);
      api.addCommand('org collapse ' + id + ' ' + (next ? 1 : 0));
      $.Msg('[ORGTOGGLE] header=' + (index | 0) + ' gid=' + id + ' collapsed=' + (next ? 1 : 0) + '\n');
      return true;
    }

    // Automation hook: one parseable line describing the organiser's live UI state.
    //
    // Carries real LAYOUT numbers, not just "does the panel exist". The rail splits its height
    // between a fixed toolbar and a fill-parent-flow list, and a bad split (list collapsed to
    // 0px, or toolbar overlapping the first tile) leaves every existence check passing while
    // the page is visibly broken -- and a screenshot cannot settle it either, because GDI
    // capture of CS2's D3D surface comes back black.
    function orgReportLine() {
      var page = watchPage();
      var bar = page && page.FindChildTraverse('FmListToolbar');
      var sortB = page && page.FindChildTraverse('FmSortBtn');
      var addB = page && page.FindChildTraverse('FmOrgAddBtn');
      var heads = 0, hidden = 0;
      for (var i = 0; i < orgHeaders.length; ++i) if (orgHeaders[i]) ++heads;
      for (var t = 0; t < tilePanels.length; ++t) if (tilePanels[t] && !tilePanels[t].visible) ++hidden;

      function h(p) { try { return Math.round(p.actuallayoutheight); } catch (e) { return -1; } }
      function y(p) { try { return Math.round(p.actualyoffset); } catch (e) { return -1; } }
      var firstTile = null;
      for (var f = 0; f < tilePanels.length && !firstTile; ++f) if (tilePanels[f] && tilePanels[f].visible) firstTile = tilePanels[f];

      // The sort DropDown's own view of itself. AddOption is wrapped in try/catch, so an option
      // list that failed to populate would otherwise look identical to a healthy control here:
      // sortOpts is how "the dropdown exists but is empty" gets caught.
      var sortOpts = -1, sortSel = 'none';
      if (orgSortDrop) {
        try { sortOpts = orgSortDrop.AccessDropDownMenu().GetChildCount(); } catch (e) { sortOpts = -2; }
        try {
          var cur = orgSortDrop.GetSelected();
          if (cur && cur.id) sortSel = ('' + cur.id).replace(ORG_SORT_OPT, '');
        } catch (e) { sortSel = 'err'; }
      }

      return '[ORG] toolbar=' + (bar ? 1 : 0) + ' sortBtn=' + (sortB ? 1 : 0) + ' addBtn=' + (addB ? 1 : 0) +
        ' sortOpts=' + sortOpts + ' sortSel=' + sortSel +
        ' sort=' + (orgState.sort || '') + ' groups=' + ((orgState.groups || []).length) +
        ' headers=' + heads + ' hiddenTiles=' + hidden +
        ' barH=' + (bar ? h(bar) : -1) + ' barY=' + (bar ? y(bar) : -1) +
        ' listH=' + (listPanel ? h(listPanel) : -1) + ' listY=' + (listPanel ? y(listPanel) : -1) +
        ' headH=' + (orgHeaders[0] ? h(orgHeaders[0]) : -1) +
        ' tileH=' + (firstTile ? h(firstTile) : -1) +
        ' newRow=' + ((orgNewFolderRow && orgNewFolderRow.visible) ? 1 : 0) + '\n';
    }
)FMORG"
