#pragma once

// SHARED UI METRICS for every filmmaker Panorama workspace (camera editor, config, camera timeline,
// graph editor, test HUD).
//
// WHY THIS FILE EXISTS
// Five scripts had each grown their own copy of the same widget recipes, and the copies drifted:
//
//     script                     btn() padding V/H   label
//     Config                          7 / 12          15
//     CameraEditor                    6 / 11          14
//     CameraTimeline, GraphEditor     4 / 9           13
//
// The clearest symptom: CameraEditorJs.h and ConfigHudJs.h both contain the *identical* line
// `btn(head, '✕ Exit', ...)` and it rendered at two different sizes, because the two `btn()`s it
// resolved to were different functions. Same control, same call, two looks.
//
// WHAT THIS FILE OWNS: the NUMBERS ONLY.
// It deliberately does NOT own the painters. Each workspace keeps its own state resolver
// (btnVisual / paintBtn) because their state models genuinely differ, and each keeps its own `S`
// palette because the accent is per-workspace identity (camera = gold, config = blue). Geometry is
// the only thing that was drifting, so geometry is the only thing hoisted.
//
// WHAT THIS FILE IS NOT: it is not a dependency of any workspace on any other workspace. It is a
// LEAF -- it includes nothing and references nothing. ConfigHudJs.h stays a standalone script (see
// its header comment); it now shares a table of numbers rather than importing another workspace's
// widget library. Adding a new workspace means including this file, not reaching into an existing
// one.
//
// Included as an adjacent raw string literal, so the delimiter differing from the including file's
// (EDJS / CFJS / ...) is fine -- the compiler concatenates adjacent literals regardless.
//
// Every workspace must declare `var UI` from here BEFORE its first btn()/tabify() call. Each script
// is its own IIFE, so the name does not collide across workspaces.

R"UIMJS(
    var UI = {
      // Default button: compact vertical padding, with the established horizontal geometry.
      btn: { padV: 5, padH: 12, font: 15, radius: 3, gap: 5 },

      // Dense transport controls (camera timeline, graph editor). A NAMED, deliberate second tier,
      // not an accident: those bars pack their controls into a short dock where a full-size button
      // does not physically fit. Kept at the values those two scripts already used, so adopting the
      // shared table changes nothing about how they lay out -- it only means the next person edits
      // one number instead of finding two.
      btnCompact: { padV: 4, padH: 9, font: 13 },

      // Bottom-right workspace action row (RENDER / CUSTOMIZE PLAYER). Both workspaces already
      // built this row to the same spec; only the label size disagreed -- and inside the camera
      // editor the two buttons in the SAME row disagreed with each other, because CUSTOMIZE was
      // built with clbl() (the Customize modal's font-boosted helper, x1.15) while RENDER used a
      // plain lbl(). One number here, used by both, in both workspaces.
      action: { h: 38, radius: 5, font: 15, gap: 12, inset: 22,
                shadow: '#000000aa 0px 3px 12px 2px' },

      // Bottom tab bar shared by the camera editor and config workspaces.
      tabBarH: 42, tabGap: 4,

      // ---- inspector geometry (Docs/panorama/panorama_ui_guide.md, Shared workspace contract) -------------
      // The 2026-09 redesign's numbers, shared by the Config panel and the Camera Editor because
      // the original design specified ONE inspector: 1920x1080 stage, a 520px column on the right, and a
      // card grammar whose padding/gaps are the same in both workspaces. Only the accent differs,
      // which is why the colours stay in each script's own `S` (see this file's header).
      col: { w: 640, pad: 22, gap: 10, rowGap: 5 },

      // A card: header row (accent edge, title, live summary, buttons, switch, chevron) + body.
      // `inner` is the width a row inside a card gets: 520 - 2*22 (column) - 2*16 (card) - 2 (border).
      card: { padV: 10, padH: 16, radius: 5, gap: 8, edgeW: 3, edgeH: 32,
              titleFont: 16, subFont: 13, inner: 442, bodyGap: 6, headerGap: 10 },

      // Value / switch tiles share a compact height and label baseline. Widths stay unchanged.
      tile: { h: 64, padTop: 5, padBottom: 5, padH: 13, radius: 4,
              labelFont: 14, labelH: 17, valueFont: 20, barH: 3, perRow: 2 },

      group: { before: 12, gap: 6, font: 14 },

      // Sub-pages inside a card are TEXT TABS ON A RULE -- never pill buttons, which would compete
      // with the real buttons in the card. Max 4 per card.
      tab: { font: 12, ls: 1.5, gap: 20, padBottom: 7, underline: 2 },
      // The panel's own top-level strip (CAMERA / EFFECTS / ...) is the same grammar, one size up.
      // Tighter than the sub-tab gap, not looser: this strip also carries "Collapse all" at the
      // right of the rule, and four captions as long as ENVIRONMENT plus that control is the
      // widest this row ever gets. At 28/1.8 it overran the 520px column and the label landed on
      // top of RAGDOLLS.
      tabTop: { font: 14, ls: 1.4, gap: 20, padBottom: 9 },

      // ---- the effect rail (Docs/features/camera-editor-feature.md) ------------------
      // Config and the Camera Editor both split their inspector into a RAIL (one row per card,
      // under group headings) and a PANE that shows only the selected card.
      rail: { w: 200, padR: 14, gap: 16, rowH: 44, groupGap: 14, rowGap: 4, edge: 3 },
      // One-line control rows inside the pane: label column | control | value column.
      line: { h: 32, label: 130, value: 66, gap: 5, padH: 10, colGap: 12 },
      // Square switches: the card header's and the rail row's.
      sw: { w: 42, h: 22, knob: 14, travel: 20 },
      swSm: { w: 34, h: 18, knob: 12, travel: 16 },

      // Header chips (MOUSE: UI / ESC EXIT / Reset) and the on/off pill.
      chip: { font: 13, padV: 5, padH: 13, radius: 3 },
      pill: { w: 44, h: 24, knob: 18, travel: 20 },        // card headers + footers
      pillSm: { w: 40, h: 22, knob: 16, travel: 18 },      // switch tiles

      // Motion: <=160ms, and only colour, opacity or a 4px entry offset. Nothing reflows.
      mo: { arrive: 'cubic-bezier(0.16,1,0.3,1)', tab: 0.16, panel: 0.16, hover: 0.12, rise: 4 }
    };
)UIMJS"
