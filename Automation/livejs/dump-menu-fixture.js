// Read-only menu data capture. Netcon records one JSON object per line; no game state changes.
// Load in the menu context and save the response with cs2-netcon.ps1 -LogPath.
(function () {
  var root = $.GetContextPanel();
  while (root.GetParent()) root = root.GetParent();
  function emit(kind, data) {
    var json = JSON.stringify(data).replace(/[\u007f-\uffff]/g, function (c) {
      return '\\u' + ('0000' + c.charCodeAt(0).toString(16)).slice(-4);
    });
    for (var offset = 0; offset < json.length; offset += 1000)
      $.Msg('MENUFIXTURE ' + JSON.stringify({ kind: kind, offset: offset, end: offset + 1000 >= json.length,
        chunk: json.substring(offset, offset + 1000) }) + '\n');
  }
  var host = root.FindChildTraverse('FilmmakerMenuTab');
  if (host) {
    var payload = JSON.parse(host.GetAttributeString('demos', '{}'));
    // The bridge limits netcon replies. Preserve every list entry and scoreboard, with
    // round data on the first full match for the initial comparison state.
    for (var d = 0; d < (payload.demos || []).length; d++) {
      if (d === 1) continue;
      var players = payload.demos[d].players || [];
      for (var k = 0; k < players.length; k++) players[k].perRound = [];
      payload.demos[d].hasRounds = false;
    }
    emit('demos', payload);
  }
  var ws = root.FindChildTraverse('MvmPlayMenuHost');
  if (ws) emit('workshop', JSON.parse(ws.GetAttributeString('ws', '{}')));
  if (typeof WorkshopAPI !== 'undefined') emit('maps', WorkshopAPI.GetAvailableWorkshopMaps());
  if (host) emit('organizer', JSON.parse(host.GetAttributeString('org', '{}')));
  if (typeof MyPersonaAPI !== 'undefined') emit('persona', { xuid: MyPersonaAPI.GetXuid() });
  var tiles = root.FindChildTraverse('gameModeButtonContainer_workshop');
  if (tiles) {
    for (var i = 0; i < tiles.GetChildCount(); i++) {
      var p = tiles.GetChild(i), title = p.FindChildTraverse('MapGroupName');
      emit('tile', { id: p.GetAttributeString('addon', ''), map: p.GetAttributeString('mapname', ''),
        title: title ? title.text : '', description: p.GetAttributeString('data-tooltip', ''),
        modes: p.GetAttributeString('data-workshop-modes', '') });
    }
  }
  var sx = root.actualuiscale_x || 1, sy = root.actualuiscale_y || 1;
  emit('viewport', { width: root.actuallayoutwidth / sx, height: root.actuallayoutheight / sy, sx: sx, sy: sy });
})();
