// Does Panorama's `visible` getter report what we wrote? The geometry probe found panels we hide with
// `panel.visible = false` (the Config bar button while Config is open, the camera body while Config
// is open, the letterbox masks) reporting visible=1 with a laid-out rect, while the screenshot shows
// them hidden. Either the getter answers something other than our flag, or the tree has stale copies.
//
//   mirv_deadlock livejs load <abs>/Automation/livejs/deadlock-probe-visible.js
(function () {
  var TAB = String.fromCharCode(9);
  var ctx = $.GetContextPanel();
  var top = ctx;
  for (var g = 0; g < 24; ++g) { var par = null; try { par = top.GetParent ? top.GetParent() : null; } catch (e) {} if (!par) break; top = par; }
  function all(root, id, out) {
    try {
      if (root.id === id) out.push(root);
      var n = root.GetChildCount ? root.GetChildCount() : 0;
      for (var i = 0; i < n; ++i) all(root.GetChild(i), id, out);
    } catch (e) {}
    return out;
  }
  var ids = ['MvmBarConfig', 'MvmBarCam', 'MvmCameraBody', 'MvmConfigBody', 'MvmMaskL', 'MvmBarButtons', 'hud_replay', 'MvmWorkspaceRoot'];
  for (var k = 0; k < ids.length; ++k) {
    var list = all(top, ids[k], []);
    $.Msg('VIS' + TAB + ids[k] + TAB + 'copies=' + list.length + '\n');
    for (var j = 0; j < list.length; ++j) {
      var p = list[j], s = '';
      try { s += ' visible=' + p.visible; } catch (e) { s += ' visible=ERR'; }
      try { s += ' IsVisible=' + (p.IsVisible ? p.IsVisible() : 'n/a'); } catch (e) { s += ' IsVisible=ERR'; }
      try { s += ' w=' + p.actuallayoutwidth + ' h=' + p.actuallayoutheight; } catch (e) {}
      try { s += ' x=' + p.actualxoffset + ' y=' + p.actualyoffset; } catch (e) {}
      try { s += ' vis-style=' + p.style.visibility; } catch (e) {}
      try { s += ' parentVisible=' + (p.GetParent() ? p.GetParent().visible : '?'); } catch (e) {}
      $.Msg('VIS' + TAB + ids[k] + TAB + j + TAB + s + '\n');
    }
  }
  // And what the workspace thinks it is showing.
  try { var st = $.MvmWorkspace.state ? $.MvmWorkspace.state() : $.__mvmLastState; $.Msg('VIS' + TAB + 'state' + TAB + 'workspace=' + (st && st.workspace) + ' open=' + (st && st.open) + '\n'); } catch (e) {}
})();
