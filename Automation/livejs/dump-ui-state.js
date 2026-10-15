// Prints the live per-frame state feed the Panorama panels read, so the browser mockup in
// UiMockup/ can be driven by a REAL moment from a demo instead of a hand-written stand-in.
//
//   mirv_filmmaker livejs context hud
//   mirv_filmmaker livejs load <abs path to this file>
//
// EVERY root is dumped, not just the biggest one: the roots do not share a feed. The Camera Editor's
// carries the camera/render/customize state; the Camera Timeline's carries `markers`, which nothing
// else has. Taking only the longest string looked right and quietly left the timeline with no keys.
//
// The console truncates a line at ~2048 bytes and a feed is ~16KB, so each goes out in numbered
// chunks: STATEBEGIN <root> <len>, then STATE <n> <slice>, then STATEEND. UiMockup/import-state.mjs
// reassembles and merges them (it checks each length, so a dropped chunk is an error rather than
// corrupt JSON).
//
// Non-ASCII is escaped because the netcon console is not UTF-8 clean -- player names and the UI's
// own glyphs would otherwise come back as '?'.
(function () {
  var CHUNK = 1500;

  function out(s) { $.Msg(s + '\n'); }

  try {
    var ctx = $.GetContextPanel();
    var top = ctx;
    for (var g = 0; g < 24; g++) {
      var par = null;
      try { par = top.GetParent ? top.GetParent() : null; } catch (e) { par = null; }
      if (!par) break;
      top = par;
    }
    function find(id) {
      try { var a = ctx.FindChildTraverse(id); if (a) return a; } catch (e) {}
      try { return top.FindChildTraverse(id); } catch (e) { return null; }
    }

    var ROOTS = ['CamEditorRoot', 'CamEditorRenderRoot', 'ConfigHudRoot', 'CamTimelineRoot',
                 'CamTimelineRangeRoot', 'GraphExpRoot', 'MarkerHudRoot', 'MovieHudRoot',
                 'AimXhairRoot', 'FxDebugHudRoot'];

    var found = 0;
    for (var i = 0; i < ROOTS.length; i++) {
      var r = find(ROOTS[i]);
      if (!r) continue;
      var raw = '';
      try { raw = r.GetAttributeString('state', ''); } catch (e) { raw = ''; }
      if (!raw) continue;
      found++;

      var esc = '';
      for (var ci = 0; ci < raw.length; ci++) {
        var cc = raw.charCodeAt(ci);
        if (cc >= 32 && cc < 127) esc += raw.charAt(ci);
        else esc += '\\u' + ('000' + cc.toString(16)).slice(-4);
      }

      out('STATEBEGIN\t' + ROOTS[i] + '\t' + esc.length);
      var n = 0;
      for (var off = 0; off < esc.length; off += CHUNK, n++) {
        out('STATE\t' + n + '\t' + esc.substr(off, CHUNK));
      }
      out('STATEEND\t' + ROOTS[i] + '\t' + n);
    }
    if (!found) out('STATEERR no root is carrying a state attribute (open a panel first)');
  } catch (e) {
    $.Msg('STATEERR ' + (e && e.stack ? e.stack : e) + '\n');
  }
})();
