// Dumps the laid-out geometry of every SOURCE:MVM panel, so the browser mockup in UiMockup/ can be
// diffed against the real game numerically instead of by eye.
//
//   mirv_filmmaker livejs load <abs path to this file>
//
// Output is one TSV row per panel, each line prefixed GEOM<tab>, kept under the console's ~2048
// char line limit. Columns:
//
//   depth  id  type  x  y  w  h  visible  text
//
// x/y/w/h are DESIGN px (device px / actualuiscale), i.e. the same 1920x1080 space the mockup's
// stage uses, so the two dumps compare directly. A panel that is not laid out reports w=h=0 rather
// than being dropped -- "present but collapsed" and "absent" are different bugs.
//
// Paired with UiMockup/compare-geometry.mjs, which reads this output and the mockup's own dump.
(function () {
  // Every id here is a real $.CreatePanel('Panel', ctx, '<id>') from the *Js.h fragments -- these
  // were guessed once and four of them silently reported MISSING forever.
  var ROOTS = [
    'CamEditorRoot', 'CamEditorRenderRoot', 'CamEditorConfirmRoot', 'CamEditorDebugRoot',
    'ConfigHudRoot', 'CamTimelineRoot', 'CamTimelineRangeRoot', 'GraphExpRoot',
    'MarkerHudRoot', 'MovieHudRoot', 'AimXhairRoot', 'FxDebugHudRoot'
  ];
  var MAX_PANELS = 4000;

  function out(s) { $.Msg(s + '\n'); }

  try {
    // livejs prefers the MAIN MENU context, but every SOURCE:MVM panel lives in the HUD context.
    // Both hang off the same Panorama root, so climb to the top and search from there -- that finds
    // our roots whichever context the script happened to be loaded into.
    var ctx = $.GetContextPanel();
    var top = ctx;
    for (var g = 0; g < 24; g++) {
      var par = null;
      try { par = top.GetParent ? top.GetParent() : null; } catch (e) { par = null; }
      if (!par) break;
      top = par;
    }
    var sx = ctx.actualuiscale_x || 1;
    var sy = ctx.actualuiscale_y || 1;
    out('GEOMTOP' + String.fromCharCode(9) + (top.id || '?'));
    out('GEOMBEGIN\tuiscale=' + sx + ',' + sy + '\twin=' + (ctx.actuallayoutwidth / sx) + 'x' + (ctx.actuallayoutheight / sy));

    function find(id) {
      try { var a = ctx.FindChildTraverse(id); if (a) return a; } catch (e) {}
      try { return top.FindChildTraverse(id); } catch (e) { return null; }
    }

    // Label text is the single most useful non-geometric field: a wrong string is a content bug and
    // a clipped string is a layout bug, and both are invisible in a pure rect diff.
    function textOf(p) {
      var t = '';
      try { if (typeof p.text === 'string') t = p.text; } catch (e) {}
      if (!t) { try { t = p.GetAttributeString('text', ''); } catch (e) {} }
      t = String(t).replace(/[\t\r\n]+/g, ' ').slice(0, 60);
      // The netcon console is not UTF-8 clean -- every glyph in this UI (the gear, the close cross,
      // the middle dot) came back as '???' and every text comparison was noise. Escape anything
      // outside plain ASCII so the byte stream survives the trip and the diff means something.
      var esc = '';
      for (var ci = 0; ci < t.length; ci++) {
        var cc = t.charCodeAt(ci);
        if (cc >= 32 && cc < 127) { esc += t.charAt(ci); }
        else { esc += '\\u' + ('000' + cc.toString(16)).slice(-4); }
      }
      return esc;
    }

    var n = 0;
    function walk(p, depth) {
      if (!p || n >= MAX_PANELS) return;
      n++;
      var w = 0, h = 0, x = 0, y = 0;
      try { w = Math.round((p.actuallayoutwidth || 0) / sx * 10) / 10; } catch (e) {}
      try { h = Math.round((p.actuallayoutheight || 0) / sy * 10) / 10; } catch (e) {}
      try {
        var a = p.GetPositionWithinWindow ? p.GetPositionWithinWindow() : null;
        // An unlaid panel reports Infinity here rather than throwing, and an Infinity sailed
        // straight into the diff as a bogus "off by -Infinity px" finding.
        // An unlaid panel reports FLT_MAX (3.4e38), not Infinity, so isFinite() lets it through
        // and it lands in the diff as a nonsense delta. Anything that large is not a position.
        if (a && Math.abs(a.x) < 1e30 && Math.abs(a.y) < 1e30) {
          x = Math.round(a.x / sx * 10) / 10; y = Math.round(a.y / sy * 10) / 10;
        }
      } catch (e) {}
      var vis = 1;
      try { vis = p.visible ? 1 : 0; } catch (e) {}
      var id = '';
      try { id = p.id || ''; } catch (e) {}
      var ty = '';
      try { ty = p.paneltype || ''; } catch (e) {}

      out(['GEOM', depth, id, ty, x, y, w, h, vis, textOf(p)].join('\t'));

      var c = 0;
      try { c = p.GetChildCount ? p.GetChildCount() : 0; } catch (e) {}
      for (var i = 0; i < c; i++) {
        var kid = null;
        try { kid = p.GetChild(i); } catch (e) {}
        walk(kid, depth + 1);
      }
    }

    for (var r = 0; r < ROOTS.length; r++) {
      var root = find(ROOTS[r]);
      if (!root) { out('GEOMROOT\t' + ROOTS[r] + '\tMISSING'); continue; }
      out('GEOMROOT\t' + ROOTS[r] + '\tPRESENT');
      walk(root, 0);
    }
    out('GEOMEND\tpanels=' + n);
  } catch (e) {
    $.Msg('GEOMERR ' + (e && e.stack ? e.stack : e) + '\n');
  }
})();
