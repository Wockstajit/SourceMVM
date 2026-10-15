#pragma once

// WORKSPACE HEADER CHIP -- ONE definition, spliced into every workspace that has a header.
//
// WHY ITS OWN FILE: the CONFIG panel and the Camera Editor each grew their own chip builder
// (`chromeChip` / `edChip`) over two different button painters (`paintBtn` / `btnVisual`), and
// the two drifted into visibly different controls. Measured side by side at 3x: identical
// geometry -- 37px tall, 96px ESC EXIT box, 14px gaps -- but CONFIG painted its active MOUSE chip
// as a soft blue fill with light-blue text while the editor painted a saturated gold fill with
// gold-on-gold text. Same control, same row, two different-looking objects.
//
// Docs/panorama/panorama_ui_guide.md maintains the shared workspace contract. The original Config
// and Camera Editor designs had BYTE-IDENTICAL headers apart from the title, with quiet chips:
//
//     font-size:13px; font-weight:600; letter-spacing:1px; color:#c8d3df;
//     border:1px solid #ffffff1f; border-radius:3px; padding:8px 13px; white-space:nowrap
//
// There is NO accent fill on a header chip in either original design, and none is needed: every chip here
// states its own state in its label ("MOUSE: UI" vs "MOUSE: GAME", "GAME UI: HIDE ALL"),
// so a fill is a second, redundant encoding of something the words already say -- and the gold
// version failed contrast reading gold text on a gold ground. `UI.chip` already carries exactly
// the original design's numbers (13 / 8 / 13 / 3), and both panels already define `S.label` as #c8d3df and
// `S.btnBorder` as #ffffff1f, so this is the shared values being used consistently rather than a
// new set of constants.
//
// NOT a standalone script. It is a raw-string chunk concatenated into a host IIFE and needs these
// already in the host's scope: `S` (theme tokens) and `UI` (PanoramaUiMetricsJs metrics).
//
// The host supplies its own button factory and its own repainter, because the two panels have
// genuinely different button machinery underneath:
//
//   var chip = mvmChromeChip(head,
//     function (p, t, f) { return btn(p, t, f, S.label); },   // host's btn()
//     paintBtn,                                               // host's repainter
//     'ESC EXIT', function () { cmd('...'); });
R"CHROMEJS(
    function mvmChromeChip(parent, makeBtn, repaint, text, onClick) {
      var b = makeBtn(parent, text, onClick);
      // The header is one row with a 14px gap; the title takes the slack, so every chip pushes
      // off its left neighbour and the last one lands on the column's right edge.
      b.style.marginLeft = '14px';
      b.style.marginRight = '0px';
      b.style.borderRadius = UI.chip.radius + 'px';
      b.style.paddingTop = UI.chip.padV + 'px';
      b.style.paddingBottom = UI.chip.padV + 'px';
      b.style.paddingLeft = UI.chip.padH + 'px';
      b.style.paddingRight = UI.chip.padH + 'px';
      // Quiet bordered text, never a filled pill -- see the note above. Both painters read these
      // three as their resting colours, so the chip looks the same in either workspace.
      b.__bgBase = S.clear;
      b.__brBase = S.btnBorder;
      b.__fgBase = S.label;
      b.__lbl.style.fontSize = UI.chip.font + 'px';
      // The original design used font-weight:600. Panorama only takes NAMED weights -- a numeric one
      // throws "Invalid value for property 'font-weight'" during the build, which aborts the
      // whole panel script and leaves a half-built panel on screen. 'semibold' is 600.
      b.__lbl.style.fontWeight = 'semibold';
      b.__lbl.style.letterSpacing = '1px';
      b.__lbl.style.whiteSpace = 'nowrap';
      if (repaint) repaint(b);
      return b;
    }

    // THE FLOATING ACTION ROW (RENDER / CUSTOMIZE PLAYER) -- one definition for every workspace.
    //
    // CONFIG's geometry is shared (Docs/panorama/panorama_ui_guide.md workspace contract): the row
    // spans the column (UI.col.w minus the inset each side), RENDER is a fixed 150px and
    // CUSTOMIZE PLAYER FILLS whatever is left. The Camera Editor built its own copy of this row
    // with no row width and a 208px CUSTOMIZE button laid out from a different fragment, so the
    // two rows differed in the one thing a shared row exists to fix. Only the accent differs
    // between hosts now, and that comes from the host's own S.accent.
    //
    //   makeInteractive(panel, label, onClick, opts) -- the host's own state machine
    //   actions: [{ text, width (number px | Panorama length), onClick, press }]
    function mvmActionRow(parent, makeInteractive, actions) {
      var row = mk('Panel', parent); row.hittest = true; row.visible = false;
      row.style.horizontalAlign = 'right'; row.style.verticalAlign = 'bottom';
      row.style.marginRight = UI.action.inset + 'px'; row.style.marginBottom = '20px';
      row.style.width = (UI.col.w - UI.action.inset * 2) + 'px';
      row.style.height = UI.action.h + 'px'; row.style.flowChildren = 'right';
      row.style.zIndex = '120';
      var out = { root: row, buttons: [] };
      for (var i = 0; i < actions.length; i++) (function (a, last) {
        var p = mk('Panel', row); p.hittest = true;
        p.style.width = (typeof a.width === 'number') ? (a.width + 'px') : a.width;
        p.style.height = UI.action.h + 'px';
        p.style.borderRadius = UI.action.radius + 'px'; p.style.boxShadow = UI.action.shadow;
        p.style.marginRight = last ? '0px' : (UI.action.gap + 'px');
        var l = lbl(p, a.text, S.accent, UI.action.font);
        l.style.horizontalAlign = 'center'; l.style.verticalAlign = 'center';
        l.style.fontWeight = 'bold'; l.style.letterSpacing = '1px';
        // Each host reads only the option names it knows; the union is harmless to both.
        makeInteractive(p, l, a.onClick,
          { fg: S.accent, fgOn: S.accent, border: S.accent, accent: S.accent, press: !!a.press });
        p.__lbl = l;
        out.buttons.push(p);
      })(actions[i], i === actions.length - 1);
      return out;
    }
)CHROMEJS"
