// Do the dial's landmarks sit where the speeds they name actually are?
//
// The captions, the dividers, the centre tick and the detent all have to agree with the handle,
// which is placed logarithmically. They used to be spaced in even fifths, so they agreed only where
// the range happened to make them. This prints, for each of the five presets, where the caption is
// drawn and where the handle for that speed would go -- so the alignment is a measurement, not an
// impression.
//
//   mirv_deadlock livejs load <abs>/Automation/livejs/deadlock-probe-dial.js
(function () {
  var TAB = String.fromCharCode(9);
  function out(s) { $.Msg('DIAL' + TAB + s + '\n'); }
  try {
    var d = $.MvmNativeHud && $.MvmNativeHud.dial ? $.MvmNativeHud.dial() : null;
    if (!d) { out('no dial on the bar'); return; }
    var api = $.MvmSpeedDial;
    var W = d.W;
    out('strip W=' + W + ' H=' + d.H);

    var ctx = $.GetContextPanel();
    var sx = ctx.actualuiscale_x || 1;
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
    var x0 = winX(d.strip);

    // Each caption: where its centre is drawn, against where its own speed sits on the track.
    var speeds = [0.1, 0.25, 1, 4, 16];
    for (var i = 0; i < d.zoneLabels.length && i < speeds.length; ++i) {
      var lab = d.zoneLabels[i];
      var lx = winX(lab) - x0, lw = 0;
      try { lw = (lab.actuallayoutwidth || 0) / sx; } catch (e) {}
      var drawnCentre = (lx + lw / 2) / W;
      var trueAt = api.sliderFor(speeds[i]);
      var txt = '';
      try { txt = lab.text; } catch (e) {}
      out('zone' + TAB + txt + TAB + 'captionCentre=' + drawnCentre.toFixed(4)
          + ' speedAt=' + trueAt.toFixed(4)
          + ' offBy=' + Math.abs(drawnCentre - trueAt).toFixed(4)
          + ' (' + Math.abs(drawnCentre - trueAt) * W + 'px)');
    }

    // The centre tick must sit on 1x, not on the middle of the strip.
    if (d.centreTick) {
      var cx = winX(d.centreTick) - x0;
      out('centreTick' + TAB + 'at=' + (cx / W).toFixed(4) + ' 1xAt=' + api.sliderFor(1).toFixed(4));
    }
    // And the handle, for the speed the engine is actually at.
    if (d.handle) {
      var hx = winX(d.handle) - x0;
      out('handle' + TAB + 'at=' + (hx / W).toFixed(4) + ' curV=' + d.curV.toFixed(4)
          + ' reads=' + (d.readout ? d.readout.text : '?'));
    }
    // Round trip: every preset must come back out of speedFor(sliderFor(sp)) unchanged.
    for (var k = 0; k < speeds.length; ++k) {
      out('roundtrip' + TAB + speeds[k] + ' -> ' + api.speedFor(api.sliderFor(speeds[k])));
    }
  } catch (err) {
    out('error' + TAB + err);
  }
})();
