// WHERE DO THE GAME'S KILL MARKERS SIT, and does that agree with the time they stand for?
//
// Reported: clicking a kill marker seeks to a point where the player is ALREADY DEAD, roughly five
// seconds late. The Regular Timeline widens the game's scrubber (#TimeScrubContainer from its own
// 80% to 100%, #scrubInfo to zero width) so the bar fills the dock band -- so the first suspicion is
// that the markers were placed against the ORIGINAL track width and no longer line up with the
// slider they sit on.
//
// This measures rather than assumes: every .EventMarker's laid-out x, the track it is in, and the
// slider's own value, all in the same space. The marker ids carry the event's time (the game names
// them kill_event_<n> / death_event_<n>), so the mapping can be checked end to end.
//
//   mirv_deadlock livejs load <abs>/Automation/livejs/deadlock-probe-markers.js
(function () {
  var TAB = String.fromCharCode(9);
  function out(s) { $.Msg('MARK' + TAB + s + '\n'); }
  try {
    var ctx = $.GetContextPanel();
    function find(id) { try { return ctx.FindChildTraverse(id); } catch (e) { return null; } }
    var sx = ctx.actualuiscale_x || 1;

    // Window-space x of a panel, so every number below is in one space.
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
    function box(label, p) {
      if (!p) { out(label + TAB + 'MISSING'); return null; }
      var w = 0, h = 0;
      try { w = (p.actuallayoutwidth || 0) / sx; h = (p.actuallayoutheight || 0) / sx; } catch (e) {}
      var s = label + TAB + 'x=' + winX(p).toFixed(1) + ' w=' + w.toFixed(1) + ' h=' + h.toFixed(1);
      try { s += ' style.width=' + p.style.width; } catch (e) {}
      out(s);
      return { x: winX(p), w: w };
    }

    var scrubber = box('ReplayScrubber', find('ReplayScrubber'));
    var slam = box('SliderAndMarkers', find('SliderAndMarkers'));
    box('scrubInfo', find('scrubInfo'));
    var cont = box('TimeScrubContainer', find('TimeScrubContainer'));
    var slider = find('TimeScrubSlider');
    var sbox = box('TimeScrubSlider', slider);
    if (slider) {
      var s = 'TimeScrubSlider' + TAB + 'value=';
      try { s += slider.value + ' min=' + slider.min + ' max=' + slider.max; } catch (e) { s += 'ERR'; }
      out(s);
      // The thumb is where the engine thinks NOW is, so it is the ground truth for the mapping.
      var thumb = null;
      try { thumb = slider.FindChildTraverse('SliderThumb'); } catch (e) {}
      var tb = box('SliderThumb', thumb);
      if (tb && sbox && sbox.w > 0) {
        out('thumbFrac' + TAB + ((tb.x - sbox.x) / sbox.w).toFixed(4));
      }
    }
    var mc = find('MarkersContainer');
    var mbox = box('MarkersContainer', mc);

    // Every marker, with the fraction of the TRACK it is drawn at. If the ids carry ticks, the two
    // columns can be compared directly.
    if (mc) {
      var track = cont || mbox;
      function walk(p, depth) {
        var n = 0;
        try { n = p.GetChildCount ? p.GetChildCount() : 0; } catch (e) {}
        for (var i = 0; i < n; ++i) {
          var c = null;
          try { c = p.GetChild(i); } catch (e) {}
          if (!c) continue;
          var id = '';
          try { id = c.id || ''; } catch (e) {}
          var w = 0;
          try { w = (c.actuallayoutwidth || 0) / sx; } catch (e) {}
          var isMarker = /_event_|EventMarker/.test(id);
          var cls = '';
          try { cls = (c.BHasClass && c.BHasClass('EventMarker')) ? 'EventMarker' : ''; } catch (e) {}
          if (isMarker || cls) {
            var x = winX(c);
            var frac = (track && track.w > 0) ? ((x - track.x) / track.w) : -1;
            out('marker' + TAB + (id || '(anon)') + TAB + 'x=' + x.toFixed(1)
                + ' w=' + w.toFixed(1) + ' frac=' + frac.toFixed(4)
                + ' class=' + cls);
          }
          if (depth < 6) walk(c, depth + 1);
        }
      }
      walk(mc, 0);
    }

    // What the engine says the demo's span is, so a marker's id can be turned into a fraction.
    try {
      var st = ($.MvmWorkspace && $.MvmWorkspace.state) ? $.MvmWorkspace.state() : $.__mvmLastState;
      if (st) out('state' + TAB + 'tick=' + st.tick + ' time=' + st.time + ' startTick=' + st.startTick + ' endTick=' + st.endTick);
    } catch (e) {}
  } catch (err) {
    out('error' + TAB + err);
  }
})();
