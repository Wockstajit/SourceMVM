// The bar LAYS OUT at 0..1920 and PAINTS only 1400..1920 -- a 520px stub, flush right. A panel that
// draws part of its own background is not a visibility problem, it is a CLIP: something up the
// chain is still scissoring to the box the bar had when it was the game's own right-hand widget.
//
// So this opens the clip on the bar and every ancestor and repaints the test fill. If the missing
// 1400px appear, the fix belongs in the hook next to the width; if they do not, the clip theory is
// wrong and the paint is coming from somewhere else entirely.
//
// Run with: mirv_deadlock livejs load <abs path to this file>

(function () {
  function find(id) {
    var root = $.GetContextPanel();
    if (!root) return null;
    try { return root.FindChildTraverse(id); } catch (e) { return null; }
  }

  var chain = ['hud_replay', 'SpectatorOptionsContainer', 'PlaybackControls', 'MainPlaybackControls'];
  for (var i = 0; i < chain.length; ++i) {
    var p = find(chain[i]);
    if (!p) { $.Msg('CLIP\t' + chain[i] + '\tMISSING\n'); continue; }
    try {
      p.style.overflow = 'noclip';
      $.Msg('CLIP\t' + chain[i] + '\toverflow=noclip\n');
    } catch (e) {
      $.Msg('CLIP\t' + chain[i] + '\tTHREW on overflow\n');
    }
  }

  // Repaint, so the screenshot after this is unambiguous.
  var fills = [['PlaybackControls', '#ff00ffcc'], ['MainPlaybackControls', '#0000ffaa'],
               ['PlayButton', '#00ff00ff']];
  for (var j = 0; j < fills.length; ++j) {
    var q = find(fills[j][0]);
    if (!q) continue;
    try { q.style.backgroundColor = fills[j][1]; } catch (e) {}
  }

  // And report what each one thinks its box is, in virtual pixels.
  var all = ['hud_replay', 'SpectatorOptionsContainer', 'PlaybackControls',
             'MainPlaybackControls', 'PlayButton', 'MvmBarButtons'];
  for (var k = 0; k < all.length; ++k) {
    var r = find(all[k]);
    if (!r) continue;
    var x = '?', y = '?', w = '?', h = '?';
    try {
      var sx = r.actualuiscale_x || 1, sy = r.actualuiscale_y || 1;
      x = Math.round(r.actualxoffset / sx);
      y = Math.round(r.actualyoffset / sy);
      w = Math.round(r.actuallayoutwidth / sx);
      h = Math.round(r.actuallayoutheight / sy);
    } catch (e) {}
    $.Msg('BOX\t' + all[k] + '\t' + x + ',' + y + ' ' + w + 'x' + h + '\n');
  }
  $.Msg('CLIP\tdone\n');
})();
