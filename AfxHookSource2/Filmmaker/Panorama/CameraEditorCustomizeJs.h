#pragma once

// CUSTOMIZE (LOADOUT) MODAL: cosmetic selection, wear controls, 3D preview wiring.
// JS fragment concatenated (via #include, as an adjacent raw string literal) into the
// kCameraEditorJs script body assembled in CameraEditorJs.h. Not a standalone script --
// shares scope/closures with the root IIFE. See CameraEditorJs.h for the entry point.

R"EDJS(
    // ===================== CUSTOMIZE (LOADOUT) MODAL ====================
    // A CS2-loadout-style modal for re-skinning the spectated player in the demo view. Only
    // reachable while spectating a player (obsMode 2/3); never in freecam. The dropdowns are
    // driven by a cosmetics dataset (Phase 2 replaces this stub with data generated from the
    // CS2 game files). Selecting an item updates the modal preview now; the per-frame econ/model
    // override that changes the actual demo render is wired in via pickCosmetic() once the native
    // "mirv_filmmaker cosmetics ..." back-end lands (Phases 3-4).

    // Rarity tiers -> colors, matching CS2's csgostyles.css (color-rarity-0..7). Tier numbers are
    // assigned by Automation/tools/generate_cosmetics_catalog.py (RARITY_MAP) and baked into the
    // generated catalog as rarColor(N) -- keep the two in sync. Tier 7 is Contraband (the M4A4 |
    // Howl is the only one); CS2 spells that tag `immortal` in paint_kits_rarity. Knives and gloves
    // are forced to Covert by the generator because the ITEM rarity wins over the paint kit's.
    var RARITY = {
      0: { n: 'Default',    c: '#6a6156ff' },
      1: { n: 'Consumer',   c: '#b0c3d9ff' },
      2: { n: 'Industrial', c: '#5e98d9ff' },
      3: { n: 'Mil-Spec',   c: '#4b69ffff' },
      4: { n: 'Restricted', c: '#8847ffff' },
      5: { n: 'Classified', c: '#d32ce6ff' },
      6: { n: 'Covert',     c: '#eb4b4bff' },
      7: { n: 'Contraband', c: '#e4ae39ff' }
    };
    function rarColor(r) { return (RARITY[r] || RARITY[0]).c; }
    // Filter chips, in CS2's own tier order. Values are the tier numbers above; a row matches a chip
    // when its meta.color equals that tier's color, which is the only rarity signal the generated
    // catalog carries per row (it stores color, not tier).
    var RARITY_FILTERS = [
      { t: 1, s: 'Consumer' }, { t: 2, s: 'Industrial' }, { t: 3, s: 'Mil-Spec' },
      { t: 4, s: 'Restricted' }, { t: 5, s: 'Classified' }, { t: 6, s: 'Covert' },
      { t: 7, s: 'Contraband' }
    ];
    function rarTierOf(meta) {
      var c = (meta && meta.color) ? ('' + meta.color).toLowerCase() : '';
      for (var k in RARITY) { if (RARITY[k].c.toLowerCase() === c) return parseInt(k, 10); }
      return -1;
    }
    function makeFauxItemId(defIndex, paintKitId) {
      try { return (BigInt('0xF000000000000000') | (BigInt(paintKitId || 0) << BigInt(16)) | BigInt(defIndex || 0)); }
      catch (e) { return null; }
    }
    function safeCall(panel, method) {
      if (!panel || !panel.IsValid || !panel.IsValid() || typeof panel[method] !== 'function') return null;
      try {
        var args = [];
        for (var i = 2; i < arguments.length; i++) args.push(arguments[i]);
        return panel[method].apply(panel, args);
      } catch (e) { return null; }
    }
    function fauxFromMeta(meta) {
      if (!meta || !meta.def) return null;
      return makeFauxItemId(meta.def, meta.paint || meta.paintKit || 0);
    }
    // Canonical econ item id (string) for a catalog meta, built by CS2's own econ system so an
    // <ItemImage> panel can render the REAL inventory thumbnail in the in-game HUD context (this is
    // exactly what avatar.js / itemtile.js do). Returns '' when the item has no real def index
    // (e.g. "default" rows) so the caller falls back to a rarity swatch.
    function econItemId(meta) {
      if (!meta) return '';
      var def = parseInt(meta.def || 0, 10) || 0;
      var paint = parseInt(meta.paint || meta.paintKit || 0, 10) || 0;
      if (def <= 0) return '';
      try {
        if (typeof InventoryAPI !== 'undefined' && InventoryAPI.GetFauxItemIDFromDefAndPaintIndex) {
          var id = InventoryAPI.GetFauxItemIDFromDefAndPaintIndex(def, paint);
          if (id && id !== '0') return id;
        }
      } catch (e) {}
      return '';
    }
    // Item id for MapPlayerPreviewPanel equips. Prefer CS2's own faux-id builder, but keep the
    // local high-bit faux-id fallback so staged catalog picks still drive the preview if the
    // InventoryAPI helper is unavailable in the HUD context.
    function previewEquipItemId(meta) {
      var id = econItemId(meta);
      if (id) return '' + id;
      var faux = fauxFromMeta(meta);
      return faux ? ('' + faux) : '';
    }

    // Stub dataset, replaced at load by the generated catalog below. It still has to be VALID:
    // every model path here must exist in the shipped game, because if the generated catalog
    // ever fails to load this is what the picker falls back to, and a path that does not exist
    // does not fail loudly -- CS2 swaps in the giant orange ERROR placeholder model.
    // Checked against pak01_dir.vpk 2026-08-28: tm_anarchist does not exist at all and
    // tm_balkan only ships variants f-l, so both T entries here were ERROR models.
    // Stub dataset: { key:value, ... } -> [label, value, {color, img}] options. Phase 2 swaps
    // this for a real JSON catalog generated from items_game.txt/csgo_english.txt. The shape is
    // already slot-based so the UI and commands match the final player-targeted API.
    var COSMETICS = {
      agent:  [['Default agent', 'default', { color: '#e6eaef', team: 'ANY' }],
               ['CT | SAS', 'ctm_sas', { color: rarColor(3), team: 'CT', def: 0, model: 'agents/models/ctm_sas/ctm_sas.vmdl' }],
               ['CT | FBI', 'ctm_fbi', { color: rarColor(4), team: 'CT', def: 0, model: 'agents/models/ctm_fbi/ctm_fbi.vmdl' }],
               ['T | Leet', 'tm_leet', { color: rarColor(3), team: 'T', def: 0, model: 'agents/models/tm_leet/tm_leet_varianta.vmdl' }],
               ['T | Balkan', 'tm_balkan', { color: rarColor(4), team: 'T', def: 0, model: 'agents/models/tm_balkan/tm_balkan_variantf.vmdl' }]],
      primary:[['Default rifle skin', '0', { color: rarColor(0), def: 7, paint: 0 }],
               ['AK-47 | Redline', '282', { color: rarColor(4), def: 7, paint: 282, wearMin: 0.10, wearMax: 0.70 }],
               ['AK-47 | Asiimov', '801', { color: rarColor(5), def: 7, paint: 801, wearMin: 0.05, wearMax: 0.70 }],
               ['AWP | Asiimov', '279', { color: rarColor(6), def: 9, paint: 279, wearMin: 0.18, wearMax: 1.00 }],
               ['AWP | Dragon Lore', '344', { color: rarColor(6), def: 9, paint: 344, wearMin: 0.00, wearMax: 0.70 }],
               ['M4A4 | Howl', '309', { color: rarColor(7), def: 16, paint: 309, wearMin: 0.00, wearMax: 0.40 }]],
      secondary:[['Default pistol skin', '0', { color: rarColor(0), def: 1, paint: 0 }],
               ['Desert Eagle | Blaze', '37', { color: rarColor(5), def: 1, paint: 37, wearMin: 0.00, wearMax: 0.08 }],
               ['USP-S | Kill Confirmed', '504', { color: rarColor(6), def: 61, paint: 504, wearMin: 0.00, wearMax: 1.00 }],
               ['Glock-18 | Fade', '38', { color: rarColor(5), def: 4, paint: 38, wearMin: 0.00, wearMax: 0.08 }]],
      knife:  [['Default CT knife', '42:0', { color: rarColor(0), def: 42, paint: 0, team: 'CT' }],
               ['Default T knife', '59:0', { color: rarColor(0), def: 59, paint: 0, team: 'T' }],
               ['Karambit | Doppler', '507:417', { color: rarColor(7), def: 507, paint: 417, wearMin: 0.00, wearMax: 0.08 }],
               ['Butterfly Knife | Fade', '515:38', { color: rarColor(7), def: 515, paint: 38, wearMin: 0.00, wearMax: 0.08 }],
               ['M9 Bayonet | Marble Fade', '508:413', { color: rarColor(7), def: 508, paint: 413, wearMin: 0.00, wearMax: 0.08 }]],
      gloves: [['Default CT Gloves', '5029:0', { color: rarColor(0), def: 5029, paint: 0, team: 'CT' }],
               ['Default T Gloves', '5028:0', { color: rarColor(0), def: 5028, paint: 0, team: 'T' }],
               ['Sport Gloves | Pandora Box', '5030:10037', { color: rarColor(7), def: 5030, paint: 10037, wearMin: 0.06, wearMax: 0.80 }],
               ['Specialist Gloves | Crimson Kimono', '5034:10033', { color: rarColor(7), def: 5034, paint: 10033, wearMin: 0.06, wearMax: 0.80 }],
               ['Moto Gloves | Spearmint', '5033:10026', { color: rarColor(7), def: 5033, paint: 10026, wearMin: 0.06, wearMax: 0.80 }]]
    };
)EDJS"
#include "CameraEditorCosmeticsCatalog.inc"
R"EDJS(
    // Current selections + the spectated player this loadout belongs to.
    var custSel = { agent: 'default', primary: '0', secondary: '0', knife: '0', gloves: '0' };
    var WEAR_PRESETS = {
      fn: { label: 'Factory New', lo: 0.00, hi: 0.07, value: 0.01 },
      mw: { label: 'Minimal Wear', lo: 0.07, hi: 0.15, value: 0.10 },
      ft: { label: 'Field-Tested', lo: 0.15, hi: 0.38, value: 0.22 },
      ww: { label: 'Well-Worn', lo: 0.38, hi: 0.45, value: 0.40 },
      bs: { label: 'Battle-Scarred', lo: 0.45, hi: 1.00, value: 0.55 }
    };
    // Low-to-high float order + the short tags drawn on the wear bar itself (the full names stay in
    // the readout above it).
    var WEAR_TIER_ORDER = ['fn', 'mw', 'ft', 'ww', 'bs'];
    var WEAR_ABBREV = { fn: 'FN', mw: 'MW', ft: 'FT', ww: 'WW', bs: 'BS' };
    var custWear = {
      primary: { preset: 'fn', custom: 0.01 },
      secondary: { preset: 'fn', custom: 0.01 },
      knife: { preset: 'fn', custom: 0.01 },
      gloves: { preset: 'ft', custom: 0.22 }
    };
    var custItemWear = {};
    var custSearchDisplayGuard = false; // true while WE set a search TextEntry's .text (skip its change handler)
    var custTargetIndex = -1, custTargetName = 'Player', custTargetTeam = '';
    var custTargetKey = 'pawn:-1', custActiveWeaponDef = 0, custActiveWeaponSlot = '', custDisplayedLoadoutSig = '';
    var custActiveWeaponPickup = false, custActiveWeaponOwnerSteam = '', custActiveWeaponOwnerName = '';
    var custLoadout = { primary: null, secondary: null, knife: null, gloves: null };
    // Which weapon slot the 3D preview currently poses the character HOLDING -- selecting the
    // Primary/Secondary/Melee slot, or dragging its wear, switches this (see selectSlot and the
    // `category` itemDrop option below), mirroring the native loadout's selected-group behavior
    // (loadout_grid.js UpdateCharModel: whichever slot you're browsing is what the model holds).
    // Gloves are not a "held" item -- they render on the hands regardless of this value.
    var custActiveCategory = 'primary';
    function setActiveCategory(slot) {
      if (slot === custActiveCategory) return;
      custActiveCategory = slot;
      updatePreview();
      updateHoldChip();
    }
    // Which slot the preview should default to holding when a player is freshly loaded into the modal:
    // prefer whatever they're actually carrying, so the model isn't shown holding an empty slot.
    function initialActiveCategory() {
      if (custLoadout.primary && custLoadout.primary.defIndex > 0) return 'primary';
      if (custLoadout.secondary && custLoadout.secondary.defIndex > 0) return 'secondary';
      return 'knife';
    }
    var WEAPON_SLOT_BY_DEF = {
      1:'secondary', 2:'secondary', 3:'secondary', 4:'secondary', 30:'secondary', 32:'secondary', 36:'secondary', 61:'secondary', 63:'secondary', 64:'secondary',
      7:'primary', 8:'primary', 9:'primary', 10:'primary', 11:'primary', 13:'primary', 14:'primary', 16:'primary', 17:'primary', 19:'primary', 23:'primary', 24:'primary', 25:'primary', 26:'primary', 27:'primary', 28:'primary', 29:'primary', 33:'primary', 34:'primary', 35:'primary', 38:'primary', 39:'primary', 60:'primary',
      42:'knife', 59:'knife', 500:'knife', 503:'knife', 505:'knife', 506:'knife', 507:'knife', 508:'knife', 509:'knife', 512:'knife', 514:'knife', 515:'knife', 516:'knife', 517:'knife', 518:'knife', 519:'knife', 520:'knife', 521:'knife', 522:'knife', 523:'knife', 525:'knife', 526:'knife'
    };
)EDJS"
R"EDJS(
    // ---- Slot rows + the always-open picker (design 7b) ----------------------------------------
    // Docs/features/customize-feature.md (original option 7b): a SLOT LIST on the left (Agent, Primary,
    // Secondary, Melee / knife, Gloves), the 3D preview in the middle, and on the right a picker that
    // never closes and always shows the selected slot -- crumb, search, rarity chips, a 3-up tile grid
    // and the wear block pinned under it. There are no popups any more: each slot owns one picker PANE
    // in the right column and only the selected slot's pane is visible.
    //
    // itemDrop() keeps its name and its contract (update / setPickup / setSearch / open), so the
    // staging, the pickup routing and every command around it are untouched; what changed is where it
    // draws -- a slot row instead of a dropdown bar, a pane instead of a popup.
    var CUST_TILE_W = 162, CUST_TILE_H = 118, CUST_TILE_GAP = 8, CUST_TILE_COLS = 3;
    var CUST_SCROLL_STEP = 64;          // px scrolled per wheel notch
    var custSlotRecs = {};              // slot -> picker rec, for the active-pane plumbing below
    var custActiveSlot = 'primary';     // the slot whose pane is showing (and whose row is gold)
    function activeSlotRec() { return custSlotRecs[custActiveSlot] || null; }
    // Select a slot: its row turns gold, its pane shows, and -- for primary / secondary / knife -- the
    // preview holds that slot (gloves and agent do not change what the model holds).
    function selectSlot(slot) {
      var rec = custSlotRecs[slot];
      if (!rec) return;
      var changed = custActiveSlot !== slot;
      custActiveSlot = slot;
      for (var k in custSlotRecs) {
        var r = custSlotRecs[k];
        r.pane.visible = (k === slot);
        r.paintField();
      }
      if (changed) { rec.scrollOff = 0; if (rec.grouped) rec.navGroup = ''; }
      if (rec.stale || changed) rec.rebuild();
      if (rec.category) setActiveCategory(rec.category);
      sizePane(rec);
    }
    // The selected slot must be one the player actually has (primary / secondary hide when empty).
    function ensureActiveSlotVisible() {
      var rec = activeSlotRec();
      if (rec && rec.field.visible !== false) return;
      var order = ['primary', 'secondary', 'knife', 'agent', 'gloves'];
      for (var i = 0; i < order.length; i++) {
        var r = custSlotRecs[order[i]];
        if (r && r.field.visible !== false) { selectSlot(order[i]); return; }
      }
    }
    function itemDrop(parent, id, onPick, opts) {
      opts = opts || {};
      var slot = opts.slot;
      // -- the slot row (column 1): 76px, a 3px rarity bar, an 84x56 thumbnail, caption / name / wear.
      var field = $.CreatePanel('Panel', parent, id, {}); field.hittest = true;
      field.style.width = '100%'; field.style.height = '76px'; field.style.flowChildren = 'right';
      field.style.marginBottom = '6px'; field.style.borderRadius = '4px'; field.style.overflow = 'clip';
      var bar = mk('Panel', field); bar.hittest = false;
      bar.style.width = '3px'; bar.style.height = '100%';
      var swatch = mk('Panel', field); swatch.hittest = false;
      swatch.style.width = '84px'; swatch.style.height = '56px'; swatch.style.verticalAlign = 'center';
      swatch.style.marginLeft = '10px'; swatch.style.marginRight = '10px'; swatch.style.borderRadius = '2px';
      swatch.style.backgroundColor = '#0c0f14';
      var txt = mk('Panel', field); txt.hittest = false;
      txt.style.flowChildren = 'down'; txt.style.width = 'fill-parent-flow(1.0)';
      txt.style.verticalAlign = 'center'; txt.style.paddingRight = '10px';
      var capRow = mk('Panel', txt); capRow.hittest = false; capRow.style.flowChildren = 'right';
      var cap = clbl(capRow, opts.caption || slot, '#6b7480ff', 10); cap.hittest = false;
      cap.style.fontWeight = 'bold'; cap.style.letterSpacing = '1.6px'; cap.style.verticalAlign = 'center';
      cap.style.textTransform = 'uppercase';   // casing in the style, copy in natural case
      // Primary/Secondary carry a small pickup marker beside the caption (slotPickup()).
      var pickupIndicator = null;
      if (opts.pickupIndicator) {
        pickupIndicator = mk('Panel', capRow); pickupIndicator.visible = false; pickupIndicator.hittest = false;
        pickupIndicator.style.width = '14px'; pickupIndicator.style.height = '14px';
        pickupIndicator.style.verticalAlign = 'center'; pickupIndicator.style.marginLeft = '6px';
        pickupIndicator.style.borderRadius = '7px'; pickupIndicator.style.backgroundColor = S.accent;
        var pickupIndicatorLbl = clbl(pickupIndicator, '!', '#241a06ff', 10); pickupIndicatorLbl.hittest = false;
        pickupIndicatorLbl.style.width = '100%'; pickupIndicatorLbl.style.textAlign = 'center';
        pickupIndicatorLbl.style.verticalAlign = 'center'; pickupIndicatorLbl.style.fontWeight = 'bold';
      }
      var disp = clbl(txt, '', '#e6eaef', 13); disp.hittest = false;
      disp.style.fontWeight = 'bold'; disp.style.marginTop = '3px'; disp.style.width = '100%';
      disp.style.whiteSpace = 'nowrap'; disp.style.textOverflow = 'ellipsis';
      var wearTag = clbl(txt, '', '#8b95a3ff', 11); wearTag.hittest = false;
      wearTag.style.marginTop = '2px'; wearTag.style.whiteSpace = 'nowrap'; wearTag.style.textOverflow = 'ellipsis';
      wearTag.style.width = '100%';
      function paintField() {
        var on = custActiveSlot === slot;
        field.style.backgroundColor = on ? '#1a2230ff' : (field.__hot ? '#18202bff' : '#141a23ff');
        field.style.border = '1px solid ' + (on ? S.accent : '#ffffff14');
        cap.style.color = on ? S.accent : '#6b7480ff';
      }
      field.SetPanelEvent('onmouseover', function () { field.__hot = true; paintField(); });
      field.SetPanelEvent('onmouseout', function () { field.__hot = false; paintField(); });
      field.SetPanelEvent('onactivate', function () { selectSlot(slot); });

      // -- the picker pane (column 3): crumb, search, chips, the tile grid; the wear block is added
      // after it by the caller, so the grid fills and the wear block sits on the column's foot.
      var pane = mk('Panel', custPickHost); pane.visible = false; pane.hittest = true;
      pane.style.width = '100%'; pane.style.height = '100%'; pane.style.flowChildren = 'down';
      var crumbRow = mk('Panel', pane); crumbRow.hittest = true;
      crumbRow.style.width = '100%'; crumbRow.style.height = '28px'; crumbRow.style.flowChildren = 'right';
      var crumbRoot = clbl(crumbRow, '', S.value, 12); crumbRoot.hittest = true;
      crumbRoot.style.fontWeight = 'bold'; crumbRoot.style.letterSpacing = '1.5px';
      crumbRoot.style.verticalAlign = 'center'; crumbRoot.style.whiteSpace = 'nowrap';
      crumbRoot.style.textTransform = 'uppercase';
      var crumbSep = clbl(crumbRow, '›', '#6b7480ff', 12); crumbSep.hittest = false;
      crumbSep.style.verticalAlign = 'center'; crumbSep.style.marginLeft = '6px'; crumbSep.style.marginRight = '6px';
      var crumbLeaf = clbl(crumbRow, '', S.value, 12); crumbLeaf.hittest = false;
      crumbLeaf.style.fontWeight = 'bold'; crumbLeaf.style.letterSpacing = '1.5px';
      crumbLeaf.style.verticalAlign = 'center'; crumbLeaf.style.whiteSpace = 'nowrap';
      crumbLeaf.style.textOverflow = 'ellipsis'; crumbLeaf.style.textTransform = 'uppercase';
      var crumbFill = mk('Panel', crumbRow); crumbFill.hittest = false;
      crumbFill.style.width = 'fill-parent-flow(1.0)'; crumbFill.style.height = '1px';
      // Search: a plain panel drawing its own text. This HUD context never gives a TextEntry real
      // keyboard focus, so the characters arrive through custChars() (the C++ WM_CHAR route) and the
      // row is [query][caret][placeholder] -- the caret is simply the next thing after the text.
      var searchWrap = mk('Panel', pane); searchWrap.hittest = true;
      searchWrap.style.width = '100%'; searchWrap.style.height = '34px'; searchWrap.style.marginTop = '10px';
      searchWrap.style.flowChildren = 'right'; searchWrap.style.paddingLeft = '10px'; searchWrap.style.paddingRight = '10px';
      // hexAlpha(), NOT string concatenation: S.accent already carries an alpha byte.
      searchWrap.style.backgroundColor = '#101823';
      searchWrap.style.border = '1px solid ' + hexAlpha(S.accent, '88');
      searchWrap.style.borderRadius = '3px';
      var search = clbl(searchWrap, '', S.value, 13); search.hittest = false;
      search.style.verticalAlign = 'center'; search.style.whiteSpace = 'nowrap';
      search.style.maxWidth = '92%'; search.style.textOverflow = 'clip';
      var caretBar = mk('Panel', searchWrap); caretBar.hittest = false;
      caretBar.style.width = '2px'; caretBar.style.height = '18px'; caretBar.style.verticalAlign = 'center';
      caretBar.style.marginLeft = '1px'; caretBar.style.backgroundColor = S.accent;
      var hint = clbl(searchWrap, '', S.muted, 13); hint.hittest = false;
      hint.style.verticalAlign = 'center'; hint.style.marginLeft = '4px'; hint.style.whiteSpace = 'nowrap';
      var chipBar = mk('Panel', pane); chipBar.hittest = true; chipBar.visible = false;
      chipBar.style.width = '100%'; chipBar.style.flowChildren = 'right'; chipBar.style.marginTop = '10px';
      // The tile grid: a clipped viewport whose content is translated to scroll, because the wheel is
      // consumed in C++ and routed in (custWheel) -- Panorama never sees it, so native scroll is out.
      var rowsView = mk('Panel', pane); rowsView.hittest = true;
      rowsView.style.width = '100%'; rowsView.style.height = 'fill-parent-flow(1.0)'; rowsView.style.marginTop = '10px';
      rowsView.style.overflow = 'clip'; rowsView.style.flowChildren = 'none';
      var rows = mk('Panel', rowsView); rows.hittest = true;
      rows.style.width = '100%'; rows.style.flowChildren = 'right-wrap';
      // Scroll cues, shown only in a direction that still has content (see sizePane).
      var fadeTop = mk('Panel', rowsView); fadeTop.hittest = false; fadeTop.visible = false;
      fadeTop.style.width = '100%'; fadeTop.style.height = '22px'; fadeTop.style.verticalAlign = 'top';
      fadeTop.style.backgroundColor = 'gradient( linear, 0% 0%, 0% 100%, from( #0b0e14ee ), to( #0b0e1400 ) )';
      var fadeBot = mk('Panel', rowsView); fadeBot.hittest = false; fadeBot.visible = false;
      fadeBot.style.width = '100%'; fadeBot.style.height = '22px'; fadeBot.style.verticalAlign = 'bottom';
      fadeBot.style.backgroundColor = 'gradient( linear, 0% 0%, 0% 100%, from( #0b0e1400 ), to( #0b0e14ee ) )';

      var rec = {
        slot: slot, field: field, disp: disp, pane: pane, onPick: onPick, opts: [], sig: '', swatch: swatch,
        query: '', searchable: opts.searchable !== false, search: search, rows: rows, rowsView: rowsView, scrollOff: 0,
        caretPanel: caretBar, fadeTop: fadeTop, fadeBot: fadeBot, actions: crumbRow, stale: true,
        category: opts.category || '', paintField: paintField,
        // Rarity chips are opt-in per slot (the agent list is a single tier). `grouped` turns the
        // list into model -> finishes, used by agents, knives and gloves.
        rarityChips: !!opts.rarityChips, rarity: {}, rarityOn: false, chipPanels: null,
        grouped: !!opts.grouped, navGroup: ''
      };
      rec.syncCaret = function () { hint.visible = !(rec.query || ''); };
      custSlotRecs[slot] = rec;
      crumbRoot.SetPanelEvent('onactivate', function () {
        if (rec.grouped && rec.navGroup) { rec.navGroup = ''; rec.scrollOff = 0; rebuildRows(); sizePane(rec); }
      });
      paintField();
)EDJS"
R"EDJS(
      function panoramaImageSrc(img) {
        if (!img) return '';
        var rel = String(img).replace(/\\/g, '/');
        if (rel.indexOf('s2r://') === 0 || rel.indexOf('file://') === 0) return rel;
        if (rel.indexOf('panorama/images/') === 0) rel = rel.substring(16);
        if (rel.slice(-11) === '_png.vtex_c') rel = rel.substring(0, rel.length - 2);
        if (rel.slice(-9) === '_png.vtex') return 's2r://panorama/images/' + rel;
        if (rel.slice(-4).toLowerCase() === '.png') rel = rel.substring(0, rel.length - 4);
        return 's2r://panorama/images/' + rel + '_png.vtex';
      }

      function applySwatch(target, meta) {
        // 1) Direct Panorama image resource path. Catalog metadata stores image refs relative to
        //    panorama/images/ and verified against <img>_png.vtex_c in the CS2 VPK. Expand them to
        //    the native CS2 resource URL here so agents, weapons, knives, and gloves all use the
        //    same loading path.
        if (meta && meta.img) {
          if (target.__iimg) target.__iimg.visible = false;
          target.__im = target.__im || mk('Image', target);
          target.__im.style.width = '100%'; target.__im.style.height = '100%';
          try { target.__im.SetScaling('stretch-to-fit-preserve-aspect'); } catch (e0) {}
          try { target.__im.SetImage(panoramaImageSrc(meta.img)); } catch (e1) {}
          target.__im.visible = true;
          target.style.backgroundColor = '#0c0f14';
          return;
        }
        var itemId = econItemId(meta);
        if (itemId) {
          // Real CS2 econ thumbnail via a native <ItemImage> panel (renders in the HUD context).
          if (!target.__iimg) {
            try { target.__iimg = $.CreatePanel('ItemImage', target, '', {}); }
            catch (e) { target.__iimg = null; }
            if (target.__iimg) {
              target.__iimg.style.width = '100%'; target.__iimg.style.height = '100%';
              try { target.__iimg.SetScaling('stretch-to-fit-preserve-aspect'); } catch (e2) {}
            }
          }
          if (target.__iimg) {
            try { target.__iimg.itemid = itemId; } catch (e3) {}
            target.__iimg.visible = true;
            if (target.__im) target.__im.visible = false;
            target.style.backgroundColor = '#0c0f14';
            return;
          }
        }
        if (target.__iimg) target.__iimg.visible = false;
        if (target.__im) target.__im.visible = false;
        // No artwork at all: the rarity colour stands in, as a block.
        target.style.backgroundColor = (meta && meta.color) ? meta.color : '#0c0f14';
      }
      // ---- filtering + grouping ----------------------------------------------------------
      // A row survives when it matches the search text AND (no rarity chip is active OR its tier is
      // one of the active ones). Stock/default rows (tier 0) are exempt from the rarity filter: they
      // are the slot's "no skin" reset, not a rarity, and filtering them out would make the reset
      // unreachable while any chip is on.
      function rowPassesFilters(o) {
        if (rec.query && (o[0] || '').toLowerCase().indexOf(rec.query) < 0) return false;
        if (rec.rarityOn) {
          var t = rarTierOf(o[2]);
          if (t > 0 && !rec.rarity[t]) return false;
        }
        return true;
      }
      // Grouping is derived from the catalog LABEL, so a model Valve adds later shows up on its own
      // with no hand-maintained list. The separator differs by slot because the catalog spells the
      // two families differently: knives and gloves are "<Model> | <Finish>", agents are
      // "<Family> · <Variant>" (e.g. "Balkan · Variant F", "Leet · Variant A").
      var GROUP_SEP = opts.groupSep || ' | ';
      // Memoised on rec.sig (the option set), because groupOf() runs once per row per rebuild and
      // rebuildRows() runs on every keystroke.
      var famCache = null, famSig = null;
      function groupFamilies() {
        if (famCache && famSig === rec.sig) return famCache;
        var set = {};
        for (var i = 0; i < rec.opts.length; i++) {
          var l = rec.opts[i][0] || '', j = l.indexOf(GROUP_SEP);
          if (j > 0) set[l.substring(0, j)] = true;
        }
        famCache = set; famSig = rec.sig;
        return set;
      }
      function groupOf(o) {
        var label = o[0] || '';
        var i = label.indexOf(GROUP_SEP);
        if (i > 0) return label.substring(0, i);
        // A bare row whose WHOLE label is a family name belongs to that family, not to the flat list:
        // the agent catalog ships "FBI" / "SAS" / "Phoenix" alongside "FBI · Variant A".
        return groupFamilies()[label] ? label : '';
      }
      function groupNames() {
        var seen = {}, out = [];
        for (var i = 0; i < rec.opts.length; i++) {
          var g = groupOf(rec.opts[i]);
          if (!g || seen[g]) continue;
          seen[g] = true; out.push(g);
        }
        return out;
      }
      // Which family the current selection lives in, so the model tiles can mark it.
      function selectedGroup() {
        for (var i = 0; i < rec.opts.length; i++) {
          var m = rec.opts[i][2] || {};
          if (m.selected) return groupOf(rec.opts[i]);
        }
        return '';
      }
      // The crumb: the slot's own name, or for a grouped slot the "ALL KNIVES" root and, inside a
      // model, "› KARAMBIT". Clicking the root goes back.
      function paintCrumb() {
        var inGroup = rec.grouped && !!rec.navGroup;
        crumbRoot.text = rec.grouped ? (opts.groupLabel || 'All') : (opts.crumb ? opts.crumb() : (opts.caption || slot));
        crumbRoot.style.color = inGroup ? '#8b95a3ff' : S.value;
        crumbSep.visible = inGroup; crumbLeaf.visible = inGroup;
        crumbLeaf.text = inGroup ? rec.navGroup : '';
        hint.text = 'Search ' + (opts.searchNoun || (opts.crumbNoun ? opts.crumbNoun() : 'finishes'));
      }
)EDJS"
R"EDJS(
      // ---- tiles ---------------------------------------------------------------------------
      // 3 columns of 118px tiles with an 8px gap, built into a right-wrap row at a fixed tile width
      // (Panorama has no grid). Each tile: the artwork, the name in its rarity colour with a ✔ when
      // equipped or "N finishes ›" on a model tile, and a 3px rarity bar along the bottom.
      // Hover/selected paint has to come from onmouseover/onmouseout (there is no :hover here), and
      // ONE resolver owns the paint so a mouseout cannot revert the selected fill.
      var tileIdx = 0;
      function tileShell(color, isSel, tipText) {
        var t = mk('Panel', rows); t.hittest = true;
        t.style.width = CUST_TILE_W + 'px'; t.style.height = CUST_TILE_H + 'px';
        t.style.marginRight = ((tileIdx % CUST_TILE_COLS) === CUST_TILE_COLS - 1) ? '0px' : (CUST_TILE_GAP + 'px');
        t.style.marginBottom = CUST_TILE_GAP + 'px';
        t.style.flowChildren = 'down'; t.style.borderRadius = '4px'; t.style.overflow = 'clip';
        tileIdx++;
        t.__hot = false;
        t.__paint = function () {
          t.style.backgroundColor = isSel ? '#1a2230ff' : (t.__hot ? '#1b2230ff' : '#141a23ff');
          t.style.border = '1px solid ' + (isSel ? (color || S.accent) : '#ffffff14');
        };
        t.style.transitionProperty = 'background-color';
        t.style.transitionDuration = '0.09s'; t.style.transitionTimingFunction = 'ease-out';
        t.SetPanelEvent('onmouseover', function () { t.__hot = true; t.__paint(); if (tipText) showTip(t, tipText, t.__nameLbl, cfs(12)); });
        t.SetPanelEvent('onmouseout', function () { t.__hot = false; t.__paint(); hideTip(); });
        t.__paint();
        return t;
      }
      function tileBody(t, meta, color, name, sub, subColor, dimArt) {
        var art = mk('Panel', t); art.hittest = false;
        art.style.width = '100%'; art.style.height = 'fill-parent-flow(1.0)';
        art.style.paddingTop = '6px'; art.style.paddingBottom = '6px';
        art.style.paddingLeft = '6px'; art.style.paddingRight = '6px';
        var sw = mk('Panel', art); sw.hittest = false;
        sw.style.width = '100%'; sw.style.height = '100%';
        applySwatch(sw, meta || {});
        if (sw.__im || sw.__iimg) sw.style.backgroundColor = S.clear;
        if (dimArt) sw.style.opacity = '0.35';
        var line = mk('Panel', t); line.hittest = false;
        line.style.width = '100%'; line.style.flowChildren = 'right';
        line.style.paddingLeft = '9px'; line.style.paddingRight = '9px'; line.style.paddingBottom = '8px';
        var nm = clbl(line, name, color, 12); nm.hittest = false;
        nm.style.width = 'fill-parent-flow(1.0)'; nm.style.fontWeight = 'bold';
        nm.style.whiteSpace = 'nowrap'; nm.style.textOverflow = 'ellipsis';
        if (sub) {
          var sl = clbl(line, sub, subColor || '#8b95a3ff', 11); sl.hittest = false;
          sl.style.marginLeft = '6px'; sl.style.whiteSpace = 'nowrap'; sl.style.fontWeight = 'bold';
        }
        var rb = mk('Panel', t); rb.hittest = false;
        rb.style.width = '100%'; rb.style.height = '3px'; rb.style.backgroundColor = color;
        return nm;
      }
      // Stock rows (tier 0: "Default CT knife", "Default rifle skin") read as the neutral reset, not
      // as a rarity: grey name, dimmed art.
      function isStockMeta(meta) { return rarTierOf(meta) <= 0 && !((parseInt(meta.paint || 0, 10) || 0) > 0) && slot !== 'agent'; }
      // One item tile. Clicking picks it; the pane stays open.
      function buildRow(o) {
        if (!rowPassesFilters(o)) return;
        if (rec.grouped && rec.navGroup && groupOf(o) && groupOf(o) !== rec.navGroup) return;
        var meta = o[2] || {};
        var stock = isStockMeta(meta);
        var color = stock ? '#c8d3dfff' : (meta.color || '#e6eaef');
        // The tile shows the SHORT name: no " · Minimal Wear" suffix (withWearLabel bakes the selected
        // item's wear into every label; the wear block says it), and no "M4A1-S | " / "Karambit | "
        // prefix -- the crumb already names the weapon or model.
        var shortName = o[0] || '';
        if (slot !== 'agent') {
          var wsep = shortName.lastIndexOf(' · ');
          if (wsep > 0 && WEAR_PRESETS[presetForWearValue(wearValue(slot))] &&
              shortName.substring(wsep + 3) === wearPresetLabel(slot)) shortName = shortName.substring(0, wsep);
        }
        if ((rec.grouped && rec.navGroup) || !rec.grouped) {
          var cut = shortName.indexOf(GROUP_SEP);
          if (cut > 0) shortName = shortName.substring(cut + GROUP_SEP.length);
        }
        var t = tileShell(meta.selected ? (stock ? '#ffffff55' : color) : color, !!meta.selected, shortName);
        t.__nameLbl = tileBody(t, meta, color, shortName, meta.selected ? '✔' : '', color, stock);
        t.SetPanelEvent('onactivate', function () { hideTip(); rec.onPick(o[1]); });
      }
      // One MODEL tile in a grouped slot: opens that model's finishes (rec.navGroup).
      function buildNavRow(label, color, sampleMeta, onGo, count, isSel) {
        var t = tileShell(isSel ? hexAlpha(color, '99') : color, !!isSel, label);
        var noun = (slot === 'agent') ? ' variant' : ' finish';
        var sub = (count || 0) + noun + ((count === 1) ? '' : (slot === 'agent' ? 's' : 'es')) + ' ›';
        t.__nameLbl = tileBody(t, sampleMeta || {}, color, label, sub, isSel ? S.accent : '#8b95a3ff', false);
        t.SetPanelEvent('onactivate', function () { hideTip(); onGo(); });
      }
      function rebuildRows() {
        if (!rec.rows || !rec.rows.IsValid || !rec.rows.IsValid()) return;
        rec.rows.RemoveAndDeleteChildren();
        tileIdx = 0;
        var i;
        paintCrumb();
        // Grouped slot, top level: the stock picks first, then one tile per model.
        if (rec.grouped && !rec.navGroup) {
          for (i = 0; i < rec.opts.length; i++) if (!groupOf(rec.opts[i])) buildRow(rec.opts[i]);
          var names = groupNames(), selGroup = selectedGroup();
          for (i = 0; i < names.length; i++) {
            (function (g) {
              var sample = null, n = 0, selHere = false;
              for (var j = 0; j < rec.opts.length; j++) {
                if (groupOf(rec.opts[j]) !== g) continue;
                if (!rowPassesFilters(rec.opts[j])) continue;
                // Prefer the SELECTED variant's artwork, so the tile shows what the player wears.
                var m = rec.opts[j][2] || {};
                if (m.selected) { selHere = true; sample = m; }
                else if (!sample) sample = m;
                n++;
              }
              if (!n) return; // every finish in this model filtered out -- hide the model too
              buildNavRow(g, (sample && sample.color) || '#e6eaef', sample,
                function () { rec.navGroup = g; rec.scrollOff = 0; rebuildRows(); sizePane(rec); }, n,
                selHere || (g === selGroup));
            })(names[i]);
          }
        } else {
          for (i = 0; i < rec.opts.length; i++) buildRow(rec.opts[i]);
        }
        if (!tileIdx) {
          var none = clbl(rec.rows, 'Nothing matches — try a shorter search', '#8b95a3ff', 13);
          none.hittest = false; none.style.marginTop = '8px'; none.style.marginLeft = '2px';
        }
      }
)EDJS"
R"EDJS(
      // Rarity chips, under the search box, DERIVED FROM THE ROWS THIS SLOT ACTUALLY HAS. rec.opts is
      // already scoped to the one weapon the player carries (filteredOptions -> effectiveSlotDef), so
      // the census is per-weapon: the M4A4 gets a Contraband chip because the Howl exists, the Glock
      // does not. A tier with no rows is not shown at all, and the bar is omitted below 2 live tiers
      // (one tier is a label, not a filter).
      function rebuildChips() {
        chipBar.RemoveAndDeleteChildren();
        rec.chipPanels = [];
        chipBar.visible = false;
        if (!rec.rarityChips) return;
        var haveTier = {}, liveTiers = [];
        for (var ti0 = 0; ti0 < RARITY_FILTERS.length; ti0++) {
          var ft = RARITY_FILTERS[ti0], n0 = 0;
          for (var j0 = 0; j0 < rec.opts.length; j0++) if (rarTierOf(rec.opts[j0][2]) === ft.t) n0++;
          if (!n0) continue;
          haveTier[ft.t] = n0; liveTiers.push(ft);
        }
        // An active filter for a tier the NEW weapon does not have would silently empty the list
        // with no visible chip to turn back off, so stale selections are dropped here.
        rec.rarityOn = false;
        for (var rk in rec.rarity) {
          if (!rec.rarity[rk]) continue;
          if (!haveTier[rk]) rec.rarity[rk] = false;
          else rec.rarityOn = true;
        }
        if (liveTiers.length < 2) return;
        chipBar.visible = true;
        for (var ci = 0; ci < liveTiers.length; ci++) {
          (function (f, last) {
            var chip = mk('Panel', chipBar); chip.hittest = true;
            chip.style.width = 'fill-parent-flow(1.0)'; chip.style.height = '24px';
            chip.style.marginRight = last ? '0px' : '4px'; chip.style.borderRadius = '3px';
            chip.style.border = '1px solid ' + RARITY[f.t].c;
            chip.style.transitionProperty = 'background-color';
            chip.style.transitionDuration = '0.10s'; chip.style.transitionTimingFunction = 'ease-out';
            var lab = clbl(chip, f.s, RARITY[f.t].c, 11);
            lab.hittest = false; lab.style.width = '100%'; lab.style.textAlign = 'center';
            lab.style.verticalAlign = 'center'; lab.style.fontWeight = 'bold'; lab.style.letterSpacing = '1px';
            chip.__hot = false;
            chip.__paint = function () {
              var on = !!rec.rarity[f.t];
              chip.style.backgroundColor = on ? hexAlpha(RARITY[f.t].c, '3d')
                : (chip.__hot ? hexAlpha(RARITY[f.t].c, '1c') : S.clear);
              lab.style.color = on ? '#ffffffff' : RARITY[f.t].c;
            };
            chip.__paint();
            chip.SetPanelEvent('onmouseover', function () { chip.__hot = true; chip.__paint(); });
            chip.SetPanelEvent('onmouseout', function () { chip.__hot = false; chip.__paint(); });
            rec.chipPanels.push(chip);
            chip.SetPanelEvent('onactivate', function () {
              rec.rarity[f.t] = !rec.rarity[f.t];
              rec.rarityOn = false;
              for (var k = 0; k < liveTiers.length; k++) if (rec.rarity[liveTiers[k].t]) rec.rarityOn = true;
              for (var p = 0; p < rec.chipPanels.length; p++) rec.chipPanels[p].__paint();
              rec.scrollOff = 0;
              rebuildRows(); sizePane(rec);
            });
          })(liveTiers[ci], ci === liveTiers.length - 1);
        }
      }
      rec.rebuildRows = rebuildRows; // the C++-routed typing path (custChars) refilters through this
      // Rebuild a pane only while it is on screen; a hidden pane is marked stale and rebuilt when
      // its slot is selected, so a populate does not lay out five grids nobody can see.
      function rebuild() {
        rec.stale = false;
        search.text = rec.query || '';
        rec.syncCaret();
        rebuildChips();
        rebuildRows();
      }
      rec.rebuild = rebuild;
      return {
        dd: field, pane: pane, actions: crumbRow, rec: rec,
        // The slot row's wear line ("Field-Tested · 0.182", "Stock", "CT agent"), on its own so the
        // wear drag can refresh just that one Label.
        setWearTag: function (text) { wearTag.text = text || ''; },
        setPickup: function (on) { if (pickupIndicator) pickupIndicator.visible = !!on; },
        setSearch: function (q) { rec.query = (q || '').toLowerCase(); rec.scrollOff = 0; rebuild(); },
        open: function () { selectSlot(slot); },
        update: function (optsArr, selValue) {
          rec.opts = optsArr || [];
          var sel = null;
          for (var d = 0; d < rec.opts.length; d++) {
            var m = rec.opts[d][2] || (rec.opts[d][2] = {});
            m.selected = (rec.opts[d][1] === selValue);
            if (m.selected) sel = rec.opts[d];
          }
          if (!sel && rec.opts.length) sel = rec.opts[0];
          var meta = sel ? (sel[2] || {}) : {};
          // The row shows the skin name; the " · Factory New" suffix withWearLabel() bakes into the
          // option label belongs to the wear line under it.
          var fullLabel = sel ? sel[0] : '—', mainLabel = fullLabel;
          var sepIdx = fullLabel ? fullLabel.lastIndexOf(' · ') : -1;
          if (slot !== 'agent' && sepIdx > 0 && WEAR_PRESETS && fullLabel.substring(sepIdx + 3) === wearPresetLabel(slot)) mainLabel = fullLabel.substring(0, sepIdx);
          rec.fullLabel = fullLabel;
          var stock = isStockMeta(meta);
          var color = stock ? '#c8d3dfff' : (meta.color || '#e6eaef');
          disp.text = mainLabel; disp.style.color = color;
          bar.style.backgroundColor = color;
          applySwatch(swatch, meta);
          // The sig tracks the OPTION SET + selection only -- NOT rec.query, which filters through
          // rebuildRows() and must never force a full rebuild.
          var sig = ''; for (var k = 0; k < rec.opts.length; k++) sig += rec.opts[k][0] + '|'; sig += '#' + selValue;
          if (sig !== rec.sig) {
            rec.sig = sig;
            if (pane.visible) rebuild(); else rec.stale = true;
          }
        }
      };
    }
)EDJS"
R"EDJS(
    // Text boost: one multiplier over every label in the modal (all created through clbl()). The
    // 7b card is drawn at its design size (CUST_SCALE 1), so the boost is 1 and the px in this file
    // are the px on screen. Live-tunable via customizeFont(pct).
    var CUST_FONT_BOOST = 1;
    function cfs(size) {
      var n = parseFloat(size);
      var k = (CUST_FONT_BOOST > 0) ? CUST_FONT_BOOST : 1; // never NaN a font size
      return (n > 0) ? Math.round(n * k) : size;
    }
    function clbl(parent, text, color, size) { return lbl(parent, text, color, cfs(size)); }

    // ---- Type scale ------------------------------------------------------
    // Named steps for the text that is not set to a design-specified size directly. The 7b layout
    // names most of its own sizes (10px captions, 12-13px names), and those are written inline.
    var TYPE = {
      caption: 12,   // micro-labels: the wear caption, rarity chips, counts
      body:    14,   // buttons, dialog copy
      strong:  17,   // emphasis
      hero:    18,   // the modal's own header
      title:   20    // dialog titles
    };

)EDJS"
R"EDJS(
    // ---- F8 (movie director HUD) skin -----------------------------------
    // Surface tokens shared with MovieHudJs.h's F8 cards (bluedots texture over a translucent base),
    // so the modal and its reset confirm read as the same product.
    var F8 = {
      // The window is a dark blue-GREY, not black: the cards and the preview inside it are darker,
      // and the separation comes from LUMINANCE, so it survives whatever is behind the modal.
      win:   'rgba(11,14,20,0.94)',
      chrome:'#0f131aff',          // the header strip (design 7b)
      scrim: 'rgba(0,0,0,0.58)',   // click-outside backdrop
      dots:  'url("s2r://panorama/images/backgrounds/bluedots_large_png.vtex")',
      dotsSize: '620px 620px', dotsOpacity: '0.03',
      hair:  '#ffffff14',          // borders and rules inside the card
      rule:  '#ffffff12',          // the column dividers
      radius: '6px',
      // ONE inline inset for the reset confirm's bands.
      inset: '26px'
    };
    // Applies the F8 surface (colour + bluedots) to a panel.
    function f8Surface(p, color, radius, dotsOpacity) {
      if (!p) return p;
      p.style.backgroundColor = color || F8.win;
      p.style.backgroundImage = F8.dots;
      p.style.backgroundSize = F8.dotsSize;
      p.style.backgroundRepeat = 'repeat';
      p.style.backgroundImgOpacity = dotsOpacity || F8.dotsOpacity;
      if (radius) p.style.borderRadius = radius;
      return p;
    }
    // OWN ROOT, not a child of the editor root: a panel's z-index is scoped to its root, so as a
    // child of the z53 editor root this modal rendered BEHIND the timeline (z55) and Graph (z65)
    // roots. Own root at z230 wins over every HUD root. Deleted alongside CamEditorRoot in
    // CameraEditorHud::Teardown.
    var custOverlay = $.CreatePanel('Panel', ctx, 'CamEditorCustomizeRoot', {});
    custOverlay.visible = false; custOverlay.hittest = true;
    custOverlay.style.width = '100%'; custOverlay.style.height = '100%'; custOverlay.style.zIndex = '230';
    custOverlay.style.backgroundColor = F8.scrim;
    custOverlay.SetPanelEvent('onactivate', function () { closeCustomize(); }); // click-outside closes
    // The card: 1240 x 720, the design's own size, centred. A fixed size rather than a percentage so
    // the three columns (262 | 440 | the picker) and the 3-up tile grid land on their design widths.
    var custWin = mk('Panel', custOverlay); custWin.hittest = true;
    custWin.style.width = '1240px'; custWin.style.height = '720px';
    custWin.style.horizontalAlign = 'center'; custWin.style.verticalAlign = 'center';
    custWin.style.backgroundColor = S.clear;
    custWin.style.flowChildren = 'down';
    var custSurface = mk('Panel', custWin); custSurface.hittest = true;
    custSurface.style.width = '100%'; custSurface.style.height = '100%';
    custSurface.style.flowChildren = 'down'; custSurface.style.overflow = 'clip';
    f8Surface(custSurface, F8.win, F8.radius);
    custSurface.style.border = '1px solid ' + F8.hair;
    // Live scale knob ($.CamEditor.customizeScale(pct)). 1 = the design size. Layout metrics are not
    // affected by a Panorama transform, so positionPreview3d() multiplies the stage rect by it.
    var CUST_SCALE = 1;
    function custScale() { return CUST_SCALE; }
    // Live opacity knob for the card surface ($.CamEditor.customizeOpacity(pct)).
    function applyCustOpacity(alpha) {
      F8.win = 'rgba(11,14,20,' + alpha + ')';
      f8Surface(custSurface, F8.win, F8.radius);
    }
    function applyCustScale() {
      custWin.style.transformOrigin = '50% 50%';
      custWin.style.transform = 'scale3d(' + CUST_SCALE + ',' + CUST_SCALE + ',1)';
      // The reset confirm is its own root-level overlay (it has to sit ABOVE this modal), so it does
      // not inherit the transform and is scaled explicitly to stay the same visual size.
      if (typeof custResetBox !== 'undefined' && custResetBox) {
        custResetBox.style.transformOrigin = '50% 50%';
        custResetBox.style.transform = 'scale3d(' + CUST_SCALE + ',' + CUST_SCALE + ',1)';
      }
      if (typeof positionPreview3d === 'function') positionPreview3d();
    }
    custWin.SetPanelEvent('onactivate', function () { /* swallow clicks inside the window */ });

    // Header, 56px: CUSTOMIZE PLAYER · <NAME>, the pickup banner, then Reset to default / Cancel /
    // Apply and ✕. The action bar lives up here so the body gets the full height.
    var custHead = mk('Panel', custSurface); custHead.style.flowChildren = 'right'; custHead.style.width = '100%';
    custHead.style.height = '56px'; custHead.style.paddingLeft = '18px'; custHead.style.paddingRight = '18px';
    custHead.style.backgroundColor = F8.chrome; custHead.style.borderBottom = '1px solid ' + F8.hair;
    var custTitleWrap = mk('Panel', custHead); custTitleWrap.style.flowChildren = 'right';
    custTitleWrap.style.width = 'fill-parent-flow(1.0)'; custTitleWrap.style.verticalAlign = 'center';
    var custTitlePrefix = clbl(custTitleWrap, 'Customize player', S.value, TYPE.hero); custTitlePrefix.style.fontWeight = 'bold';
    custTitlePrefix.style.letterSpacing = '2px'; custTitlePrefix.style.verticalAlign = 'center';
    custTitlePrefix.style.textTransform = 'uppercase';   // casing in the style, copy in natural case
    var custTitleDot = clbl(custTitleWrap, '·', '#6b7480ff', TYPE.hero); custTitleDot.style.verticalAlign = 'center';
    custTitleDot.style.marginLeft = '14px'; custTitleDot.style.marginRight = '14px';
    var custTitleName = clbl(custTitleWrap, 'Player', S.accent, TYPE.hero); custTitleName.style.fontWeight = 'bold';
    custTitleName.style.letterSpacing = '2px'; custTitleName.style.verticalAlign = 'center';
    // The player name is demo-supplied: cased by text-transform rather than toUpperCase(), so the raw
    // string stays intact for the ellipsis.
    custTitleName.style.textTransform = 'uppercase';
    custTitleName.style.whiteSpace = 'nowrap'; custTitleName.style.textOverflow = 'ellipsis';
    custTitleName.style.maxWidth = '220px';
    // Pickup ownership is header metadata beside the target name, unchanged from before.
    var custPickupBanner = mk('Panel', custTitleWrap); custPickupBanner.visible = false; custPickupBanner.hittest = false;
    custPickupBanner.style.width = 'fit-children'; custPickupBanner.style.maxWidth = '470px'; custPickupBanner.style.height = '28px';
    custPickupBanner.style.flowChildren = 'right'; custPickupBanner.style.verticalAlign = 'center';
    custPickupBanner.style.marginLeft = '14px'; custPickupBanner.style.paddingLeft = '8px'; custPickupBanner.style.paddingRight = '10px';
    custPickupBanner.style.backgroundColor = '#3a2a10cc'; custPickupBanner.style.border = '1px solid #ffb34755';
    custPickupBanner.style.borderRadius = '4px';
    var custPickupIcon = mk('Panel', custPickupBanner); custPickupIcon.hittest = false;
    custPickupIcon.style.width = '16px'; custPickupIcon.style.height = '16px'; custPickupIcon.style.verticalAlign = 'center';
    custPickupIcon.style.marginRight = '8px'; custPickupIcon.style.borderRadius = '8px'; custPickupIcon.style.backgroundColor = S.accent;
    var custPickupIconLbl = clbl(custPickupIcon, '!', '#241a06ff', 11); custPickupIconLbl.hittest = false;
    custPickupIconLbl.style.fontWeight = 'bold'; custPickupIconLbl.style.width = '100%'; custPickupIconLbl.style.textAlign = 'center';
    custPickupIconLbl.style.verticalAlign = 'center';
    var custPickupLbl = clbl(custPickupBanner, '', '#ffdba6', 11); custPickupLbl.style.width = 'fit-children';
    custPickupLbl.style.maxWidth = '430px'; custPickupLbl.style.letterSpacing = '1px';
    custPickupLbl.style.fontWeight = 'bold'; custPickupLbl.style.whiteSpace = 'nowrap';
    custPickupLbl.style.textOverflow = 'ellipsis'; custPickupLbl.style.verticalAlign = 'center';
    custPickupLbl.style.textTransform = 'uppercase';   // text stays natural-case, see updatePickupBanner
    // Reset to default / Cancel / Apply are built into this row by the action block further down;
    // the ✕ is appended after them there, so it stays the last thing on the line.
    var custHeadActions = mk('Panel', custHead); custHeadActions.style.flowChildren = 'right';
    custHeadActions.style.verticalAlign = 'center'; custHeadActions.style.marginLeft = '14px';

    // Body: three columns -- the slot list (262), the preview (440, built by the preview fragment),
    // and the picker (the rest).
    var custBody = mk('Panel', custSurface); custBody.style.flowChildren = 'right'; custBody.style.width = '100%';
    custBody.style.height = 'fill-parent-flow(1.0)';
    var custSlotCol = mk('Panel', custBody); custSlotCol.hittest = true;
    custSlotCol.style.width = '262px'; custSlotCol.style.height = '100%'; custSlotCol.style.flowChildren = 'down';
    custSlotCol.style.paddingTop = '14px'; custSlotCol.style.paddingBottom = '14px';
    custSlotCol.style.paddingLeft = '12px'; custSlotCol.style.paddingRight = '12px';
    custSlotCol.style.borderRight = '1px solid ' + F8.rule;
)EDJS"
#include "CameraEditorCustomizePreviewJs.h"
R"EDJS(
    function normalizeTeamName(t) {
      t = (t || '').toString().toUpperCase();
      if (t === '3' || t === 'CT' || t.indexOf('COUNTER') >= 0) return 'CT';
      if (t === '2' || t === 'T' || t.indexOf('TERROR') >= 0) return 'T';
      return '';
    }
    function normalizeLoadoutWeapon(w) {
      if (!w || !(parseInt(w.defIndex || 0, 10) > 0)) return null;
      return {
        entityIndex: parseInt(w.entityIndex || -1, 10),
        defIndex: parseInt(w.defIndex || 0, 10) || 0,
        paintKit: parseInt(w.paintKit || 0, 10) || 0,
        wear: num(w.wear),
        // Per-slot pickup ownership (C++ derives it from the weapon ENTITY's original-owner xuid,
        // so it is independent of which weapon the player currently has out).
        pickup: !!w.pickup,
        ownerSteamId: w.ownerSteamId ? ('' + w.ownerSteamId) : '',
        ownerName: w.ownerName ? ('' + w.ownerName) : ''
      };
    }
    // Slot-level pickup state: TRUE when the weapon sitting in this loadout slot originally belongs
    // to another player. Unlike custActiveWeaponPickup this does NOT flicker off when the player
    // switches to a grenade/knife/bomb -- the slot keeps its entity-derived ownership until the
    // weapon is actually dropped, the player changes, or a seek rebuilds the loadout.
    function slotPickup(slot) {
      if (slot !== 'primary' && slot !== 'secondary') return false;
      var w = custLoadout[slot];
      return !!(w && w.pickup && w.ownerSteamId);
    }
    function slotOwnerSteam(slot) { var w = custLoadout[slot]; return (w && w.ownerSteamId) || ''; }
    function slotOwnerName(slot) {
      var w = custLoadout[slot];
      if (!w || !w.ownerSteamId) return '';
      return w.ownerName || ('Player ' + w.ownerSteamId);
    }
    function collectPanelText(panel, out, depth) {
      if (!panel || depth > 24) return;
      try {
        if (typeof panel.text === 'string' && panel.text) out.push(panel.text);
      } catch (e) {}
      var count = 0;
      try { count = panel.GetChildCount ? panel.GetChildCount() : 0; } catch (e2) { count = 0; }
      for (var i = 0; i < count; i++) {
        var child = null;
        try { child = panel.GetChild(i); } catch (e3) { child = null; }
        collectPanelText(child, out, depth + 1);
      }
    }
    function hudWeaponText() {
      var roots = ['HudWeaponSelection', 'hud-WPN-main', 'HudAmmo'];
      var text = [];
      for (var i = 0; i < roots.length; i++) {
        var p = null;
        try { p = $.GetContextPanel().FindChildTraverse(roots[i]); } catch (e) { p = null; }
        collectPanelText(p, text, 0);
      }
      return text.join(' | ');
    }
    function inferActivePaintFromHud(slot) {
      if (!slot) return null;
      var activeText = hudWeaponText();
      if (!activeText) return null;
      var arr = COSMETICS[slot] || [];
      var activeDef = parseInt(custActiveWeaponDef || 0, 10) || 0;
      for (var i = 0; i < arr.length; i++) {
        var label = arr[i][0] || '', meta = arr[i][2] || {};
        var def = parseInt(meta.def || 0, 10) || 0;
        var paint = parseInt(meta.paint || meta.paintKit || 0, 10) || 0;
        if (!label || paint <= 0 || def !== activeDef) continue;
        if (activeText.indexOf(label) >= 0) {
          return { value: arr[i][1], defIndex: def, paintKit: paint };
        }
      }
      return null;
    }
    function applyCustomizeTargetLoadout(ct) {
      custLoadout = { primary: null, secondary: null, knife: null, gloves: null, agentModel: '' };
      if (ct && ct.weapons) {
        custLoadout.primary = normalizeLoadoutWeapon(ct.weapons.primary);
        custLoadout.secondary = normalizeLoadoutWeapon(ct.weapons.secondary);
        custLoadout.knife = normalizeLoadoutWeapon(ct.weapons.knife);
        custLoadout.gloves = normalizeLoadoutWeapon(ct.weapons.gloves);
      }
      if (ct && typeof ct.agentModel === 'string') custLoadout.agentModel = ct.agentModel;
      var activeSlot = WEAPON_SLOT_BY_DEF[custActiveWeaponDef] || '';
      if (activeSlot && !custLoadout[activeSlot])
        custLoadout[activeSlot] = { entityIndex: parseInt(ct && ct.activeWeaponIndex || -1, 10), defIndex: custActiveWeaponDef, paintKit: 0, wear: 0.01 };
      if (activeSlot && custLoadout[activeSlot] && !(parseInt(custLoadout[activeSlot].paintKit || 0, 10) > 0)) {
        var inferred = inferActivePaintFromHud(activeSlot);
        if (inferred) custLoadout[activeSlot].paintKit = inferred.paintKit;
      }
    }
    function customizeLoadoutSignature() {
      function part(slot) {
        var w = custLoadout[slot] || {};
        return [parseInt(w.defIndex || 0, 10) || 0, parseInt(w.paintKit || 0, 10) || 0, Number(w.wear || 0).toFixed(4)].join(':');
      }
      return [custTargetKey || '', normalizeTeamName(custTargetTeam), part('primary'), part('secondary'), part('knife'), part('gloves'), custLoadout.agentModel || ''].join('|');
    }
    // The weapon this slot's FINISH dropdown re-skins: always the one the player is actually
    // holding. The weapon PICKER is gone -- you no longer choose an arbitrary gun out of the
    // catalog and skin that. The skin is still stored per weapon def, so it reappears whenever
    // the player has that weapon again.
    function effectiveSlotDef(slot) {
      if (slot !== 'primary' && slot !== 'secondary') return 0;
      return (custLoadout[slot] && parseInt(custLoadout[slot].defIndex || 0, 10)) || 0;
    }
    // Route finish edits to the ORIGINAL owner when the slot's held weapon is a pickup and the
    // dropdown is editing that exact weapon. Keyed off the SLOT's entity-derived ownership
    // (slotPickup), with the legacy active-weapon fields kept only as a fallback, so switching to a
    // grenade/knife mid-edit no longer flips the routing (or the warning) on and off.
    function shouldRoutePickupToOwner(slot) {
      if (slot !== 'primary' && slot !== 'secondary') return false;
      var held = (custLoadout[slot] && parseInt(custLoadout[slot].defIndex || 0, 10)) || 0;
      if (slotPickup(slot)) return held > 0 && effectiveSlotDef(slot) === held;
      if (!custActiveWeaponPickup || !custActiveWeaponOwnerSteam || custActiveWeaponDef <= 0) return false;
      if (custActiveWeaponSlot !== slot) return false;
      return effectiveSlotDef(slot) === custActiveWeaponDef;
    }
    function pickupOwnerSteam(slot) {
      if (slotPickup(slot)) return slotOwnerSteam(slot);
      return custActiveWeaponOwnerSteam;
    }
    function pickupOwnerName(slot) {
      if (slotPickup(slot)) return slotOwnerName(slot);
      return custActiveWeaponOwnerName || (custActiveWeaponOwnerSteam ? ('Player ' + custActiveWeaponOwnerSteam) : '');
    }
)EDJS"
// Split here: MSVC caps a single raw string literal at ~16380 bytes (C2026). Adjacent
// literals are concatenated by the compiler, so this stays one JS blob.
R"EDJS(
    function resolveCosmeticTarget(slot) {
      var target = (custTargetKey && custTargetKey.indexOf('steam:') === 0)
        ? custTargetKey.substring(6) : 'current';
      if (shouldRoutePickupToOwner(slot)) return pickupOwnerSteam(slot) || target;
      return target;
    }
    // Pickup attribution is driven by SLOT ownership, so it stays up while the player waves a
    // grenade/knife around and only clears when the pickup is actually gone.
    function pickupSlots() {
      var out = [];
      if (slotPickup('primary')) out.push('primary');
      if (slotPickup('secondary')) out.push('secondary');
      return out;
    }
    function updatePickupBanner() {
      var slots = pickupSlots();
      if (primaryDrop && primaryDrop.setPickup) primaryDrop.setPickup(slotPickup('primary'));
      if (secondaryDrop && secondaryDrop.setPickup) secondaryDrop.setPickup(slotPickup('secondary'));
      if (!slots.length) {
        custPickupBanner.visible = false;
        logPickupState('banner');
        return;
      }
      var owners = [], seenOwners = {};
      for (var i = 0; i < slots.length; i++) {
        var s = slots[i];
        var owner = slotOwnerName(s);
        if (!seenOwners[owner]) { seenOwners[owner] = true; owners.push(owner); }
      }
      var holder = custTargetName || 'THIS PLAYER';
      var ownerLoadout = owners.length === 1 ? (owners[0] + '\'S LOADOUT') : (owners.join(' / ') + ' LOADOUTS');
      // Natural case in the string, uppercase in the STYLE (custPickupLbl sets text-transform). These
      // two names come from the demo, and JS toUpperCase() is locale-insensitive and length-changing:
      // German 'ß' becomes 'SS', which grows the string -- and this label is ellipsised at a maxWidth,
      // so uppercasing could itself push a name into truncation. text-transform cases at draw time and
      // leaves the underlying text intact. '’' is the real apostrophe, not the typewriter one.
      custPickupLbl.text = 'Picked up  ·  changes ' + ownerLoadout + ', not ' + holder + '’s';
      custPickupBanner.visible = true;
      logPickupState('banner');
    }
    // Debug: one console line whenever the pickup/warning state changes, so netcon + mvm_debug can
    // verify behavior across weapon switches, player switches, and seeks. Only while mvm_debug is
    // recording: ungated it printed on every state change, and console output is synchronous.
    var lastPickupLogSig = '';
    function logPickupState(tag) {
      if (!mvmDebugOn()) return;
      try {
        function slotDbg(s) {
          var w = custLoadout[s];
          if (!w) return s + '=none';
          return s + '=def' + w.defIndex + (w.pickup ? (' PICKUP owner=' + w.ownerSteamId + '(' + (w.ownerName || '?') + ')') : ' owned');
        }
        var sig = '[mvmpickup] target=' + custTargetKey + '(' + custTargetName + ')' +
          ' activeDef=' + custActiveWeaponDef + ' activeSlot=' + (custActiveWeaponSlot || '-') +
          ' activePickup=' + (custActiveWeaponPickup ? 1 : 0) +
          ' ' + slotDbg('primary') + ' ' + slotDbg('secondary') +
          ' warn=[' + pickupSlots().join(',') + '] banner=' + (custPickupBanner.visible ? 1 : 0);
        if (sig === lastPickupLogSig) return;
        lastPickupLogSig = sig;
        $.Msg(sig + ' (' + tag + ')');
      } catch (e) {}
    }
    function filteredOptions(slot) {
      var arr = COSMETICS[slot] || [];
      if (slot === 'primary' || slot === 'secondary') {
        var slotDef = effectiveSlotDef(slot);
        if (slotDef <= 0) return [];
        var weaponOut = [];
        for (var wi = 0; wi < arr.length; wi++) {
          var wm = arr[wi][2] || {};
          var wPaint = parseInt(wm.paint || wm.paintKit || 0, 10) || 0;
          var wDef = parseInt(wm.def || 0, 10) || 0;
          if ((wPaint === 0 && (wDef === 0 || wDef === slotDef)) || (wPaint !== 0 && wDef === slotDef))
            weaponOut.push(arr[wi]);
        }
        return weaponOut;
      }
      if (slot === 'knife') return knifeOptionsWithCurrentDefault(arr);
      if (slot !== 'agent') return arr;
      var team = normalizeTeamName(custTargetTeam);
      var out = [];
      for (var i = 0; i < arr.length; i++) {
        var meta = arr[i][2] || {};
        if (!team || meta.team === 'ANY' || meta.team === team) out.push(arr[i]);
      }
      return out.length ? out : arr;
    }
    function knifeNameForDef(defIndex, arr) {
      if (defIndex === 42) return 'Default CT knife';
      if (defIndex === 59) return 'Default T knife';
      for (var i = 0; i < arr.length; i++) {
        var meta = arr[i][2] || {};
        if ((parseInt(meta.def || 0, 10) || 0) !== defIndex) continue;
        var label = arr[i][0] || '';
        var split = label.indexOf(' | ');
        return split > 0 ? label.substring(0, split) : label;
      }
      return 'Knife #' + defIndex;
    }
    function knifeOptionsWithCurrentDefault(arr) {
      var w = custLoadout.knife || null;
      var def = w ? (parseInt(w.defIndex || 0, 10) || 0) : 0;
      if (def <= 0) return arr;
      var value = def + ':0';
      for (var i = 0; i < arr.length; i++) if (arr[i][1] === value) return arr;
      var out = [[knifeNameForDef(def, arr), value, { color: rarColor(0), def: def, paint: 0 }]];
      for (var j = 0; j < arr.length; j++) out.push(arr[j]);
      return out;
    }
    // ---- Float range -------------------------------------------------------------------------
    // A finish's float cap comes from the catalog (wearMin/wearMax). The per-item "no cap" flag
    // (custWear[slot].noCap, persisted per weapon+finish through custItemWear) replaces that cap
    // with the full 0..1 range, so an uncapped skin can be pushed to floats it never ships with.
    function wearCapDisabled(slot) {
      var w = custWear[slot];
      return !!(w && w.noCap);
    }
    function wearRangeForSlot(slot) {
      if (wearCapDisabled(slot)) return { lo: 0.0, hi: 1.0 };
      var opt = selOpt(slot, custSel[slot]);
      var meta = opt ? (opt[2] || {}) : {};
      var lo = (typeof meta.wearMin === 'number') ? clamp01(meta.wearMin) : 0.0;
      var hi = (typeof meta.wearMax === 'number') ? clamp01(meta.wearMax) : 1.0;
      if (hi < lo) { var t = lo; lo = hi; hi = t; }
      return { lo: lo, hi: hi };
    }
    function clampWearForSlot(slot, value) {
      var r = wearRangeForSlot(slot);
      value = clamp01(value);
      if (value < r.lo) value = r.lo;
      if (value > r.hi) value = r.hi;
      return value;
    }
    // The bar spans the REACHABLE range, not the raw 0..1 float, so the slider's own 0..1 maps onto
    // [lo, hi]. That is what lets a 0.00-0.15 skin show Factory New / Minimal Wear stretched across
    // the whole control instead of squeezed into its first seventh.
    function wearFracToValue(slot, frac) {
      var r = wearRangeForSlot(slot), d = r.hi - r.lo;
      return r.lo + (d > 0 ? clamp01(frac) * d : 0);
    }
    function wearValueToFrac(slot, value) {
      var r = wearRangeForSlot(slot), d = r.hi - r.lo;
      return d > 0 ? clamp01((clamp01(value) - r.lo) / d) : 0;
    }
    // Only the tiers the range actually intersects, each clipped to that range.
    function wearTiersForSlot(slot) {
      var r = wearRangeForSlot(slot), out = [];
      for (var i = 0; i < WEAR_TIER_ORDER.length; i++) {
        var key = WEAR_TIER_ORDER[i], p = WEAR_PRESETS[key];
        var lo = Math.max(r.lo, p.lo), hi = Math.min(r.hi, p.hi);
        if (hi > lo) out.push({ key: key, lo: lo, hi: hi });
      }
      // A degenerate cap (wearMin === wearMax) intersects nothing under the strict `hi > lo` test
      // and would leave the bar unlabelled, so it collapses to the single tier it sits in.
      if (!out.length) out.push({ key: presetForWearValue(r.lo), lo: r.lo, hi: r.hi });
      return out;
    }
    function wearValue(slot) {
      var w = custWear[slot] || (custWear[slot] = defaultWearForSlot(slot));
      var preset = WEAR_PRESETS[w.preset] || WEAR_PRESETS.fn;
      var raw = (w.preset === 'custom') ? num(w.custom) : preset.value;
      return clampWearForSlot(slot, raw);
    }
    function wearPresetLabel(slot) {
      return (WEAR_PRESETS[presetForWearValue(wearValue(slot))] || WEAR_PRESETS.fn).label;
    }
    function withWearLabel(slot, label) {
      if (!label || !custWear[slot]) return label || '';
      var opt = selOpt(slot, custSel[slot]);
      var meta = opt ? (opt[2] || {}) : {};
      if (!meta.paint) return label;
      return label + ' · ' + wearPresetLabel(slot);
    }
    function defaultWearForSlot(slot) {
      return slot === 'gloves' ? { preset: 'ft', custom: 0.22 } : { preset: 'fn', custom: 0.01 };
    }
    function presetForWearValue(value) {
      value = clamp01(num(value));
      if (value < 0.07) return 'fn';
      if (value < 0.15) return 'mw';
      if (value < 0.38) return 'ft';
      if (value < 0.45) return 'ww';
      return 'bs';
    }
    // noCap rides along with the wear state so the float-cap toggle is remembered PER weapon+finish
    // (custItemWear's key), exactly like the float itself -- switching skins and back restores it.
    function copyWearState(w, slot) {
      var d = defaultWearForSlot(slot);
      w = w || d;
      var custom = (typeof w.custom === 'number' && isFinite(w.custom)) ? w.custom : d.custom;
      return { preset: w.preset || d.preset, custom: custom, noCap: !!w.noCap };
    }
    function itemWearKey(slot, value) {
      return slot + ':' + (value === undefined || value === null ? (custSel[slot] || '0') : ('' + value));
    }
    function saveActiveItemWear(slot) {
      if (!wearControls || !wearControls[slot]) return;
      custItemWear[itemWearKey(slot)] = copyWearState(custWear[slot], slot);
    }
    function loadActiveItemWear(slot) {
      if (!wearControls || !wearControls[slot]) return;
      var key = itemWearKey(slot);
      custWear[slot] = copyWearState(custItemWear[key], slot);
    }
)EDJS"
R"EDJS(
    function decoratedOptions(slot) {
      var src = filteredOptions(slot), out = [];
      for (var i = 0; i < src.length; i++) {
        var meta = src[i][2] || {};
        var m = {}; for (var k in meta) m[k] = meta[k];
        out.push([withWearLabel(slot, src[i][0]), src[i][1], m]);
      }
      return out;
    }
)EDJS"
R"EDJS(
    function optionExists(slot, value) {
      var arr = filteredOptions(slot);
      for (var i = 0; i < arr.length; i++) if (arr[i][1] === value) return true;
      return false;
    }
    function loadoutDefaultSelection(slot) {
      var w = custLoadout[slot] || null;
      if (!w || !(parseInt(w.defIndex || 0, 10) > 0)) return '0';
      var def = parseInt(w.defIndex || 0, 10) || 0;
      var paint = parseInt(w.paintKit || 0, 10) || 0;
      if ((slot === 'primary' || slot === 'secondary') && paint > 0 && optionExists(slot, '' + paint)) return '' + paint;
      if ((slot === 'primary' || slot === 'secondary') && optionExists(slot, def + ':0')) return def + ':0';
      if (slot === 'knife' && paint > 0 && optionExists(slot, def + ':' + paint)) return def + ':' + paint;
      if (slot === 'knife' && optionExists(slot, def + ':0')) return def + ':0';
      return '0';
    }
    // Gloves: if the spectated player wears custom gloves, default to that glove type; otherwise
    // fall back to the team's default gloves (CT 5029 / T 5028), per the "no gloves -> default by
    // side" rule. The exact glove finish isn't networked through the fallback fields, so a custom
    // pair resolves to the first finish of that glove model (its type is what matters here).
    function glovesDefaultSelection() {
      var arr = COSMETICS.gloves || [];
      var w = custLoadout.gloves || null;
      var def = w ? (parseInt(w.defIndex || 0, 10) || 0) : 0;
      var paint = w ? (parseInt(w.paintKit || 0, 10) || 0) : 0;
      if (def > 0 && paint > 0) {
        for (var i = 0; i < arr.length; i++) {
          var m = arr[i][2] || {};
          if ((parseInt(m.def || 0, 10) || 0) === def && (parseInt(m.paint || 0, 10) || 0) === paint) return arr[i][1];
        }
      }
      return (normalizeTeamName(custTargetTeam) === 'CT') ? '5029:0' : '5028:0';
    }
    function agentDefaultSelection() {
      // Match the player's live model path (read off the pawn, e.g.
      // 'agents/models/ctm_st6/ctm_st6_variantg.vmdl') to a catalog agent by its meta.model.
      // NOTE: the catalog key is SINGULAR 'agent' (matching the slot name), not 'agents'.
      var model = custLoadout.agentModel || '';
      if (model) {
        var arr = COSMETICS.agent || [];
        for (var i = 0; i < arr.length; i++) {
          var m = arr[i][2] || {};
          if (m.model && m.model === model) return arr[i][1];
        }
      }
      return 'default';
    }
    function resetSelectionsFromLoadout() {
      custActiveCategory = initialActiveCategory();
      custSel = {
        agent: agentDefaultSelection(),
        primary: loadoutDefaultSelection('primary'),
        secondary: loadoutDefaultSelection('secondary'),
        knife: loadoutDefaultSelection('knife'),
        gloves: glovesDefaultSelection()
      };
      custWear = {
        primary: defaultWearForSlot('primary'),
        secondary: defaultWearForSlot('secondary'),
        knife: defaultWearForSlot('knife'),
        gloves: defaultWearForSlot('gloves')
      };
      for (var slot in custLoadout) {
        var w = custLoadout[slot];
        if (!w || !(parseInt(w.paintKit || 0, 10) > 0)) continue;
        var wear = clamp01(num(w.wear));
        custWear[slot] = { preset: presetForWearValue(wear), custom: wear };
      }
      custItemWear = {};
      saveActiveItemWear('primary');
      saveActiveItemWear('secondary');
      saveActiveItemWear('knife');
      saveActiveItemWear('gloves');
    }

    // RIGHT: final loadout-style controls. No arms/viewmodel override and no CT/T glove split.
)EDJS"
R"EDJS(
    // Column 3, the picker: padding 14px 16px. It holds one pane per slot (built by itemDrop), and
    // only the selected slot's pane is visible.
    var ctrlCol = mk('Panel', custBody); ctrlCol.style.width = 'fill-parent-flow(1.0)'; ctrlCol.style.height = '100%';
    ctrlCol.style.paddingTop = '14px'; ctrlCol.style.paddingBottom = '14px';
    ctrlCol.style.paddingLeft = '16px'; ctrlCol.style.paddingRight = '16px';
    ctrlCol.style.flowChildren = 'down'; ctrlCol.hittest = true;
    var custPickHost = ctrlCol;
)EDJS"
// Split here: MSVC caps a single raw string literal at ~16380 bytes (C2026). Adjacent
// literals are concatenated by the compiler, so this stays one JS blob.
R"EDJS(
    // Rarity colors arrive as #rrggbbaa; swap the alpha byte so one color can be laid down at
    // several strengths in a gradient without hand-maintaining a second palette.
    function hexAlpha(c, aa) {
      c = '' + (c || S.accent);
      if (c.charAt(0) !== '#' || c.length < 7) return c;
      return '#' + c.substring(1, 7) + aa;
    }
    // One direct float control, built in the demo-bar speed scrubber's visual language (dark inset
    // strip, thin tinted handle, zone tags) minus its scrolling gradation lines. The strip carries a
    // rarity-colored fill up to the current float and a tag per REACHABLE wear tier; a nearly
    // transparent native Slider on top supplies reliable Panorama dragging. Its 0..1 spans the
    // reachable range (see wearFracToValue), not the raw float. Fixed geometry at every state: no
    // dropdown, focused TextEntry, or conditional row can resize the card.
    //
    // Design 7b re-skin: the block is pinned to the foot of the picker column -- 10px 12px padding on
    // #141a23, a 24px head (WEAR, the centred readout, Cap on / Cap off) over a 24px bar with the
    // reachable tiers, a rarity-tinted fill and a 3px white handle.
    var WEAR_HEAD_H = 24, WEAR_RAIL_H = 24;
    function wearSliderBlock(parent, slot, category) {
      var wrap = mk('Panel', parent); wrap.style.width = '100%';
      wrap.style.flowChildren = 'down'; wrap.style.marginTop = '10px';
      wrap.style.paddingTop = '10px'; wrap.style.paddingBottom = '10px';
      wrap.style.paddingLeft = '12px'; wrap.style.paddingRight = '12px';
      wrap.style.borderRadius = '4px'; wrap.style.backgroundColor = '#141a23ff';
      wrap.style.border = '1px solid #ffffff14';

      // The head is deliberately NOT a flow row. The float readout has to sit in the true horizontal
      // centre of the block; in a flow row its position is whatever the 'WEAR' label and the CAP
      // button happen to leave over, so it reads off-centre and it SHIFTS every time the CAP button
      // changes size. With no flow, all three children are placed by their own alignment and cannot
      // push each other around.
      var head = mk('Panel', wrap); head.style.width = '100%'; head.style.height = WEAR_HEAD_H + 'px';
      var name = clbl(head, 'Wear', '#8b95a3ff', 11); name.hittest = false;
      name.style.textTransform = 'uppercase'; name.style.fontWeight = 'bold';
      name.style.letterSpacing = '1.5px'; name.style.horizontalAlign = 'left'; name.style.verticalAlign = 'center';
      // Centred, and the largest type in the block: this is the number being read while dragging.
      // Do NOT give it width/height -- a Label sized to its parent anchors to the parent's top-left
      // and the alignment properties stop applying (same trap as the demo-bar readout).
      var value = clbl(head, 'Factory New · 0.010', S.value, 14); value.hittest = false;
      value.style.fontWeight = 'bold'; value.style.whiteSpace = 'nowrap';
      value.style.horizontalAlign = 'center'; value.style.verticalAlign = 'center';
      // MONOSPACED DIGITS. This label is rewritten on every SliderValueChanged and it is centred, so
      // with proportional digits its width changes each update and the readout jitters horizontally
      // under the cursor. That is the very thing the non-flowing head row above was built to prevent
      // (it removed the shift caused by the CAP button; the digits were still causing their own).
      value.style.fontFamily = S.fontMonodigit;
      value.style.transformOrigin = '50% 50%';
      value.style.transitionProperty = 'transform, color';
      value.style.transitionDuration = '0.13s'; value.style.transitionTimingFunction = 'ease-out';
      // Per-item float-cap toggle. Selected (gold) = uncapped, because uncapped is the state worth
      // calling out; the catalog cap is the resting default. It GROWS while uncapped and pops on
      // each toggle, so the state change is felt and not just read -- scaled from its right edge so
      // the growth can never reach the centred readout.
      var capBtn = btn(head, 'Cap on', function () { capPop(); toggleWearCap(slot); }, S.inactive);
      capBtn.style.horizontalAlign = 'right'; capBtn.style.marginRight = '0px';
      capBtn.style.height = WEAR_HEAD_H + 'px'; capBtn.style.verticalAlign = 'center';
      capBtn.style.paddingTop = '0px'; capBtn.style.paddingBottom = '0px';
      capBtn.__lbl.style.verticalAlign = 'center';
      capBtn.style.paddingLeft = '9px'; capBtn.style.paddingRight = '9px';
      capBtn.__lbl.style.letterSpacing = '1px'; capBtn.__lbl.style.textTransform = 'uppercase';
      capBtn.style.transformOrigin = '100% 50%';
      capBtn.style.transitionProperty = 'transform';
      capBtn.style.transitionDuration = '0.15s'; capBtn.style.transitionTimingFunction = 'ease-out';
      // The UNCAPPED state used to REST at scale3d(1.14) permanently. A resting transform resamples
      // the button's already-rasterised label, and this modal is drawn at CUST_SCALE (0.765) on top of
      // that -- so the CAP text was scaled twice and read soft. The state already has two static
      // channels (btnSel's gold fill, and the CAP ON / CAP OFF label itself), so the extra weight is
      // now REAL type and padding. The transform is reserved for the transient pop.
      function capRest(noCap) {
        if (capBtn.__noCap === noCap) return;   // style writes force a relayout and sync() is hot
        capBtn.__noCap = noCap;
        capBtn.__lbl.style.fontSize = cfs(noCap ? TYPE.body : TYPE.caption) + 'px';
        capBtn.style.paddingLeft = (noCap ? 11 : 9) + 'px';
        capBtn.style.paddingRight = (noCap ? 11 : 9) + 'px';
      }
      capRest(false);
      function capApply(s) { capBtn.style.transform = 'scale3d(' + s.toFixed(3) + ',' + s.toFixed(3) + ',1)'; }
      // Overshoot, then settle. SEQUENCE-GUARDED, exactly like goLive() below: toggling CAP twice
      // inside 150ms otherwise let the FIRST schedule fire and write the resting scale, collapsing the
      // second pop the instant it began.
      var capPopSeq = 0;
      function capPop() {
        capApply(1.22);
        var mySeq = ++capPopSeq;
        try {
          $.Schedule(0.15, function () {
            if (mySeq !== capPopSeq) return;    // a newer pop already owns the settle
            if (capBtn && capBtn.IsValid && capBtn.IsValid()) capApply(1);
          });
        } catch (e) { capApply(1); }
      }
)EDJS"
// Split here: MSVC caps a single raw string literal at ~16380 bytes (C2026). Adjacent
// literals are concatenated by the compiler, so this stays one JS blob.
R"EDJS(
      var rail = mk('Panel', wrap); rail.style.width = '100%'; rail.style.height = WEAR_RAIL_H + 'px';
      rail.style.marginTop = '6px'; rail.style.borderRadius = '3px'; rail.style.overflow = 'clip';
      rail.style.border = '1px solid #ffffff14'; rail.style.backgroundColor = '#0c0f14ff';
      var fill = mk('Panel', rail); fill.hittest = false; fill.style.height = '100%';
      fill.style.width = '0%'; fill.style.position = '0px 0px 0px';
      // Width is NOT transitioned: SliderValueChanged fires continuously through a drag, and an
      // animated width turns into visible rubber-banding behind the cursor. Only the colour eases.
      fill.style.transitionProperty = 'background-color';
      fill.style.transitionDuration = '0.16s'; fill.style.transitionTimingFunction = 'ease-out';
      // Boundary ticks BETWEEN the tiers -- the thing that was missing: without them there was no
      // visible mark saying where Factory New ends and Minimal Wear begins, so the tags floated over
      // an undivided bar. Pooled at (tiers - 1) and positioned in percent by layoutZones().
      var zoneTicks = [];
      for (var ti = 1; ti < WEAR_TIER_ORDER.length; ti++) {
        var zt = mk('Panel', rail); zt.hittest = false; zt.visible = false;
        zt.style.position = '0px 0px 0px'; zt.style.width = '2px'; zt.style.height = '100%';
        zt.style.backgroundColor = '#ffffff2a';
        zt.style.transitionProperty = 'background-color';
        zt.style.transitionDuration = '0.14s'; zt.style.transitionTimingFunction = 'ease-out';
        zoneTicks.push(zt);
      }
      // Tier tags, pooled at the maximum tier count. Each is a full-height BOX spanning its tier,
      // with the label centred inside it by alignment: a Label positioned and sized itself sat
      // low, left-aligned and clipped by the rail in game.
      var zoneLabels = [];
      for (var zi = 0; zi < WEAR_TIER_ORDER.length; zi++) {
        var zl = mk('Panel', rail); zl.hittest = false; zl.visible = false;
        zl.style.position = '0px 0px 0px'; zl.style.width = '20%'; zl.style.height = '100%';
        zl.style.transitionProperty = 'opacity';
        zl.style.transitionDuration = '0.14s'; zl.style.transitionTimingFunction = 'ease-out';
        var zt2 = clbl(zl, '', '#c6cedaff', 10); zt2.hittest = false;
        zt2.style.horizontalAlign = 'center'; zt2.style.verticalAlign = 'center';
        zt2.style.fontWeight = 'bold'; zt2.style.letterSpacing = '1px'; zt2.style.whiteSpace = 'nowrap';
        zt2.style.transitionProperty = 'color';
        zt2.style.transitionDuration = '0.14s'; zt2.style.transitionTimingFunction = 'ease-out';
        zl.__lbl = zt2;
        zoneLabels.push(zl);
      }
      var handle = mk('Panel', rail); handle.hittest = false; handle.style.width = '3px';
      handle.style.height = '100%'; handle.style.verticalAlign = 'center'; handle.style.borderRadius = '2px';
      handle.style.backgroundColor = S.value;
      // Same split the demo-bar scrubber uses: the handle's POSITION snaps (so it stays under the
      // cursor) while its size, colour and glow ease. Transitioning position instead makes the drag
      // feel laggy, which is the opposite of reactive.
      handle.style.transitionProperty = 'width, height, background-color, box-shadow';
      handle.style.transitionDuration = '0.09s'; handle.style.transitionTimingFunction = 'ease-out';
      var slider = $.CreatePanel('Slider', rail, 'CustWearSlider' + slot, { direction: 'horizontal' });
      slider.AddClass('HorizontalSlider'); slider.style.width = '100%'; slider.style.height = '100%';
      slider.style.opacity = '0.01';
      // The native parts still drew a dark block beside our handle in game (the 1% opacity did
      // not reach the thumb), so they are made fully transparent themselves. The Slider stays
      // hit-testable: it is the drag mechanism.
      function hideNativeSliderParts() {
        var ids = ['SliderThumb', 'SliderTrack', 'SliderTrackProgress'];
        for (var hi = 0; hi < ids.length; hi++) {
          try {
            var np = slider.FindChildTraverse(ids[hi]);
            if (np) { np.style.opacity = '0'; np.style.backgroundColor = S.clear; np.style.border = '0px solid ' + S.clear; np.style.boxShadow = 'rgba(0,0,0,0) 0px 0px 0px 0px'; }
          } catch (e) {}
        }
      }
      hideNativeSliderParts();
      try { $.Schedule(0.1, hideNativeSliderParts); } catch (e) {}   // the parts may be built a frame late
      var last = null, lastZones = null, lastFrac = 0, lastColor = S.accent;
      var handleW = 3;   // kept in sync by react(); seatHandle() needs it to centre the handle
      // Interaction state. `hot` is hover, `live` is "the float changed in the last moment" -- a
      // drag produces a stream of SliderValueChanged with no matching mouse events, so the drag is
      // inferred from the value stream and decays on a timer.
      var hot = false, live = false, liveSeq = 0;
      var tierPopSeq = 0;   // settle-guard for the tier-change pop in sync(); same idea as liveSeq
      slider.SetPanelEvent('onmouseover', function () { hot = true; react(); });
      slider.SetPanelEvent('onmouseout', function () { hot = false; react(); });
      function goLive() {
        live = true; liveSeq++;
        var mySeq = liveSeq;
        react();
        try {
          $.Schedule(0.25, function () {
            if (mySeq !== liveSeq) return;               // a newer change already re-armed it
            if (!slider || !slider.IsValid || !slider.IsValid()) return;
            live = false; react();
          });
        } catch (e) { live = false; }
      }
      // Everything whose look depends on hover/drag rather than on the value itself.
      function react() {
        var active = hot || live;
        handleW = (live ? 5 : (hot ? 4 : 3));
        handle.style.width = handleW + 'px';
        
        seatHandle();   // the width just changed, so the x that centres it on the value changed too
        handle.style.boxShadow = active
          ? (hexAlpha(lastColor, '80') + ' 0px 0px 9px 1px')
          : 'rgba(0,0,0,0) 0px 0px 0px 0px';
        for (var i = 0; i < zoneTicks.length; i++) zoneTicks[i].style.backgroundColor = active ? '#ffffff5c' : '#ffffff2a';
        if (lastZones) paintZoneText(active);
      }
      // Handle position, split out so react() can re-seat it when the handle's WIDTH changes. The
      // travel is (rail - handle), not (rail - 4): with a baked 4 the handle drifted off the value as
      // soon as it grew on hover, which is the opposite of "native to where I am".
      function seatHandle() {
        var cssW = (rail.actuallayoutwidth || 0) / (rail.actualuiscale_x || 1);
        if (cssW > handleW) handle.style.x = (lastFrac * (cssW - handleW)).toFixed(1) + 'px';
        else handle.style.position = (lastFrac * 100).toFixed(1) + '% 0px 0px';
      }
      function paint(frac, color) {
        frac = clamp01(frac);
        var c = color || S.accent;
        lastFrac = frac; lastColor = c;
        seatHandle();
        fill.style.width = (frac * 100).toFixed(2) + '%';
        fill.style.backgroundColor = 'gradient( linear, 0% 0%, 100% 0%, from( ' + hexAlpha(c, '2e')
          + ' ), color-stop( 0.7, ' + hexAlpha(c, '5c') + ' ), to( ' + hexAlpha(c, '8a') + ' ) )';
      }
      // Tag colour/opacity, split out of layoutZones so react() can re-drive it on hover without
      // recomputing any geometry. The active tier always reads full strength; the rest lift toward
      // legible only while the control is being used, so at rest the bar stays quiet.
      function paintZoneText(active) {
        for (var i = 0; i < zoneLabels.length; i++) {
          var zl = zoneLabels[i], t = lastZones.tiers[i];
          if (!t || !zl.visible) continue;
          var on = (t.key === lastZones.activeKey);
          zl.__lbl.style.color = on ? (lastZones.color || S.accent) : '#c6cedaff';
          zl.style.opacity = on ? '1' : (active ? '0.7' : '0.34');
        }
      }
      // Sections are laid out from the tier list rather than from fixed float positions, so a capped
      // skin shows only what it can reach and those tiers stretch edge to edge.
      function layoutZones(tiers, lo, hi, activeKey, color) {
        lastZones = { tiers: tiers, lo: lo, hi: hi, activeKey: activeKey, color: color };
        var span = hi - lo;
        var cssW = (rail.actuallayoutwidth || 0) / (rail.actualuiscale_x || 1);
        for (var i = 0; i < zoneLabels.length; i++) {
          var zl = zoneLabels[i], t = tiers[i];
          if (!t) { zl.visible = false; continue; }
          var left = span > 0 ? ((t.lo - lo) / span) : 0;
          var w = span > 0 ? ((t.hi - t.lo) / span) : 1;
          // A sliver tier (e.g. the 0.07-0.08 of Minimal Wear a 0.00-0.08 finish can reach) is too
          // narrow for its tag, which would then spill over the neighbouring one. Drop the tag and
          // keep the section: the readout above still names the tier the float is actually in.
          zl.visible = !(cssW > 0 && w * cssW < 26);
          if (!zl.visible) continue;
          zl.__lbl.text = WEAR_ABBREV[t.key] || '';
          // In px once the rail has a width (relayout() re-runs this when it does): a percent x in
          // `position` is not portable -- the browser mockup resolves it against the tag, not the rail.
          if (cssW > 0) {
            zl.style.position = (left * cssW).toFixed(1) + 'px 0px 0px';
            zl.style.width = (w * cssW).toFixed(1) + 'px';
          } else {
            zl.style.position = (left * 100).toFixed(2) + '% 0px 0px';
            zl.style.width = (w * 100).toFixed(2) + '%';
          }
        }
        // A tick marks each INTERNAL boundary, so tier i's tick sits at tier i+1's low edge. The
        // last tier has no trailing tick (the rail's own edge is the boundary there).
        for (var k = 0; k < zoneTicks.length; k++) {
          var nt = tiers[k + 1];
          if (!nt) { zoneTicks[k].visible = false; continue; }
          zoneTicks[k].visible = true;
          var tf = span > 0 ? ((nt.lo - lo) / span) : 0;
          zoneTicks[k].style.position = (cssW > 0 ? ((tf * cssW).toFixed(1) + 'px') : ((tf * 100).toFixed(2) + '%')) + ' 0px 0px';
        }
        paintZoneText(hot || live);
      }
      $.RegisterEventHandler('SliderValueChanged', slider, function (panel, v) {
        var nextFrac = clamp01(v);
        if (last !== null && Math.abs(nextFrac - last) < 0.0005) return;
        last = nextFrac;
        goLive();
        if (category) setActiveCategory(category);
        // light = true: the drag must not pay for a full repopulate per tick (see setWear).
        setWear(slot, 'custom', wearFracToValue(slot, nextFrac), true);
      });
      return {
        slider: slider,
        // A pane is built hidden, so the first sync() laid the tier tags out against a 0-wide rail
        // (every tag "fits"). Re-lay them out once the rail has a real width, and again if it changes.
        relayout: function () {
          if (!lastZones || !wrap.visible) return;
          var cssW = (rail.actuallayoutwidth || 0) / (rail.actualuiscale_x || 1);
          if (!(cssW > 0) || Math.abs(cssW - (wrap.__lw || 0)) < 1) return;
          wrap.__lw = cssW;
          layoutZones(lastZones.tiers, lastZones.lo, lastZones.hi, lastZones.activeKey, lastZones.color);
          paint(lastFrac, lastColor);
        },
        setVisible: function (on) { if (wrap.visible !== on) wrap.visible = on; },
        isVisible: function () { return wrap.visible !== false; },
        sync: function (next, color) {
          next = clamp01(next);
          var r = wearRangeForSlot(slot);
          var frac = wearValueToFrac(slot, next);
          last = frac;
          // NEVER write the Slider's own value back while the user is dragging it. The round trip
          // frac -> wearFracToValue -> clampWearForSlot -> wearValueToFrac does not land on exactly
          // the input frac, so this wrote a slightly different value straight back into the control
          // being dragged: the handle jumped away from the cursor and the correction re-fired the
          // event. That fight is what made the slider feel like it had a mind of its own.
          if (!live && Math.abs((slider.value || 0) - frac) >= 0.0005) slider.value = frac;
          var noCap = wearCapDisabled(slot);
          capBtn.__lbl.text = noCap ? 'Cap off' : 'Cap on';
          btnSel(capBtn, noCap);
          // Uncapped is the loud state, so the button rests heavier there -- as type and padding, not
          // as a transform (see capRest). Idempotent, so this is safe on sync()'s hot path and it can
          // no longer fight a pop that is still in flight.
          capRest(noCap);
          var tier = wearPresetLabel(slot);
          value.text = tier + ' · ' + next.toFixed(3);
          // Crossing into a different tier is the one value change worth announcing: the readout takes
          // the new tier's colour and pops, so a drag past a boundary registers without being read.
          // The FIRST sync only seeds the tier -- opening the modal is not a tier change, and without
          // this every visible wear block would flash once on open.
          var tierChanged = (value.__tier !== undefined && value.__tier !== tier);
          value.__tier = tier;                 // seeded even on the first sync, or nothing ever pops
          if (tierChanged) {
            value.style.color = color || S.accent;
            value.style.transform = 'scale3d(1.09,1.09,1)';
            // SEQUENCE-GUARDED (same idea as liveSeq). Dragging fast across two tier boundaries inside
            // 160ms is easy -- a capped finish can expose five tiers across one rail -- and the FIRST
            // settle would then fire on top of the SECOND pop, snapping the readout back to S.value
            // while it was sitting in a freshly-entered tier. The announcement died exactly when the
            // drag was moving fast enough to need it.
            var mySeq = ++tierPopSeq;
            try {
              $.Schedule(0.16, function () {
                if (mySeq !== tierPopSeq) return;   // a newer tier change owns the settle
                if (!value || !value.IsValid || !value.IsValid()) return;
                value.style.transform = 'scale3d(1,1,1)';
                value.style.color = S.value;
              });
            } catch (e) { value.style.transform = 'scale3d(1,1,1)'; value.style.color = S.value; }
          }
          layoutZones(wearTiersForSlot(slot), r.lo, r.hi, presetForWearValue(next), color);
          paint(frac, color);
          react();
        }
      };
    }
)EDJS"
// Split here: MSVC caps a single raw string literal at ~16380 bytes (C2026). Adjacent
// literals are concatenated by the compiler, so this stays one JS blob.
R"EDJS(
    // ---- The five slots ---------------------------------------------------------------------
    // Agent, Primary, Secondary, Melee / knife, Gloves: one row each in the slot list, one picker
    // pane each in the picker column. Agents, knives and gloves drill down by MODEL (the catalog
    // label carries the axis: "Balkan · Variant F", "Karambit | Doppler"), so a model tile opens
    // that model's variants or finishes.
    var agentDrop = itemDrop(custSlotCol, 'CustAgent', function (v) { pickCosmetic('agent', v); },
      { slot: 'agent', caption: 'Agent', grouped: true, groupLabel: 'All agents', groupSep: ' · ', searchNoun: 'agents' });
    // A secondary action in the picker's crumb row: 28px, quiet.
    function custCrumbBtn(parent, text, onClick, tip) {
      var b = btn(parent, text, onClick, S.label, { tip: tip });
      b.__bgBase = '#0f131aff'; b.__brBase = '#ffffff1f'; b.__bgHover = '#1a2029ff';
      b.style.height = '28px'; b.style.verticalAlign = 'center';
      b.style.paddingTop = '0px'; b.style.paddingBottom = '0px';
      b.style.paddingLeft = '10px'; b.style.paddingRight = '10px';
      b.style.marginRight = '0px'; b.style.marginLeft = '6px'; b.style.borderRadius = '3px';
      b.__lbl.style.fontSize = '12px'; b.__lbl.style.verticalAlign = 'center'; b.__lbl.style.whiteSpace = 'nowrap';
      btnVisual(b);
      return b;
    }
    // A DIFFERENT agent per player, so a server full of clones becomes a believable mixed team. The
    // pool comes from this modal's own catalog (the backend has no agent list), registered in batches
    // so no single console command grows unbounded. The backend does the per-player pick because it
    // is the side that can enumerate players, and it keeps the pick on-team.
    function randomizeAgents() {
      // A bulk action needs BOTH teams, regardless of the selected player's dropdown filter.
      var opts = COSMETICS.agent || [];
      var models = [];
      for (var i = 0; i < opts.length; i++) {
        var m = (opts[i][2] || {}).model;
        if (m) models.push(m);
      }
      if (!models.length) return;
      cmd('mirv_filmmaker cosmetics enabled 1');
      cmd('mirv_filmmaker cosmetics agentpool clear');
      // Model paths have different lengths; bound the actual command, not the model count.
      var prefix = 'mirv_filmmaker cosmetics agentpool add ', batch = prefix;
      for (var b = 0; b < models.length; b++) {
        var next = (batch === prefix ? '' : ' ') + models[b];
        if (batch.length + next.length > 240 && batch !== prefix) {
          cmd(batch); batch = prefix; next = models[b];
        }
        batch += next;
      }
      if (batch !== prefix) cmd(batch);
      cmd('mirv_filmmaker cosmetics allplayers agent random');
      cmd('mirv_filmmaker cosmetics uilog [agent] randomize-all pool=' + models.length);
      previewLog('agentRandom', 'pool=' + models.length);
    }
    var agentRandomBtn = custCrumbBtn(agentDrop.actions, 'Randomize agents', randomizeAgents,
      'Give every player a different agent from their own team.');
    // "Apply to all players" for the agent slot. It writes immediately (like Reset, not like the
    // staged Apply): there is no per-player staging surface for ten players to review. The backend
    // is team-aware, so a T model only lands on T players.
    var agentAllBtn = custCrumbBtn(agentDrop.actions, 'Apply agent to all players', function () {
      var opt = selOpt('agent', custSel.agent);
      var meta = opt ? (opt[2] || {}) : {};
      var model = meta.model || 'default';
      cmd('mirv_filmmaker cosmetics enabled 1');
      cmd('mirv_filmmaker cosmetics allplayers agent ' + model);
      cmd('mirv_filmmaker cosmetics uilog [agent] apply-to-all ' + model);
      previewLog('agentAll', 'model=' + model);
    }, 'Put the selected agent on every player of the matching team.');
    // The weapon a firearm slot carries, for its crumb and search placeholder ("AK-47 finishes").
    function slotWeaponName(slot) {
      var arr = filteredOptions(slot);
      for (var i = 0; i < arr.length; i++) {
        var l = arr[i][0] || '', cut = l.indexOf(' | ');
        if (cut > 0) return l.substring(0, cut);
      }
      return slot === 'primary' ? 'Primary' : 'Secondary';
    }
    function firearmOpts(slot, caption) {
      return { slot: slot, caption: caption, category: slot, pickupIndicator: true, rarityChips: true,
        crumb: function () { return slotWeaponName(slot) + ' finishes'; },
        crumbNoun: function () { return slotWeaponName(slot) + ' finishes'; } };
    }
    var primaryDrop = itemDrop(custSlotCol, 'CustPrimary', function (v) { pickCosmetic('primary', v); }, firearmOpts('primary', 'Primary'));
    var primaryWear = wearSliderBlock(primaryDrop.pane, 'primary', 'primary');
    var secondaryDrop = itemDrop(custSlotCol, 'CustSecondary', function (v) { pickCosmetic('secondary', v); }, firearmOpts('secondary', 'Secondary'));
    var secondaryWear = wearSliderBlock(secondaryDrop.pane, 'secondary', 'secondary');
    var knifeDrop = itemDrop(custSlotCol, 'CustKnife', function (v) { pickCosmetic('knife', v); },
      { slot: 'knife', caption: 'Melee / knife', category: 'knife', grouped: true, groupLabel: 'All knives', searchNoun: 'knives' });
    var knifeWear = wearSliderBlock(knifeDrop.pane, 'knife', 'knife');
    var glovesDrop = itemDrop(custSlotCol, 'CustGloves', function (v) { pickCosmetic('gloves', v); },
      { slot: 'gloves', caption: 'Gloves', grouped: true, groupLabel: 'All gloves', searchNoun: 'gloves' });
    var glovesWear = wearSliderBlock(glovesDrop.pane, 'gloves');
    var wearControls = { primary: primaryWear, secondary: secondaryWear, knife: knifeWear, gloves: glovesWear };
    // Primary / secondary rows exist only for a weapon the player carries (populateCustomize).
    var primaryBlock = primaryDrop.dd, secondaryBlock = secondaryDrop.dd, knifeBlock = knifeDrop.dd;
    // Selected option's [label, meta] for a slot (for the preview).
    function selOpt(slot, value) {
      var arr = filteredOptions(slot);
      for (var i = 0; i < arr.length; i++) if (arr[i][1] === value) return arr[i];
      return arr.length ? arr[0] : null;
    }
    function coerceSelection(slot) {
      if ((slot === 'primary' || slot === 'secondary') && !(custLoadout[slot] && custLoadout[slot].defIndex > 0)) {
        custSel[slot] = '0';
        return;
      }
      var arr = filteredOptions(slot);
      if (!arr.length) return;
      for (var i = 0; i < arr.length; i++) if (arr[i][1] === custSel[slot]) return;
      saveActiveItemWear(slot);
      custSel[slot] = arr[0][1];
      loadActiveItemWear(slot);
    }
)EDJS"
// Split here: MSVC caps a single raw string literal at ~16380 bytes (C2026). Adjacent
// literals are concatenated by the compiler, so this stays one JS blob.
R"EDJS(
    // Preview refresh -> drive the 3D character (or the 2D fallback). applyPreview() is defined in
    // the preview block above.
    function updatePreview() { applyPreview(); }
    function updateWearControl(slot) {
      var d = customizeDrops[slot];
      if (d && d.setWearTag) d.setWearTag(slotWearLine(slot));
      var wc = wearControls[slot]; if (!wc) return;
      var opt = selOpt(slot, custSel[slot]), meta = opt ? (opt[2] || {}) : {};
      // A default knife/glove/weapon has no finish to wear down, so the whole wear block disappears
      // for it rather than offering a float that changes nothing.
      var hasFinish = (parseInt(meta.paint || meta.paintKit || 0, 10) || 0) > 0;
      wc.setVisible(hasFinish);
      if (!hasFinish) return;
      wc.sync(wearValue(slot), meta.color || S.accent);
    }
    // The slot row's third line: "Field-Tested · 0.182" for a finish, "Stock" without one, and the
    // side for an agent ("CT agent").
    function slotWearLine(slot) {
      if (slot === 'agent') {
        var tm = normalizeTeamName(custTargetTeam);
        return tm ? (tm + ' agent') : 'Agent';
      }
      var opt = selOpt(slot, custSel[slot]);
      var meta = opt ? (opt[2] || {}) : {};
      if (!((parseInt(meta.paint || meta.paintKit || 0, 10) || 0) > 0)) return 'Stock';
      return wearPresetLabel(slot) + ' · ' + wearValue(slot).toFixed(3);
    }
    // `light` is the DRAG path. The full path costs ~10ms per call (measured over netcon on a live
    // demo), because populateCustomize() re-derives every slot's option list -- decoratedOptions()
    // deep-copies the meta of all 613 knife rows, all 96 glove rows and so on -- and then re-signs
    // each list as a concatenated string, while updatePreview() re-equips the 3D preview model. A
    // Slider fires SliderValueChanged continuously, so paying that per tick blew the frame budget:
    // the handle stuttered behind the cursor and quick clicks got swallowed. That was the "weird and
    // glitched" slider.
    //
    // The light path touches only what a float change can actually affect: this slot's stored wear,
    // its own wear control, the row's trailing wear tag, and the dirty state. The heavy pass is
    // debounced to just after the drag settles (see wearSettleSoon), so the model still follows.
    function setWear(slot, preset, customValue, light) {
      if (!custWear[slot]) custWear[slot] = defaultWearForSlot(slot);
      if (preset) custWear[slot].preset = preset;
      if (customValue !== null && customValue !== undefined) custWear[slot].custom = clamp01(customValue);
      saveActiveItemWear(slot);
      markTouched(slot);
      if (light) {
        updateWearControl(slot);   // also refreshes the slot row's wear line
        wearSettleSoon(slot);
        return;
      }
      populateCustomize();
      updatePreview();
    }
    // Debounced heavy pass. Re-armed on every tick, so it runs once after the user stops moving
    // rather than once per tick. Generation-counted because $.Schedule cannot be cancelled.
    var wearSettleSeq = 0;
    function wearSettleSoon(slot) {
      wearSettleSeq++;
      var mySeq = wearSettleSeq;
      try {
        $.Schedule(0.13, function () {
          if (mySeq !== wearSettleSeq) return;      // a newer tick re-armed it
          if (!custOverlay.visible) return;
          populateCustomize();
          updatePreview();
        });
      } catch (e) { populateCustomize(); updatePreview(); }
    }
    // Flip this item's float cap. The float on screen is preserved across the flip; re-enabling the
    // cap pulls it back into the finish's legal range instead of silently keeping an illegal value.
    function toggleWearCap(slot) {
      if (!custWear[slot]) custWear[slot] = defaultWearForSlot(slot);
      var current = wearValue(slot);
      custWear[slot].noCap = !custWear[slot].noCap;
      custWear[slot].preset = 'custom';
      custWear[slot].custom = clampWearForSlot(slot, current);
      saveActiveItemWear(slot);
      markTouched(slot);
      populateCustomize();
      updatePreview();
    }
    // Sends the actual "mirv_filmmaker cosmetics player ..." command for one slot's CURRENT selection.
    // Called from commitTouchedSlots() (Apply / Reset-to-Default / Clear-All) -- NOT from every
    // pick/wear change anymore; the modal now stages edits and only writes to the demo on Apply (or
    // the immediate Reset/Clear actions), matching the reference mock's Apply/Cancel action bar.
    function applyCosmeticCommand(slot) {
      // The agent slot has no wear entry (agents are not skinned), so it must NOT be gated on
      // custWear[slot] -- doing so silently dropped every agent pick (no command was ever sent).
      if (custTargetIndex < 0) return;
      if (slot !== 'agent' && !custWear[slot]) return;
      // Target the player by SteamID -- the backend store is SteamID-keyed. Picked-up weapons keep the
      // original owner's econ id, so route finish commands to that owner when editing the held pickup.
      var target = resolveCosmeticTarget(slot);
      var routedPickup = shouldRoutePickupToOwner(slot);
      var opt = selOpt(slot, custSel[slot]);
      var meta = opt ? (opt[2] || {}) : {};
      var def = parseInt(meta.def || 0, 10) || 0;
      var pk = parseInt(meta.paint || meta.paintKit || 0, 10) || 0;
      // Default (paint-kit-less) items have no wear surface in the UI, so they must not carry one
      // into the demo either -- a stale float from a previously selected finish would ride along.
      var wear = (pk > 0 ? wearValue(slot) : 0).toFixed(4);
      // Echo the VERBATIM skin label the user clicked (exactly the dropdown text, e.g. "AK-47 |
      // Redline" / "Skeleton Knife") to the game console + MVM debug log, so the human-readable name
      // appears right next to the native before/after weapon snapshot. Strip characters that would
      // break the console-command tokenizer; the native "uilog" handler joins the tokens back.
      var label = (opt && opt[0]) ? ('' + opt[0]) : '(unknown)';
      var safeLabel = label.replace(/["';\r\n]+/g, ' ').replace(/\s+/g, ' ');
      var routeNote = routedPickup ? (' [owner ' + pickupOwnerSteam(slot) + ']') : '';
      cmd('mirv_filmmaker cosmetics uilog [' + slot + '] ' + safeLabel + routeNote + ' (def ' + def + ' paint ' + pk + ' wear ' + wear + ')');
      var sent = false;
      var command = '';
      if ((slot === 'primary' || slot === 'secondary') && def > 0) {
        command = 'mirv_filmmaker cosmetics player ' + target + ' weapon ' + def + ' paint ' + pk + ' wear ' + wear + ' seed 0';
        sent = true;
      } else if (slot === 'knife' && def > 0) {
        command = 'mirv_filmmaker cosmetics player ' + target + ' knife ' + def + ' paint ' + pk + ' wear ' + wear + ' seed 0';
        sent = true;
      } else if (slot === 'gloves' && def > 0) {
        command = 'mirv_filmmaker cosmetics player ' + target + ' gloves ' + def + ' paint ' + pk + ' wear ' + wear + ' seed 0';
        sent = true;
      } else if (slot === 'agent') {
        command = 'mirv_filmmaker cosmetics player ' + target + ' agent ' + (meta.model || 'default');
        sent = true;
      }
      // Enable before sending the profile mutation so normal UI use does not emit the temporary
      // "cosmetics are disabled" hint from the native command handler.
      if (sent) {
        cmd('mirv_filmmaker cosmetics enabled 1');
        cmd(command);
      }
    }
    // Apply a selection. Updates the UI + local preview immediately (stages the pending change); the
    // actual demo-facing command only fires when the user commits via Apply / Reset to Default /
    // Clear All (commitTouchedSlots()) -- see markTouched().
    function pickCosmetic(slot, value) {
      if (slot === 'primary' || slot === 'secondary' || slot === 'knife') custActiveCategory = slot;
      saveActiveItemWear(slot);
      custSel[slot] = value;
      loadActiveItemWear(slot);
      markTouched(slot);
      populateCustomize();
      updatePreview();
      previewLog('pick', 'slot=' + slot + ' value=' + value);
    }
    // A primary/secondary block exists only for a weapon the current player actually carries.
    function hasCustomizeWeapon(slot) {
      var weapon = custLoadout[slot] || null;
      return !!(weapon && (parseInt(weapon.defIndex || 0, 10) > 0));
    }
    function populateCustomize() {
      coerceSelection('agent');
      coerceSelection('primary');
      coerceSelection('secondary');
      coerceSelection('knife');
      coerceSelection('gloves');
      // Firearm sections mirror the player's actual inventory. Knife/gloves/agent remain available
      // regardless, but an empty primary/secondary slot must not expose an unrelated weapon picker.
      primaryBlock.visible = hasCustomizeWeapon('primary');
      secondaryBlock.visible = hasCustomizeWeapon('secondary');
      knifeBlock.visible = true;
      // A slot that just disappeared cannot stay selected.
      ensureActiveSlotVisible();
      agentDrop.update(decoratedOptions('agent'), custSel.agent);
      primaryDrop.update(decoratedOptions('primary'), custSel.primary);
      secondaryDrop.update(decoratedOptions('secondary'), custSel.secondary);
      knifeDrop.update(decoratedOptions('knife'), custSel.knife);
      glovesDrop.update(decoratedOptions('gloves'), custSel.gloves);
      updateWearControl('primary');
      updateWearControl('secondary');
      updateWearControl('knife');
      updateWearControl('gloves');
      updatePickupBanner();
      updateActionBar();
      updateWearControl('agent');
      updateHoldChip();
    }
)EDJS"
R"EDJS(
    // ---- Manual scroll + C++-routed typing --------------------------------------------------
    // The modal is an EXCLUSIVE input surface: MovieMode/main.cpp swallow the raw wheel + typed keys
    // from the game (so nothing leaks to the demo behind the modal) and forward them here via
    // $.CamEditor.custWheel / custChars. Because Panorama never sees the raw wheel, the tile grid is
    // scrolled by translating its content inside a clipped viewport; because CS2 doesn't give the
    // in-game HUD a TextEntry with real keyboard focus, the search string is built from the
    // forwarded characters. Both always act on the SELECTED slot's pane -- the picker never closes.
    function custMeasureH(panel) {
      if (!panel || !panel.IsValid || !panel.IsValid()) return 0;
      var sy = root.actualuiscale_y || ctx.actualuiscale_y || 1;
      return (panel.actuallayoutheight || 0) / (sy || 1);
    }
    // Apply a style value only when it changed, so per-frame maintenance doesn't thrash layout.
    function setStyleIf(panel, key, prop, value) {
      if (panel['__' + key] === value) return;
      panel['__' + key] = value;
      panel.style[prop] = value;
    }
    // Clamp the pane's scroll to its content and show each scroll cue only in a direction that
    // still has tiles (1px of slack absorbs rounding).
    function sizePane(rec) {
      if (!rec || !rec.pane || !rec.pane.visible || !rec.rowsView || !rec.rows) return;
      var viewH = custMeasureH(rec.rowsView), contentH = custMeasureH(rec.rows);
      var maxOff = Math.max(0, contentH - viewH);
      if (rec.scrollOff > maxOff) rec.scrollOff = maxOff;
      if (rec.scrollOff < 0) rec.scrollOff = 0;
      setStyleIf(rec.rows, 'sy', 'transform', 'translateY(' + (-Math.round(rec.scrollOff)) + 'px)');
      if (rec.fadeTop) rec.fadeTop.visible = (rec.scrollOff > 1);
      if (rec.fadeBot) rec.fadeBot.visible = ((maxOff - rec.scrollOff) > 1);
    }
    // The HOLDING chip on the preview: which slot the model has in hand.
    var HOLD_NAMES = { primary: 'Primary', secondary: 'Secondary', knife: 'Knife' };
    function updateHoldChip() {
      if (typeof custHoldVal !== 'undefined' && custHoldVal) custHoldVal.text = HOLD_NAMES[custActiveCategory] || 'Knife';
    }
    // Every render() frame while the editor is up: keep the selected pane's grid scrolled within its
    // (possibly changed) content and blink the search caret. Cheap no-op when the modal is closed.
    function custScrollMaintain() {
      if (!custOverlay.visible) return;
      sizePane(activeSlotRec());
      var wc = wearControls[custActiveSlot];
      if (wc && wc.relayout) wc.relayout();
      custCaretBlink();
    }
    // Blinks the selected pane's search caret. Driven from the per-frame pump rather than a
    // $.Schedule chain, and time-based so the blink rate does not follow the framerate.
    function custCaretBlink() {
      var rec = activeSlotRec();
      if (!rec || !rec.caretPanel || !rec.pane || !rec.pane.visible) return;
      var c = rec.caretPanel;
      if (!c.IsValid || !c.IsValid()) return;
      var on = ((Date.now() % 1000) < 560);
      if (c.__on !== on) { c.__on = on; c.style.opacity = on ? '1' : '0'; }
    }
    // Wheel notch forwarded from C++ (MovieMode swallowed the raw wheel). +1 = up, -1 = down. It
    // scrolls the selected slot's tile grid.
    function custWheel(notches, x, y) {
      if (!custOverlay.visible) return 'closed';
      var n = parseInt(notches, 10) || 0;
      if (!n) return 'ok';
      var rec = activeSlotRec();
      if (rec) { rec.scrollOff += -n * CUST_SCROLL_STEP; sizePane(rec); }
      return 'ok';
    }
    // One forwarded character (WM_CHAR code) into the selected pane's search. Escape backs out one
    // step -- out of a model, then out of the modal -- so the user is never trapped with keys captured.
    function custChar(code) {
      var rec = activeSlotRec();
      if (code === 27) {
        if (rec && rec.grouped && rec.navGroup) { rec.navGroup = ''; rec.scrollOff = 0; rec.rebuildRows(); sizePane(rec); }
        else closeCustomize();
        return;
      }
      if (!rec || !rec.searchable) return;
      if (code === 8) { rec.query = (rec.query || '').slice(0, -1); }
      else if (code === 13) { return; }
      else if (code >= 32 && code !== 127) { rec.query = (rec.query || '') + String.fromCharCode(code).toLowerCase(); }
      else { return; }
      if (rec.search && rec.search.IsValid && rec.search.IsValid()) {
        try { custSearchDisplayGuard = true; rec.search.text = rec.query; custSearchDisplayGuard = false; } catch (e) {}
      }
      if (rec.syncCaret) rec.syncCaret();
      // A search reaches into every model of a grouped slot, so it starts from the model list.
      if (rec.grouped && rec.navGroup && rec.query) rec.navGroup = '';
      if (rec.rebuildRows) rec.rebuildRows();
      rec.scrollOff = 0; sizePane(rec);
    }
    function custChars(csv) {
      if (!custOverlay.visible || csv == null) return 'closed';
      var parts = ('' + csv).split(',');
      for (var i = 0; i < parts.length; i++) { var code = parseInt(parts[i], 10); if (code >= 0) custChar(code); }
      return 'ok';
    }
)EDJS"
R"EDJS(
    // Tells the C++ side (CameraEditorHud::UpdateCustomizeModalState, "customizeopen" attribute)
    // whether the modal is open, so MovieMode/GetSuspendMirvInput can treat it as an exclusive
    // input surface (swallow Space/clicks/wheel, suspend free-cam mouse-look) -- see Filmmaker.h
    // CameraEditor_CustomizeModalOpen(). Published on every open/close AND every render() frame
    // as a safety net, since custOverlay.visible also flips from other paths (editor disabled,
    // target no longer a player, etc.) that don't go through openCustomize/closeCustomize.
    function publishCustomizeOpenState() {
      try { root.SetAttributeString('customizeopen', custOverlay.visible ? '1' : '0'); } catch (e) {}
    }
    function openCustomize() {
      closeAllDrops(); closeSettings();
      custTouched = { agent: false, primary: false, secondary: false, knife: false, gloves: false };
      custDirty = false;
      resetSelectionsFromLoadout();
      custTitleName.text = custTargetName || 'Player';   // cased by text-transform, see the builder
      // Every pane opens fresh: no leftover search, at the top of its model list.
      for (var sk in custSlotRecs) {
        var sr = custSlotRecs[sk];
        sr.query = ''; sr.navGroup = ''; sr.scrollOff = 0; sr.stale = true;
      }
      populateCustomize();
      // The picker opens on the slot the model is holding (what the player actually carries).
      custActiveSlot = '';
      selectSlot(custActiveCategory || initialActiveCategory());
      custDisplayedLoadoutSig = customizeLoadoutSignature();
      custOverlay.visible = true; // visible BEFORE building the 3D preview so its scene composites
      // Enter transition -- OPACITY ONLY, and deliberately not animateIn(). The obvious choice (its
      // small translateY) would move custWin for the length of the animation, and the 3D preview is a
      // ROOT-level panel positioned from prevStage's MEASURED rect (positionPreview3d, which render()
      // re-drives per frame). GetPositionWithinWindow is transform-aware, so an animating parent would
      // drag the preview clip around behind it, and custWin already owns a transform for CUST_SCALE.
      // Opacity changes no geometry, so the fade cannot touch any of that.
      //
      // Snap to 0 now, release on the NEXT frame: Panorama only animates a value that changed across a
      // layout pass, so writing both in one frame is a no-op (same reason animateIn defers).
      try {
        custWin.style.transitionProperty = 'opacity';
        custWin.style.transitionTimingFunction = 'ease-out';
        custWin.style.transitionDuration = '0s';
        custWin.style.opacity = '0';
        $.Schedule(0, function () {
          if (!custWin || (custWin.IsValid && !custWin.IsValid())) return;
          custWin.style.transitionDuration = '0.14s';
          custWin.style.opacity = '1';
        });
      } catch (e) { try { custWin.style.opacity = '1'; } catch (e2) {} }
      previewLog('open');
      // CREATE the 3D panel only AFTER the overlay is visible AND laid out (next layout passes). Do
      // not call updatePreview() eagerly here: that creates a throwaway panel, then the delayed
      // recreate replaces it and can leave the final render target black. One delayed create gets
      // one Panorama-only composition refresh.
      try { $.Schedule(0.18, recreatePreview); } catch (e) { recreatePreview(); }
      publishCustomizeOpenState();
    }
    function closeCustomize() {
      // closeCustomize() is also called EVERY frame as a safety net while the modal "shouldn't"
      // be open (render()'s `if (!cur)` branch, and its `if (!onPlayer)` branch whenever the mirv
      // free cam is on -- which stays on after a Follow "Live" preview stops, or after any camera
      // path place/preview). When the modal is already closed this must be a TRUE no-op: the
      // unconditional closeAllDrops() below was tearing down every OTHER customDrop popup (Follow
      // inspector Type/Target/Attach/Source/Event) on the very frame it opened, which read as
      // "the dropdowns stop working after leaving Live mode".
      if (!custOverlay.visible) return;
      closeAllDrops();
      destroyPreview3d();
      custOverlay.visible = false;
      custResetConfirm.visible = false;
      // Closing (X / click-outside / Cancel) always discards whatever hasn't been committed via
      // Apply/Reset/Clear -- the modal never writes to the demo except through those three paths.
      custTouched = { agent: false, primary: false, secondary: false, knife: false, gloves: false };
      custDirty = false;
      publishCustomizeOpenState();
      previewLog('close'); // always a REAL close now (the early-return above filters no-ops)
    }
    var customizeDrops = { agent: agentDrop, primary: primaryDrop, secondary: secondaryDrop, knife: knifeDrop, gloves: glovesDrop };
)EDJS"
R"EDJS(
    // ===================== HEADER ACTIONS =====================
    // Reset/Clear are immediate (they mirror picking every slot's vanilla default, resp. wiping the
    // whole stored profile, and commit right away like a native "reset" action). Finish/Wear/Agent/
    // Gloves picks are STAGED (pickCosmetic/setWear only update custSel/custWear + the local preview)
    // and only reach the demo via Apply -- Cancel/✕/click-outside discard them (closeCustomize()
    // clears custTouched/custDirty without ever calling applyCosmeticCommand).
    var custTouched = { agent: false, primary: false, secondary: false, knife: false, gloves: false };
    var custDirty = false;
    function markTouched(slot) { custTouched[slot] = true; custDirty = true; updateActionBar(); }
    function commitTouchedSlots() {
      var any = false;
      for (var slot in custTouched) if (custTouched[slot]) { applyCosmeticCommand(slot); any = true; }
      custTouched = { agent: false, primary: false, secondary: false, knife: false, gloves: false };
      custDirty = false;
      updateActionBar();
      return any;
    }
    // "Reset to Default" reverts the target to their ACTUAL recorded loadout (whatever skin/agent/
    // knife/gloves the demo captured), not to a blank "no skin" state -- it wipes every stored
    // override for this player and reseeds the modal from the live loadout, same as opening it fresh.
    function resetToDefault() {
      closeAllDrops();
      var target = (custTargetKey && custTargetKey.indexOf('steam:') === 0) ? custTargetKey.substring(6) : 'current';
      cmd('mirv_filmmaker cosmetics enabled 1');
      // Pickup finishes are stored under the original owner. Reset only the two held slots there,
      // preserving that owner's other cosmetics, then clear the selected player's own profile.
      var clearedPickupSlots = {};
      ['primary', 'secondary'].forEach(function (slot) {
        if (!shouldRoutePickupToOwner(slot)) return;
        var owner = pickupOwnerSteam(slot), def = effectiveSlotDef(slot);
        if (!owner || owner === target || def <= 0) return;
        var key = owner + ':' + def;
        if (clearedPickupSlots[key]) return;
        clearedPickupSlots[key] = true;
        cmd('mirv_filmmaker cosmetics clearWeapon ' + owner + ' ' + def);
      });
      cmd('mirv_filmmaker cosmetics clearPlayer ' + target);
      custTouched = { agent: false, primary: false, secondary: false, knife: false, gloves: false };
      custDirty = false;
      resetSelectionsFromLoadout();
      populateCustomize();
      updatePreview();
      updateActionBar();
    }
    // Who Apply would actually write to -- mirrors the pickup-banner logic (updatePickupBanner) so the
    // button label and the warning banner never disagree about the target.
    function applyLabelTarget() {
      var ps = pickupSlots();
      if (ps.length) return slotOwnerName(ps[0]);
      if (custActiveWeaponPickup && custActiveWeaponOwnerSteam) return custActiveWeaponOwnerName || ('Player ' + custActiveWeaponOwnerSteam);
      return custTargetName || 'Player';
    }
    function updateActionBar() {
      // Plain "Apply", as the design has it. Who a picked-up weapon's finish actually lands on is
      // said once, by the pickup banner beside the title (applyLabelTarget() mirrors that logic
      // for the debug state).
      custApplyBtn.__lbl.text = 'Apply';
      // Through the BASE fields, never style.* -- btnVisual() owns this panel's paint, so a direct
      // style write would be reverted by the next hover/press repaint (the custClose trap).
      custApplyBtn.__bgBase = custDirty ? S.accent : S.btnBg;
      custApplyBtn.__fgBase = custDirty ? '#10141aff' : S.dim;
      custApplyBtn.__bgHover = custDirty ? S.accentHover : null;
      custApplyBtn.__fgHover = custDirty ? '#10141aff' : S.dim;
      custApplyBtn.__bgDown = custDirty ? S.accentDown : null;
      custApplyBtn.__fgDown = custDirty ? '#10141aff' : null;
      // hittest=false is also what suppresses the hover/press states: Panorama delivers no
      // onmouseover to a non-hittest panel, so an un-actionable APPLY cannot advertise itself as
      // clickable. Clear any state it was already holding when it goes inert mid-hover.
      custApplyBtn.hittest = custDirty;
      if (!custDirty) { custApplyBtn.__hot = false; custApplyBtn.__down = false; }
      btnVisual(custApplyBtn);
    }
    // A header action: 34px tall, 12px bold. Routed through interactive() so it has hover and press
    // states. The click handler is passed IN, not wired with SetPanelEvent afterwards: SetPanelEvent
    // REPLACES a panel's handler and would drop interactive()'s own press-state handler.
    function actionBtn(parent, text, color, textColor, onClick, opts) {
      var b = mk('Panel', parent); b.style.verticalAlign = 'center';
      b.style.height = '34px'; b.style.borderRadius = '3px'; b.style.marginLeft = '8px';
      b.style.paddingLeft = '16px'; b.style.paddingRight = '16px';
      var l = clbl(b, text, textColor || S.value, 12); l.style.fontWeight = 'bold';
      l.style.verticalAlign = 'center'; l.style.whiteSpace = 'nowrap';
      // fgHover pinned to the button's own text colour: btnVisual's default sends a hovered label to
      // S.value, which would flip APPLY's dark-on-gold to light-on-gold and wash RESET's red to white.
      var o = { bg: color, fg: textColor || S.value, border: S.clear,
                fgHover: textColor || S.value, press: true };
      if (opts) for (var k in opts) o[k] = opts[k];
      interactive(b, l, onClick, o);
      return b;
    }
    // custResetConfirm / custResetBody are `var`s declared below and so are hoisted; they are
    // assigned long before any click can reach these closures.
    var custResetBtn = actionBtn(custHeadActions, 'Reset to default', S.btnBg, '#ff8a8aff', function () {
      custResetBody.text = 'This removes every skin, agent, knife, and glove override for ' + (custTargetName || 'this player') + ' and reverts to their original loadout.';
      custResetConfirm.visible = true;
    }, { accent: S.dangerBorder, bgHover: S.dangerBg, bgDown: S.dangerBg });
    custResetBtn.style.paddingLeft = '14px'; custResetBtn.style.paddingRight = '14px';
    custResetBtn.style.marginLeft = '0px';
    var custCancelBtn = actionBtn(custHeadActions, 'Cancel', S.btnBg, S.value, function () { closeCustomize(); });
    var custApplyBtn = actionBtn(custHeadActions, 'Apply', S.accent, '#10141aff', function () {
      if (custDirty) { commitTouchedSlots(); closeCustomize(); }
    }, { bgHover: S.accentHover, bgDown: S.accentDown, fgDown: '#10141aff' });
    custApplyBtn.style.paddingLeft = '24px'; custApplyBtn.style.paddingRight = '24px';
    var custClose = btn(custHeadActions, '✕', function () { closeCustomize(); }, S.value, { press: true });
    custClose.style.marginRight = '0px'; custClose.style.marginLeft = '8px';
    custClose.style.width = '34px'; custClose.style.height = '34px';
    custClose.style.paddingLeft = '0px'; custClose.style.paddingRight = '0px';
    custClose.style.paddingTop = '0px'; custClose.style.paddingBottom = '0px';
    custClose.style.borderRadius = '4px';
    // Palette via the BASE fields, never style.* directly: btnVisual() owns backgroundColor/border.
    custClose.__bgBase = '#ffffff14'; custClose.__brBase = '#ffffff1f'; btnVisual(custClose);
    custClose.__lbl.style.fontSize = '15px'; custClose.__lbl.style.width = '100%'; custClose.__lbl.style.textAlign = 'center';
    custClose.__lbl.style.verticalAlign = 'center';

    // The HOLDING chip in the preview's top-left corner. The 3D preview is a ROOT-level panel laid
    // over the stage (positionPreview3d), so a chip inside the stage would sit underneath it; this
    // one is root-level too, one z-step above the preview, and positionPreview3d moves it with it.
    var custHoldChip = mk('Panel', custOverlay); custHoldChip.hittest = false; custHoldChip.visible = false;
    custHoldChip.style.zIndex = '234'; custHoldChip.style.flowChildren = 'right'; custHoldChip.style.height = '24px';
    custHoldChip.style.paddingLeft = '9px'; custHoldChip.style.paddingRight = '9px'; custHoldChip.style.borderRadius = '3px';
    custHoldChip.style.backgroundColor = '#00000088'; custHoldChip.style.border = '1px solid #ffffff1f';
    var custHoldCap = clbl(custHoldChip, 'Holding ·', '#c8d3dfff', 11); custHoldCap.hittest = false;
    custHoldCap.style.fontWeight = 'bold'; custHoldCap.style.letterSpacing = '1.2px';
    custHoldCap.style.verticalAlign = 'center'; custHoldCap.style.textTransform = 'uppercase';
    var custHoldVal = clbl(custHoldChip, '', S.accent, 11); custHoldVal.hittest = false;
    custHoldVal.style.fontWeight = 'bold'; custHoldVal.style.letterSpacing = '1.2px'; custHoldVal.style.marginLeft = '4px';
    custHoldVal.style.verticalAlign = 'center'; custHoldVal.style.textTransform = 'uppercase';

    // Confirm dialog for "Reset to Default" -- it immediately wipes the target's WHOLE stored
    // cosmetic profile (not just the currently staged edits; Cancel already covers those), so it
    // gets one guard click. Own root, and it has to outrank custOverlay, since it is the confirm ON
    // TOP of the customize window.
    var custResetConfirm = $.CreatePanel('Panel', ctx, 'CamEditorCustomizeConfirmRoot', {});
    custResetConfirm.visible = false; custResetConfirm.hittest = true;
    custResetConfirm.style.width = '100%'; custResetConfirm.style.height = '100%'; custResetConfirm.style.zIndex = '260';
    custResetConfirm.style.backgroundColor = F8.scrim;
    custResetConfirm.SetPanelEvent('onactivate', function () { custResetConfirm.visible = false; });
    var custResetBox = mk('Panel', custResetConfirm); custResetBox.hittest = true;
    custResetBox.style.width = '480px'; custResetBox.style.horizontalAlign = 'center'; custResetBox.style.verticalAlign = 'center';
    f8Surface(custResetBox, 'rgba(0,0,0,0.92)', F8.radius);
    custResetBox.style.border = '1px solid #f0b32366';
    custResetBox.style.boxShadow = '#000000b0 0px 0px 24px 5px';
    custResetBox.style.flowChildren = 'down'; custResetBox.style.paddingTop = '24px'; custResetBox.style.paddingBottom = '22px';
    custResetBox.style.paddingLeft = F8.inset; custResetBox.style.paddingRight = F8.inset;
    custResetBox.SetPanelEvent('onactivate', function () { /* swallow clicks inside the box */ });
    var custResetTitle = clbl(custResetBox, 'Reset to default?', S.value, TYPE.title); custResetTitle.style.fontWeight = 'bold';
    custResetTitle.style.letterSpacing = '1px'; custResetTitle.style.textTransform = 'uppercase'; custResetTitle.style.marginBottom = '10px';
    var custResetBody = clbl(custResetBox, '', S.label, TYPE.body); custResetBody.style.whiteSpace = 'normal'; custResetBody.style.lineHeight = '140%'; custResetBody.style.marginBottom = '18px';
    var custResetActions = mk('Panel', custResetBox); custResetActions.style.flowChildren = 'right'; custResetActions.style.horizontalAlign = 'right';
    var custResetNo = btn(custResetActions, 'No', function () { custResetConfirm.visible = false; }, S.value, { press: true });
    custResetNo.style.minWidth = '110px'; custResetNo.__lbl.style.textAlign = 'center'; custResetNo.__lbl.style.width = '100%';
    var custResetYes = btn(custResetActions, 'Yes, reset', function () { custResetConfirm.visible = false; resetToDefault(); }, S.danger, { press: true });
    custResetYes.style.minWidth = '160px'; custResetYes.style.marginRight = '0px';
    custResetYes.__lbl.style.textAlign = 'center'; custResetYes.__lbl.style.width = '100%';
    // Applied last: the whole modal (and every child) is built by now, and positionPreview3d exists,
    // so the 3D clip is re-fitted to the scaled frame in the same pass.
    applyCustScale();
)EDJS"
R"EDJS(
    // Persistent Customize action in the shared bottom-right action row. It stays aligned with
    // Render across Path/Follow/Attach/Lock-on tabs (visibility is set in render()).
    // CUSTOMIZE PLAYER is built with RENDER in CameraEditorJs.h now (mvmActionRow), so the two
    // share one row definition with the CONFIG panel; `customizeBtn` is still the handle.
)EDJS"
