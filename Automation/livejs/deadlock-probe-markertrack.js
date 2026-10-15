// WHICH box are the kill markers positioned inside?
//
// Measured: the slider's thumb maps value/max linearly across the full 398px track, but the event
// markers span only ~92% of it (a perfect straight line, x = 0.0051325*tick - 1.7). So the markers
// are laid out against a DIFFERENT box from the one the slider maps clicks onto. This walks from a
// marker up to the scrubber printing every ancestor's width, margins and classes, so the box can be
// named instead of inferred.
//
//   mirv_deadlock livejs load <abs>/Automation/livejs/deadlock-probe-markertrack.js
(function () {
  var TAB = String.fromCharCode(9);
  function out(s) { $.Msg('MT' + TAB + s + '\n'); }
  try {
    var ctx = $.GetContextPanel();
    var sx = ctx.actualuiscale_x || 1;
    var mc = ctx.FindChildTraverse('MarkersContainer');
    if (!mc) { out('no MarkersContainer'); return; }

    function winX(p) {
      var x = 0, q = p;
      for (var i = 0; q && i < 40; ++i) {
        var v = 0;
        try { v = q.actualxoffset || 0; } catch (e) {}
        if (Math.abs(v) < 1e9) x += v;
        try { q = q.GetParent ? q.GetParent() : null; } catch (e) { q = null; }
      }
      return x / sx;
    }
    function desc(p) {
      var s = '';
      try { s += (p.id || '(anon)'); } catch (e) { s += '?'; }
      try { s += ' <' + (p.paneltype || 'Panel') + '>'; } catch (e) {}
      try { s += ' x=' + winX(p).toFixed(1) + ' w=' + ((p.actuallayoutwidth || 0) / sx).toFixed(1); } catch (e) {}
      // Everything that could shift or shrink the box the markers are placed in.
      var props = ['width', 'marginLeft', 'marginRight', 'paddingLeft', 'paddingRight', 'position', 'transform', 'horizontalAlign'];
      for (var i = 0; i < props.length; ++i) {
        var v = null;
        try { v = p.style[props[i]]; } catch (e) {}
        if (v !== null && v !== undefined && v !== '' && v !== 'null') s += ' ' + props[i] + '=' + v;
      }
      // The classes the game's own stylesheet keys off (.MarkerTrack, .ReplayMarkerRange, ...).
      var classes = ['MarkerTrack', 'ReplayMarkerRange', 'OverwatchMarkerRange', 'Marker', 'EventMarker', 'kill_event', 'death_event'];
      var has = [];
      for (var c = 0; c < classes.length; ++c) {
        try { if (p.BHasClass && p.BHasClass(classes[c])) has.push(classes[c]); } catch (e) {}
      }
      if (has.length) s += ' class=[' + has.join(' ') + ']';
      return s;
    }

    // Find one marker, then walk up to the scrubber.
    var marker = null;
    (function findMarker(p, d) {
      if (marker || d > 8) return;
      var n = 0;
      try { n = p.GetChildCount ? p.GetChildCount() : 0; } catch (e) {}
      for (var i = 0; i < n && !marker; ++i) {
        var c = null;
        try { c = p.GetChild(i); } catch (e) {}
        if (!c) continue;
        var id = '';
        try { id = c.id || ''; } catch (e) {}
        if (/_event_/.test(id)) { marker = c; return; }
        findMarker(c, d + 1);
      }
    })(mc, 0);

    if (!marker) { out('no marker found under MarkersContainer'); return; }
    out('CHAIN, marker first:');
    var q = marker;
    for (var i = 0; q && i < 10; ++i) {
      out('  ' + i + TAB + desc(q));
      var id = '';
      try { id = q.id || ''; } catch (e) {}
      if (id === 'ReplayScrubber') break;
      try { q = q.GetParent ? q.GetParent() : null; } catch (e) { q = null; }
    }

    // And MarkersContainer's whole subtree one level down, to see the wrappers.
    out('MarkersContainer children:');
    var n2 = 0;
    try { n2 = mc.GetChildCount(); } catch (e) {}
    for (var j = 0; j < n2; ++j) {
      var c2 = null;
      try { c2 = mc.GetChild(j); } catch (e) {}
      if (!c2) continue;
      var kids = 0;
      try { kids = c2.GetChildCount(); } catch (e) {}
      out('  ' + j + TAB + desc(c2) + ' children=' + kids);
    }
  } catch (err) {
    out('error' + TAB + err);
  }
})();
