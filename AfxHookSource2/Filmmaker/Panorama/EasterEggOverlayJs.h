#pragma once

// Panorama JS for the easter-egg overlay (EasterEggOverlayHud.cpp). Built ONCE into whichever
// context is up (in-game HUD, else main menu). C++ writes the "state" attribute on #MvmEggRoot
// and calls $.MvmEggs.render() every frame while something animates.
//
// Fragments (adjacent raw literals, one shared scope -- a plain `var` in one is visible in all):
//   EasterEggSparklesJs.h  E-girl mode: pastel wash on the whole context, haze, sparkle cursor
//   EasterEggHintJs.h      the rare floating hints, drawn in the workspace wordmark's
//                          "Gen X soft club" motion-blur style (WorkspaceShellJs.h)
//   EasterEggVegasJs.h     the Sony Vegas intervention: full-screen clip + big streaked text
//   EasterEggClippyJs.h    the paperclip, with an Office 97 balloon
//
// Everything here is display-only and hittest=false except Clippy's buttons, which issue
// `mirv_filmmaker egg clippy ...` console commands.

namespace Filmmaker {

inline const char* kEasterEggOverlayJs = R"EGJS(
(function () {
  try {
    var old = $('#MvmEggRoot'); if (old) old.DeleteAsync(0);
    var ctx = $.GetContextPanel();

    var root = $.CreatePanel('Panel', ctx, 'MvmEggRoot');
    root.hittest = false;
    root.style.width = '100%'; root.style.height = '100%';
    root.style.position = '0px 0px 0px';
    // Above every Source MVM root (the highest, the Customize confirm, is 260): an easter egg that
    // hides behind the editor chrome is not much of an easter egg.
    root.style.zIndex = '900';
    root.style.overflow = 'noclip noclip';

    var EGG = { pink: '#ff8fd8', hot: '#ff5fc8', lav: '#c9a7ff', sky: '#9fe6ff', mint: '#a8ffd8',
      cream: '#fff4fb', gold: '#f0b323', blue: '#80bdff' };
    function now() { return Date.now() / 1000; }
    function pick(list) { return list[Math.floor(Math.random() * list.length) % list.length]; }
    function clamp01(v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
    function cmd(c) {
      try { GameInterfaceAPI.ConsoleCommand(c); }
      catch (e) { $.Msg('[eggs] cmd failed: ' + e + '\n'); }
    }
    function layer(id, z) {
      var p = $.CreatePanel('Panel', root, id);
      p.hittest = false;
      p.style.width = '100%'; p.style.height = '100%';
      p.style.position = '0px 0px 0px';
      p.style.zIndex = z;
      p.style.overflow = 'noclip noclip';
      return p;
    }
    // CSS px of the context. actuallayoutwidth is physical pixels; style px are scaled by
    // actualuiscale (see AimCrosshairHudJs.h for the measured overshoot this avoids).
    function cssSize() {
      var sx = Number(root.actualuiscale_x), sy = Number(root.actualuiscale_y);
      if (!(sx > 0)) sx = 1;
      if (!(sy > 0)) sy = 1;
      var w = (root.actuallayoutwidth || 0) / sx, h = (root.actuallayoutheight || 0) / sy;
      if (!(w > 0 && w < 1e5)) w = 1920;
      if (!(h > 0 && h < 1e5)) h = 1080;
      return { w: w, h: h };
    }
    function blur(p, h, v) { p.style.blur = 'gaussian(' + h.toFixed(1) + ', ' + v.toFixed(1) + ', 1)'; }
    function expoOut(t) { return t >= 1 ? 1 : 1 - Math.pow(2, -11 * t); }
    function expoIn(t) { return t <= 0 ? 0 : Math.pow(2, 11 * (t - 1)); }
    function lbl(parent, text, color, size) {
      var l = $.CreatePanel('Label', parent, '');
      l.hittest = false;
      // In game a child with no alignment is CENTRED in its parent's leftover space; margins are
      // then measured from the wrong edge. Default every label to the top-left corner.
      l.style.horizontalAlign = 'left'; l.style.verticalAlign = 'top';
      l.text = text;
      l.style.color = color;
      l.style.fontSize = size + 'px';
      return l;
    }

    var st = {}, lastFrame = now(), firstRender = true;
)EGJS"
#include "EasterEggSparklesJs.h"
#include "EasterEggWorldJs.h"
#include "EasterEggHintJs.h"
#include "EasterEggVegasJs.h"
#include "EasterEggClippyJs.h"
#include "EasterEggOsuJs.h"
#include "EasterEggSnakeJs.h"
R"EGJS(
    $.MvmEggs = {
      render: function () {
        try { st = JSON.parse(root.GetAttributeString('state', '{}')); } catch (e) { return; }
        var t = now(), dt = Math.min(0.1, Math.max(0, t - lastFrame));
        lastFrame = t;
        // An event that fired before this context existed (main menu -> map) is history, not news.
        if (firstRender) {
          firstRender = false;
          hintSeen = st.hint ? st.hint.id : 0; vegasSeen = st.vegas || 0;
          baySeen = st.bay || 0; chapterSeen = st.wes ? (st.wes.chapter || 0) : 0;
        }
        // A render whose capture could include UI: nothing of ours may reach the take.
        root.visible = !st.hidden;
        // Stop movie/audio before the hidden-root early return; hiding a panel does not stop media.
        try { vegasSync(); } catch (ev) { $.Msg('[eggs] vegas sync: ' + ev + '\n'); }
        if (st.hidden) return;
        var sz = cssSize();
        try { worldFrame(t, sz); } catch (e0) { $.Msg('[eggs] world: ' + e0 + '\n'); }
        try { egirlFrame(t, dt, sz); } catch (e1) { $.Msg('[eggs] egirl: ' + e1 + '\n'); }
        try { hintFrame(t, sz); } catch (e2) { $.Msg('[eggs] hint: ' + e2 + '\n'); }
        try { vegasFrame(t, sz); } catch (e3) { $.Msg('[eggs] vegas: ' + e3 + '\n'); }
        try { clippyFrame(t, sz); } catch (e4) { $.Msg('[eggs] clippy: ' + e4 + '\n'); }
        try { osuFrame(t, dt, sz); } catch (e5) { $.Msg('[eggs] osu: ' + e5 + '\n'); }
        try { snakeFrame(t, sz); } catch (e6) { $.Msg('[eggs] snake: ' + e6 + '\n'); }
      },
      unwash: function () { setWash(false); }
    };
  } catch (e) { $.Msg('[eggs] build failed: ' + e + '\n'); }
})();
)EGJS";

} // namespace Filmmaker
