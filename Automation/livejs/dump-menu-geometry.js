// Read-only geometry capture for the native menu and shared mod controls.
(function () {
  var root = $.GetContextPanel();
  while (root.GetParent()) root = root.GetParent();
  var sx = root.actualuiscale_x || 1, sy = root.actualuiscale_y || 1;
  $.Msg('MENUGEOM ' + JSON.stringify({ viewport: [root.actuallayoutwidth / sx, root.actuallayoutheight / sy], scale: [sx, sy] }) + '\n');
  var ids = ['MainMenuNavBar', 'JsMainMenuContent', 'JsWatchContent', 'watch-navbar', 'FmDownloadedBody',
    'FmMatchList', 'FmListToolbar', 'FmInfo', 'FmMatchInfo', 'FmDemoTitle', 'Scoreboard', 'meta',
    'players-table__labels-row', 'players-table-CT', 'players-table-TERRORIST', 'id-mi-player-stats',
    'id-mi-round-stats', 'id-mi-round-stats__tick-labels', 'MatchTileMenu', 'WorkshopSearchBar',
    'MvmWorkshopFilters', 'MvmWsSort', 'MvmWsHide', 'gameModeButtonContainer_workshop'];
  function row(p) {
    var pos = p.GetPositionWithinWindow();
    return { id: p.id, type: p.paneltype, x: pos.x / sx, y: pos.y / sy,
      w: p.actuallayoutwidth / sx, h: p.actuallayoutheight / sy, visible: p.visible,
      classes: p.GetClasses ? p.GetClasses() : [], text: p.text || '' };
  }
  ids.forEach(function (id) {
    var p = root.FindChildTraverse(id); if (!p) return;
    $.Msg('MENUGEOM ' + JSON.stringify(row(p)) + '\n');
    for (var i = 0; i < Math.min(3, p.GetChildCount()); i++) $.Msg('MENUGEOM ' + JSON.stringify(row(p.GetChild(i))) + '\n');
  });
})();
