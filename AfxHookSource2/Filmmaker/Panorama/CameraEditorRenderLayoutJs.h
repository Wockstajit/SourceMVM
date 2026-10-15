#pragma once

// Render-workspace geometry and input isolation. Kept separate from CameraEditorRenderJs.h so the
// workspace shell, the three-column split and the spectator-click boundary do not become coupled
// to the individual Render tabs and controls.
//
// LAYOUT (replaces the earlier preview-on-top / full-width-dock-below arrangement, which squeezed
// the preview into the upper half and gave the controls a very wide, very short strip):
//
//   +-----------+----------------------------+-------------+
//   |   JOB     |          PREVIEW           |   RENDER    |
//   |  (left)   |     (centre, as large as   |  settings   |
//   |  summary  |      the columns allow)    |  inspector  |
//   |  + START  |                            |  (right)    |
//   +-----------+----------------------------+-------------+
//   | [ Regular Timeline | Camera | Graph ]          [gear]|
//   +-------------------------------------------------------+
//   | bottom editor: native demo bar / camera timeline /    |
//   | graph editor -- its height moves the columns' bottom  |
//   +-------------------------------------------------------+
//
// The settings column deliberately matches the Camera Editor's inspector width so switching
// between the two workspaces does not move the whole screen. Both columns run the full height
// above the bottom tab bar, so changing the bottom editor (which changes tabY) rescales the
// preview and both columns together.

R"EDJS(
    var RENDER_JOB_W = 340;                 // left column: job summary + start/cancel
    var RENDER_SETTINGS_W = INSPECTOR_W;    // right column: matches the editor inspector exactly

    var renderRoot = $.CreatePanel('Panel', ctx, 'CamEditorRenderRoot', {});
    renderRoot.visible = false; renderRoot.hittest = false;
    renderRoot.style.width = '100%'; renderRoot.style.height = '100%';
    renderRoot.style.zIndex = '54';

    function makeRenderShield() {
      var p = mk('Panel', renderRoot); p.visible = false; p.hittest = false;
      p.style.backgroundColor = S.clear;
      p.SetPanelEvent('onactivate', function () { /* swallow non-preview spectator clicks */ });
      return p;
    }
    // Created BEFORE the columns so the columns stay interactive above the shields.
    var renderShieldTop = makeRenderShield();
    var renderShieldLeft = makeRenderShield();
    var renderShieldRight = makeRenderShield();
    var renderShieldBelow = makeRenderShield();

    // A workspace column: opaque, absolutely positioned, own line-map wash.
    // NO border on the preview-facing edge. A 1px rule there renders as a bright vertical seam
    // running the full height against the live image, which reads as a rendering artefact rather
    // than structure -- the columns are already separated from the preview by the black letterbox.
    function makeRenderColumn(borderSide) {
      var col = mk('Panel', renderRoot); col.hittest = true;
      col.style.horizontalAlign = 'left'; col.style.verticalAlign = 'top';
      col.style.backgroundColor = 'rgba(15,17,20,1)';
      col.style.flowChildren = 'down';
      col.SetPanelEvent('onactivate', function () { /* swallow clicks inside the column */ });
      // CS2's own GO-button treatment: a dim animated line map. ignoreParentFlow so it never
      // steals layout space from the controls above it.
      var tex = mk('Panel', col); tex.hittest = false;
      tex.style.ignoreParentFlow = 'true';
      tex.style.width = '100%'; tex.style.height = '100%';
      tex.style.backgroundImage = 'url("file://{resources}/videos/linemap.webm")';
      tex.style.backgroundRepeat = 'repeat'; tex.style.backgroundPosition = '20% 60%';
      tex.style.backgroundSize = 'auto auto';
      // About a quarter of the original 7.5%: in the side columns the line map is texture, not decoration.
      tex.style.backgroundImgOpacity = '0.02';
      tex.style.washColor = S.render; tex.style.brightness = '0.48';
      return col;
    }
    var renderJobCol = makeRenderColumn('right');
    var renderSettingsCol = makeRenderColumn('left');

    function placeRenderShield(panel, x, y, w, h, active) {
      w = Math.max(0, Math.floor(w)); h = Math.max(0, Math.floor(h));
      panel.style.position = Math.floor(x) + 'px ' + Math.floor(y) + 'px 0px';
      panel.style.width = w + 'px'; panel.style.height = h + 'px';
      panel.visible = active && w > 0 && h > 0;
      panel.hittest = panel.visible;
    }

    // tabTop  -- y of the bottom tab bar; both columns and the preview area end here.
    // px0..py1 -- the preview rect the editor layout solved for, in root pixels.
    function layoutRenderWorkspace(tabTop, rootWidth, cursorOn, px0, py0, px1, py1) {
      if (!finitePositive(rootWidth) || !finitePositive(tabTop)) return;
      var h = Math.max(120, Math.floor(tabTop));
      var jobW = Math.min(RENDER_JOB_W, Math.floor(rootWidth * 0.30));
      var setW = Math.min(RENDER_SETTINGS_W, Math.floor(rootWidth * 0.34));

      renderJobCol.style.position = '0px 0px 0px';
      renderJobCol.style.width = jobW + 'px';
      renderJobCol.style.height = h + 'px';

      // Full height, deliberately. Sizing this column to its content was tried and reverted: the
      // area it vacated is the workspace's black letterbox, so a short tab left a hard-edged black
      // rectangle in the corner that read as a hole rather than as breathing room. Both columns
      // stay full-height slabs flanking the preview; the emptiness inside is handled by the
      // content's own rhythm, not by shrinking the frame.
      renderSettingsCol.style.position = Math.floor(rootWidth - setW) + 'px 0px 0px';
      renderSettingsCol.style.width = setW + 'px';
      renderSettingsCol.style.height = h + 'px';

      // Only clicks INSIDE the preview rect may reach CS2's spectator switching; everything else
      // in the band between the columns is swallowed. The columns themselves are hit-tested
      // panels, so the shields only have to cover the gaps around the preview.
      var active = !!cursorOn;
      placeRenderShield(renderShieldTop, jobW, 0, rootWidth - setW - jobW, py0, active);
      placeRenderShield(renderShieldLeft, jobW, py0, px0 - jobW, py1 - py0, active);
      placeRenderShield(renderShieldRight, px1, py0, (rootWidth - setW) - px1, py1 - py0, active);
      placeRenderShield(renderShieldBelow, jobW, py1, rootWidth - setW - jobW, h - py1, active);

      root.SetAttributeString('renderhit',
        'cursor=' + (active ? 1 : 0) +
        ' preview=' + Math.floor(px0) + ',' + Math.floor(py0) + ',' +
        Math.floor(px1) + ',' + Math.floor(py1) +
        ' cols=' + jobW + ',' + setW + ' tabTop=' + h);
    }
)EDJS"
