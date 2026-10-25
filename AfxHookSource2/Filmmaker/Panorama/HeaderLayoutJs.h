#pragma once

// The main menu's top bar as fixed layout, for the buttons we add to it: WHAT'S NEW left of the
// centre group (WhatsNewJs.h, in FilmmakerGuiJs.h) and HC TRACKER right of it (Community/
// CommunityJs.h). ONE definition, spliced into both scripts, so the two stay mirror images.
//
// NOT a standalone script: a raw-string chunk concatenated into a host IIFE.
//
// Each button is centred in the empty stretch on its side of the centre group, so the space before
// and after it is the same. The stretches come from fixed layout, measured at 1080p on 2026-10-24,
// not from live panel positions: pausing hides the quit icon and collapses the whole centre group
// (INVENTORY ... NEWS carry .DisableDuringDemoPlayback), and a collapsed INVENTORY keeps reporting
// its old layout. So a spot taken from here is the same in the home menu and the pause menu.
//
//   left stretch:  from the left icons' end (175px from the left; pause keeps home, Movie Making
//                  and settings where they are) to INVENTORY's word (250px left of the bar's centre)
//   right stretch: from NEWS's word end (248px right of the centre) to the friends column (the last
//                  64px of the bar)

R"HLJS(
    var MVM_HEADER = { iconsEnd: 175, inventoryFromCentre: 250, newsFromCentre: 248, friendsColumn: 64 };
    // The bar's width in layout px (1920 at 16:9).
    function mvmHeaderBarWidth(root) {
      var bar = root.FindChildTraverse('JsMainMenuTopNavBar') || root;
      return (bar.actuallayoutwidth || 1920) / (bar.actualuiscale_x || 1);
    }
    // Where a button `w` layout px wide goes in the left (side < 0) or right stretch: its left edge
    // `x` and the room it leaves before and after itself.
    function mvmHeaderSlot(side, w, barW) {
      var lo = side < 0 ? MVM_HEADER.iconsEnd : barW / 2 + MVM_HEADER.newsFromCentre;
      var hi = side < 0 ? barW / 2 - MVM_HEADER.inventoryFromCentre : barW - MVM_HEADER.friendsColumn;
      var x = (lo + hi) / 2 - w / 2;
      return { x: x, before: x - lo, after: hi - (x + w) };
    }
)HLJS"
