// Step 2: print the state object the workspace was last rendered with, plus the speed dial's own
// input probe, so the browser mockup can be fed the game's REAL state instead of a hand-written one.
//
//   mirv_deadlock livejs load <abs>/Automation/livejs/deadlock-stash-state.js   (first)
//   mirv_deadlock livejs load <abs>/Automation/livejs/deadlock-dump-ui-state.js
//   node UiMockupDeadlock/import-state.mjs <captured console output>
//
// The JSON is split into lines of at most 1500 characters, each prefixed STATE<tab><n><tab>, and
// joined back in order by the importer. One long line would be at the mercy of whatever the console
// path truncates at, and a truncated JSON is worse than none because it parses as something.
//
// DIAL<tab> carries the dial's last pump: the cursor it saw and the strip rect it tested it against.
// If those two are in different coordinate spaces the control is silently dead, and this line is
// how that would show.
(function () {
  var TAB = String.fromCharCode(9);
  try {
    var st = $.__mvmLastState;
    if (!st) { $.Msg('STATE' + TAB + 'none' + TAB + 'load deadlock-stash-state.js first, then wait a frame\n'); }
    else {
      var json = JSON.stringify(st);
      var CH = 1500, n = 0;
      $.Msg('STATEBEGIN' + TAB + json.length + '\n');
      for (var i = 0; i < json.length; i += CH, ++n) {
        $.Msg('STATE' + TAB + n + TAB + json.substr(i, CH) + '\n');
      }
      $.Msg('STATEEND' + TAB + n + '\n');
    }
  } catch (e) {
    $.Msg('STATE' + TAB + 'error' + TAB + e + '\n');
  }
  try {
    var d = $.MvmNativeHud && $.MvmNativeHud.dial ? $.MvmNativeHud.dial() : null;
    $.Msg('DIAL' + TAB + ($.MvmSpeedDial ? $.MvmSpeedDial.dbg(d) : 'no $.MvmSpeedDial') + '\n');
    // Does this context's panel type answer GetPositionWithinWindow at all? The dial falls back
    // to summing layout offsets if not, and the two can disagree by the HUD's own origin.
    var strip = d && d.strip;
    $.Msg('DIALAPI' + TAB + (strip ? (typeof strip.GetPositionWithinWindow) : 'no strip')
          + TAB + 'xoff=' + (strip ? strip.actualxoffset : '?') + ' yoff=' + (strip ? strip.actualyoffset : '?') + '\n');
  } catch (e) {
    $.Msg('DIAL' + TAB + 'error' + TAB + e + '\n');
  }
})();
