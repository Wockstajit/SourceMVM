#pragma once

// Shared geometry for Config and Camera inspector controls. Painters and command handlers
// stay with their owning workspace. Requires UI from PanoramaUiMetricsJs.h.
R"ILJS(
    function mvmTileSlot(parent) {
      var line = parent.__tileLine;
      // An intervening note or group ends the row, even when created by a tab fragment.
      if (line && parent.GetChild(parent.GetChildCount() - 1) !== line) line = null;
      if (!line || line.__n >= UI.tile.perRow) {
        line = $.CreatePanel('Panel', parent, '', {}); line.hittest = false;
        line.style.flowChildren = 'right'; line.style.width = '100%';
        line.style.marginTop = UI.col.rowGap + 'px';
        line.__n = 0; line.__slots = [];
        // Reserve both columns so a trailing control never stretches wider than its peers.
        for (var i = 0; i < UI.tile.perRow; i++) {
          var slot = $.CreatePanel('Panel', line, '', {}); slot.hittest = false;
          slot.style.width = 'fill-parent-flow(1.0)'; slot.style.height = UI.tile.h + 'px';
          slot.style.verticalAlign = 'top';
          if (i < UI.tile.perRow - 1) slot.style.marginRight = UI.col.gap + 'px';
          line.__slots.push(slot);
        }
        parent.__tileLine = line;
      }
      return line.__slots[line.__n++];
    }
    function mvmTileBox(slot) {
      var tile = $.CreatePanel('Panel', slot, '', {});
      tile.style.width = '100%'; tile.style.height = '100%';
      tile.style.flowChildren = 'down'; tile.style.verticalAlign = 'top';
      tile.style.paddingTop = UI.tile.padTop + 'px'; tile.style.paddingBottom = UI.tile.padBottom + 'px';
      tile.style.paddingLeft = UI.tile.padH + 'px'; tile.style.paddingRight = UI.tile.padH + 'px';
      tile.style.borderRadius = UI.tile.radius + 'px';
      return tile;
    }
    function mvmActionGroup(parent) {
      parent.__tileLine = null;
      var group = $.CreatePanel('Panel', parent, '', {}); group.hittest = false;
      group.style.flowChildren = 'down'; group.style.width = '100%';
      // Proximity and a caption group commands; a second bordered card only adds an inset.
      group.style.marginTop = UI.group.before + 'px';
      return group;
    }
)ILJS"
