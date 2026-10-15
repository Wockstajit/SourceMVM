#pragma once

// Panorama JS for FX debug squares (user request 2026-07-03): small labeled squares pinned
// top-right that flash when ParticleFx confirms a swap in each effect class. Labels live
// INSIDE the square so a screenshot proves which channel fired. Built once into the HUD
// context by FxDebugHud.cpp (same pattern as MarkerHudJs.h).
//   C++ -> JS : host attribute "state" (JSON), then $.FxDebugHud.render().

namespace Filmmaker {

inline const char* kFxDebugHudJs = R"FXDBGJS(
(function () {
  try {
    var existing = $('#FxDebugHudRoot'); if (existing) existing.DeleteAsync(0);
    var ctx = $.GetContextPanel();

    var root = $.CreatePanel('Panel', ctx, 'FxDebugHudRoot', {});
    root.hittest = false;
    root.style.width = '100%';
    root.style.height = '100%';
    root.style.zIndex = '51';

    var col = $.CreatePanel('Panel', root, 'FxDebugHudCol', {});
    col.hittest = false;
    col.style.flowChildren = 'down';
    col.style.horizontalAlign = 'right';
    col.style.verticalAlign = 'top';
    col.style.marginTop = '220px'; // fallback; positionBelowDirector() overwrites each render
    col.style.marginRight = '16px';

    // Sit just under the Movie Director card (#MovieHudCardRight), not over it.
    var lastFxTop = -1, lastFxRight = -1;
    function positionBelowDirector() {
      var director = $('#MovieHudCardRight');
      var rsy = root.actualuiscale_y || 1, rsx = root.actualuiscale_x || 1;
      var rootTop = (root.actualyoffset || 0) / rsy;
      var rootLeft = (root.actualxoffset || 0) / rsx;
      var top, right;
      if (director && director.actuallayoutheight > 0) {
        var sy = director.actualuiscale_y || 1, sx = director.actualuiscale_x || 1;
        var dirTop = (director.actualyoffset || 0) / sy;
        var dirLeft = (director.actualxoffset || 0) / sx;
        var dirH = director.actuallayoutheight / sy;
        top = (dirTop - rootTop) + dirH + 8;
        var dirW = (director.actuallayoutwidth || 0) / sx;
        var rootW = (root.actuallayoutwidth || 0) / rsx;
        right = (rootW > 0) ? Math.round(rootW - (dirLeft - rootLeft) - dirW) : 16;
        if (right < 16) right = 16;
      } else {
        top = 220;
        right = 16;
      }
      top = Math.round(top);
      if (top !== lastFxTop) { col.style.marginTop = top + 'px'; lastFxTop = top; }
      if (right !== lastFxRight) { col.style.marginRight = right + 'px'; lastFxRight = right; }
    }

    var channels = [
      { key: 'litMuzzle',  label: 'MUZZLE',   dim: 'rgba(40,40,40,0.75)',  lit: '#ffe066' },
      { key: 'litTracer',  label: 'TRACER',   dim: 'rgba(40,40,40,0.75)',  lit: '#66ff99' },
      { key: 'litOnSmoke', label: 'ON-SMOKE', dim: 'rgba(50,20,50,0.75)',  lit: '#ff66ff' },
      { key: 'litModSmoke',label: 'MOD-SMK',  dim: 'rgba(20,40,50,0.75)',  lit: '#66e5ff' }
    ];

    var squares = {};
    for (var i = 0; i < channels.length; i++) {
      var ch = channels[i];
      var cell = $.CreatePanel('Panel', col, '', {});
      cell.hittest = false;
      cell.style.width = '72px';
      cell.style.height = '28px';
      cell.style.marginBottom = '4px';
      cell.style.backgroundColor = ch.dim;
      cell.style.border = '1px solid #ffffff88';
      cell.style.borderRadius = '2px';

      var lbl = $.CreatePanel('Label', cell, '', {});
      lbl.hittest = false;
      lbl.text = ch.label;
      lbl.style.width = '100%';
      lbl.style.height = '100%';
      lbl.style.color = '#ffffffff';
      lbl.style.fontSize = '10px';
      lbl.style.fontWeight = 'bold';
      lbl.style.textAlign = 'center';
      lbl.style.textShadow = '0 0 4px #000000ff';
      lbl.style.verticalAlign = 'center';

      squares[ch.key] = { sq: cell, dim: ch.dim, lit: ch.lit };
    }

    var api = {};
    api.render = function () {
      positionBelowDirector();
      var raw = root.GetAttributeString('state', '{}');
      var st; try { st = JSON.parse(raw); } catch (e) { st = {}; }
      root.visible = !!st.enabled;
      for (var k in squares) {
        if (!squares.hasOwnProperty(k)) continue;
        var ent = squares[k];
        ent.sq.style.backgroundColor = st[k] ? ent.lit : ent.dim;
      }
    };
    $.FxDebugHud = api;
  } catch (e) { $.Msg('[fxdebughud] init failed: ' + e + '\n'); }
})();
)FXDBGJS";

} // namespace Filmmaker
