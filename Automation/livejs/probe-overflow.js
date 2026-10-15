// CONFIG panel layout-overflow probe (development-only diagnostic).
//
//   mirv_filmmaker config on
//   mirv_filmmaker livejs load <abs path to this file>
//
// Expands every collapsible card first -- a collapsed body is invisible and would be
// skipped, so an unexpanded run can report a clean 0 and mean nothing -- then walks
// ConfigHudRoot and reports any panel whose right edge passes its parent's width.
//
// Expected output on a healthy panel:  [ovf] AFTER-EXPAND overflowing=0
//
// This caught the card-body width bug: `width: 100%` resolves against the parent's
// BORDER box in this host, so a full-width child of a card with 11px padding was laid
// out at x=13 and still given the card's full width, hanging 13px past the right edge
// and clipping pills, cycle buttons and slider readouts. Fix: width: fill-parent-flow(1.0).
//
// Authored with its own try/catch -- a thrown error is otherwise silent (UiLiveReload.cpp).
(function () {
  try {
    var root = $('#ConfigHudRoot');
    if (!root) { $.Msg('[ovf] no ConfigHudRoot (is "mirv_filmmaker config on" active?)\n'); return; }

    var opened = 0;
    function expand(p, d) {
      if (!p || d > 12) return;
      try {
        if (typeof p.__setOpen === 'function') { p.__setOpen(true); opened++; }
        for (var i = 0; i < p.GetChildCount(); i++) expand(p.GetChild(i), d + 1);
      } catch (e) {}
    }
    expand(root, 0);
    $.Msg('[ovf] expanded cards: ' + opened + '\n');

    // One layout pass for the expansion to take effect before measuring.
    $.Schedule(0.15, function () {
      try {
        var hits = [];
        function nameOf(c, i) {
          var n = '';
          try { n = c.id || ''; } catch (e) {}
          if (!n) { try { n = c.paneltype || '?'; } catch (e2) { n = '?'; } }
          try { if (c.text) n += ':' + ('' + c.text).substring(0, 16); } catch (e3) {}
          return n + '[' + i + ']';
        }
        function walk(p, path, depth) {
          if (depth > 14) return;
          var W = 0, n = 0;
          try { W = p.actuallayoutwidth || 0; n = p.GetChildCount(); } catch (e) { return; }
          for (var i = 0; i < n; i++) {
            var c = null;
            try { c = p.GetChild(i); } catch (e2) { continue; }
            if (!c) continue;
            var cw = 0, cx = 0, vis = true;
            try { cw = c.actuallayoutwidth || 0; cx = c.actualxoffset || 0; vis = c.visible; } catch (e3) {}
            var label = nameOf(c, i);
            if (vis && W > 0 && cw > 0 && (cx + cw) - W > 1.0) {
              hits.push(path + '/' + label + ' OVER=' + ((cx + cw) - W).toFixed(1)
                + ' (x=' + cx.toFixed(0) + ' w=' + cw.toFixed(0) + ' parentW=' + W.toFixed(0) + ')');
            }
            if (vis) walk(c, path + '/' + label, depth + 1);
          }
        }
        walk(root, 'root', 0);
        $.Msg('[ovf] AFTER-EXPAND overflowing=' + hits.length + '\n');
        for (var k = 0; k < hits.length && k < 30; k++) $.Msg('[ovf] ' + hits[k] + '\n');
        if (hits.length > 30) $.Msg('[ovf] (+' + (hits.length - 30) + ' more)\n');
      } catch (e2) { $.Msg('[ovf] ERR2 ' + e2 + '\n'); }
    });
  } catch (e) { $.Msg('[ovf] ERR ' + e + '\n'); }
})();
