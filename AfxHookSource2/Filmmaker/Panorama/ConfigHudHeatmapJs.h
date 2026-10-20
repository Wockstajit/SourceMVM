#pragma once

// SCENE OBJECTS > Round heat map (Docs/features/round-heatmap.md): a switch at the top of the card,
// above its tabs, so it works from the Outliner and from Assets & Lights. It paints the floor red
// where the players walked during the round being watched, around the camera, so placed objects can
// go where nobody walks. A placement aid only: recordings never show it. Backend:
// Filmmaker/Heatmap/RoundHeatmap.cpp (`mirv_filmmaker heatmap`, state in st.heatmap).

R"CFJS(
    // ===================== SCENE OBJECTS: Round heat map =====================
    var hmBox = mk('Panel', envSceneSec);
    hmBox.hittest = false; hmBox.hittestchildren = true;
    hmBox.style.width = '100%'; hmBox.style.flowChildren = 'down';
    function hmCommand(t) { cmd('mirv_filmmaker heatmap ' + t + ' quiet'); }
    var hmOn = false;
    var hmToggle = togglePill(hmBox, 'Round heat map', 'Shows where players walked this round',
      function () { hmCommand(hmOn ? 'off' : 'on'); },
      'Paints the floor red where players walked during this round, around the camera. Brighter red means more people went there. ' +
      'Nothing is painted where nobody walked, so you can place objects there. Recordings never show it.');
    endTileLine(hmBox);
    var hmStatus = lbl(hmBox, '', S.hint, 12);
    hmStatus.hittest = false;
    hmStatus.style.marginLeft = '2px'; hmStatus.style.marginBottom = '2px';
    hmStatus.visible = false;
    var hmMore = mk('Panel', hmBox);
    hmMore.hittest = false; hmMore.hittestchildren = true;
    hmMore.style.width = '100%'; hmMore.style.flowChildren = 'down';
    hmMore.visible = false;
    var hmRadius = sliderRow(hmMore, { label: 'Area', min: 256, max: 4096, decimals: 0, unit: 'u',
      tip: 'How far around the camera the floor is painted, in game units. It follows the camera as you move.',
      onChange: function (v) { hmCommand('radius ' + v); } });
    var hmTeams = modeRow(hmMore, 'Players', [['all', 'Everyone'], ['t', 'T'], ['ct', 'CT']], '236px',
      function (k) { hmCommand('teams ' + k); }, 'Whose paths are painted.', 13);

    function hmStatusText(h) {
      if (!h.on) return '';
      if (h.state === 'nodemo') return 'Open a demo to see where its players walked.';
      if (h.state === 'loading') return 'Reading the rounds from the demo...';
      if (h.state === 'error') return 'This demo could not be read. ' + (h.error || '');
      if (h.state === 'noround') return 'No round has started yet at this point of the demo.';
      if (h.state !== 'ready') return '';
      var where = 'Round ' + h.round + ' of ' + h.rounds;
      return h.players === 0 ? where + '. Nobody walked near the camera.' : where + '.';
    }
    renderHooks.push(function (st) {
      var h = (st && st.heatmap) || {};
      hmOn = !!h.on;
      hmToggle.set(hmOn);
      if (hmMore.visible !== hmOn) hmMore.visible = hmOn;
      var text = hmStatusText(h);
      if (hmStatus.text !== text) hmStatus.text = text;
      if (hmStatus.visible !== (text !== '')) hmStatus.visible = text !== '';
      if (hmOn) {
        hmRadius.sync(typeof h.radius === 'number' ? h.radius : 1500);
        hmTeams.sync(h.teams || 'all');
      }
    });
)CFJS"
