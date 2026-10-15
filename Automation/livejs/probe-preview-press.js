// Backend-only Config preview input probe. It never moves the Windows cursor or steals focus.
// Load with `mirv_filmmaker livejs context hud`, then `livejs load <absolute path>`.
// Start: `mirv_filmmaker config eval $.MvmPreviewPress(0.4,0.02,10)` (normalized preview u/v,
// hold seconds). Real backend picking/grabbing still runs; only the UI input feed is substituted.
// It logs the actual backend selection/grab and visible hint, releases, and restores render().
(function () {
  if (!$.ConfigHud || !$.ConfigHud.render) throw new Error('Open Config before loading this probe');
  var ctx = $.GetContextPanel(), top = ctx;
  for (var i = 0; i < 24 && top.GetParent(); i++) top = top.GetParent();
  var root = ctx.FindChildTraverse('ConfigHudRoot') || top.FindChildTraverse('ConfigHudRoot');
  if (!root) throw new Error('ConfigHudRoot not found');
  var original = $.ConfigHud.render, active = false;
  $.MvmPreviewPress = function (u, v, seconds) {
    if (active) throw new Error('Preview probe already active');
    u = Number(u); v = Number(v); seconds = Number(seconds);
    if (!(u >= 0 && u < 1 && v >= 0 && v < 1 && seconds > 0 && seconds <= 30))
      throw new Error('Use preview coordinates [0,1) and a hold of (0,30] seconds');
    active = true;
    var until = Date.now() + seconds * 1000, frame = 0, last = '';
    $.ConfigHud.render = function () {
      var raw = root.GetAttributeString('state', ''), s = JSON.parse(raw), down = Date.now() < until;
      var rect = root.GetAttributeString('previewrect', '').split(/\s+/).map(Number);
      if (rect.length !== 4) rect = [0, 0, 1, 1];
      var origin = root.GetPositionWithinWindow();
      s.mx = origin.x + (rect[0] + u * (rect[2] - rect[0])) * root.actuallayoutwidth;
      s.my = origin.y + (rect[1] + v * (rect[3] - rect[1])) * root.actuallayoutheight;
      s.cursor = true; s.lmb = down; s.rmb = false; s.shift = false;
      root.SetAttributeString('state', JSON.stringify(s));
      try {
        original();
        var overlay = root.FindChildTraverse('PropsEditOverlay'), hint = '';
        for (var j = 0; overlay && j < overlay.GetChildCount(); j++) {
          var child = overlay.GetChild(j);
          if (child.visible && child.text) hint = child.text;
        }
        var result = JSON.stringify({down: down, selected: s.props.selected,
          grab: s.props.grab, envEditing: s.envEditing, hint: hint});
        if (result !== last || frame === 5) $.Msg('PREVIEW_PRESS ' + result + '\n');
        last = result; frame++;
      } finally {
        root.SetAttributeString('state', raw);
        if (!down) { $.ConfigHud.render = original; active = false; }
      }
    };
  };
  $.Msg('PREVIEW_PRESS ready\n');
})();
