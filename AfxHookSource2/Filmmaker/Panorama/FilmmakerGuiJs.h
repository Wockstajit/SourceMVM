#pragma once

// Panorama JS that EDITS CS2's native "Watch matches and tournaments" tab in place:
//   * swaps the main-menu Watch nav button icon to the filmmaker (film) icon
//   * hides the "Majors"/Tournaments tab from the Watch navbar
//   * adds "Add demo folder" + "Refresh" icon buttons to the navbar (top-right)
//   * takes over the Downloaded tab body with OUR parsed demo data, rendered
//     ONE-TO-ONE with CS2's native Watch -> Downloaded page (left MatchList of
//     map tiles, right scoreboard with CT/T scoreboxes + per-round timeline).
//
// HOW THE NATIVE LOOK IS ACHIEVED (the important part):
//   The Watch page only loads mainmenu_watch.vcss. Panels we $.CreatePanel with
//   matchtile/matchinfo/scoreboard classes therefore get NO styling at page
//   level (that was the cause of the overlapping/unstyled Downloaded layout).
//   The native engine solves this by loading each piece from its own layout
//   FILE -- every layout carries its <styles><include> which cascade to that
//   panel's subtree. We do EXACTLY what native matchlist.js/watchtile.js do:
//     - each left tile is built by panel.BLoadLayout('.../matchtiles/player.xml')
//       (brings matchtile.vcss + the MatchTile/MatchMap/MatchInfo structure)
//     - the right detail is built by panel.BLoadLayout('.../matchinfo.xml')
//       (brings matchinfo_scoreboard.vcss + matchinfo.vcss + the full scoreboard
//        DOM with native ids, and registers snippet_mi-round-summary-bar)
//   We then populate those native ids the same way the native scripts do
//   (SetDialogVariable + FindChildTraverse).
//
// TAB ISOLATION (the other cause that was broken):
//   We do NOT replace the navbar buttons' native onactivate handlers. The
//   Downloaded RadioButton keeps its native handler
//   (mainmenu_watch.NavigateToTab('JsDownloaded',...)), so native bookkeeping
//   (_m_activeTab) stays correct and clicking "Your Matches" properly re-hides
//   JsDownloaded -- and our body, which lives INSIDE JsDownloaded, hides with it.
//   We only ADD an 'Activated' listener on the Downloaded button (via
//   $.RegisterEventHandler, which runs alongside the native handler) that builds
//   our content and hides the native downloaded list. Crucially, render()/refresh
//   NEVER force JsDownloaded visible -- visibility is owned entirely by native
//   tab state. "Your Matches" is left completely untouched (native empty state).
//
// A hidden host panel (#FilmmakerMenuTab) carries the C++<->JS bridge attributes
// (unchanged contract with FilmmakerMenu.cpp):
//   C++ -> JS : sets host "demos" attribute (JSON), runs $.Filmmaker.render().
//   JS  -> C++: appends to host "cmd": "refresh", "addfolder", "watch <index>",
//               "openfolder <index>", "prepare <index>" (selected legacy demo),
//               "rounds <index>", "renamedemo <index> <new name>" (name last, may
//               contain spaces -- it renames the .dem and its sidecars on disk).

namespace Filmmaker {

inline const char* kFilmmakerGuiJs = R"FMJS(
(function () {
  try {
    ['FilmmakerMenuTab', 'FilmmakerOpenMenuButton', 'FmDownloadedBody'].forEach(function (id) {
      var p = $('#' + id); if (p) p.DeleteAsync(0);
    });

    function topRootOf(p) { var r = p; while (r && r.GetParent && r.GetParent()) r = r.GetParent(); return r; }
    var ctx = $.GetContextPanel();
    var root3 = topRootOf(ctx);
    var menuContent = root3.FindChildTraverse('JsMainMenuContent');
    var pageParent = menuContent || ctx;

    var host = $.CreatePanel('Panel', pageParent, 'FilmmakerMenuTab', {});
    host.visible = false; host.style.width = '0px'; host.style.height = '0px';

    var FILM = 's2r://panorama/images/icons/ui/film.vsvg';
    var TEAMS = ['CT', 'TERRORIST'];
    var PLAYERSTATS = ['kills', 'assists', 'deaths', 'mvps', 'score'];

    function statVal(p, name) {
      if (name === 'kills') return p.k;
      if (name === 'assists') return p.a;
      if (name === 'deaths') return p.d;
      if (name === 'mvps') return p.mvps || 0;
      return p.score;
    }
    function mapDisplay(map) { if (!map) return 'Demo'; var t = '#SFUI_Map_' + map; var l = $.Localize(t); return (l === t) ? map : l; }
    // Friendly demo name for the detail title: file basename without the .dem extension.
    function demoName(demo) {
      var f = (demo && demo.fileName) || '';
      f = f.replace(/^.*[\\/]/, '').replace(/\.dem$/i, '');
      return f || mapDisplay(demo && demo.map);
    }
    // Middle-ellipsis crop for long demo/match names so search rows stay one line.
    // Keeps the head AND tail (the tail digits are what distinguish match730_ demos)
    // e.g. 'match730_003827583940474962026_0709708846_125' -> 'match730_00382…846_125'.
    function cropName(s, head, tail) {
      s = '' + (s || ''); head = head || 16; tail = tail || 7;
      if (s.length <= head + tail + 1) return s;
      return s.substr(0, head) + '…' + s.substr(s.length - tail);
    }
    function setMapIcon(img, map) {
      if (!img) return;
      try { $.RegisterEventHandler('ImageFailedLoad', img, function () { try { img.SetImage('s2r://panorama/images/map_icons/map_icon_none_png.vtex'); } catch (e) {} }); } catch (e) {}
      try { img.SetImage('s2r://panorama/images/map_icons/map_icon_' + (map || '') + '.svg'); } catch (e) {}
    }
    function steamId64(accountId) {
      try { if (typeof BigInt !== 'undefined' && accountId) return (BigInt('76561197960265728') + BigInt(accountId)).toString(); } catch (e) {}
      return '';
    }
    var myAccount = 0;
    try {
      if (typeof MyPersonaAPI !== 'undefined' && MyPersonaAPI.GetXuid && typeof BigInt !== 'undefined') {
        var x = MyPersonaAPI.GetXuid();
        if (x) myAccount = Number(BigInt(x) - BigInt('76561197960265728'));
      }
    } catch (e) {}

    function outcomeOf(demo) {
      // 1 win, 0 tie, -1 loss, or null if my player isn't in this demo.
      // Coloring is tied to the CURRENTLY logged-in Steam account: a demo you
      // aren't a player in (e.g. recorded on a different account) stays grey --
      // that is expected, not a bug.
      if (!demo.hasScoreboard || !myAccount || !demo.players) return null;
      var mine = null;
      for (var i = 0; i < demo.players.length; ++i) if (demo.players[i].accountId === myAccount) { mine = demo.players[i]; break; }
      if (!mine) return null;
      var myScore = (mine.team === 1) ? demo.teamScore1 : demo.teamScore0;
      var oppScore = (mine.team === 1) ? demo.teamScore0 : demo.teamScore1;
      return myScore > oppScore ? 1 : (myScore < oppScore ? -1 : 0);
    }

    // ---- state ----
    var demos = [];
    var sel = -1, selPlayer = -1;
    var selKey = null;              // stable key of the selected demo (survives refreshes)
    var tilePanels = [], playerRowPanels = [];
    var renderedKeys = [], tileSigs = [];  // for diffing the left list across refreshes
    var detailSig = null, detailRendered = false;
    var body = null, listPanel = null, info = null, emptyMsg = null, demoTitle = null;
    // Inline rename of the selected demo, on its own title (see buildTitleEditor).
    var demoTitleEdit = null, titleRenaming = false, titleRenamePending = null;
    var titleRenameKey = null;   // which demo the open editor belongs to (see renderDetail)
    var scoreboardPanel = null, labelsRow = null, ctTable = null, tTable = null;
    var playerStats = null, roundContainer = null, tickLabels = null, watchBtn = null, folderBtn = null, deleteBtn = null;
    var repairPanel = null, repairLbl = null, repairDetailLbl = null, repairTrack = null, repairFill = null;
    var repairStatus = null, watchBusy = false, repairJobBusy = false, anyRepairJob = false, watchUnavailable = false;
    var repairProgressToken = 0, repairProgressRunning = false;
    var autoPrepared = {}, autoPreparePendingKey = null;
    var statHeadersBuilt = false, detailBuilt = false;

    // Stable per-demo identity (folder+file). Used as a key so refreshes that
    // don't change the demo SET never rebuild rows / move selection / reset scroll.
    function tileKey(demo) {
      if (demo && (demo.folder != null || demo.fileName != null)) return (demo.folder || '') + '|' + (demo.fileName || '');
      return 'idx' + (demo ? demo.index : '?');
    }
    // Signature of the data a left tile actually shows; tiles only update in place
    // when this changes.
    function tileSig(demo) {
      return [demo.hasScoreboard ? 1 : 0, demo.teamScore0, demo.teamScore1, demo.dateText || '', demo.map || '', demo.compatibility || '', demo.repairCache || '', outcomeOf(demo)].join('');
    }
    // Signature of the right-detail data; the detail only re-renders when it changes.
    function detailSignature(demo) {
      var s = [demo.map || '', demo.duration || 0, demo.dateText || '', demo.compatibility || '', demo.repairCache || '', demo.networkProtocol || 0, demo.buildNum || 0, demo.hasScoreboard ? 1 : 0, demo.teamScore0, demo.teamScore1, demo.hasRounds ? 1 : 0];
      var pl = demo.players || [];
      for (var i = 0; i < pl.length; ++i) { var p = pl[i]; s.push((p.name || '') + ':' + p.k + ',' + p.a + ',' + p.d + ',' + p.score + ',' + (p.mvps || 0) + ',' + p.team + ',' + ((p.perRound && p.perRound.length) || 0)); }
      return s.join('');
    }
    function keysEqual(a, b) { if (a.length !== b.length) return false; for (var i = 0; i < a.length; ++i) if (a[i] !== b[i]) return false; return true; }
    function indexOfKey(k) { for (var i = 0; i < demos.length; ++i) if (tileKey(demos[i]) === k) return i; return -1; }
)FMJS"
// ---- native Watch page hooks (icon, hide Majors, navbar buttons, tab wiring) ----
R"FMJS(
    function watchPage() { return root3.FindChildTraverse('JsWatchContent'); }

    function applyWatchIcon() {
      var btn = root3.FindChildTraverse('MainMenuNavBarWatch');
      if (!btn) return;
      var iconImg = null;
      for (var i = 0; i < btn.GetChildCount(); ++i) { try { btn.GetChild(i).SetImage(FILM); iconImg = btn.GetChild(i); break; } catch (e) {} }
      // One-time: brand this nav button as "Movie Making" and give it the matchmaking
      // GO-button treatment recolored YELLOW -- the same animated line-map video the
      // green "GO" button uses, tinted gold via wash-color, behind the film icon,
      // with a gold glow that blooms brighter on hover. The film icon is kept as-is.
      // SetPanelEvent REPLACES handlers, so the tooltip + glow share one callback.
      if (!btn.__fmMovie) {
        btn.__fmMovie = true;
        // Gold animated background BEHIND the icon. The button is flow-children:none
        // + overflow:noclip (32x32), so this stacks cleanly and the glow is not
        // clipped. linemap.webm is the same native asset the GO button uses; if it
        // ever fails to load, the solid gold fill + border + glow still read as a
        // highlighted, lit-up button.
        var bg = $.CreatePanel('Panel', btn, 'FmMovieBg', {});
        bg.style.width = '100%'; bg.style.height = '100%';
        bg.style.zIndex = '0'; bg.style.borderRadius = '3px';
        bg.style.backgroundColor = 'rgba(49,40,9,0.65)';
        bg.style.border = '1px solid rgba(240,179,35,0.45)';
        bg.style.washColor = '#f0b323';            // tints the moving lines gold
        bg.style.brightness = '1.5';
        bg.style.backgroundImage = 'url("file://{resources}/videos/linemap.webm")';
        bg.style.backgroundRepeat = 'repeat';
        bg.style.backgroundPosition = '20% 60%';
        bg.style.backgroundSize = 'auto auto';
        bg.style.backgroundImgOpacity = '0.55';
        bg.style.boxShadow = 'rgba(240,179,35,0.45) 0px 0px 10px 2px';
        bg.style.transitionProperty = 'brightness, box-shadow';
        bg.style.transitionDuration = '0.2s';
        bg.style.transitionTimingFunction = 'ease-in-out';
        bg.hittest = false;
        if (iconImg) { iconImg.style.zIndex = '2'; }   // keep the film icon on top
        btn.SetPanelEvent('onmouseover', function () {
          try { UiToolkitAPI.ShowTextTooltipOnPanel(btn, 'Movie Making'); } catch (e) {}
          try { bg.style.brightness = '2.2'; bg.style.boxShadow = 'rgba(240,179,35,0.8) 0px 0px 24px 7px'; } catch (e) {}
        });
        btn.SetPanelEvent('onmouseout', function () {
          try { UiToolkitAPI.HideTextTooltip(); } catch (e) {}
          try { bg.style.brightness = '1.5'; bg.style.boxShadow = 'rgba(240,179,35,0.45) 0px 0px 10px 2px'; } catch (e) {}
        });
      }
    }

    function removeMajors(page) {
      var t = page.FindChildTraverse('WatchNavBarButtonTournaments');
      if (t) { t.visible = false; t.AddClass('hide'); }
      var je = page.FindChildTraverse('JsEvents'); if (je) je.visible = false;
    }

    function navButtonsHost(page) {
      var nav = page.FindChildTraverse('watch-navbar');
      if (!nav || nav.GetChildCount() < 1) return null;
      return nav.GetChild(nav.GetChildCount() - 1); // right-aligned container
    }

    function addNavButtons(page) {
      // MatchmakingReconnectPanel is a top-right main-menu overlay (~404px wide,
      // y 0..107, visible+hittest even when it shows nothing here). It paints
      // ABOVE the watch navbar and swallows pointer events over the navbar's top
      // ~36px (y 71..107) -- exactly where our Search/Refresh/Add folder buttons
      // sit. That made only the BOTTOM half of each button hover. Matchmaking
      // reconnect is meaningless in this movie-making fork, so drop its hittest
      // (leave it visible) to free the navbar's top strip. Without this the
      // earlier full-height/centered button fix still can't beat the overlay.
      var reconnect = root3.FindChildTraverse('MatchmakingReconnectPanel');
      if (reconnect) reconnect.hittest = false;

      // Hide native right-side controls (the lone refresh IconButton) if present.
      var right = navButtonsHost(page);
      if (!right) return;
      for (var i = 0; i < right.GetChildCount(); ++i) {
        var c = right.GetChild(i);
        if (('' + c.id).indexOf('Fm') !== 0) c.visible = false;
      }
      // Button row is the FULL navbar-row height (52px) with its children centered,
      // so the whole row is one continuous hover zone. The buttons themselves are
      // compact 32px pills centered inside it (set in mkNavBtn). The old top-strip
      // dead zone that forced full-height buttons is now killed upstream by dropping
      // MatchmakingReconnectPanel's hittest, so the pills hover edge-to-edge.
      // Explicit pixel height (not 100%) because the host sizes to fit its children
      // -- a 100% child would collapse it to zero (the old "buttons missing" bug).
      var bar = page.FindChildTraverse('FmNavBtns');
      if (!bar) bar = $.CreatePanel('Panel', right, 'FmNavBtns', {});
      bar.style.flowChildren = 'right';
      bar.style.horizontalAlign = 'right'; bar.style.verticalAlign = 'center';
      bar.style.height = '52px'; bar.hittest = true;

      function mkNavBtn(id, icon, text, onClick) {
        if (page.FindChildTraverse(id)) return;
        var b = $.CreatePanel('Button', bar, id, {});
        b.style.flowChildren = 'right';
        b.style.width = 'fit-children';   // size to icon+label; never stretch full-width (that overlap let only the last button hover)
        b.style.height = '32px';          // pill height; sits centered in the 52px hover row
        b.style.paddingLeft = '12px'; b.style.paddingRight = '12px';
        b.style.marginLeft = '6px';
        b.style.borderRadius = '4px';
        b.style.verticalAlign = 'center';
        b.hittest = true;
        // Icon + label are vertically centered as a unit; align the label baseline to
        // the icon centre so text never rides high above the glyph.
        var im = $.CreatePanel('Image', b, '', { src: icon });
        im.style.width = '15px'; im.style.height = '15px'; im.style.marginRight = '8px';
        im.style.verticalAlign = 'center'; im.style.washColor = '#c7ced6';
        im.hittest = false;
        var label = $.CreatePanel('Label', b, '', { text: text });
        label.style.verticalAlign = 'center'; label.style.fontSize = '14px';
        label.style.fontWeight = 'medium'; label.style.letterSpacing = '0.3px';
        label.style.color = '#c7ced6ff';
        label.hittest = false;
        b.SetPanelEvent('onactivate', onClick);
        // OBVIOUS hover feedback: solid gold fill + dark text/icon. A faint white
        // overlay was invisible against the blue navbar, so flip the foreground
        // colors too -- there's no mistaking when the hover is live.
        b.SetPanelEvent('onmouseover', function () {
          try {
            b.style.backgroundColor = '#f0b323ff';
            label.style.color = '#0d0f12ff';
            im.style.washColor = '#0d0f12';
          } catch (e) {}
        });
        b.SetPanelEvent('onmouseout', function () {
          try {
            b.style.backgroundColor = 'rgba(0,0,0,0)';
            label.style.color = '#c7ced6ff';
            im.style.washColor = '#c7ced6';
          } catch (e) {}
        });
      }
      // Order (left->right): Search, Refresh, Clear all. Adding demos moved to the list rail's
      // "+" (FmOrgAddBtn), next to the sort control -- it belongs with the things that change
      // what the LIST contains, not with the page-wide actions.
      mkNavBtn('FmSearchBtn', 's2r://panorama/images/icons/ui/search.vsvg', 'Search', function () { toggleSearch(page); });
      mkNavBtn('FmRefreshBtn', 's2r://panorama/images/icons/ui/refresh.vsvg', 'Refresh', function () { api.addCommand('refresh'); });
      mkNavBtn('FmClearAllBtn', 's2r://panorama/images/icons/ui/trash.vsvg', 'Clear all', function () {
        try {
          UiToolkitAPI.ShowGenericPopupOkCancel(
            'Clear all demos?',
            'Remove every demo from this list?\nDemo files stay on disk.',
            '',
            function () {
              api.addCommand('clearall');
              selKey = null; sel = -1; detailRendered = false;
            },
            function () {}
          );
        } catch (e) {
          $.Msg('[filmmaker] confirmation popup unavailable; nothing was removed.');
        }
      });
    }

    // The Downloaded navbar RadioButton. Order in the center container is
    // [Your Matches, Downloaded, Tournaments].
    function downloadedButton(page) {
      var nav = page.FindChildTraverse('watch-navbar'); if (!nav) return null;
      var center = nav.GetChild(0); if (!center || center.GetChildCount() < 2) return null;
      return center.GetChild(1);
    }

    // ADD (not replace) a listener so the native onactivate -> NavigateToTab still
    // runs and keeps tab bookkeeping correct. Ours just injects our content.
    function hookDownloadedButton(page) {
      var dlBtn = downloadedButton(page);
      if (!dlBtn || dlBtn.__fmHooked) return;
      dlBtn.__fmHooked = true;
      try {
        $.RegisterEventHandler('Activated', dlBtn, function () { $.Schedule(0.0, function () { takeoverDownloaded(page); }); });
      } catch (e) {
        // Fallback: chain through the native navigation ourselves.
        dlBtn.SetPanelEvent('onactivate', function () {
          $.DispatchEvent('NavigateToTab', 'JsDownloaded', 'downloaded menu button');
          $.Schedule(0.0, function () { takeoverDownloaded(page); });
        });
      }
    }

    // Build our content into JsDownloaded and hide the native downloaded list,
    // WITHOUT changing JsDownloaded's own visibility (native tab state owns that).
    function ensureBody() {
      var page = watchPage(); if (!page) return false;
      var dl = page.FindChildTraverse('JsDownloaded'); if (!dl) return false;
      if (!body || !body.IsValid() || !dl.FindChildTraverse('FmDownloadedBody')) buildBody(dl);
      return !!body;
    }

    // Hide native MatchListAndInfo siblings so only our body shows in JsDownloaded.
    function hideNativeSiblings(dl) {
      for (var i = 0; i < dl.GetChildCount(); ++i) {
        var c = dl.GetChild(i);
        if (c.id !== 'FmDownloadedBody') c.visible = false;
      }
    }

    // Called when Downloaded becomes the active tab (native already toggled
    // JsDownloaded visible). Refresh native-sibling hiding + our content.
    function takeoverDownloaded(page) {
      page = page || watchPage(); if (!page) return false;
      var dl = page.FindChildTraverse('JsDownloaded'); if (!dl) return false;
      if (!ensureBody()) return false;
      hideNativeSiblings(dl);
      if (body) { body.visible = true; body.SetReadyForDisplay(true); }
      renderInto();
      return true;
    }

    function applyWatchEdits() {
      applyWatchIcon();
      var page = watchPage();
      if (!page) return false;
      removeMajors(page);
      addNavButtons(page);
      hookDownloadedButton(page);
      ensureBody();   // build/keep content ready; do NOT force-show the tab

      // Re-assert display readiness whenever Downloaded is the tab native state already has
      // showing. Panorama can leave a panel laid out, correctly sized and visible=true while
      // still not painting -- SetReadyForDisplay(false) -- and only takeoverDownloaded ever set
      // it back. Any path that reached the page WITHOUT a real tab activation therefore left
      // the rail measurably perfect (bodyExists=1, tiles=28, listH=1032) and completely blank
      // on screen; navigating away and back was the only cure. This makes it self-healing.
      //
      // Deliberately gated on the native class, never forced: visibility stays owned by CS2's
      // tab state, exactly as the TAB ISOLATION note at the top of this file requires.
      var dlNow = page.FindChildTraverse('JsDownloaded');
      if (body && dlNow && !dlNow.BHasClass('WatchMenu--Hide')) {
        try { body.visible = true; body.SetReadyForDisplay(true); } catch (e) {}
      }

      renderInto();
      return true;
    }
)FMJS"
// ---- body skeleton: list container ($.CreatePanel) + matchinfo.xml detail ----
R"FMJS(
    function mk(parent, type, id, cls, attrs) {
      var o = attrs || {}; if (cls) o.class = cls;
      return $.CreatePanel(type, parent, id || '', o);
    }

    function buildBody(dl) {
      hideNativeSiblings(dl);
      body = $.CreatePanel('Panel', dl, 'FmDownloadedBody', {});
      body.style.width = '100%'; body.style.height = '100%';

      // Outer flow matches the native MatchListAndInfo snippet shell.
      var outer = mk(body, 'Panel', '', 'no-flow full-width full-height');
      var row = mk(outer, 'Panel', '', 'left-right-flow full-width full-height');

      // Left match list. These classes live in mainmenu_watch.vcss (page scope).
      var listC = mk(row, 'Panel', 'id-match-list-container', 'MatchList subsection-content__background-color');
      // Organiser rail toolbar (sort + add + inline new-folder), directly above the tiles.
      // It flows first, so the scrolling tile list below must be told to take the remainder --
      // a 100%-height list under a 40px toolbar would overflow the container by 40px.
      buildListToolbar(listC);
      listPanel = mk(listC, 'Panel', 'FmMatchList', 'MatchList MatchList-scroll MatchList--Filled');
      listPanel.style.height = 'fill-parent-flow(1.0)';

      // Right detail host. matchinfo.xml is loaded into FmMatchInfo (a child) so
      // FmInfo keeps its own background classes and can also hold the empty msg.
      info = mk(row, 'Panel', 'FmInfo', 'subsection-content__background-color full-width full-height no-flow');
      var infoOuter = info; // FmInfo stays put; `info` is reassigned to the matchinfo host below.

      var detailHost = mk(info, 'Panel', 'FmMatchInfo', '');
      detailHost.style.width = '100%'; detailHost.style.height = '100%';
      buildDetailLayout(detailHost);

      emptyMsg = mk(info, 'Panel', 'FmEmptyMsg', 'left-right-flow horizontal-center vertical-center');
      var ei = mk(emptyMsg, 'Image', '', 'info-icon', { texturewidth: '32', textureheight: '32' });
      try { ei.SetImage('s2r://panorama/images/icons/ui/info.vsvg'); } catch (e) {}
      mk(emptyMsg, 'Label', '', 'Info-Message', { text: 'No demos found. Use the + button above the list to add a demo folder.' });
      emptyMsg.visible = false;

      // Demo NAME title, shown in the empty banner area above the map/duration/date
      // meta (the red-square spot in the reference). Created LAST on the persistent
      // FmInfo host so it overlays on top of the matchinfo content and survives
      // matchinfo.xml (re)loads into FmMatchInfo.
      demoTitle = mk(infoOuter, 'Label', 'FmDemoTitle', '');
      demoTitle.style.horizontalAlign = 'center'; demoTitle.style.verticalAlign = 'top';
      demoTitle.style.marginTop = '18px'; demoTitle.style.width = '80%';
      demoTitle.style.fontSize = '22px'; demoTitle.style.fontWeight = 'bold';
      demoTitle.style.color = '#ffffffff'; demoTitle.style.textAlign = 'center';
      demoTitle.style.textOverflow = 'ellipsis'; demoTitle.style.fontFamily = 'Stratum2, "Arial Unicode MS"';
      demoTitle.style.textShadow = '0px 2px 4px 1.0 #000000cc';
      demoTitle.visible = false;

      // THE TITLE IS THE RENAME FIELD.
      //
      // The name shown here IS the file name on disk, so this is the one place in the page where
      // editing text in place and renaming the file are the same action -- no dialog, and nothing
      // to explain. Click it and type; Enter renames, Esc puts it back. The right-click menu's
      // "Rename demo" lands here too rather than opening something of its own.
      //
      // A separate TextEntry rather than an editable Label: Panorama has no editable Label, and
      // swapping the panel type in place would lose the ellipsis/shadow styling the title needs
      // when it is NOT being edited. The two share geometry and only one is ever visible.
      demoTitle.hittest = true;
      demoTitle.SetPanelEvent('onactivate', function () { beginTitleRename(null); });
      demoTitle.SetPanelEvent('onmouseover', function () {
        try {
          if (titleRenaming) return;
          demoTitle.style.color = '#ffd766ff';
          UiToolkitAPI.ShowTextTooltipOnPanel(demoTitle, 'Click to rename this demo file');
        } catch (e) {}
      });
      demoTitle.SetPanelEvent('onmouseout', function () {
        try { demoTitle.style.color = '#ffffffff'; UiToolkitAPI.HideTextTooltip(); } catch (e) {}
      });

      demoTitleEdit = mk(infoOuter, 'TextEntry', 'FmDemoTitleEdit', '', { maxchars: '96' });
      demoTitleEdit.style.horizontalAlign = 'center'; demoTitleEdit.style.verticalAlign = 'top';
      demoTitleEdit.style.marginTop = '14px'; demoTitleEdit.style.width = '80%';
      demoTitleEdit.style.height = '38px';
      demoTitleEdit.style.fontSize = '22px'; demoTitleEdit.style.fontWeight = 'bold';
      demoTitleEdit.style.color = '#ffffffff'; demoTitleEdit.style.textAlign = 'center';
      demoTitleEdit.style.fontFamily = 'Stratum2, "Arial Unicode MS"';
      demoTitleEdit.style.backgroundColor = '#0d0f12f2';
      demoTitleEdit.style.border = '1px solid rgba(240,179,35,0.85)';
      demoTitleEdit.style.borderRadius = '4px';
      demoTitleEdit.style.paddingLeft = '10px'; demoTitleEdit.style.paddingRight = '10px';
      demoTitleEdit.visible = false;
      // Enter is 'oninputsubmit' on a TextEntry, NOT 'onactivate' (the same trap the organiser's
      // folder rename documents -- wiring commit to onactivate makes Enter silently do nothing).
      demoTitleEdit.SetPanelEvent('oninputsubmit', function () { endTitleRename(true); });
      demoTitleEdit.SetPanelEvent('oncancel', function () { endTitleRename(false); });
      // Best-effort: 'onblur' is not something CS2's own scripts rely on, so it is a bonus path
      // and not the mechanism. Clicking away COMMITS rather than discards, because the text is
      // already what the user typed and throwing it out is the surprising outcome.
      demoTitleEdit.SetPanelEvent('onblur', function () { endTitleRename(true); });

      if (!listPanel) $.Msg('[filmmaker] body build incomplete (FmMatchList missing).');
    }

    // Load CS2's real matchinfo.xml so its <styles> cascade in and the native
    // scoreboard DOM (with its ids + the round-summary-bar snippet) exists. Then
    // wire up references to the native ids and trim the buttons we do not use.
    function buildDetailLayout(detailHost) {
      detailBuilt = false; statHeadersBuilt = false;
      try { detailHost.BLoadLayout('file://{resources}/layout/matchinfo.xml', false, false); } catch (e) {}
      info = detailHost;
      // Detect success by presence of the native scoreboard, not the (unreliable)
      // BLoadLayout return value.
      var ok = !!detailHost.FindChildTraverse('Scoreboard');
      if (ok) {
        detailHost.RemoveClass('mi-sb--hidden'); // matchinfo.xml hides the board until data is ready
        scoreboardPanel = detailHost.FindChildTraverse('Scoreboard');
        labelsRow = detailHost.FindChildTraverse('players-table__labels-row');
        ctTable = detailHost.FindChildTraverse('players-table-CT');
        tTable = detailHost.FindChildTraverse('players-table-TERRORIST');
        playerStats = detailHost.FindChildTraverse('id-mi-player-stats');
        roundContainer = detailHost.FindChildTraverse('id-mi-round-stats__container');
        tickLabels = detailHost.FindChildTraverse('id-mi-round-stats__tick-labels');
        watchBtn = detailHost.FindChildTraverse('id-mi-watch');
        if (watchBtn) watchBtn.text = 'WATCH';
        // Fully collapse buttons that make no sense for a local demo (download is
        // already done; no souvenir / error / downloading spinner).
        ['id-mi-error-delete', 'id-mi-downloading', 'id-mi-download', 'id-mi-souvenir'].forEach(function (id) {
          var b = detailHost.FindChildTraverse(id); if (b) { b.visible = false; b.AddClass('hide'); }
        });
        // Reuse the native copy-button footprint for the selected demo's project folder. This
        // keeps Watch aligned with the native page and adds no extra width to the action row.
        // Re-asserted per selection, NOT set up once here -- see ensureFolderBtn.
        ensureFolderBtn();
        // The native delete button (bottom-right, next to Watch) becomes our "remove from
        // list". It does NOT delete the .dem -- re-adding the folder brings it back --
        // so it is wired per-demo in wireWatch() below rather than here.
        deleteBtn = detailHost.FindChildTraverse('id-mi-delete');
        if (deleteBtn) {
          // matchinfo.xml owns a native delete action. Remove it before installing
          // our list-only action so a click can never delete the file on disk.
          try { deleteBtn.ClearPanelEvent('onactivate'); } catch (e) {}
          deleteBtn.style.opacity = '1'; deleteBtn.hittest = true; deleteBtn.enabled = true;
          deleteBtn.RemoveClass('hide'); deleteBtn.visible = true;
        }
        if (watchBtn && watchBtn.GetParent()) {
          var actionRow = watchBtn.GetParent();
          repairPanel = actionRow.FindChildTraverse('FmRepairStatus');
          if (!repairPanel) repairPanel = $.CreatePanel('Panel', actionRow, 'FmRepairStatus', {});
          repairPanel.hittest = false;
          repairPanel.style.flowChildren = 'down';
          repairPanel.style.verticalAlign = 'center';
          repairPanel.style.marginRight = '14px';
          repairPanel.style.width = '330px';
          // This is the primary compatibility readout, not caption copy. Keep a fixed footprint
          // so the larger type remains readable without making the native Watch row jump.
          repairPanel.style.height = '50px';
          repairLbl = repairPanel.FindChildTraverse('FmRepairStatusText');
          if (!repairLbl) repairLbl = $.CreatePanel('Label', repairPanel, 'FmRepairStatusText', {});
          repairLbl.style.fontFamily = 'Stratum2, "Arial Unicode MS"';
          repairLbl.style.fontSize = '17px';
          repairLbl.style.fontWeight = 'bold';
          repairLbl.style.letterSpacing = '0.5px';
          repairLbl.style.textOverflow = 'ellipsis';
          repairLbl.style.width = '100%';
          repairDetailLbl = repairPanel.FindChildTraverse('FmRepairStatusDetail');
          if (!repairDetailLbl) repairDetailLbl = $.CreatePanel('Label', repairPanel, 'FmRepairStatusDetail', {});
          repairDetailLbl.style.fontFamily = 'Stratum2, "Arial Unicode MS"';
          repairDetailLbl.style.fontSize = '14px';
          repairDetailLbl.style.color = '#aeb8c2ff';
          repairDetailLbl.style.width = '100%';
          repairDetailLbl.style.textOverflow = 'ellipsis';
          repairTrack = repairPanel.FindChildTraverse('FmRepairProgressTrack');
          if (!repairTrack) repairTrack = $.CreatePanel('Panel', repairPanel, 'FmRepairProgressTrack', {});
          repairTrack.style.width = '100%'; repairTrack.style.height = '4px';
          repairTrack.style.marginTop = '4px'; repairTrack.style.overflow = 'clip';
          repairTrack.style.backgroundColor = 'rgba(240,179,35,0.18)';
          repairFill = repairTrack.FindChildTraverse('FmRepairProgressFill');
          if (!repairFill) repairFill = $.CreatePanel('Panel', repairTrack, 'FmRepairProgressFill', {});
          repairFill.style.width = '78px'; repairFill.style.height = '4px';
          repairFill.style.backgroundColor = '#f0b323ff';
          try { actionRow.MoveChildBefore(repairPanel, watchBtn); } catch (e) {}
          updateRepairStatus();
        }
        detailBuilt = !!(scoreboardPanel && ctTable && tTable && watchBtn);
      }
      if (!detailBuilt) {
        $.Msg('[filmmaker] matchinfo.xml load failed; detail will be limited.');
        scoreboardPanel = labelsRow = ctTable = tTable = playerStats = roundContainer = tickLabels = watchBtn = folderBtn = deleteBtn = null;
        repairPanel = repairLbl = repairDetailLbl = repairTrack = repairFill = null;
        stopRepairProgress();
        watchBusy = false; // the button is gone; a stale busy flag would kill the rebuilt one
      }
      buildStatHeaders();
    }

    function buildStatHeaders() {
      if (statHeadersBuilt || !labelsRow) return;
      PLAYERSTATS.forEach(function (stat) {
        var c = $.CreatePanel('Panel', labelsRow, '', {});
        c.AddClass('sb-row__cell'); c.AddClass('sb-row__cell--' + stat); c.AddClass('matchinfo-scoreboard-header-stat-cell');
        $.CreatePanel('Label', c, '', { text: $.Localize('#Scoreboard_' + stat + '_header') });
      });
      statHeadersBuilt = true;
    }
)FMJS"
// ---- left match-tile list (native matchtiles/player.xml) ----
R"FMJS(
    // Reconcile the left list against `demos` AND the organiser's render plan (folder headers,
    // group membership, sort order). Returns true if a structural rebuild happened.
    //
    // When neither the demo SET nor the plan changed -- the common case during a background
    // scan, where only player data fills in -- tiles are updated IN PLACE: no
    // RemoveAndDeleteChildren, so scroll position, selection and focus are preserved and there
    // is no flicker. Collapsing a folder is deliberately NOT part of the plan signature, so it
    // never triggers a rebuild either; orgApplyCollapse just toggles tile visibility.
    //
    // tilePanels stays indexed by POSITION IN `demos` even though the tiles are created in
    // display order -- setCheckedTile()/selectDemo() index it that way.
    function reconcileList() {
      if (!listPanel) return false;
      var plan = orgBuildPlan();
      var planSig = orgPlanSig(plan);
      var newKeys = demos.map(tileKey);

      // Same demos and same folders on screen: never destroy rows. Data changes update tiles in
      // place, and a changed ORDER (a new sort, or a background scan filling in the value the
      // active sort reads) is expressed by MOVING the existing panels. Rebuilding to reorder
      // was resetting scroll and selection every time the scan published.
      if (tilePanels.length === demos.length && keysEqual(newKeys, renderedKeys) && orgHeaderSetMatches(plan)) {
        for (var i = 0; i < demos.length; ++i) {
          var sig = tileSig(demos[i]);
          if (sig !== tileSigs[i]) { updateTile(tilePanels[i], demos[i]); tileSigs[i] = sig; }
        }
        var ordered = true;
        if (planSig !== renderedPlanSig) {
          ordered = orgApplyPlanOrder(plan);
          if (ordered) renderedPlanSig = planSig;
        }
        if (ordered) {
          orgApplyCollapse(true);
          orgRefreshAccents();
          return false;
        }
        // A panel the plan needed was missing; fall through and rebuild.
      }
      listPanel.RemoveAndDeleteChildren();
      tilePanels = new Array(demos.length); tileSigs = new Array(demos.length);
      orgResetHeaders();
      for (var k = 0; k < plan.length; ++k) {
        var e = plan[k];
        if (e.t === 'h') { orgCreateHeader(e); continue; }
        var demo = demos[e.pos];
        var tile = createTile(demo, e.pos);
        orgWireTile(tile, demo);
        orgTileGroup[e.pos] = e.gid;
        tilePanels[e.pos] = tile;
        tileSigs[e.pos] = tileSig(demo);
      }
      renderedKeys = newKeys;
      renderedPlanSig = planSig;
      orgApplyCollapse(false);
      return true;
    }

    function createTile(demo, i) {
      // Mirror native matchlist.js: a RadioButton with player.xml loaded into it.
      // BLoadLayout applies the root panel's class to the button and brings
      // matchtile.vcss into scope, so the tile is styled exactly like native.
      var tile = $.CreatePanel('RadioButton', listPanel, 'fm-tile-' + i, { group: 'FmMatchList' });
      try { tile.BLoadLayout('file://{resources}/layout/matchtiles/player.xml', true, false); } catch (e) {}
      // Detect success by presence of the layout's panels, not the return value,
      // so we never stack a fallback on top of a layout that actually loaded.
      tile.__loaded = !!tile.FindChildTraverse('mapname');
      tile.RemoveClass('MatchTile--Collapse');
      // Bind by stable KEY (not list index) so the handler stays correct even
      // when the list is later reconciled in place.
      tile.SetPanelEvent('onactivate', (function (key) { return function () { selectByKey(key); }; })(tileKey(demo)));
      if (tile.__loaded) populateTile(tile, demo); else buildTileFallback(tile, demo, outcomeOf(demo));
      return tile;
    }

    function updateTile(tile, demo) {
      if (tile.__loaded) { populateTile(tile, demo); }
      else { tile.RemoveAndDeleteChildren(); buildTileFallback(tile, demo, outcomeOf(demo)); }
    }

    function applyOutcomeClass(tile, oc) {
      tile.SetHasClass('MatchVictory', oc === 1);
      tile.SetHasClass('MatchLoss', oc === -1);
      tile.SetHasClass('MatchTied', oc === 0);
    }

    // Compatibility is not the thin gold organiser edge (that edge only means "played before").
    // Give legacy, repaired, and unsupported demos their own strong marker on every tile.
    function paintCompatibilityBadge(tile, demo) {
      if (!tile) return;
      // Valve already reserves id-download-state at the right edge of every loaded tile. Host
      // the marker there: arbitrary children appended to the layout root are clipped behind the
      // native MatchTile subtree and never become visible.
      var nativeSlot = tile.FindChildTraverse('id-download-state');
      var slot = nativeSlot || tile;
      var badge = slot.__fmCompatBadge;
      if (!badge || !badge.IsValid || !badge.IsValid()) {
        badge = $.CreatePanel('Panel', slot, tile.id + '-compat', {});
        slot.__fmCompatBadge = badge;
        badge.hittest = false;
        badge.style.width = '100%'; badge.style.height = '100%';
        var warningIcon = $.CreatePanel('Image', badge, '', {});
        warningIcon.SetImage('s2r://panorama/images/icons/ui/warning.vsvg');
        warningIcon.style.width = '100%'; warningIcon.style.height = '100%';
        warningIcon.style.horizontalAlign = 'center'; warningIcon.style.verticalAlign = 'center';
        // A TICK, not a letter 'R'. The badge is 26x24 and shares the tile's right edge with
        // CS2's own download-state chip, so a bare capital sitting in a green square read as a
        // stray glyph rather than as a state -- and it had to be explained to mean "repaired".
        // A check is the same thing every other "this one is ready" marker in the game uses, and
        // it needs no legend. check.vsvg is CS2's own icon (the organiser's confirm button uses
        // it too), so it is guaranteed to resolve.
        var repairedGlyph = $.CreatePanel('Image', badge, '', { texturewidth: '12', textureheight: '12' });
        try { repairedGlyph.SetImage('s2r://panorama/images/icons/ui/check.vsvg'); } catch (eChk) {}
        repairedGlyph.style.width = '12px'; repairedGlyph.style.height = '12px';
        repairedGlyph.style.horizontalAlign = 'center'; repairedGlyph.style.verticalAlign = 'center';
        repairedGlyph.style.washColor = '#eafff2ff';
        badge.__warningIcon = warningIcon; badge.__repairedGlyph = repairedGlyph;
        if (!nativeSlot) {
          badge.style.horizontalAlign = 'right'; badge.style.verticalAlign = 'top';
          badge.style.marginTop = '9px'; badge.style.marginRight = '28px';
          badge.style.width = '25px'; badge.style.height = '23px'; badge.style.zIndex = '20';
        }
      }
      var cache = demo.repairCache || 'none';
      var compatibility = demo.compatibility || 'unknown';
      var special = cache === 'repaired' || compatibility === 'legacy' || compatibility === 'unsupported';
      badge.visible = special;
      if (nativeSlot) {
        slot.SetHasClass('downloaded', !special);
        slot.SetHasClass('download-animation', !special);
        slot.style.width = '26px'; slot.style.height = '24px';
        slot.style.borderRadius = '3px'; slot.style.overflow = 'noclip';
      }
      if (!special) {
        if (nativeSlot) {
          slot.style.backgroundColor = 'rgba(0,0,0,0)';
          slot.style.border = '2px solid rgba(0,0,0,0)';
        }
        return;
      }
      if (cache === 'repaired') {
        badge.__warningIcon.visible = false; badge.__repairedGlyph.visible = true;
        (nativeSlot ? slot : badge).style.backgroundColor = 'rgba(24,150,83,0.96)';
        (nativeSlot ? slot : badge).style.border = '2px solid rgba(144,255,190,0.8)';
      } else {
        badge.__repairedGlyph.visible = false; badge.__warningIcon.visible = true;
        badge.__warningIcon.style.washColor = compatibility === 'unsupported' ? '#ff7e45ff' : '#f0b323ff';
        (nativeSlot ? slot : badge).style.backgroundColor = 'rgba(0,0,0,0)';
        (nativeSlot ? slot : badge).style.border = '2px solid rgba(0,0,0,0)';
      }
    }

    function populateTile(tile, demo) {
      var oc = outcomeOf(demo);
      applyOutcomeClass(tile, oc);
      setMapIcon(tile.FindChildTraverse('mapicon'), demo.map);
      var modeIcon = tile.FindChildTraverse('modeicon');
      if (modeIcon) { try { modeIcon.SetImage('s2r://panorama/images/icons/ui/competitive.vsvg'); } catch (e) {} }
      var mapName = tile.FindChildTraverse('mapname'); if (mapName) mapName.text = mapDisplay(demo.map);

      var s0 = tile.FindChildTraverse('score_team0');
      var sv = tile.FindChildTraverse('vs');
      var s1 = tile.FindChildTraverse('score_team1');
      if (demo.hasScoreboard) {
        if (s0) s0.text = String(demo.teamScore0);
        if (sv) sv.text = '-';
        if (s1) s1.text = String(demo.teamScore1);
      } else {
        if (s0) s0.text = mapDisplay(demo.map);
        if (sv) sv.text = '';
        if (s1) s1.text = '';
      }
      if (s0) s0.SetHasClass('tint--CT', !!demo.hasScoreboard);
      if (s1) s1.SetHasClass('tint--TERRORIST', !!demo.hasScoreboard);

      var ts = tile.FindChildTraverse('timestamp'); if (ts) ts.text = demo.dateText || '';
      var outcome = tile.FindChildTraverse('outcome');
      if (outcome) {
        outcome.SetHasClass('MatchInfo--Hide', oc === null);
        if (oc !== null) outcome.text = oc === 1 ? $.Localize('#WatchMenu_Outcome_Won', outcome) : oc === -1 ? $.Localize('#WatchMenu_Outcome_Lost', outcome) : $.Localize('#WatchMenu_Outcome_Tied', outcome);
      }
      var dl = tile.FindChildTraverse('id-download-state'); if (dl) dl.AddClass('downloaded');
      paintCompatibilityBadge(tile, demo);
    }

    function setCheckedTile(i) { for (var t = 0; t < tilePanels.length; ++t) { try { tilePanels[t].checked = (t === i); } catch (e) {} } }

    function buildTileFallback(tile, demo, oc) {
      var map = $.CreatePanel('Panel', tile, '', { class: 'MatchMap' });
      setMapIcon($.CreatePanel('Image', map, '', { class: 'MatchMap__Icon' }), demo.map);
      $.CreatePanel('Label', map, '', { class: 'MatchInfo MatchInfo__MapName', text: mapDisplay(demo.map) });
      var infoC = $.CreatePanel('Panel', tile, '', { class: 'MatchInfoContainer' });
      var scoreRow = $.CreatePanel('Panel', infoC, '', { class: 'left-right-flow' });
      if (demo.hasScoreboard) {
        $.CreatePanel('Label', scoreRow, '', { class: 'MatchInfo__Score tint--CT', text: String(demo.teamScore0) });
        $.CreatePanel('Label', scoreRow, '', { text: '-' });
        $.CreatePanel('Label', scoreRow, '', { class: 'MatchInfo__Score tint--TERRORIST', text: String(demo.teamScore1) });
      } else {
        $.CreatePanel('Label', scoreRow, '', { class: 'MatchInfo__Score', text: mapDisplay(demo.map) });
      }
      $.CreatePanel('Label', infoC, '', { class: 'MatchInfo', text: demo.dateText || '' });
      paintCompatibilityBadge(tile, demo);
    }
)FMJS"
// ---- right detail: meta + CT/T scoreboard player rows ----
R"FMJS(
    function makeRow(table, p, teamId) {
      var row = $.CreatePanel('Panel', table, '', {});
      row.AddClass('sb-row'); row.AddClass('left-right-flow');
      var stats = $.CreatePanel('Panel', row, '', { class: 'left-right-flow' });

      var avCell = $.CreatePanel('Panel', stats, '', {});
      avCell.AddClass('sb-row__cell'); avCell.AddClass('sb-row__cell--avatar');
      var av = $.CreatePanel('CSGOAvatarImage', avCell, '', {});
      // Native (matchinfo.js) puts the team class on the AVATAR IMAGE, not the
      // cell. That class carries a team-colored placeholder background-image which
      // the loaded Steam avatar then covers. Putting it on the cell instead left
      // the placeholder showing through the cell's 2px padding as a "border"
      // framing every avatar -- which the native scoreboard does not have.
      av.AddClass('sb-row__cell--avatar--' + TEAMS[teamId]);
      var sid = steamId64(p.accountId);
      if (sid) { try { av.PopulateFromSteamID(sid); } catch (e) { try { av.steamid = sid; } catch (e2) {} } }

      var nameCell = $.CreatePanel('Panel', stats, '', {});
      nameCell.AddClass('sb-row__cell'); nameCell.AddClass('sb-row__cell--name');
      var nm = $.CreatePanel('Label', nameCell, '', { text: p.name || '[unknown]' });
      nm.AddClass('sb-tint--' + TEAMS[teamId]);

      PLAYERSTATS.forEach(function (stat) {
        if (stat === 'mvps') {
          var mp = $.CreatePanel('Panel', stats, '', {});
          mp.AddClass('sb-row__cell'); mp.AddClass('sb-row__cell--mvps'); mp.AddClass('sb-tint--' + TEAMS[teamId]);
          var star = $.CreatePanel('Image', mp, '', {}); star.AddClass('sb-row__cell--mvps__star'); star.SetImage('s2r://panorama/images/icons/ui/star.vsvg');
          var cnt = $.CreatePanel('Label', mp, '', { text: String(p.mvps || 0) }); cnt.AddClass('sb-row__cell--mvps__count'); cnt.AddClass('mi-mvps-shrink-overflow');
          if (!p.mvps) mp.AddClass('hide-mvps');
        } else {
          var c = $.CreatePanel('Panel', stats, '', {});
          c.AddClass('sb-row__cell'); c.AddClass('sb-row__cell--' + stat);
          var l = $.CreatePanel('Label', c, '', { text: String(statVal(p, stat)) }); l.AddClass('sb-tint--' + TEAMS[teamId]);
        }
      });

      row.SetPanelEvent('onactivate', (function (idx) { return function () { selectPlayer(idx); }; })(p.__idx));
      playerRowPanels[p.__idx] = row;
    }

    function byScore(a, b) { return (b.score || 0) - (a.score || 0); }

    function renderDetail(demo) {
      if (!info) return;
      // renderDetail also runs for the demo ALREADY on screen -- the background rescan bumps the
      // payload and maybeRefreshDetail re-renders whenever the scoreboard fills in. Closing the
      // rename here unconditionally meant a scan landing a few hundred ms after the right-click
      // silently took the field away again. Only a DIFFERENT demo may close it; the field is
      // bound to the title, so a title that now says something else must not still be accepting
      // the old name.
      if (titleRenaming && demoTitleEdit && tileKey(demo) !== titleRenameKey) {
        titleRenaming = false;
        demoTitleEdit.visible = false;
      }
      if (demoTitle) {
        demoTitle.text = demoName(demo);
        // ...and do not paint the label back over an open editor.
        demoTitle.visible = !titleRenaming;
      }
      info.SetDialogVariable('map_name', mapDisplay(demo.map));
      info.SetDialogVariable('durationLabel', $.Localize('#CSGO_Watch_Info_1'));
      var mins = demo.duration ? Math.max(Math.floor(demo.duration / 60), 1) : 0;
      info.SetDialogVariable('duration', mins ? ($.ConstructString ? $.ConstructString('#CSGO_Watch_Minute:f', { value: mins }) : (mins + ' min')) : '-');
      info.SetDialogVariable('dateOrRoundLabel', $.Localize('#CSGO_Watch_Info_2'));
      info.SetDialogVariable('dateOrRound', demo.dateText || '-');
      info.SetDialogVariable('score_CT', demo.hasScoreboard ? String(demo.teamScore0) : '-');
      info.SetDialogVariable('score_TERRORIST', demo.hasScoreboard ? String(demo.teamScore1) : '-');
      info.SetDialogVariable('sb_team_name--CT', $.Localize('#teamname_CT'));
      info.SetDialogVariable('sb_team_name--TERRORIST', $.Localize('#teamname_TERRORIST'));

      setMapIcon(info.FindChildTraverse('id-mi-map-icon'), demo.map);
      var modeIcon = info.FindChildTraverse('id-mi-mode-icon');
      if (modeIcon) { try { modeIcon.SetImage('s2r://panorama/images/icons/ui/competitive.vsvg'); } catch (e) {} }

      if (scoreboardPanel) scoreboardPanel.visible = true;
      if (ctTable) ctTable.RemoveAndDeleteChildren();
      if (tTable) tTable.RemoveAndDeleteChildren();
      playerRowPanels = [];

      if (!(demo.hasScoreboard && demo.players && demo.players.length)) {
        if (ctTable) {
          var no = $.CreatePanel('Label', ctTable, '', { text: 'No scoreboard data for this demo.' });
          no.style.padding = '20px'; no.style.color = '#8b949c';
        }
        if (playerStats) playerStats.AddClass('mi-player-stats__collapse');
        return;
      }

      var t0 = [], t1 = [];
      demo.players.forEach(function (p, idx) { p.__idx = idx; (p.team === 1 ? t1 : t0).push(p); });
      t0.sort(byScore); t1.sort(byScore);
      t0.forEach(function (p) { makeRow(ctTable, p, 0); });
      t1.forEach(function (p) { makeRow(tTable, p, 1); });
    }
)FMJS"
// ---- per-round performance timeline (native mi-round-summary-bar) ----
R"FMJS(
    function fillRoundStats(p) {
      if (!roundContainer || !playerStats) return;
      roundContainer.RemoveAndDeleteChildren();
      if (tickLabels) tickLabels.RemoveAndDeleteChildren();
      var pr = (p && p.perRound) || [];
      playerStats.SetHasClass('mi-player-stats__collapse', pr.length === 0);

      var title = info.FindChildTraverse('id-mi-player-stats-title');
      if (title) { info.SetDialogVariable('playerNameTitle', p ? (p.name || '[unknown]') : ''); title.text = $.Localize('#MatchInfo_RoundDataTitle', info); }
      if (!pr.length) return;

      // Native CS2's round-stats axis (matchinfo.js _FillRoundStats): mark each
      // side-swap with a "<>" between the two halves, number the major/minor ticks,
      // and style each bar's little tick mark. We DETECT the swaps straight from the
      // per-round side data (the half-time swap, plus any overtime swaps) instead of
      // recomputing the schedule, so it stays correct regardless of format/overtime.
      function sideSwap(i) { return i > 0 && pr[i].side !== pr[i - 1].side; }
      var firstSwap = -1;
      for (var s = 1; s < pr.length; ++s) { if (sideSwap(s)) { firstSwap = s; break; } }
      var maxRounds = firstSwap > 0 ? firstSwap * 2 : pr.length; // regulation rounds (swap at maxRounds/2)
      function isMajorTick(n) { return n === 1 || n === maxRounds || n === pr.length || (n > maxRounds && (n - maxRounds) % 6 === 0); }
      function isMinorTick(n) {
        if (n >= maxRounds) return false;
        if (maxRounds % 5 === 0) return n % 5 === 0;
        if (maxRounds % 4 === 0) return n % 4 === 0;
        if (maxRounds <= 12 && maxRounds % 3 === 0) return n % 3 === 0;
        if (maxRounds <= 8 && maxRounds % 2 === 0) return n % 2 === 0;
        return false;
      }
      function tickStyle(idx) { // idx is 0-based; n is 1-based round number
        var n = idx + 1;
        if (sideSwap(idx)) return 'mi-round-tick--right-of-team-switch';
        if (idx + 1 < pr.length && sideSwap(idx + 1)) return 'mi-round-tick--left-of-team-switch';
        if (isMajorTick(n)) return 'mi-round-tick--major';
        if (isMinorTick(n)) return 'mi-round-tick--minor';
        return 'mi-round-tick--sub';
      }
      function tickLabel(idx) {
        var n = idx + 1;
        if (sideSwap(idx)) return '<>';
        // Native _GetLabelForTick suppresses the number on the round immediately
        // LEFT of a side swap (the round before halftime/overtime swaps), so the
        // '<>' marker isn't crowded by a number. We were missing this, which is
        // why round 12 (left of the round-13 swap) printed "12" before the '<>'.
        if (idx + 1 < pr.length && sideSwap(idx + 1)) return '';
        if (isMajorTick(n) || isMinorTick(n)) return String(n);
        return '';
      }

      for (var i = 0; i < pr.length; ++i) {
        var r = pr[i];
        var side = TEAMS[r.side === 1 ? 1 : 0];
        var bar = $.CreatePanel('Button', roundContainer, '', {});
        try { bar.BLoadLayoutSnippet('snippet_mi-round-summary-bar'); } catch (e) { continue; }
        bar.AddClass('round-selection-button'); bar.AddClass('no-hover');

        var roundBar = bar.FindChildTraverse('id-mi-round-summary-bar__container');
        var iconContainer = bar.FindChildTraverse('id-mi-icons__container');
        if (!roundBar || !iconContainer) continue;
        var winBar = roundBar.GetChild(0).GetChild(0);
        var border = roundBar.GetChild(1);
        var lossBar = roundBar.GetChild(2).GetChild(0);
        var tickMark = roundBar.GetChild(2).GetChild(1); // the little major/minor/swap mark under the bar
        if (tickMark) { tickMark.RemoveClass('mi-round-tick--sub'); tickMark.AddClass(tickStyle(i)); }
        var winIcons = bar.FindChildTraverse('id-mi-eliminations-win');

        winBar.AddClass('sb-tint--' + side); border.AddClass('sb-tint--' + side); if (winIcons) winIcons.AddClass('sb-tint--' + side);
        if (r.w) { lossBar.AddClass('mi-round-summary-bar--EMPTY'); winBar.RemoveClass('mi-round-summary-bar--EMPTY'); }
        else { winBar.AddClass('mi-round-summary-bar--EMPTY'); lossBar.RemoveClass('mi-round-summary-bar--EMPTY'); }

        var star = bar.FindChildTraverse('id-mvp-star');
        if (star) { if (r.mvp) { star.RemoveClass('hide'); star.AddClass('sb-tint--' + side); } else { star.AddClass('hide'); } }

        var nk = r.k || 0, nh = r.hs || 0;
        if (winIcons) for (var k = 0; k < 5; ++k) {
          var kIcon = winIcons.FindChildTraverse('id-mi-icon-elimination_' + k);
          var hIcon = winIcons.FindChildTraverse('id-mi-icon-elimination--headshot_' + k);
          if (!kIcon || !hIcon) continue;
          if (k >= nk) { kIcon.AddClass('hide'); hIcon.AddClass('hide'); }
          else if (k >= nh) { kIcon.RemoveClass('hide'); hIcon.AddClass('hide'); }
          else { kIcon.AddClass('hide'); hIcon.RemoveClass('hide'); }
        }

        var death = iconContainer.FindChildTraverse('id-mi-elimination-death');
        if (death) { if (r.d) death.RemoveClass('hide'); else death.AddClass('hide'); }

        if (tickLabels) {
          var tk = $.CreatePanel('Panel', tickLabels, '', { class: 'mi-tick' });
          var label = tickLabel(i);
          // The "<>" swap marker straddles the gap between halves (native aligns it
          // with the mi-tick-class-halftime-align class).
          tk.SetHasClass('mi-tick-class-halftime-align', label === '<>');
          $.CreatePanel('Label', tk, '', { class: 'mi-tick-label', text: label });
        }
      }
    }

    function selectPlayer(i) {
      selPlayer = i;
      var demo = demos[sel];
      if (!demo || !demo.players) return;
      var p = demo.players[i];
      if (playerRowPanels[selPlayer]) { try { playerRowPanels[selPlayer].checked = false; } catch (e) {} }
      if (playerRowPanels[i]) { try { playerRowPanels[i].checked = true; } catch (e) {} }
      fillRoundStats(p);
    }

    function bestPlayerIndex(players) { var b = 0; for (var k = 1; k < players.length; ++k) if ((players[k].score || 0) > (players[b].score || 0)) b = k; return b; }

    function startRepairProgress() {
      if (!repairTrack || !repairFill) return;
      repairTrack.visible = true;
      if (repairProgressRunning) return;
      repairProgressRunning = true;
      var token = ++repairProgressToken;
      var x = -78;
      function step() {
        if (!repairProgressRunning || token !== repairProgressToken || !repairFill) return;
        x += 8; if (x > 330) x = -78;
        repairFill.style.position = x + 'px 0px 0px';
        $.Schedule(0.035, step);
      }
      step();
    }

    function stopRepairProgress() {
      repairProgressRunning = false;
      ++repairProgressToken;
      if (repairTrack) repairTrack.visible = false;
    }

    function showRepairStatus(title, detail, color, progress) {
      if (!repairPanel || !repairLbl || !repairDetailLbl) return;
      repairPanel.visible = true;
      repairLbl.text = title;
      repairLbl.style.color = color;
      repairDetailLbl.text = detail || '';
      repairDetailLbl.visible = !!detail;
      if (progress) startRepairProgress(); else stopRepairProgress();
    }

    function maybeAutoPrepare(demo) {
      if (!demo || demo.compatibility !== 'legacy' || demo.repairCache !== 'none' || anyRepairJob) return;
      var key = tileKey(demo);
      if (autoPrepared[key]) return;
      autoPrepared[key] = true;
      autoPreparePendingKey = key;
      setWatchBusy(true);
      showRepairStatus('CHECKING COMPATIBILITY', 'Checking now; repair starts automatically if required.', '#f0b323ff', true);
      api.addCommand('prepare ' + demo.index);
      // If the command cannot begin (for example, the page is being torn down), do not leave a
      // permanently disabled Watch button. A real worker promptly replaces this local pending
      // state with the authoritative repair payload.
      $.Schedule(2.0, function () {
        if (autoPreparePendingKey === key && !repairJobBusy) {
          autoPreparePendingKey = null;
          setWatchBusy(false);
          stopRepairProgress();
        }
      });
    }

    function updateRepairStatus() {
      var state = repairStatus ? (repairStatus.state | 0) : 0;
      var text = repairStatus && repairStatus.text ? repairStatus.text : '';
      var demo = (sel >= 0 && sel < demos.length) ? demos[sel] : null;
      var ownsStatus = !!(demo && repairStatus && repairStatus.demoIndex === demo.index);
      // Busy only for the demo the check/repair job belongs to. A global flag disabled Watch on
      // EVERY demo while one demo's (minutes-long) background check ran, and the click's own
      // self-clear waited on it too: picking a demo that needed checking and then a supported one
      // left Watch dead (user report 2026-10-13).
      anyRepairJob = state === 1 || state === 2; // the engine runs one job at a time
      repairJobBusy = ownsStatus && anyRepairJob;
      watchUnavailable = !!(demo && demo.compatibility === 'unsupported');
      if (repairJobBusy) autoPreparePendingKey = null;
      setWatchBusy(repairJobBusy || (!!demo && autoPreparePendingKey === tileKey(demo)));
      if (!repairPanel) return;
      if (ownsStatus && state === 1) {
        showRepairStatus('CHECKING COMPATIBILITY', 'Scanning demo packets; repair is automatic if required.', '#f0b323ff', true);
      } else if (ownsStatus && state === 2) {
        showRepairStatus('REPAIRING LEGACY DEMO', text, '#f0b323ff', true);
      } else if (ownsStatus && state === 4) {
        showRepairStatus('REPAIR FAILED', text, '#e45b5bff', false);
      } else if (ownsStatus && state === 5) {
        showRepairStatus('UNSUPPORTED DEMO VERSION', text, '#e0a03aff', false);
      } else if (demo && demo.repairCache === 'repaired') {
        showRepairStatus('VALIDATED REPAIR', 'Ready to watch', '#65d995ff', false);
      } else if (demo && demo.repairCache === 'clean') {
        showRepairStatus('SUPPORTED', 'Legacy demo checked; no repair needed', '#65d995ff', false);
      } else if (demo && demo.compatibility === 'unsupported') {
        showRepairStatus('UNSUPPORTED DEMO', 'Build ' + (demo.buildNum || '?') + ' · protocol ' + (demo.networkProtocol || '?'), '#f09a4aff', false);
      } else if (demo && demo.compatibility === 'legacy') {
        showRepairStatus('LEGACY DEMO', 'Compatibility has not been checked yet', '#f0b323ff', false);
      } else if (demo && demo.compatibility === 'supported') {
        showRepairStatus('SUPPORTED', 'Current demo version', '#65d995ff', false);
      } else if (demo) {
        showRepairStatus('COMPATIBILITY UNKNOWN', 'Demo header version unavailable', '#aeb8c2ff', false);
      } else {
        repairPanel.visible = false;
        stopRepairProgress();
      }
      maybeAutoPrepare(demo);
    }

    // Watch is disabled (and dimmed) while a demo job runs. The neighbouring FmRepairStatus
    // label says WHY -- CHECKING / REPAIRING -- so this only has to stop the input.
    function setWatchBusy(busy) {
      watchBusy = busy;
      if (!watchBtn) return;
      watchBtn.enabled = !busy && !watchUnavailable;
      watchBtn.style.opacity = (busy || watchUnavailable) ? '0.45' : '1';
    }

)FMJS"
// Split for MSVC's ~16380-byte cap on a single raw string literal (C2026), not for any
// semantic reason. Adjacent raw literals concatenate, so the seam is invisible to the script.
R"FMJS(
    // THE ACTION-ROW BUTTONS HAVE TO BE RE-ASSERTED, NOT SET UP ONCE.
    //
    // `id-mi-copy` belongs to CS2's matchinfo.xml, and that subtree is RE-CREATED under
    // FmMatchInfo as the page runs -- which is why the demo title had to be parented to the
    // persistent FmInfo host to survive. Doing this once at build time therefore left `folderBtn`
    // pointing at a panel that no longer exists: measured live, the button on screen reported
    // hittestchildren=true and 40x40 (its native size) instead of our 64x48, i.e. none of our
    // styling, none of our handlers. It looked like a button and did nothing, which is exactly
    // the reported bug -- and it explains why it worked once and then stopped.
    //
    // So: resolve it every time the selection is wired, and redo the setup whenever we are
    // looking at a panel we have not already prepared. __fmFolderReady is the marker; it lives on
    // the panel, so a replaced panel arrives without it and is set up again, and an untouched one
    // costs a property read.
    function ensureFolderBtn() {
      if (!info) return null;
      var b = info.FindChildTraverse('id-mi-copy');
      folderBtn = b || null;
      if (!b || b.__fmFolderReady) return folderBtn;
      b.__fmFolderReady = true;
      try { b.ClearPanelEvent('onactivate'); } catch (e) {}
      b.RemoveAndDeleteChildren();
      b.RemoveClass('hide'); b.visible = true;
      // hittestchildren=false is load-bearing. A child panel takes the hit before its parent, so
      // with the icon hit-testable the click lands on the image and `onactivate` on the button
      // never fires. Setting it on the container also covers anything added here later.
      b.hittest = true; b.enabled = true;
      b.hittestchildren = false;
      b.style.width = '64px'; b.style.height = '48px';
      b.style.marginRight = '8px'; b.style.padding = '0px';
      b.style.backgroundColor = '#4a5158ff';
      b.style.transitionProperty = 'transform, background-color';
      b.style.transitionDuration = '0.10s';
      // An ICON, not the word FOLDER. This sits next to WATCH and a trash can in a row of
      // 48px-tall square-ish buttons, and it was the only one carrying a word -- which made it
      // read as a label rather than as one of the actions. popout.vsvg is CS2's own "open this
      // outside the game" glyph, which is what the button does.
      var icon = $.CreatePanel('Image', b, '', { texturewidth: '20', textureheight: '20' });
      try { icon.SetImage('s2r://panorama/images/icons/ui/popout.vsvg'); } catch (ePop) {}
      icon.style.width = '20px'; icon.style.height = '20px';
      icon.style.horizontalAlign = 'center'; icon.style.verticalAlign = 'center';
      icon.style.washColor = '#f2f5f7ff';
      icon.hittest = false;
      b.SetPanelEvent('onmousedown', function () { b.style.transform = 'scale3d(0.96,0.96,1)'; });
      b.SetPanelEvent('onmouseup', function () { b.style.transform = 'scale3d(1,1,1)'; });
      b.SetPanelEvent('onmouseover', function () {
        b.style.backgroundColor = '#626b74ff';
        try { UiToolkitAPI.ShowTextTooltipOnPanel(b, 'Show demo in Explorer'); } catch (e) {}
      });
      b.SetPanelEvent('onmouseout', function () {
        b.style.transform = 'scale3d(1,1,1)';
        b.style.backgroundColor = '#4a5158ff';
        try { UiToolkitAPI.HideTextTooltip(); } catch (e) {}
      });
      return folderBtn;
    }

    function wireWatch(demo) {
      // Re-resolve first: a matchinfo reload since the last selection would otherwise leave the
      // handler below on a dead panel.
      ensureFolderBtn();
      if (folderBtn) folderBtn.SetPanelEvent('onactivate', (function (idx) {
        return function () { api.addCommand('openfolder ' + idx); };
      })(demo.index));
      if (watchBtn) watchBtn.SetPanelEvent('onactivate', (function (idx) {
        return function () {
          // Client-side single flight as well as the engine-side one: a second click before
          // the status payload comes back would otherwise queue a duplicate watch command.
          if (watchBusy || watchUnavailable) return;
          // setWatchBusy, not a bare assignment: the raw write left the button LOOKING
          // enabled while the guard above silently swallowed every click.
          setWatchBusy(true);
          // Self-clearing floor. On the FAST path -- a valid repaired copy, or a clean marker
          // -- the demo resolves synchronously and the repair state never leaves Idle, so the
          // repair signature never changes, so api.render never fires, so updateRepairStatus()
          // never runs and nothing ever cleared this. The flag stayed true for the rest of the
          // session and Watch was dead on exactly the demos the caching made fast. A real job
          // still owns the flag. Status payloads are edge-triggered rather than pushed every
          // frame, so the timer must consult the last authoritative job state instead of
          // blindly undoing the disabled state halfway through a long scan.
          $.Schedule(1.5, function () { if (watchBusy && !repairJobBusy) setWatchBusy(false); });
          api.addCommand('watch ' + idx);
        };
      })(demo.index));
      // Remove-from-list. Keeps the .dem on disk; "Add folder" on the containing folder
      // restores it. Selection moves off the removed demo so the detail pane is never left
      // showing something that is no longer in the list.
      if (deleteBtn) deleteBtn.SetPanelEvent('onactivate', (function (idx, name) {
        return function () {
          try {
            UiToolkitAPI.ShowGenericPopupOkCancel(
              'Remove demo?',
              'Remove "' + name + '" from the list?\nThe demo file stays on disk.',
              '',
              function () {
                api.addCommand('hide ' + idx);
                selKey = null; sel = -1; detailRendered = false;
              },
              function () {}
            );
          } catch (e) {
            $.Msg('[filmmaker] confirmation popup unavailable; nothing was removed.');
          }
        };
      })(demo.index, demo.fileName || 'this demo'));
      if (demo.hasScoreboard && !demo.hasRounds) api.addCommand('rounds ' + demo.index);
    }

    // Full (re)render of the right detail for a newly chosen demo: resets the
    // player selection to the top fragger. Used only when the SELECTED demo
    // changes (user click or first load) -- not on every background refresh.
    function selectDemo(i) {
      var demo = demos[i]; if (!demo) return;
      sel = i; selKey = tileKey(demo); selPlayer = -1;
      if (emptyMsg) emptyMsg.visible = false;
      if (info) info.visible = true;
      setCheckedTile(i);
      renderDetail(demo); detailSig = detailSignature(demo); detailRendered = true;
      wireWatch(demo);
      updateRepairStatus();
      var players = demo.players || [];
      if (players.length) selectPlayer(bestPlayerIndex(players)); else fillRoundStats(null);
    }

    // User clicked a tile -> resolve by stable key (robust to in-place updates).
    function selectByKey(key) { var i = indexOfKey(key); if (i >= 0) selectDemo(i); }

)FMJS"
// Split for MSVC's ~16380-byte cap on a single raw string literal (C2026), not for any
// semantic reason. Adjacent raw literals concatenate, so the seam is invisible to the script.
R"FMJS(
    // ---- rename the selected demo, in its own title -------------------------------------
    // `key` selects a demo first (the right-click menu path, where the demo clicked may not be
    // the one on screen); null means "whatever is selected" (the click-the-title path).
    function beginTitleRename(key) {
      if (key != null) {
        var i = indexOfKey(key);
        if (i < 0) return;
        if (i !== sel) selectDemo(i);
      }
      var demo = demos[sel];
      if (!demo || !demoTitle || !demoTitleEdit || titleRenaming) return;
      titleRenaming = true;
      titleRenameKey = tileKey(demo);
      try { UiToolkitAPI.HideTextTooltip(); } catch (e) {}
      demoTitle.style.color = '#ffffffff';
      demoTitle.visible = false;
      demoTitleEdit.text = demoName(demo);
      demoTitleEdit.visible = true;
      // FOCUS HAS TO BE CHASED, not asked for once.
      //
      // SetFocus on a panel Panorama has not laid out yet is dropped on the floor -- that alone
      // is why a single attempt leaves the caret missing (the same trap orgOpenNewFolder
      // documents). Coming from the RIGHT-CLICK MENU there is a second one on top of it: the
      // context menu is still closing when its jsCallback runs, and it takes focus back with it
      // on the way out. So the field opens but will not accept a keystroke, which reads as
      // "Rename did not do anything".
      //
      // Retry until the panel actually reports the focus, over about a third of a second. Each
      // attempt is two calls on one panel; it stops the moment it wins, and it stops if the user
      // closed the field in the meantime.
      (function () {
        var tries = 0;
        function grab() {
          if (!titleRenaming) return;
          try {
            if (demoTitleEdit.BHasKeyFocus && demoTitleEdit.BHasKeyFocus()) return;
            demoTitleEdit.SetFocus();
            demoTitleEdit.SelectAll();
          } catch (e) {}
          if (++tries < 7) $.Schedule(0.05, grab);
        }
        $.Schedule(0.0, grab);
      })();
    }

    function endTitleRename(commit) {
      if (!titleRenaming) return;
      titleRenaming = false;
      titleRenameKey = null;
      var demo = demos[sel];
      var text = '';
      try { text = ('' + (demoTitleEdit.text || '')); } catch (e) {}
      // The command queue is newline-delimited, so a name may not contain one. Strip control
      // characters here as well as in C++: this side keeps the queue parseable, that side is
      // what protects the file system.
      text = text.replace(/[\r\n\t]/g, ' ').trim();
      try { demoTitleEdit.visible = false; } catch (e) {}
      try { demoTitle.visible = true; } catch (e) {}
      if (!commit || !demo) return;
      var current = demoName(demo);
      if (!text || text === current) return;
      // Show the new name immediately. The rescan that follows the rename takes a moment, and
      // leaving the old name up in the meantime reads as "it did not work".
      try { demoTitle.text = text; } catch (e) {}
      // Re-select the demo under its NEW key once the list comes back: the key is folder +
      // fileName, so a rename changes it and the selection would otherwise fall back to the
      // first demo in the list.
      // Predict the SAME name C++ will land on, or the re-select below misses: RenameDemoFile
      // accepts a typed ".dem" instead of appending a second one, so strip it here too.
      var fileText = /\.dem$/i.test(text) ? text : (text + '.dem');
      titleRenamePending = (demo.folder || '') + '|' + fileText;
      // Name LAST so it may contain spaces -- same shape as "org rename".
      api.addCommand('renamedemo ' + demo.index + ' ' + text);
    }

    // The selected demo is unchanged but its data may have filled in (phase-2
    // scan). Re-render the detail ONLY if its data actually changed, and keep the
    // current player selection so the round timeline does not jump around.
    function maybeRefreshDetail(demo) {
      var s = detailSignature(demo);
      if (s === detailSig) return;
      detailSig = s;
      var keep = selPlayer;
      renderDetail(demo);
      wireWatch(demo);
      var players = demo.players || [];
      if (players.length) selectPlayer((keep >= 0 && keep < players.length) ? keep : bestPlayerIndex(players));
      else fillRoundStats(null);
    }

    function renderInto() {
      if (!listPanel) return;
      reconcileList();  // stable: only rebuilds rows when the demo SET changes
      if (!demos.length) {
        if (info) {
          info.visible = true;
          var emptyDetail = info.FindChildTraverse('FmMatchInfo'); if (emptyDetail) emptyDetail.visible = false;
        }
        if (demoTitle) demoTitle.visible = false;
        if (demoTitleEdit) { demoTitleEdit.visible = false; titleRenaming = false; }
        if (emptyMsg) emptyMsg.visible = true;
        selKey = null; sel = -1; detailRendered = false;
        return;
      }
      if (emptyMsg) emptyMsg.visible = false;
      if (info) info.visible = true;
      var populatedDetail = info && info.FindChildTraverse('FmMatchInfo'); if (populatedDetail) populatedDetail.visible = true;
      // A rename just landed: adopt the new key so the selection follows the file instead of
      // snapping back to the top of the list. Cleared whether or not the demo turned up -- a
      // failed rename must not leave this armed for the next refresh.
      if (titleRenamePending != null) {
        var renamed = indexOfKey(titleRenamePending);
        if (renamed >= 0) { selKey = titleRenamePending; sel = renamed; detailRendered = false; }
        titleRenamePending = null;
      }
      // Resolve selection by stable key so a refresh keeps the same demo selected.
      var idx = selKey != null ? indexOfKey(selKey) : -1;
      if (idx < 0) idx = (sel >= 0 && sel < demos.length) ? sel : 0; // first load / selection removed
      if (idx !== sel || !detailRendered) {
        selectDemo(idx);
      } else {
        setCheckedTile(idx);
        maybeRefreshDetail(demos[idx]);
      }
    }
)FMJS"
// ---- demo search (button -> input + suggestion dropdown) ----
R"FMJS(
    var searchWrap = null, searchInput = null, searchResults = null;

    // Lazily build the search input + dropdown once, parented to the watch page so it
    // floats below the navbar (right side, clear of the friends rail). Inline-styled so
    // it does not depend on the settings stylesheet being in scope on this page.
    function buildSearch(page) {
      if (searchWrap && searchWrap.IsValid && searchWrap.IsValid()) return;
      // Parent to the PAGE root (JsWatchContent, no flow), NOT main-content (top-bottom-flow):
      // a flow parent inserts the search panel into the layout and shoves the navbar/tabs down
      // every time it opens. The page root lets it float by align+margins with no reflow.
      var hostPanel = page;
      searchWrap = $.CreatePanel('Panel', hostPanel, 'FmSearchWrap', {});
      searchWrap.style.flowChildren = 'down';
      searchWrap.style.horizontalAlign = 'right'; searchWrap.style.verticalAlign = 'top';
      searchWrap.style.marginTop = '62px'; searchWrap.style.marginRight = '72px';
      searchWrap.style.width = '360px';
      // High zIndex so the search floats ABOVE the demo detail (the long match name
      // sits right under here and used to show through / overlap it).
      searchWrap.style.zIndex = '1000';
      // hittest follows visibility: a hidden-but-laid-out high-zIndex overlay would
      // otherwise keep eating pointer events over the area it covers.
      searchWrap.visible = false; searchWrap.hittest = false;

      searchInput = $.CreatePanel('TextEntry', searchWrap, 'FmSearchInput', { placeholder: 'Search map, player or demo name' });
      searchInput.style.width = '100%'; searchInput.style.height = '36px';
      // OPAQUE fill -- nothing behind the box should be visible through it.
      searchInput.style.backgroundColor = '#15181dff';
      searchInput.style.border = '1px solid rgba(255,255,255,0.16)';
      searchInput.style.borderRadius = '4px'; searchInput.style.color = '#eef2f6ff';
      searchInput.style.paddingLeft = '11px'; searchInput.style.paddingRight = '11px';
      searchInput.style.fontSize = '15px';
      searchInput.style.boxShadow = 'rgba(0,0,0,0.55) 0px 8px 22px 0px';
      searchInput.SetPanelEvent('ontextentrychange', function () { updateSuggestions(); });
      try { $.RegisterEventHandler('TextEntryChanged', searchInput, function () { updateSuggestions(); }); } catch (e) {}

      searchResults = $.CreatePanel('Panel', searchWrap, 'FmSearchResults', {});
      searchResults.style.flowChildren = 'down'; searchResults.style.width = '100%';
      searchResults.style.marginTop = '4px'; searchResults.style.maxHeight = '340px';
      searchResults.style.overflow = 'squish scroll';
      searchResults.style.backgroundColor = '#0e1014ff';   // fully opaque dropdown
      searchResults.style.border = '1px solid rgba(255,255,255,0.12)';
      searchResults.style.borderRadius = '4px';
      searchResults.style.boxShadow = 'rgba(0,0,0,0.6) 0px 10px 26px 0px';
      searchResults.visible = false;
    }

    function toggleSearch(page) {
      buildSearch(page);
      if (!searchWrap) return;
      var show = !searchWrap.visible;
      searchWrap.visible = show; searchWrap.hittest = show;
      if (show) { try { searchInput.SetFocus(); } catch (e) {} updateSuggestions(); }
      else { try { searchInput.text = ''; } catch (e) {} if (searchResults) searchResults.visible = false; }
    }

    // Return the matched text (for the gray sub-line) if this demo matches q, else null.
    function demoMatches(demo, q) {
      if (mapDisplay(demo.map).toLowerCase().indexOf(q) >= 0) return 'map: ' + mapDisplay(demo.map);
      if (demoName(demo).toLowerCase().indexOf(q) >= 0) return 'demo: ' + demoName(demo);
      var pl = demo.players || [];
      for (var i = 0; i < pl.length; ++i) { var n = pl[i].name || ''; if (n && n.toLowerCase().indexOf(q) >= 0) return 'player: ' + n; }
      return null;
    }

    function updateSuggestions() {
      if (!searchResults) return;
      searchResults.RemoveAndDeleteChildren();
      var q = ''; try { q = (searchInput.text || '').toLowerCase().trim(); } catch (e) {}
      if (!q) { searchResults.visible = false; return; }
      var shown = 0;
      for (var i = 0; i < demos.length && shown < 12; ++i) {
        var why = demoMatches(demos[i], q);
        if (why == null) continue;
        (function (demo, why) {
          var rowBtn = $.CreatePanel('Button', searchResults, '', {});
          rowBtn.style.flowChildren = 'down'; rowBtn.style.width = '100%';
          rowBtn.style.paddingTop = '7px'; rowBtn.style.paddingBottom = '7px';
          rowBtn.style.paddingLeft = '12px'; rowBtn.style.paddingRight = '12px';
          rowBtn.style.borderBottom = '1px solid rgba(255,255,255,0.05)';
          // Line 1: map name (accent) + cropped demo name, forced to a single line so a
          // long match730_ name can never wrap or overflow the dropdown width.
          var t = $.CreatePanel('Label', rowBtn, '', { text: mapDisplay(demo.map) + '  ·  ' + cropName(demoName(demo)) });
          t.style.color = '#eef2f6ff'; t.style.fontSize = '14px';
          t.style.whiteSpace = 'nowrap'; t.style.textOverflow = 'ellipsis'; t.style.width = '100%';
          var sub = $.CreatePanel('Label', rowBtn, '', { text: cropName(why, 30, 8) }); // gray hint of WHY it matched
          sub.style.color = '#8a93a0ff'; sub.style.fontSize = '12px';
          sub.style.whiteSpace = 'nowrap'; sub.style.textOverflow = 'ellipsis'; sub.style.width = '100%';
          rowBtn.SetPanelEvent('onmouseover', (function (r) { return function () { try { r.style.backgroundColor = 'rgba(240,179,35,0.16)'; } catch (e) {} }; })(rowBtn));
          rowBtn.SetPanelEvent('onmouseout', (function (r) { return function () { try { r.style.backgroundColor = 'rgba(0,0,0,0)'; } catch (e) {} }; })(rowBtn));
          rowBtn.SetPanelEvent('onactivate', (function (key) { return function () { revealDemo(key); }; })(tileKey(demo)));
        })(demos[i], why);
        ++shown;
      }
      searchResults.visible = shown > 0;
    }

    // Jump to a demo from a suggestion: switch to the Downloaded tab, select it, and
    // scroll its tile into view; then close the search.
    function revealDemo(key) {
      var page = watchPage();
      var dlBtn = page && downloadedButton(page);
      if (dlBtn) { try { dlBtn.checked = true; } catch (e) {} try { $.DispatchEvent('Activated', dlBtn); } catch (e) {} }
      $.Schedule(0.05, function () {
        takeoverDownloaded(page);
        selectByKey(key);
        var idx = indexOfKey(key);
        if (idx >= 0 && tilePanels[idx]) { try { tilePanels[idx].ScrollParentToMakePanelFit(3, false); } catch (e) {} }
      });
      if (searchWrap) searchWrap.visible = false;
      try { searchInput.text = ''; } catch (e) {}
      if (searchResults) searchResults.visible = false;
    }
)FMJS"
// ---- demo organiser: rail toolbar, folders, drag-to-file (own fragment) ----
#include "DemoOrganizerJs.h"
// ---- bridge API + bootstrap ----
R"FMJS(
    var api = {};
    api.addCommand = function (command) { host.SetAttributeString('cmd', host.GetAttributeString('cmd', '') + command + '\n'); };
    api.render = function () {
      var raw = host.GetAttributeString('demos', '');
      var data = null; if (raw) { try { data = JSON.parse(raw); } catch (e) {} }
      demos = (data && data.demos) || [];
      repairStatus = (data && data.repair) || null;
      if (sel >= demos.length) sel = -1;
      applyWatchEdits();   // builds/keeps content ready; never forces tab visible
      updateRepairStatus();
    };
    // Organiser payload (sort mode, folders, last-played). Pushed on its own version gate by
    // FilmmakerMenu::PushOrganizer, independently of the demo scan, so this re-renders the list
    // without waiting for a rescan.
    api.renderOrg = function () {
      orgParse();
      applyWatchEdits();
    };
    api.show = function () {
      if (typeof MainMenu !== 'undefined' && MainMenu.NavigateToTab) MainMenu.NavigateToTab('JsWatch', 'mainmenu_watch');
      var tries = 0; var tick = function () {
        if (applyWatchEdits()) {
          var page = watchPage();
          var dlBtn = page && downloadedButton(page);
          if (dlBtn) {
            // Programmatic "click" so native NavigateToTab runs (correct tab state)
            // and our Activated handler injects our content.
            try { dlBtn.checked = true; } catch (e) {}
            try { $.DispatchEvent('Activated', dlBtn); } catch (e) {}
            $.Schedule(0.05, function () { takeoverDownloaded(page); });
          }
          return;
        }
        if (++tries > 20) return;
        $.Schedule(0.1, tick);
      };
      $.Schedule(0.05, tick);
    };

    // ---- automation hooks (used by Automation\verify\verify-watch-ui.ps1) ----
    // The netcon console truncates a single command at 256 bytes, so the
    // verifier cannot ship a long ui_eval payload. These helpers keep the heavy
    // logic in the injected script (RunScript has no length limit); automation
    // drives them with short calls like  $.Filmmaker.gotoTab('downloaded').
    api.tabBodyId = function (name) {
      name = ('' + (name || '')).toLowerCase();
      if (name.indexOf('your') >= 0 || name === 'ym' || name === 'matches') return 'JsYourMatches';
      if (name.indexOf('tourn') >= 0) return 'JsTournaments';
      return 'JsDownloaded';
    };
    // Switch Watch sub-tabs the way native NavigateToTab does (toggle the
    // WatchMenu--Hide class on the tab bodies) -- the native handler lives in the
    // mainmenu_watch module, which is NOT in this script's JS scope, and a
    // synthetic 'Activated' on the navbar RadioButton only highlights it without
    // navigating. For Downloaded we then run our normal takeover so our body
    // shows and native siblings stay hidden.
    api.gotoTab = function (name) {
      var page = watchPage();
      if (!page) { $.Msg('[GOTO] nopage=1\n'); return false; }
      var targetId = api.tabBodyId(name);

      // TRIED AND REVERTED: dispatching the native 'NavigateToTab' event here (the one the real
      // Downloaded button sends) to keep mainmenu_watch's _m_activeTab bookkeeping in sync.
      // It did NOT fix the blank-page-after-automation problem and made it worse -- the native
      // handler lands asynchronously, after the class writes below, and re-applies its own idea
      // of the active tab, leaving JsDownloaded hidden while the navbar still highlights it.
      // Do not re-add without a way to sequence against that async handler.
      var dl = page.FindChildTraverse('JsDownloaded');
      var container = dl ? dl.GetParent() : null;
      if (container) {
        for (var i = 0; i < container.GetChildCount(); ++i) {
          var c = container.GetChild(i);
          if (!c.BHasClass('WatchMenu')) continue;
          if (c.id === targetId) c.RemoveClass('WatchMenu--Hide');
          else c.AddClass('WatchMenu--Hide');
        }
      }
      // Reflect the selection on the navbar radio buttons ([Your Matches,
      // Downloaded, Tournaments]) so the highlight matches the visible tab.
      var nav = page.FindChildTraverse('watch-navbar');
      var center = nav && nav.GetChild(0);
      if (center && center.GetChildCount() >= 2) {
        try { center.GetChild(0).checked = (targetId === 'JsYourMatches'); } catch (e) {}
        try { center.GetChild(1).checked = (targetId === 'JsDownloaded'); } catch (e) {}
      }
      if (targetId === 'JsDownloaded') takeoverDownloaded(page);
      $.Msg('[GOTO] ' + targetId + '\n');
      return true;
    };
    // Emit a single parseable [VERIFY] line describing the live tab + body state
    // (mirrors what verify-watch-ui.ps1 used to inline as one giant ui_eval).
    api.report = function () {
      var page = watchPage();
      if (!page) { $.Msg('[VERIFY] nopage=1\n'); return; }
      var dl = page.FindChildTraverse('JsDownloaded');
      var ym = page.FindChildTraverse('JsYourMatches');
      var b = page.FindChildTraverse('FmDownloadedBody');
      var list = page.FindChildTraverse('FmMatchList');
      // Count TILES, not children: the organiser interleaves folder headers into this same
      // flow, so GetChildCount() and GetChild(0) are no longer "the demos" and "the first
      // demo". Probing child 0 for a native tile reported styledTile=0 the moment a folder
      // existed, which looked like the native layout had stopped loading.
      var kids = list ? list.GetChildCount() : -1;
      var tiles = 0, first = null;
      for (var t = 0; t < kids; ++t) {
        var kid = list.GetChild(t);
        if (('' + kid.id).indexOf('FmOrgHead') === 0) continue;
        ++tiles;
        if (!first) first = kid;
      }
      if (kids < 0) tiles = -1;
      var styled = first ? (first.FindChildTraverse('mapname') ? 1 : 0) : -1;
      var sb = (b && b.FindChildTraverse('Scoreboard')) ? 1 : 0;
      var ctT = b && b.FindChildTraverse('players-table-CT');
      var tT = b && b.FindChildTraverse('players-table-TERRORIST');
      var rows = b ? ((ctT ? ctT.GetChildCount() : 0) + (tT ? tT.GetChildCount() : 0)) : -1;
      $.Msg('[VERIFY] dlHide=' + (dl && dl.BHasClass('WatchMenu--Hide') ? 1 : 0) +
        ' ymHide=' + (ym && ym.BHasClass('WatchMenu--Hide') ? 1 : 0) +
        ' bodyExists=' + (b ? 1 : 0) + ' tiles=' + tiles + ' styledTile=' + styled +
        ' scoreboard=' + sb + ' playerRows=' + rows + '\n');
    };
    // Step-5 helper: select the 2nd tile and tag the 1st so the verifier can
    // prove rows are not recreated (tag survives -> scroll preserved) under churn.
    api.tagFirst = function (tag) {
      var page = watchPage(); var l = page && page.FindChildTraverse('FmMatchList');
      if (!l) { $.Msg('[SEL] nolist\n'); return; }
      if (l.GetChildCount() > 1) { var t = l.GetChild(1); try { t.checked = true; $.DispatchEvent('Activated', t); } catch (e) {} }
      if (l.GetChildCount() > 0) l.GetChild(0).SetAttributeInt('fmtag', tag | 0);
      $.Msg('[SEL] done\n');
    };
    // Step-5 helper: emit list-stability snapshot (tile count / first-row tag / selection).
    //
    // `checked` is the checked panel's DOM index. That is only a proxy for "which demo is
    // selected", and it stopped being a faithful one once the organiser began rendering tiles
    // in SORT order while tilePanels stayed indexed by position in `demos` -- the DOM index of
    // a given demo now moves whenever the sort or the folder layout changes, with the selection
    // itself untouched. `selKey` is the actual invariant the step is trying to assert: the
    // stable identity (folder|fileName) of the selected demo. Compare that.
    api.stab = function () {
      var page = watchPage(); var l = page && page.FindChildTraverse('FmMatchList');
      var n = l ? l.GetChildCount() : -1;
      var tiles = 0;
      if (l) for (var t = 0; t < n; t++) { if (('' + l.GetChild(t).id).indexOf('FmOrgHead') !== 0) ++tiles; }
      if (n < 0) tiles = -1;
      var tag = -1;
      if (l) for (var f = 0; f < n; f++) {
        var kid = l.GetChild(f);
        if (('' + kid.id).indexOf('FmOrgHead') === 0) continue;
        tag = kid.GetAttributeInt('fmtag', -1);
        break;
      }
      var ck = -1; if (l) for (var i = 0; i < n; i++) { if (l.GetChild(i).checked) { ck = i; break; } }
      // Spaces would break the key=value split the verifier does; the identity only has to be
      // comparable across snapshots, not readable.
      var key = (selKey == null) ? 'none' : ('' + selKey).replace(/\s+/g, '_');
      $.Msg('[STAB] tiles=' + tiles + ' tag=' + tag + ' checked=' + ck + ' sel=' + sel + ' selKey=' + key + '\n');
    };
    // Step-6 helper: emit nav-button presence/visibility/shared-row report.
    api.btnReport = function () {
      var page = watchPage();
      var search = page && page.FindChildTraverse('FmSearchBtn');
      var ref = page && page.FindChildTraverse('FmRefreshBtn');
      // The add control now lives in the LEFT LIST RAIL, not the navbar, so it is reported
      // here but deliberately excluded from the shared-navbar-row check below.
      var add = page && page.FindChildTraverse('FmOrgAddBtn');
      var clear = page && page.FindChildTraverse('FmClearAllBtn');
      var bar = page && page.FindChildTraverse('FmNavBtns');
      function firstChildHittestOff(p) {
        var im = p && p.GetChildCount && p.GetChildCount() > 0 ? p.GetChild(0) : null;
        return im && im.hittest === false ? 1 : 0;
      }
      var same = (bar && search && ref && clear && search.GetParent() === bar && ref.GetParent() === bar && clear.GetParent() === bar) ? 1 : 0;
      $.Msg('[BTN] search=' + (search ? 1 : 0) + ' searchVis=' + (search && search.visible ? 1 : 0) +
        ' searchImgNoHit=' + firstChildHittestOff(search) +
        ' ref=' + (ref ? 1 : 0) + ' refVis=' + (ref && ref.visible ? 1 : 0) +
        ' refImgNoHit=' + firstChildHittestOff(ref) +
        ' add=' + (add ? 1 : 0) + ' addVis=' + (add && add.visible ? 1 : 0) +
        ' addImgNoHit=' + firstChildHittestOff(add) +
        ' clear=' + (clear ? 1 : 0) + ' clearVis=' + (clear && clear.visible ? 1 : 0) +
        ' clearImgNoHit=' + firstChildHittestOff(clear) + ' sameRow=' + same + '\n');
    };
    // Organiser state report (rail toolbar present, active sort, folder headers, collapsed
    // tiles). Built in DemoOrganizerJs.h so the reporting lives with the thing it reports on.
    api.orgReport = function () { $.Msg(orgReportLine()); };
    // Collapse/expand the Nth folder header. Numeric arg: netcon mangles string literals.
    api.orgToggle = function (index) { return orgToggleHeader(index); };
    // Open the new-folder block (what the rail's "+" menu does) and report whether the field
    // actually took keyboard focus.
    api.orgNewFolder = function () { return orgOpenNewFolderUi(); };
    // Select the demo whose file name contains `part` (what a tile click does) and report the
    // Watch button. Netcon-friendly: synthetic clicks never reach Panorama.
    api.selectFile = function (part) {
      for (var i = 0; i < demos.length; i++)
        if (String(demos[i].fileName || '').indexOf(part) >= 0) { selectDemo(i); break; }
      api.watchReport();
    };
    api.watchReport = function () {
      var d = (sel >= 0 && sel < demos.length) ? demos[sel] : null;
      $.Msg('[WATCH] sel=' + (d ? d.fileName : '-') + ' compat=' + (d ? d.compatibility : '-') +
        ' busy=' + (watchBusy ? 1 : 0) + ' unavailable=' + (watchUnavailable ? 1 : 0) +
        ' jobBusy=' + (repairJobBusy ? 1 : 0) + ' btn=' + (watchBtn ? (watchBtn.IsValid() ? 'valid' : 'dead') : 'none') +
        ' enabled=' + (watchBtn && watchBtn.IsValid() && watchBtn.enabled ? 1 : 0) + '\n');
    };

    $.Filmmaker = api;

    var watchNav = root3.FindChildTraverse('MainMenuNavBarWatch');
    if (watchNav) {
      applyWatchIcon();
      watchNav.SetPanelEvent('onactivate', function () {
        if (typeof MainMenu !== 'undefined' && MainMenu.NavigateToTab) MainMenu.NavigateToTab('JsWatch', 'mainmenu_watch');
        var tries = 0; var tick = function () { if (applyWatchEdits() || ++tries > 20) return; $.Schedule(0.1, tick); };
        $.Schedule(0.05, tick);
      });
    }

    applyWatchEdits();
    orgParse();   // seed sort/folders before the first list render, so nothing reshuffles after
    api.render();
    $.Msg('[filmmaker] native Watch tab hooked (demos=' + demos.length + ').');
  } catch (err) {
    $.Msg('[filmmaker] gui error: ' + err);
  }
})();
)FMJS";

} // namespace Filmmaker
