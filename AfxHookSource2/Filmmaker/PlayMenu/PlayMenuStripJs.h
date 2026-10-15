#pragma once

// Fragment 1 of the Play-page script: the $.MvmPlay namespace, the hidden host panel that
// carries the C++<->JS bridge attributes, the self-heal tick, and the tab strip itself.
//
// WHY A TICK AND NOT A ONE-SHOT: mainmenu_play.xml is created and destroyed as the user
// navigates the main menu, and CS2 restores the saved server setting (which may be Official
// Matchmaking) every time it comes back. A single pass at inject time would be undone by the
// first navigation. The tick is cheap: it caches PlayTypeTopNav and only re-traverses the
// panel tree when that cached panel stops being valid.
//
// WHAT IT DOES NOT DO: it never replaces a native onactivate handler. The tabs keep CS2's own
// handlers, so PlayMenu's internal bookkeeping (m_serverSetting, the map-group containers)
// stays correct; we only hide panels, relabel one, and -- when the page lands on the hidden
// Official tab -- dispatch Activated on Practice exactly the way a click would.

namespace Filmmaker {

inline const char* kPlayMenuStripJs = R"FMJS(
(function () {
  try {
    function topRootOf(p) { var r = p; while (r && r.GetParent && r.GetParent()) r = r.GetParent(); return r; }
    var ctx = $.GetContextPanel();
    var root = topRootOf(ctx);

    // Sweep EVERYTHING a previous injection left behind, by id prefix.
    //
    // Anything we parent into a NATIVE container outlives the next injection -- deleting our own
    // host does not reach it -- and the fresh fragment, whose reference to it is null, builds a
    // second one. That is how `mirv_filmmaker play rebuild` produced two workshop sidebars.
    //
    // Three things this got wrong first, all measured with $.MvmPlay.dupes():
    //   * `FindChildTraverse(id)` returns only the FIRST match, so a single named sweep cannot
    //     clear a page that already has three.
    //   * A registry of live panel references on `$` does not survive: each injection round gets
    //     a fresh context, so the list read back empty and swept nothing.
    //   * A hard-coded id list here would have to know every panel every other fragment creates,
    //     which is exactly the coupling the fragments exist to avoid.
    // A prefix walk has none of those problems: at this point in the round nothing of ours has
    // been created yet, so every 'Mvm' panel in the tree is by definition a leftover.
    var doomed = [];
    (function collect(p) {
      try {
        if (p.id && p.id.substring(0, 3) === 'Mvm') { doomed.push(p); return; }  // children go with it
        for (var i = 0; i < p.GetChildCount(); ++i) collect(p.GetChild(i));
      } catch (e) {}
    })(root);
    for (var d = 0; d < doomed.length; ++d) {
      try { doomed[d].DeleteAsync(0); } catch (e) {}
    }

    var menuContent = root.FindChildTraverse('JsMainMenuContent');
    var host = $.CreatePanel('Panel', menuContent || ctx, 'MvmPlayMenuHost', {});
    host.visible = false; host.style.width = '0px'; host.style.height = '0px';

    var M = {};
    $.MvmPlay = M;
    M.host = host;
    M.root = root;
    M.ticks = 0;
    M.hooks = [];          // extra per-tick work, registered by later fragments
    M.strips = 0;          // how many times we bounced the page off Official Matchmaking

    // JS -> C++ command queue. FilmmakerMenu-style: newline separated, drained and cleared by
    // the C++ pump every frame.
    M.cmd = function (line) {
      try { host.SetAttributeString('cmd', host.GetAttributeString('cmd', '') + line + '\n'); } catch (e) {}
    };

    // ---- panel helpers -------------------------------------------------------------------
    // PlayTypeTopNav is the whole top navbar; everything the strip touches lives inside it, so
    // caching it turns a full-tree traversal per tick into a validity check.
    var navCache = null;
    M.nav = function () {
      try { if (navCache && navCache.IsValid()) return navCache; } catch (e) { navCache = null; }
      try { navCache = root.FindChildTraverse('PlayTypeTopNav'); } catch (e) { navCache = null; }
      return navCache;
    };
    M.find = function (id) { try { return root.FindChildTraverse(id); } catch (e) { return null; } };
    M.hide = function (p) {
      if (!p) return;
      try { p.visible = false; } catch (e) {}
      try { p.enabled = false; } catch (e) {}
    };
    // The Practice tab's caption is an unnamed <Label> child of the RadioButton, so it has to
    // be found by shape. GameModeAlert (the notification dot) is the other child and must not
    // be mistaken for it.
    M.labelOf = function (p) {
      if (!p) return null;
      try {
        for (var i = 0; i < p.GetChildCount(); ++i) {
          var c = p.GetChild(i);
          if (!c || c.id === 'GameModeAlert') continue;
          if (c.paneltype === 'Label') return c;
        }
      } catch (e) {}
      return null;
    };

    // Clicks a native button for real. The THIRD argument is not optional: native
    // mainmenu.js:999 dispatches ('Activated', panel, 'mouse'), and without the source string
    // the event does not reach the panel's onactivate. Setting `checked` alone only moves the
    // radio highlight -- CS2's own _UpdateServerSettingBtns relies on that NOT firing the
    // handler -- so a strip that only set checked would leave PlayMenu's internal
    // m_serverSetting pointing at the tab we just hid.
    function activate(p) {
      if (!p) return false;
      try { p.checked = true; } catch (e) {}
      try { $.DispatchEvent('Activated', p, 'mouse'); } catch (e) { return false; }
      return true;
    }
    M.activate = activate;

    // ---- the strip -----------------------------------------------------------------------
    function applyStrip() {
      var nv = M.nav();
      if (!nv) return false;

      var off = nv.FindChildTraverse('Play-official');
      var com = nv.FindChildTraverse('PlayCommunity');
      var lis = nv.FindChildTraverse('Play-listen');

      M.hide(off);
      M.hide(com);

      if (lis) {
        var lbl = M.labelOf(lis);
        if (lbl) { try { if (lbl.text !== 'Practice') lbl.text = 'Practice'; } catch (e) {} }
      }

      // CS2 restores the last used server setting on every visit. If that is Official the page
      // would open on a tab we just hid, leaving the user staring at matchmaking controls with
      // no way back. Dispatch the same event a click sends so PlayMenu.OnPressListenServers()
      // runs and its bookkeeping stays in sync. Guarded on off.checked, which our own dispatch
      // clears -- so this fires once per landing, not once per tick.
      if (off && lis) {
        var onOfficial = false;
        try { onOfficial = !!off.checked; } catch (e) {}
        if (onOfficial) {
          M.strips++;
          activate(lis);
        }
      }
      return true;
    }
    M.applyStrip = applyStrip;

    // ---- tick ----------------------------------------------------------------------------
    // STOPS WHEN THIS INJECTION'S HOST DIES. That is what makes a rebuild produce one set of
    // panels instead of two.
    //
    // $.Schedule loops are not tied to the script that started them: a re-injection leaves the
    // PREVIOUS round's tick running forever, holding its own `openBtn` / `bar` references. The
    // sweep at the top of the new round deletes those panels, the old loop sees its references
    // die, and it dutifully rebuilds them -- a second workshop sidebar, one more per rebuild.
    // (Measured: 1, then 2, then 3.)
    //
    // The sweep also deletes the old HOST, and the old loop closes over it, so "is my host still
    // alive" is the one question that distinguishes this round from a dead one -- no counter or
    // generation id needed, and none would survive anyway (each injection gets a fresh context).
    function tick() {
      var mine = false;
      try { mine = !!(host && host.IsValid()); } catch (e) { mine = false; }
      if (!mine) return;   // superseded by a newer injection: stop, do not reschedule

      M.ticks++;
      try { applyStrip(); } catch (e) {}
      for (var i = 0; i < M.hooks.length; ++i) {
        try { M.hooks[i](); } catch (e) {}
      }
      $.Schedule(0.5, tick);
    }
    $.Schedule(0.1, tick);

    // ---- shared modal plumbing -------------------------------------------------------------
    // Two things every overlay on this page needs, learned the hard way from shipping one
    // without them:
    //
    //   1. A CLICK-EATING BACKDROP. A floating panel does not stop input reaching what is behind
    //      it, so clicking "Head" in a settings modal also clicked the map tile underneath and
    //      the page navigated out from under the user.
    //   2. A PARENT THAT CARRIES THE MENU STYLESHEET. Panorama cascades styles from the layout
    //      file that owns a subtree, so a panel parented to the TOP ROOT gets no text styling at
    //      all and every Label renders in the default red-on-nothing fallback. Parent under the
    //      same panel the host lives in (JsMainMenuContent) and the menu's own styles apply.
    //      The cost is that the friends list, which owns its own stacking context, can still
    //      overlap the right edge; that is cosmetic, whereas unreadable text is not.
    //
    // The backdrop is a Button rather than a Panel because only a Button reliably swallows the
    // click, and giving it a close handler makes click-outside-to-dismiss fall out for free.
    M.overlayParent = function () {
      try {
        var p = M.host.GetParent();
        if (p) return p;
      } catch (e) {}
      return M.root;
    };
    // Every panel any fragment creates MUST have an id starting with 'Mvm' -- that prefix is what
    // the sweep above finds. A panel created with '' for an id is invisible to it and becomes the
    // next duplicate.
    //
    // netcon diagnostic: every panel of ours currently in the tree, with a count. One of each is
    // correct; a 2 means an injection left a copy behind. Short enough to drive over netcon,
    // which truncates an eval at 256 bytes -- the walk has to live here, not in the command.
    M.dupes = function () {
      var seen = {};
      function walk(p) {
        try {
          if (p.id && p.id.substring(0, 3) === 'Mvm') seen[p.id] = (seen[p.id] || 0) + 1;
          for (var i = 0; i < p.GetChildCount(); ++i) walk(p.GetChild(i));
        } catch (e) {}
      }
      walk(M.root);
      var out = '[mvmpanels]';
      for (var k in seen) if (seen.hasOwnProperty(k)) out += ' ' + k + '=' + seen[k];
      return out + '\n';
    };

    M.makeOverlay = function (id, width) {
      var parent = M.overlayParent();
      var back = $.CreatePanel('Button', parent, id + 'Back', {});
      back.style.width = '100%';
      back.style.height = '100%';
      back.style.backgroundColor = 'rgba(0,0,0,0.72)';
      back.style.zIndex = '298';
      back.visible = false;

      var panel = $.CreatePanel('Panel', parent, id, {});
      panel.style.zIndex = '299';
      panel.style.horizontalAlign = 'center';
      panel.style.verticalAlign = 'middle';
      panel.style.width = width || '640px';
      panel.style.padding = '22px';
      panel.style.flowChildren = 'down';
      panel.style.backgroundColor = '#0d1116';
      panel.style.border = '1px solid #ffffff40';
      panel.visible = false;

      var overlay = { back: back, panel: panel };
      back.SetPanelEvent('onactivate', function () { M.showOverlay(overlay, false); });
      return overlay;
    };
    M.showOverlay = function (overlay, show) {
      if (!overlay) return false;
      try { overlay.panel.visible = show; } catch (e) {}
      try { overlay.back.visible = show; } catch (e) {}
      return show;
    };
    M.overlayVisible = function (overlay) {
      try { return !!(overlay && overlay.panel.visible); } catch (e) { return false; }
    };
    M.overlayAlive = function (overlay) {
      try { return !!(overlay && overlay.panel.IsValid() && overlay.back.IsValid()); }
      catch (e) { return false; }
    };

    // Explicit checked styling. The native gamemode-setting-radiobutton class draws a small tick
    // box that is legible in a sidebar of five rows but not in a modal of twenty controls -- at
    // a glance there was no telling which option in a row was the active one.
    M.markChecked = function (btn, on) {
      try { btn.checked = !!on; } catch (e) {}
      try {
        btn.style.backgroundColor = on ? '#3d7ebf' : 'rgba(255,255,255,0.05)';
        btn.style.color = on ? '#ffffff' : 'rgba(255,255,255,0.60)';
      } catch (e) {}
    };

    // ---- netcon navigation ---------------------------------------------------------------
    // CS2's console tokenizer rewrites a quoted string inside an eval ('Activated' comes out as
    // ' Activated ' and DispatchEvent rejects it), so live checks cannot pass string literals.
    // These wrap the navigation a check needs so the eval carries no quotes at all:
    //   mirv_filmmaker play eval $.MvmPlay.gotoPlay()
    //   mirv_filmmaker play eval $.MvmPlay.gotoWorkshop()
    M.gotoPlay = function () { return activate(M.find('MainMenuNavBarPlay')); };
    M.gotoWorkshop = function () {
      var nv = M.nav();
      return activate(nv ? nv.FindChildTraverse('PlayWorkshop') : null);
    };
    M.gotoPractice = function () {
      var nv = M.nav();
      return activate(nv ? nv.FindChildTraverse('Play-listen') : null);
    };

    // ---- netcon report -------------------------------------------------------------------
    // ui_eval truncates at 256 bytes, so live checks go through this instead of inline JS:
    //   mirv_filmmaker play eval $.Msg($.MvmPlay.report())
    M.report = function () {
      var nv = M.nav();
      var off = nv ? nv.FindChildTraverse('Play-official') : null;
      var lis = nv ? nv.FindChildTraverse('Play-listen') : null;
      var com = nv ? nv.FindChildTraverse('PlayCommunity') : null;
      var lbl = M.labelOf(lis);
      return '[mvmplay] ticks=' + M.ticks + ' nav=' + (nv ? 1 : 0) +
             ' offVis=' + (off ? (off.visible ? 1 : 0) : '-') +
             ' comVis=' + (com ? (com.visible ? 1 : 0) : '-') +
             ' lis=' + (lbl ? lbl.text : '-') +
             ' bounced=' + M.strips + '\n';
    };
  } catch (e) {
    try { $.Msg('[mvmplay] strip inject failed: ' + e + '\n'); } catch (e2) {}
  }
})();
)FMJS";

} // namespace Filmmaker
