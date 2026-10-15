// Toggle the active page's content while retaining CS2's native backdrop and shell.
// Run once, capture a screenshot, then run again to restore. Never saves preferences.
(function () {
  var root = $.GetContextPanel(); while (root.GetParent()) root = root.GetParent();
  var saved = root.__mvmArtworkCapture;
  if (saved) {
    saved.forEach(function (item) { if (item.panel.IsValid()) item.panel.style.opacity = item.opacity; });
    root.__mvmArtworkCapture = null; $.Msg('MENUARTWORK restored\n'); return;
  }
  saved = [];
  ['JsSettings', 'JsPlay', 'JsWatchContent'].forEach(function (id) {
    var panel = root.FindChildTraverse(id);
    if (panel && panel.visible) { saved.push({ panel: panel, opacity: panel.style.opacity || '1' }); panel.style.opacity = '0'; }
  });
  root.__mvmArtworkCapture = saved;
  $.Msg('MENUARTWORK hidden; run this file again after the screenshot to restore\n');
})();
