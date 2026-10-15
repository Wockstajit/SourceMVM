// Does Deadlock's replay transport appear at the bar's NATIVE width?
//
// Everything else is ruled out: the layout is right, nothing is hidden, no opacity, no transform,
// no clip, and the blit is suppressed. What is left is that widening the bar is itself what breaks
// it -- `CitadelHudReplay` is a C++ panel and may render from a surface sized by its own stylesheet
// (272px), in which case our inline width moves the layout and not the paint.
//
// So this hands the width back and watches. If the transport appears -- small, flush right -- then
// the bar works at its own size and the hosting has to keep it there and place our group beside it
// rather than stretching it.
//
// Run with: mirv_deadlock livejs load <abs path to this file>

(function () {
  function find(id) {
    var root = $.GetContextPanel();
    if (!root) return null;
    try { return root.FindChildTraverse(id); } catch (e) { return null; }
  }

  var bar = find('hud_replay');
  if (!bar) { $.Msg('NATIVEW\tno hud_replay\n'); return; }

  // Clear the test fills first, or a magenta band will mask what we are looking for.
  var fills = ['PlaybackControls', 'MainPlaybackControls', 'PlayButton'];
  for (var i = 0; i < fills.length; ++i) {
    var f = find(fills[i]);
    if (!f) continue;
    try { f.style.backgroundColor = null; } catch (e) {}
  }

  try {
    bar.style.width = '272px';
    bar.style.horizontalAlign = 'right';
    $.Msg('NATIVEW\tbar handed back to 272px, right-aligned\n');
  } catch (e) {
    $.Msg('NATIVEW\tTHREW setting the native width\n');
  }

  $.Schedule(0.5, function () {
    var ids = ['hud_replay', 'PlaybackControls', 'MainPlaybackControls', 'PlayButton',
               'ReplayScrubber', 'MvmBarButtons'];
    for (var j = 0; j < ids.length; ++j) {
      var p = find(ids[j]);
      if (!p) continue;
      var x = '?', y = '?', w = '?', h = '?';
      try {
        var sx = p.actualuiscale_x || 1, sy = p.actualuiscale_y || 1;
        x = Math.round(p.actualxoffset / sx);
        y = Math.round(p.actualyoffset / sy);
        w = Math.round(p.actuallayoutwidth / sx);
        h = Math.round(p.actuallayoutheight / sy);
      } catch (e) {}
      $.Msg('NATIVEBOX\t' + ids[j] + '\t' + x + ',' + y + ' ' + w + 'x' + h + '\n');
    }
    $.Msg('NATIVEW\tdone\n');
  });
})();
