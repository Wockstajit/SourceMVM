// Dumps the laid-out geometry of the Deadlock workspace AND the game's own replay bar, so the
// browser mockup in UiMockupDeadlock/ can be diffed against the real game numerically rather than
// by eye.
//
//   mirv_deadlock livejs load <abs path to this file>
//
// Output is one TSV row per panel, each line prefixed GEOM<tab>, kept under the console's line
// limit. Columns:
//
//   depth  id  type  x  y  w  h  visible  text
//
// x/y/w/h are DESIGN px (device px / actualuiscale), the same 1920x1080 space the mockup's stage
// uses, so the two dumps compare directly. A panel that is not laid out reports w=h=0 rather than
// being dropped -- "present but collapsed" and "absent" are different bugs, and this port has
// already shipped one of each.
//
// THE REPLAY BAR IS DUMPED TOO, and that is the point of having a Deadlock-specific probe rather
// than reusing the CS2 one. The Regular Timeline is not a panel of ours that resembles the game's
// bar; it IS `#hud_replay`, hosted -- resized, its dead space emptied, its speed stepper hidden and
// our dial put in its place. Every one of those is a change to a panel we do not own, so the only
// way to know the mockup reproduces it is to measure the same tree in both.
//
// GEOMTRUNCATED reports traversal limits or unreadable children; GEOMEND is the emitted row count,
// not a promise that the whole tree was visited. Traversal is read-only.
//
// Paired with UiMockupDeadlock/compare-geometry.mjs, which reads this output and the mockup's own.
(function () {
  // OUR roots, then the game's. Every id is a real $.CreatePanel from the Deadlock *Js.h fragments
  // or a real panel out of citadel_hud_replay.xml.
  var OURS = ['MvmWorkspaceRoot'];
  var THEIRS = ['hud_replay', 'TooltipManager', 'ContextMenuManager'];
  var MAX_PANELS = 4000;
  var MAX_DEPTH = 64;
  var TAB = String.fromCharCode(9);

  function out(s) { $.Msg(s + '\n'); }

  try {
    var ctx = $.GetContextPanel();

    // Climb to the Panorama root. Our panels are built onto the HUD context, but the native layers
    // we scale (TooltipManager, ContextMenuManager) are SIBLINGS of HudCore rather than descendants
    // of it, so a search from the context alone misses them -- and missing them is exactly the bug
    // that left a full-size tooltip over a shrunken HUD.
    var top = ctx;
    for (var g = 0; g < 24; ++g) {
      var par = null;
      try { par = top.GetParent ? top.GetParent() : null; } catch (e) { par = null; }
      if (!par) break;
      top = par;
    }

    var sx = ctx.actualuiscale_x || 1;
    var sy = ctx.actualuiscale_y || 1;

    out('GEOMTOP' + TAB + (top.id || '?'));
    out('GEOMBEGIN' + TAB + 'uiscale=' + sx + ',' + sy +
        TAB + 'win=' + Math.round(ctx.actuallayoutwidth / sx) + 'x' + Math.round(ctx.actuallayoutheight / sy));

    function find(id) {
      try { var a = ctx.FindChildTraverse(id); if (a) return a; } catch (e) {}
      try { return top.FindChildTraverse(id); } catch (e) { return null; }
    }

    var count = 0;
    var childReads = 0;
    var truncated = {};
    function truncate(reason) {
      if (truncated[reason]) return;
      truncated[reason] = true;
      out('GEOMTRUNCATED' + TAB + reason);
    }

    // WINDOW SPACE, accumulated down the walk. actualxoffset/actualyoffset are relative to the
    // PARENT, so a dump of them alone is a tree of local offsets that cannot be compared to the
    // mockup's absolute rects -- the first diff put the game's `MvmSelDelete` at 320,0 against the
    // mockup's 1743,259 for the same panel. Each row adds its own offset to its parent's origin.
    //
    // A collapsed panel reports FLT_MAX (3.4e38) for its offsets. That is Panorama's "not laid
    // out", not a position; it is written as 0,0 with w=h=0 so the row still says the panel EXISTS,
    // which is a different fact from it being absent.
    var HUGE = 1e9;
    function row(p, depth, ox, oy) {
      count++;
      var id = '', type = '', lx = 0, ly = 0, x = 0, y = 0, w = 0, h = 0, vis = 0, text = '';
      try { id = p.id || '-'; } catch (e) { id = '?'; }
      try { type = p.paneltype || p.type || 'Panel'; } catch (e) { type = 'Panel'; }
      try { lx = (p.actualxoffset || 0) / sx; } catch (e) {}
      try { ly = (p.actualyoffset || 0) / sy; } catch (e) {}
      try { w = Math.round((p.actuallayoutwidth || 0) / sx); } catch (e) {}
      try { h = Math.round((p.actuallayoutheight || 0) / sy); } catch (e) {}
      var collapsed = !(Math.abs(lx) < HUGE && Math.abs(ly) < HUGE);
      if (collapsed) { lx = 0; ly = 0; w = 0; h = 0; }
      x = Math.round(ox + lx); y = Math.round(oy + ly);
      try { vis = (p.visible && !collapsed) ? 1 : 0; } catch (e) {}
      // Label text is the most useful non-geometric field: a wrong string is a content bug and is
      // invisible in a rect comparison.
      try {
        text = (p.text === undefined || p.text === null) ? '' : String(p.text);
        text = text.replace(/[\r\n\t]+/g, ' ').substr(0, 40);
      } catch (e) { text = ''; }
      out('GEOM' + TAB + depth + TAB + id + TAB + type + TAB + x + TAB + y + TAB + w + TAB + h +
          TAB + vis + TAB + text);

      return { x: ox + lx, y: oy + ly };
    }

    // Keep only one frame per depth. A wide panel cannot allocate an unbounded child queue,
    // and a deep/cyclic tree cannot exhaust V8's call stack. Stop fetching children at the cap.
    function walk(p, ox, oy) {
      if (count >= MAX_PANELS) { truncate('panel-limit'); return; }
      var stack = [{ p: p, depth: 0, x: ox, y: oy, entered: false }];
      while (stack.length) {
        var f = stack[stack.length - 1];
        if (!f.entered) {
          var origin = row(f.p, f.depth, f.x, f.y);
          f.x = origin.x; f.y = origin.y; f.entered = true; f.i = 0; f.n = 0;
          try { f.n = f.p.GetChildCount ? f.p.GetChildCount() : 0; } catch (e) {}
          if (f.depth >= MAX_DEPTH && f.n > 0) {
            truncate('depth-limit'); stack.pop(); continue;
          }
        }
        if (f.i >= f.n) { stack.pop(); continue; }
        if (count >= MAX_PANELS) { truncate('panel-limit'); return; }
        if (childReads >= MAX_PANELS) { truncate('child-limit'); return; }
        var c = null;
        childReads++;
        try { c = f.p.GetChild(f.i++); } catch (e) { truncate('child-read-error'); f.i = f.n; }
        if (c) stack.push({ p: c, depth: f.depth + 1, x: f.x, y: f.y, entered: false });
      }
    }

    function dump(list, label) {
      out('GEOMGROUP' + TAB + label);
      for (var i = 0; i < list.length; ++i) {
        var p = find(list[i]);
        if (!p) { out('GEOMMISSING' + TAB + list[i]); continue; }
        out('GEOMROOT' + TAB + list[i]);
        // The root's own window origin is the sum of ITS ancestors' offsets -- the replay bar sits
        // several levels under HudCore, and the mockup reports it where it is on screen.
        var ax = 0, ay = 0, anc = null;
        try { anc = p.GetParent ? p.GetParent() : null; } catch (e) { anc = null; }
        for (var g2 = 0; anc && g2 < 32; ++g2) {
          var px = 0, py = 0;
          try { px = (anc.actualxoffset || 0) / sx; py = (anc.actualyoffset || 0) / sy; } catch (e) {}
          if (Math.abs(px) < HUGE && Math.abs(py) < HUGE) { ax += px; ay += py; }
          try { anc = anc.GetParent ? anc.GetParent() : null; } catch (e) { anc = null; }
        }
        walk(p, ax, ay);
      }
    }

    dump(OURS, 'mvm');
    dump(THEIRS, 'native');
    out('GEOMEND' + TAB + count);
  } catch (err) {
    out('GEOMERR' + TAB + err);
  }
})();
