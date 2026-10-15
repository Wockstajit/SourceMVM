// WHICH PANEL IS WRITING STYLES EVERY FRAME.
//
// A style write that lands can allocate a D3D11 vertex buffer, and Panorama indexes those with an
// unsigned short, so a panel restyled every frame kills the game with
// `CVertexBufferDx11 ... EnsureCapacity allocation count overflow` -- after first freezing parts of
// the screen on an old still, which reads as a rendering bug rather than as an error.
//
// $.MvmStyle.set memoises, so in a steady frame the tally should be ZERO. This resets it, lets the
// game run for a second, and prints what actually wrote.
//
// Run with: mirv_deadlock livejs load <abs path to this file>

(function () {
  var M = $.MvmStyle;
  if (!M || !M.styleStats) { $.Msg('STYLEPROBE\tno tally in this build\n'); return; }

  M.styleReset();
  $.Msg('STYLEPROBE\treset; sampling one second\n');

  $.Schedule(1.0, function () {
    var s = M.styleStats(15);
    $.Msg('STYLEPROBE\ttotal=' + s.total + ' writes in ~1s\n');
    for (var i = 0; i < s.top.length; ++i) {
      $.Msg('STYLETOP\t' + s.top[i][1] + '\t' + s.top[i][0] + '\n');
    }
    $.Msg('STYLEPROBE\tdone\n');
  });
})();
