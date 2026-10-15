#pragma once

// Panorama JS for the true-aim crosshair. Built ONCE into the in-game HUD (CSGOHud) context.
// Display-only: C++ sets the "state" attribute then calls $.AimXhair.render().
//
// LAYOUT: one full-screen, transparent, hittest=false container (#AimXhairRoot) holding a
// single #AimXhairReticle group that is MOVED each frame. Four arms + optional dot are built
// once and only the container's position changes, so a per-frame update is one style write
// rather than a DOM rebuild.
//
// C++ projects true aim into normalised screen coordinates. Panorama maps those coordinates to
// the HUD panel's actual layout dimensions.

namespace Filmmaker {

inline const char* kAimCrosshairHudJs = R"AXJS(
(function () {
  try {
    var existing = $('#AimXhairRoot'); if (existing) existing.DeleteAsync(0);
    var ctx = $.GetContextPanel();

    var root = $.CreatePanel('Panel', ctx, 'AimXhairRoot');
    root.style.width = '100%'; root.style.height = '100%';
    root.style.position = '0px 0px 0px';
    root.style.zIndex = '400';
    // Never eat input: this sits over the whole screen and the demo controls live underneath.
    root.hittest = false;
    // The containers below are deliberately ZERO-SIZED so the arms can be laid out symmetrically
    // around the origin with plain offsets. Panorama's default overflow (squish) CLIPS children
    // to the parent box, which on a 0x0 parent means nothing draws at all -- every one of these
    // needs noclip or the whole reticle is invisible.
    root.style.overflow = 'noclip noclip';

    // Full-screen drawing surface. GMod draws the four lines directly around ToScreen(). Doing
    // the same here avoids converting between Panorama CSS sizes and actual-layout coordinates.
    var ret = $.CreatePanel('Panel', root, 'AimXhairReticle');
    ret.style.width = '100%'; ret.style.height = '100%';
    ret.style.position = '0px 0px 0px';
    ret.hittest = false;

    function mkArm(id) {
      var p = $.CreatePanel('Panel', ret, id);
      p.style.position = '0px 0px 0px';
      p.hittest = false;
      return p;
    }
    var armL = mkArm('AimXhairL'), armR = mkArm('AimXhairR');
    var armU = mkArm('AimXhairU'), armD = mkArm('AimXhairD');
    var dot  = mkArm('AimXhairDot');

    // Faint centre reference: where the camera actually points. The gap between this and the
    // reticle IS the aim error the effects are introducing.
    var ctr = $.CreatePanel('Panel', root, 'AimXhairCentre');
    ctr.style.width = '100%'; ctr.style.height = '100%';
    ctr.style.position = '0px 0px 0px';
    ctr.hittest = false;
    var ctrH = $.CreatePanel('Panel', ctr, 'AimXhairCentreH');
    var ctrV = $.CreatePanel('Panel', ctr, 'AimXhairCentreV');
    ctrH.hittest = false; ctrV.hittest = false;
    ctrH.style.position = '0px 0px 0px'; ctrV.style.position = '0px 0px 0px';

    var lastKey = '';
    var dbg = { px: -1, py: -1, w: 0, h: 0, vis: 0, built: 0, armw: 0, armh: 0, rootvis: 0 };

    $.AimXhair = {
      // Quote-free diagnostics: the netcon console strips quotes, so a probe cannot use a
      // string selector. Returns one packed number that says whether the panels are laid out
      // and where the reticle ended up.
      dbg: function (i) {
        return [dbg.built, dbg.w, dbg.h, dbg.px, dbg.py, dbg.vis, dbg.armw, dbg.armh, dbg.rootvis][i];
      },
      render: function () {
        // C++ writes "state" onto #AimXhairRoot (the panel it located by id), NOT onto the
        // context panel -- reading it from ctx yields {} forever and nothing ever draws.
        var st;
        try { st = JSON.parse(root.GetAttributeString('state', '{}')); } catch (e) { return; }

        if (!st.on || !st.valid) { root.visible = false; return; }
        root.visible = true;
        // Diagnostic: paints the whole root translucent red. If the screen does NOT go red while
        // this is on, the container is not reaching the screen at all (wrong parent, covered, or
        // zero layout) and no amount of reticle styling will help -- that distinction is
        // otherwise invisible, because a correctly-positioned reticle and an unrendered one look
        // exactly the same.
        root.style.backgroundColor = st.debugbg ? 'rgba(255,0,0,0.25)' : 'rgba(0,0,0,0)';

        var w = root.actuallayoutwidth, h = root.actuallayoutheight;
        dbg.built = 1; dbg.w = Math.round(w || 0); dbg.h = Math.round(h || 0);
        if (!(w > 0 && h > 0)) return;

        // actuallayoutwidth/height are physical layout pixels, but style.position's `px` values
        // are Panorama CSS pixels and get multiplied by actualuiscale_x/y. Feeding (w/2,h/2)
        // directly into style.position therefore overshoots down/right whenever UI scale != 1
        // (at 1600x1200, (800,600) visibly landed near (889,662)). Convert the drawing surface
        // to CSS coordinates first; percentage-derived aim positions then land in physical
        // screen space after Panorama applies its scale.
        var scaleX = Number(root.actualuiscale_x), scaleY = Number(root.actualuiscale_y);
        if (!(scaleX > 0)) scaleX = 1;
        if (!(scaleY > 0)) scaleY = 1;
        var cssW = w / scaleX, cssH = h / scaleY;

        // sx/sy are normalised screen fractions in [0,1] (sx: 0=left, 1=right; sy: 1=up).
        var sx = st.sx, sy = st.sy;
        if (!isFinite(sx) || !isFinite(sy)) { ret.visible = false; return; }
        // Config and Camera Editor publish the exact normalised rectangle used by both the D3D
        // world blit and native HUD scaler. Map into that same rectangle, and scale the reticle
        // geometry with it, so this overlay behaves like part of the game HUD rather than a
        // full-screen editor overlay.
        var vx = 0, vy = 0, vw = cssW, vh = cssH, uiScale = 1;
        if (st.hudScale && st.view && st.view.length >= 4) {
          var x0 = Number(st.view[0]), y0 = Number(st.view[1]);
          var x1 = Number(st.view[2]), y1 = Number(st.view[3]);
          if (isFinite(x0) && isFinite(y0) && isFinite(x1) && isFinite(y1) && x1 > x0 && y1 > y0) {
            vx = x0 * cssW; vy = y0 * cssH; vw = (x1 - x0) * cssW; vh = (y1 - y0) * cssH;
            uiScale = Math.min(x1 - x0, y1 - y0);
          }
        }

        // sy=1 is UP while screen y grows DOWNWARD, hence the flip on the vertical only.
        var px = vx + sx * vw;
        var py = vy + (1.0 - sy) * vh;

        // Behind the camera, or so far off-axis the projection is meaningless: hide rather than
        // smear the reticle across the edge of the screen.
        if (!st.front || px < vx - vw || px > vx + 2 * vw || py < vy - vh || py > vy + 2 * vh) { ret.visible = false; }
        else {
          ret.visible = true;
          ret.style.position = '0px 0px 0px';
        }
        dbg.built = 1; dbg.w = Math.round(w); dbg.h = Math.round(h);
        dbg.px = Math.round(px); dbg.py = Math.round(py); dbg.vis = ret.visible ? 1 : 0;
        // Measured size of one arm: distinguishes "styled but never laid out" (0) from
        // "laid out and simply not visible on screen".
        dbg.armw = Math.round(armR.actuallayoutwidth || 0);
        dbg.armh = Math.round(armR.actuallayoutheight || 0);
        dbg.rootvis = root.visible ? 1 : 0;

        ctr.visible = !!st.centre;

        var size = st.size * uiScale, gap = st.gap * uiScale, t = st.thick * uiScale, col = st.col;
        var half = t / 2;
        // Draw directly around the projected point, just like the Lua's surface.DrawLine calls.
        // Four position writes per frame are cheap and remove every parent-box scaling ambiguity.
        function place(p, x, y, pw, ph) {
          p.style.width = pw + 'px'; p.style.height = ph + 'px';
          p.style.backgroundColor = col;
          p.style.position = x.toFixed(1) + 'px ' + y.toFixed(1) + 'px 0px';
        }
        place(armL, px - gap - size, py - half, size, t);
        place(armR, px + gap,        py - half, size, t);
        place(armU, px - half,       py - gap - size, t, size);
        place(armD, px - half,       py + gap,        t, size);

        dot.visible = !!st.dot;
        if (st.dot) place(dot, px - half, py - half, t, t);

        var cc = 'rgba(255,255,255,0.35)';
        var centreX = vx + vw * 0.5, centreY = vy + vh * 0.5;
        ctrH.style.width = '11px'; ctrH.style.height = '1px'; ctrH.style.backgroundColor = cc;
        ctrH.style.position = (centreX - 5) + 'px ' + centreY + 'px 0px';
        ctrV.style.width = '1px'; ctrV.style.height = '11px'; ctrV.style.backgroundColor = cc;
        ctrV.style.position = centreX + 'px ' + (centreY - 5) + 'px 0px';
      }
    };
  } catch (e) {}
})();
)AXJS";

} // namespace Filmmaker
