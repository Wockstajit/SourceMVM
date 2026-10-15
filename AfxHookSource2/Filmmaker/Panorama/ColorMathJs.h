#pragma once

// Shared, engine-independent picker math. RGB values are 0..255; HSV is normalized.
R"CLRJS(
    function mvmRgbToHsv(rgb, previousHue) {
      var r = clamp01(rgb[0] / 255), g = clamp01(rgb[1] / 255), b = clamp01(rgb[2] / 255);
      var hi = Math.max(r, g, b), lo = Math.min(r, g, b), d = hi - lo;
      var h = previousHue || 0;
      if (d > 0.000001) {
        h = hi === r ? (g - b) / d : hi === g ? (b - r) / d + 2 : (r - g) / d + 4;
        h = ((h / 6) % 1 + 1) % 1;
      }
      return [h, hi > 0 ? d / hi : 0, hi];
    }
    function mvmHsvToRgb(h, s, v) {
      h = ((h % 1) + 1) % 1; s = clamp01(s); v = clamp01(v);
      var i = Math.floor(h * 6), f = h * 6 - i;
      var p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
      var c = [[v,t,p],[q,v,p],[p,v,t],[p,q,v],[t,p,v],[v,p,q]][i % 6];
      return [Math.round(c[0] * 255), Math.round(c[1] * 255), Math.round(c[2] * 255)];
    }
)CLRJS"
