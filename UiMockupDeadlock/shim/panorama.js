// Deadlock's Panorama context -> DOM.
//
// The extracted workspace script runs against this UNCHANGED, so the mockup shows the real in-game
// UI rather than a copy of it.
//
// WHAT MAKES THIS DEADLOCK'S CONTEXT AND NOT CS2'S, and why it is a separate shim rather than a
// flag on the CS2 one:
//
//   * THERE ARE NO ENGINE BINDINGS. Measured in game on build 6687 by enumerating the global object
//     from a live RunScript: no GameInterfaceAPI, no UiToolkitAPI, and Deadlock's own
//     CitadelConCommand is bound per-XML-context by the panel class that declares it and is
//     undefined here. They are left undefined below ON PURPOSE. A shim that provided them would let
//     a panel reach for one, work perfectly in the browser, and be a dead control in game -- which
//     is the single most expensive class of mistake this harness exists to prevent.
//
//   * A COMMAND IS A MARKED CONSOLE LINE. With no bindings, the only way out is $.Msg: the panel
//     prints a line opening with STX, and a tier0 logging listener in DeadlockUiCommandBridge.cpp
//     recognises the marker and runs the rest on the main thread. This shim splits the same marker
//     off the same stream, so the browser's command path is the game's command path and not a
//     shortcut around it.
//
//   * STATE ARRIVES AS A CALL ARGUMENT. CS2's panels read a JSON string off a panel attribute;
//     Deadlock's C++ composes `$.MvmWorkspace.render({...})` and runs it. So there is no attribute
//     feed to emulate -- app.js simply calls render() with the state object.
//
// The two layout rules that matter most, because they are what makes a Panorama tree look the way it
// does in game:
//
//   1. A panel with NO flowChildren stacks its children at its own origin, all overlapping, and
//      sizes itself to the largest. That is CSS grid with every child in cell 1/1 -- not absolute
//      positioning, which would collapse the parent to zero. The Deadlock workspace leans on this
//      everywhere: it places almost everything by `position` inside a stacking parent.
//   2. A panel with flowChildren lays its children in a line and never SHRINKS them. That is flex
//      with flex-shrink:0.

(function (global) {
  'use strict';

  // Keep in step with kUiCommandMarker in AfxHookSource2/Deadlock/DeadlockUiCommandBridge.h.
  var CMD_MARKER = '\x02MVMCMD ';

  // The stage is laid out at a true 1920x1080 and CSS-scaled to fit the browser window.
  // getBoundingClientRect reports POST-transform pixels, so every measurement handed back to the
  // script is divided by this to keep it in the logical space the game uses. Panorama's own uiscale
  // is reported as 1.0 for the same reason: in logical space there is no extra scale, and the
  // workspace divides its measured width by it before reporting the screen size.
  function stageScale() {
    return (global.MvmDeadlockMockup && global.MvmDeadlockMockup.stageScale) || 1;
  }
  var UI_SCALE = 1.0;

  var panels = [];
  var unknownStyle = {};
  var logLines = [];
  var commands = [];

  // Every style write is counted. Not a curiosity: in game a style write re-does the panel's
  // geometry, and a 3D transform allocates a composited layer with its own D3D11 vertex buffer,
  // which Panorama indexes with an UNSIGNED SHORT. The workspace once re-transformed ~20 HUD panels
  // every frame and exhausted 65535 entries in under a minute, growing the process from 3.2 GB to
  // 14.3 GB before it died. M.set memoises to prevent that, and this counter is how the browser can
  // show whether the memo is still working -- a steady-state frame should write nothing at all.
  var styleWrites = { count: 0, byPanel: Object.create(null) };

  function log(s) {
    logLines.push(s);
    if (logLines.length > 600) logLines.shift();
    if (global.MvmDeadlockMockup && global.MvmDeadlockMockup.onLog) {
      global.MvmDeadlockMockup.onLog(s);
    }
  }

  // ---------------------------------------------------------------- values

  function splitArgs(s) {
    var out = [], depth = 0, cur = '';
    for (var i = 0; i < s.length; i++) {
      var c = s[i];
      if (c === '(') depth++;
      if (c === ')') depth--;
      if (c === ',' && depth === 0) { out.push(cur.trim()); cur = ''; continue; }
      cur += c;
    }
    if (cur.trim()) out.push(cur.trim());
    return out;
  }

  // Panorama's gradient() does not exist in CSS. Deadlock's button bevel is a two-stop vertical
  // gradient and its speed dial zones are gradients too, so dropping these would turn every control
  // flat and the mockup would stop being a fair likeness of the panel.
  function gradientToCss(v) {
    var m = /^gradient\s*\(([\s\S]*)\)\s*$/i.exec(String(v).trim());
    if (!m) return v;
    var args = splitArgs(m[1]);
    var kind = (args.shift() || 'linear').trim().toLowerCase();
    var stops = [], geom = [];
    args.forEach(function (a) {
      var fm = /^from\s*\(([\s\S]*)\)$/i.exec(a);
      var tm = /^to\s*\(([\s\S]*)\)$/i.exec(a);
      var cm = /^color-stop\s*\(([\s\S]*)\)$/i.exec(a);
      if (fm) stops.push({ pos: 0, color: fm[1].trim() });
      else if (tm) stops.push({ pos: 1, color: tm[1].trim() });
      else if (cm) {
        var p = splitArgs(cm[1]);
        stops.push({ pos: parseFloat(p[0]), color: (p[1] || '#fff').trim() });
      } else geom.push(a);
    });
    stops.sort(function (a, b) { return a.pos - b.pos; });
    var list = stops.map(function (s) {
      return s.color + ' ' + (s.pos * 100).toFixed(1) + '%';
    }).join(', ');
    if (kind === 'radial') {
      var parts = (geom[0] || '50% 50%').split(/\s+/);
      return 'radial-gradient(ellipse at ' + (parts[0] || '50%') + ' ' + (parts[1] || '50%') + ', ' + list + ')';
    }
    // A linear gradient's geometry is "x0% y0%, x1% y1%". Deadlock only ever runs them vertically
    // (0% 0% -> 0% 100%) or horizontally, so the axis is taken from which pair differs.
    var a0 = (geom[0] || '0% 0%').split(/\s+/);
    var a1 = (geom[1] || '0% 100%').split(/\s+/);
    var horizontal = parseFloat(a0[0]) !== parseFloat(a1[0]);
    return 'linear-gradient(' + (horizontal ? 'to right' : 'to bottom') + ', ' + list + ')';
  }

  function colorValue(v) {
    var s = String(v);
    return /gradient\s*\(/i.test(s) ? gradientToCss(s) : s;
  }

  // Panorama: "x y blur spread color". CSS wants the colour first.
  function textShadowToCss(v) {
    var p = String(v).trim().split(/\s+/);
    if (p.length >= 5) return p[4] + ' ' + p[0] + ' ' + p[1] + ' ' + p[2];
    return v;
  }

  // Panorama: "[fill] color x y blur spread" in several orders. Deadlock's panels use the simple
  // "x y blur color" and "x y blur spread color" forms.
  function boxShadowToCss(v) {
    var s = String(v).trim();
    if (s === 'none') return 'none';
    var p = s.replace(/^fill\s+/i, '').split(/\s+/);
    var colorAt = -1;
    for (var i = 0; i < p.length; i++) if (/^(#|rgb)/i.test(p[i])) { colorAt = i; break; }
    if (colorAt < 0) return s;
    var color = p[colorAt];
    var lens = p.filter(function (x, k) { return k !== colorAt; });
    return lens.slice(0, 4).join(' ') + ' ' + color;
  }

  // Deadlock's assets live in its own VPK. The reference tree extracted under
  // reference/deadlock/panorama is the source, copied into assets/ by fetch-assets.mjs; a compiled
  // resource changes extension on extraction (.vtex -> .png).
  function resolveAsset(v) {
    var str = String(v);
    var m = /url\(\s*["']?([^"')]+)["']?\s*\)/i.exec(str);
    var raw = m ? m[1] : str;
    if (/^(https?:|data:|\.|\/)/i.test(raw)) return raw;
    var p = null;
    if (/^s2r:\/\//i.test(raw)) p = raw.replace(/^s2r:\/\//i, '');
    else if (/^file:\/\/\{images\}\//i.test(raw)) p = 'panorama/images/' + raw.replace(/^file:\/\/\{images\}\//i, '');
    else if (/^file:\/\/\{resources\}\//i.test(raw)) p = raw.replace(/^file:\/\/\{resources\}\//i, '');
    if (!p) return null;
    p = p.replace(/\.vtex$/i, '.png').replace(/\.vsvg(_c)?$/i, '.svg').replace(/_psd\.png$/i, '_psd.png');
    if (!/\.(png|svg|jpe?g|webp)$/i.test(p)) return null;
    return 'assets/' + p;
  }

  var OVERFLOW = { noclip: 'visible', clip: 'hidden', scroll: 'auto', squish: 'hidden' };

  // ---------------------------------------------------------------- Panel

  var HALIGN_GRID = { left: 'start', center: 'center', right: 'end' };
  var VALIGN_GRID = { top: 'start', center: 'center', middle: 'center', bottom: 'end' };
  var HALIGN_FLEX = { left: 'flex-start', center: 'center', right: 'flex-end' };
  var VALIGN_FLEX = { top: 'flex-start', center: 'center', middle: 'center', bottom: 'flex-end' };

  var TEXT_TYPES = { Label: 1, Button: 1, TextEntry: 1 };

  function Panel(type, id) {
    this.type = type || 'Panel';
    this.id = id || '';
    this.children = [];
    this.parent = null;
    this._sty = Object.create(null);
    this._attrs = Object.create(null);
    this._events = Object.create(null);
    this._visible = true;
    this._hittest = true;
    this._enabled = true;
    this._deleted = false;
    this._text = '';
    this._bg = { color: '', image: '', wash: '' };
    this._native = false;          // true for the stand-in game panels, so they can be listed apart

    var el = document.createElement('div');
    el.className = 'pnl pnl-' + this.type;
    el.__panel = this;
    this.el = el;
    // The Panorama id goes onto the DOM node. Without it the tree is a thousand anonymous divs and
    // neither a geometry diff against the game nor a person in devtools can say WHICH panel they
    // are looking at -- and the whole point is that an edit here traces back to a named panel.
    if (this.id) {
      el.setAttribute('data-pid', this.id);
      if (!document.getElementById(this.id)) el.id = this.id;
    }

    if (TEXT_TYPES[this.type]) {
      this.textEl = document.createElement('span');
      this.textEl.className = 'pnl-text';
      el.appendChild(this.textEl);
    }
    if (this.type === 'Slider') this._buildSlider();

    var self = this;
    this.style = new Proxy(Object.create(null), {
      set: function (t, k, v) {
        // PANORAMA VALIDATES AND THROWS, and reproducing that is load-bearing. `style.x = ''`
        // aborts the whole script, taking the rest of the panel with it, and a missing token
        // stringifies to "undefined" rather than to nothing -- so an empty-string check alone
        // sails straight past it. Both are easy to write and invisible in a parse check.
        if (v === '') throw new Error("Panorama: empty value for style '" + String(k) + "'");
        if (v === null || v === undefined) {
          // A deliberate clear. The script uses this to release a native panel's override.
          t[k] = v;
          self._setStyle(k, null);
          return true;
        }
        styleWrites.count++;
        styleWrites.byPanel[self.id || '(anon)'] = (styleWrites.byPanel[self.id || '(anon)'] || 0) + 1;
        t[k] = v;
        self._setStyle(k, v);
        return true;
      },
      get: function (t, k) { return t[k] === undefined ? '' : t[k]; },
      deleteProperty: function (t, k) { delete t[k]; self._setStyle(k, null); return true; }
    });

    panels.push(this);
    this._applyLayout();
    this._applyPlacement();
  }

  Panel.prototype._applyBackground = function () {
    var el = this.el, bg = this._bg;
    var isGradient = /gradient\s*\(/i.test(bg.color);
    var layers = [];
    if (bg.image) layers.push(bg.image);
    if (isGradient) layers.push(bg.color);

    // Panorama's wash-color MULTIPLIES the panel's content, so a monochrome glyph comes out tinted
    // and the transparent margin around it stays transparent. Masking a solid fill with the icon is
    // the same result; an overlay would paint a square wherever the icon is not, which is how the
    // native bar's pips turn into grey blocks.
    if (bg.wash && bg.image) {
      el.style.backgroundImage = '';
      el.style.backgroundColor = bg.wash;
      el.style.webkitMaskImage = bg.image; el.style.maskImage = bg.image;
      el.style.webkitMaskSize = el.style.maskSize = this._sty.backgroundSize || 'contain';
      el.style.webkitMaskRepeat = el.style.maskRepeat = 'no-repeat';
      el.style.webkitMaskPosition = el.style.maskPosition = 'center';
      return;
    }
    el.style.webkitMaskImage = el.style.maskImage = '';
    el.style.backgroundImage = layers.length ? layers.join(',') : '';
    el.style.backgroundColor = isGradient ? '' : bg.color;
  };

  Panel.prototype._buildSlider = function () {
    var self = this;
    this._min = 0; this._max = 1; this._value = 0;
    var input = document.createElement('input');
    input.type = 'range';
    input.className = 'pnl-slider';
    input.min = '0'; input.max = '1'; input.step = 'any'; input.value = '0';
    input.addEventListener('input', function () {
      self._value = parseFloat(input.value);
      var fn = self._events['onvaluechanged'];
      if (fn) { try { fn(); } catch (e) { log('[onvaluechanged] ' + e); } }
    });
    this.el.appendChild(input);
    this.inputEl = input;

    // THE SLIDER'S OWN SKIN, as Panorama builds it. A Slider with the HorizontalSlider class gets
    // SliderTrack / SliderTrackProgress / SliderThumb children, and the speed dial finds them by id
    // and makes them TRANSPARENT -- it cannot hide them, because a panel that is invisible stops
    // taking the drag. Without these children here the dial's chrome-stripping code would find
    // nothing, report itself unfinished and re-scan every frame, and the browser would not show the
    // stray scroll bar the stripping exists to remove.
    // All FOUR parts, in the game's order. SliderDefault is the marker for the control's default
    // value, zero-sized unless the slider has one; leaving it out put the two trees out of step at
    // exactly that row when diffed against the game.
    var self2 = this;
    ['SliderTrack', 'SliderTrackProgress', 'SliderThumb', 'SliderDefault'].forEach(function (id) {
      var part = new Panel('Panel', id);
      part._attach(self2);
      part.el.classList.add('pnl-sliderpart', 'pnl-sliderpart-' + id);
    });
  };

  // ---- style ------------------------------------------------------------------

  // Properties whose effect depends on the parent's flow, so they are recomputed rather than
  // written straight through.
  var LAYOUT_PROPS = {
    width: 1, height: 1, horizontalAlign: 1, verticalAlign: 1, flowChildren: 1,
    position: 1, transform: 1, align: 1
  };

  var DIRECT = {
    color: 'color', opacity: 'opacity', zIndex: 'zIndex',
    marginTop: 'marginTop', marginBottom: 'marginBottom', marginLeft: 'marginLeft',
    marginRight: 'marginRight', margin: 'margin',
    paddingTop: 'paddingTop', paddingBottom: 'paddingBottom', paddingLeft: 'paddingLeft',
    paddingRight: 'paddingRight', padding: 'padding',
    border: 'border', borderTop: 'borderTop', borderBottom: 'borderBottom',
    borderLeft: 'borderLeft', borderRight: 'borderRight',
    borderRadius: 'borderRadius',
    fontSize: 'fontSize', fontWeight: 'fontWeight', fontFamily: 'fontFamily',
    fontStyle: 'fontStyle', letterSpacing: 'letterSpacing', lineHeight: 'lineHeight',
    textAlign: 'textAlign', textTransform: 'textTransform', whiteSpace: 'whiteSpace',
    textDecoration: 'textDecoration',
    minWidth: 'minWidth', maxWidth: 'maxWidth', minHeight: 'minHeight', maxHeight: 'maxHeight',
    transitionDuration: 'transitionDuration', transitionProperty: 'transitionProperty',
    transitionTimingFunction: 'transitionTimingFunction', transitionDelay: 'transitionDelay',
    transition: 'transition', transformOrigin: 'transformOrigin',
    backgroundSize: 'backgroundSize', backgroundPosition: 'backgroundPosition',
    backgroundRepeat: 'backgroundRepeat', visibility: 'visibility'
  };

  Panel.prototype._setStyle = function (k, v) {
    this._sty[k] = v;
    var el = this.el;

    if (v === null || v === undefined) {
      if (k === 'backgroundColor') { this._bg.color = ''; this._applyBackground(); return; }
      if (k === 'backgroundImage') { this._bg.image = ''; this._applyBackground(); return; }
      if (k === 'washColor') { this._bg.wash = ''; this._applyBackground(); return; }
      if (LAYOUT_PROPS[k]) { this._applyPlacement(); if (k === 'flowChildren') this._applyLayout(); return; }
      if (DIRECT[k]) el.style[DIRECT[k]] = '';
      return;
    }

    if (LAYOUT_PROPS[k]) {
      this._applyPlacement();
      if (k === 'flowChildren') this._applyLayout();
      return;
    }

    switch (k) {
      case 'cursor': el.style.cursor = ({ sizewe: 'ew-resize', hand: 'pointer' })[v] || v; return;
      case 'backgroundColor': this._bg.color = colorValue(v); this._applyBackground(); return;
      case 'backgroundImage': {
        var url = resolveAsset(v);
        this._bg.image = url ? 'url("' + url + '")' : '';
        el.classList.toggle('pnl-noimg', !url);
        this._applyBackground();
        return;
      }
      case 'washColor': this._bg.wash = colorValue(v); this._applyBackground(); return;
      case 'textShadow': el.style.textShadow = textShadowToCss(v); return;
      case 'boxShadow': el.style.boxShadow = boxShadowToCss(v); return;
      case 'brightness': el.style.filter = 'brightness(' + v + ')'; return;
      // Deadlock blurs behind its HUD chrome, and matching that is most of what makes a panel read
      // as native rather than pasted on. Panorama's gaussian(x,y,passes) maps onto a backdrop blur.
      case 'blurFilter': {
        var g = /gaussian\s*\(\s*([\d.]+)/i.exec(String(v));
        el.style.backdropFilter = g ? 'blur(' + g[1] + 'px)' : '';
        el.style.webkitBackdropFilter = el.style.backdropFilter;
        return;
      }
      case 'overflow': {
        var p = String(v).trim().split(/\s+/);
        el.style.overflowX = OVERFLOW[p[0]] || 'visible';
        el.style.overflowY = OVERFLOW[p[1] || p[0]] || 'visible';
        return;
      }
      case 'textOverflow':
        el.classList.toggle('pnl-cliptext', v === 'ellipsis' || v === 'shrink' || v === 'clip');
        el.style.textOverflow = (v === 'shrink') ? 'ellipsis' : v;
        return;
      case 'sound': case 'soundOut': return;
      default:
        if (DIRECT[k]) { el.style[DIRECT[k]] = colorValue(v); return; }
        unknownStyle[k] = (unknownStyle[k] || 0) + 1;
        try { el.style[k] = v; } catch (e) { /* ignore */ }
    }
  };

  Panel.prototype._applyLayout = function () {
    var flow = this._sty.flowChildren;
    var d;
    if (flow === 'right') { d = 'flex'; this.el.style.flexDirection = 'row'; }
    else if (flow === 'down') { d = 'flex'; this.el.style.flexDirection = 'column'; }
    else if (flow === 'left') { d = 'flex'; this.el.style.flexDirection = 'row-reverse'; }
    else if (flow === 'up') { d = 'flex'; this.el.style.flexDirection = 'column-reverse'; }
    else { d = 'grid'; this.el.style.flexDirection = ''; }
    this._display = d;
    this.el.style.display = this._visible ? d : 'none';
    for (var i = 0; i < this.children.length; i++) this.children[i]._applyPlacement();
  };

  function parseSize(val) {
    if (val === null || val === undefined || val === '') return null;
    var s = String(val).trim();
    var m = /^fill-parent-flow\(\s*([\d.]+)\s*\)$/.exec(s);
    if (m) return { fill: parseFloat(m[1]) };
    if (s === 'fit-children') return { css: 'max-content' };
    return { css: s };
  }

  Panel.prototype._applyPlacement = function () {
    var el = this.el, s = this._sty;
    var pflow = this.parent ? this.parent._sty.flowChildren : null;
    var isRow = pflow === 'right' || pflow === 'left';
    var isCol = pflow === 'down' || pflow === 'up';
    var stacking = !isRow && !isCol;

    var w = parseSize(s.width), h = parseSize(s.height);

    el.style.flexGrow = ''; el.style.flexShrink = '0'; el.style.flexBasis = '';
    el.style.gridArea = ''; el.style.justifySelf = ''; el.style.alignSelf = '';
    el.style.position = 'relative';

    if (w && w.fill) {
      if (isRow) { el.style.flexGrow = String(w.fill); el.style.flexBasis = '0'; el.style.minWidth = '0'; el.style.width = ''; }
      else el.style.width = '100%';
    } else el.style.width = w ? w.css : '';

    if (h && h.fill) {
      if (isCol) { el.style.flexGrow = String(h.fill); el.style.flexBasis = '0'; el.style.minHeight = '0'; el.style.height = ''; }
      else el.style.height = '100%';
    } else el.style.height = h ? h.css : '';

    // A CROSS-AXIS 100% IN A FLOW PARENT STRETCHES. Panorama resolves `height: 100%` against the
    // parent's laid-out height even when that parent is sized by its children -- which is the
    // circular dependency the game's own replay scrubber is built on: `#TimeScrubContainer` is
    // `height: 100%` of `#SliderAndMarkers`, which has no height of its own. CSS refuses that
    // percentage (the parent's height is indefinite) and falls back to content height, so the
    // container came out 21px tall against the game's 44. Stretching the flex item is the same
    // answer the engine gives.
    var stretchCross = false;
    if (isRow && h && !h.fill && h.css === '100%') { el.style.height = ''; stretchCross = true; }
    if (isCol && w && !w.fill && w.css === '100%') { el.style.width = ''; stretchCross = true; }

    var ha = s.horizontalAlign, va = s.verticalAlign;
    if (stacking) {
      el.style.gridArea = '1 / 1';
      el.style.justifySelf = HALIGN_GRID[ha] || 'start';
      el.style.alignSelf = VALIGN_GRID[va] || 'start';
    } else if (isRow) {
      el.style.alignSelf = stretchCross ? 'stretch' : (VALIGN_FLEX[va] || 'flex-start');
    } else {
      el.style.alignSelf = stretchCross ? 'stretch' : (HALIGN_FLEX[ha] || 'flex-start');
    }

    this._composeTransform();
  };

  Panel.prototype._composeTransform = function () {
    var s = this._sty, parts = [];
    // Panorama's `position` is "x y z" in px and offsets the panel WITHOUT affecting layout, which
    // is a transform, not CSS position. The Deadlock workspace places almost everything this way.
    if (s.position) {
      var p = String(s.position).trim().split(/\s+/);
      parts.push('translate3d(' + (p[0] || '0px') + ', ' + (p[1] || '0px') + ', 0)');
    }
    if (s.transform && s.transform !== 'none') parts.push(s.transform);
    this.el.style.transform = parts.join(' ');
  };

  // ---- tree -------------------------------------------------------------------

  Panel.prototype._attach = function (parent) {
    if (this.parent) {
      var i = this.parent.children.indexOf(this);
      if (i >= 0) this.parent.children.splice(i, 1);
    }
    this.parent = parent;
    if (parent) { parent.children.push(this); parent.el.appendChild(this.el); }
    this._applyPlacement();
  };

  Panel.prototype.FindChildTraverse = function (id) {
    for (var i = 0; i < this.children.length; i++) {
      var c = this.children[i];
      if (c.id === id) return c;
      var f = c.FindChildTraverse(id);
      if (f) return f;
    }
    return null;
  };
  Panel.prototype.FindChild = function (id) {
    for (var i = 0; i < this.children.length; i++) if (this.children[i].id === id) return this.children[i];
    return null;
  };
  Panel.prototype.FindChildInLayoutFile = function (id) { return this.FindChildTraverse(id); };
  Panel.prototype.GetChild = function (i) { return this.children[i] || null; };
  Panel.prototype.GetChildCount = function () { return this.children.length; };
  Panel.prototype.Children = function () { return this.children.slice(); };
  Panel.prototype.GetParent = function () { return this.parent; };
  Panel.prototype.SetParent = function (p) { this._attach(p); };

  Panel.prototype.RemoveAndDeleteChildren = function () {
    while (this.children.length) this.children[0]._destroy();
  };
  Panel.prototype._destroy = function () {
    while (this.children.length) this.children[0]._destroy();
    if (this.parent) {
      var i = this.parent.children.indexOf(this);
      if (i >= 0) this.parent.children.splice(i, 1);
    }
    if (this.el.parentNode) this.el.parentNode.removeChild(this.el);
    var pi = panels.indexOf(this);
    if (pi >= 0) panels.splice(pi, 1);
    this._deleted = true;
  };
  Panel.prototype.DeleteAsync = function (delay) {
    var self = this;
    setTimeout(function () { self._destroy(); }, (delay || 0) * 1000);
  };
  Panel.prototype.IsValid = function () { return !this._deleted; };

  // ---- classes ----------------------------------------------------------------
  //
  // Classes are real here, not a token bag. The native replay bar is hidden TWO ways -- `Hidden` on
  // the bar collapses it, `Hidden` on #SpectatorOptionsContainer slides it 272px off its own edge --
  // and the whole of the Regular Timeline host is about adding and removing those. A stub that
  // recorded class names without styling anything could not show the difference between a bar that
  // is shown and one parked off screen.

  Panel.prototype.AddClass = function (c) { this.el.classList.add('c-' + c); };
  Panel.prototype.RemoveClass = function (c) { this.el.classList.remove('c-' + c); };
  Panel.prototype.BHasClass = function (c) { return this.el.classList.contains('c-' + c); };
  Panel.prototype.SetHasClass = function (c, on) { this.el.classList.toggle('c-' + c, !!on); };
  Panel.prototype.ToggleClass = function (c) { this.el.classList.toggle('c-' + c); };

  Panel.prototype.GetAttributeString = function (n, d) {
    return (n in this._attrs) ? String(this._attrs[n]) : (d === undefined ? '' : d);
  };
  Panel.prototype.SetAttributeString = function (n, v) { this._attrs[n] = v; };
  Panel.prototype.GetAttributeInt = function (n, d) {
    var v = parseInt(this._attrs[n], 10);
    return isFinite(v) ? v : d;
  };
  Panel.prototype.SetAttributeInt = function (n, v) { this._attrs[n] = v; };
  Panel.prototype.SetDialogVariable = function (n, v) { this._dialogVars = this._dialogVars || {}; this._dialogVars[n] = v; this._renderText(); };
  Panel.prototype.SetDialogVariableInt = Panel.prototype.SetDialogVariable;

  var DOM_EVENT = {
    onactivate: 'click', onmouseover: 'mouseenter', onmouseout: 'mouseleave',
    oncontextmenu: 'contextmenu', onmousedown: 'mousedown', onmouseup: 'mouseup',
    ondblclick: 'dblclick'
  };

  Panel.prototype.SetPanelEvent = function (name, fn) {
    this._events[name] = fn;
    var dom = DOM_EVENT[name];
    if (!dom || this['_wired_' + name]) return;
    this['_wired_' + name] = true;
    var self = this;
    this.el.addEventListener(dom, function (e) {
      if (!self._hittest || !self._enabled) return;
      if (name === 'oncontextmenu') e.preventDefault();
      if (name === 'onactivate') e.stopPropagation();
      var h = self._events[name];
      if (h) { try { h(); } catch (err) { log('[' + name + '] ' + err); } }
    });
  };
  Panel.prototype.ClearPanelEvent = function (n) { delete this._events[n]; };

  Panel.prototype.SetImage = function (src) { this._setStyle('backgroundImage', 'url("' + src + '")'); };
  Panel.prototype.SetFocus = function () { if (this.inputEl) this.inputEl.focus(); return true; };
  Panel.prototype.SetAcceptsFocus = function () {};
  Panel.prototype.GetPositionWithinWindow = function () {
    var r = this.el.getBoundingClientRect();
    var host = Shim.hostRect();
    var k = stageScale();
    return { x: (r.left - host.left) / k, y: (r.top - host.top) / k };
  };

  // Panorama's actualxoffset/actualyoffset: from the PARENT panel's origin, before that parent's
  // scroll. The rects are measured after the stage's CSS scale and the scroll offset is not, so
  // only the rect difference is divided by it.
  function parentOffset(panel, edge) {
    var r = panel.el.getBoundingClientRect();
    var p = panel.parent;
    if (!p) return (r[edge] - Shim.hostRect()[edge]) / stageScale();
    var pr = p.el.getBoundingClientRect();
    return (r[edge] - pr[edge]) / stageScale() + (edge === 'left' ? p.el.scrollLeft : p.el.scrollTop);
  }

  Panel.prototype._renderText = function () {
    if (!this.textEl) return;
    var t = String(this._text === null || this._text === undefined ? '' : this._text);
    var self = this;
    t = t.replace(/\{[sdif]:([A-Za-z0-9_]+)\}/g, function (m, n) {
      return (self._dialogVars && n in self._dialogVars) ? String(self._dialogVars[n]) : m;
    });
    this.textEl.textContent = t;
  };

  Object.defineProperties(Panel.prototype, {
    visible: {
      get: function () { return this._visible; },
      set: function (v) {
        this._visible = !!v;
        this.el.style.display = this._visible ? (this._display || 'grid') : 'none';
      }
    },
    hittest: {
      get: function () { return this._hittest; },
      // Panorama's hittest=false makes THIS panel transparent to the mouse and says nothing about
      // its children. CSS pointer-events:none also kills every descendant, so a full-screen root
      // with hittest=false would take the whole workspace out of reach. The inline 'none' stays for
      // the panel itself; the pnl-nohit rule hands its children back their clicks, and a child that
      // is itself hittest=false keeps its own inline none, which outranks that rule.
      set: function (v) {
        this._hittest = !!v;
        this.el.style.pointerEvents = v ? 'auto' : 'none';
        this.el.classList.toggle('pnl-nohit', !v);
      }
    },
    hittestchildren: {
      get: function () { return this._hittestchildren !== false; },
      set: function (v) { this._hittestchildren = !!v; this.el.classList.toggle('no-hit-children', !v); }
    },
    enabled: {
      get: function () { return this._enabled; },
      set: function (v) { this._enabled = !!v; this.el.classList.toggle('pnl-disabled', !v); }
    },
    text: {
      get: function () { return this._text; },
      set: function (v) { this._text = v; this._renderText(); }
    },
    value: {
      get: function () { return this._value; },
      set: function (v) {
        this._value = v;
        if (this.inputEl) this.inputEl.value = String(v);
        // The thumb and the progress fill are panels in Panorama, so they have to be moved by hand
        // the way the engine moves them -- otherwise the native scrubber's position in the browser
        // would not follow the value the state feed reports.
        var t = this.FindChild('SliderThumb'), pr = this.FindChild('SliderTrackProgress');
        var span = (this._max - this._min) || 1;
        var f = Math.max(0, Math.min(1, (v - this._min) / span));
        if (t) t.el.style.left = (f * 100) + '%';
        if (pr) pr.el.style.width = (f * 100) + '%';
      }
    },
    min: {
      get: function () { return this._min; },
      set: function (v) { this._min = v; if (this.inputEl) this.inputEl.min = String(v); }
    },
    max: {
      get: function () { return this._max; },
      set: function (v) { this._max = v; if (this.inputEl) this.inputEl.max = String(v); }
    },
    checked: {
      get: function () { return !!this._checked; },
      set: function (v) { this._checked = !!v; this.el.classList.toggle('pnl-checked', !!v); }
    },
    actuallayoutwidth: { get: function () { return this.el.getBoundingClientRect().width / stageScale(); } },
    actuallayoutheight: { get: function () { return this.el.getBoundingClientRect().height / stageScale(); } },
    contentwidth: { get: function () { return this.el.scrollWidth / stageScale(); } },
    contentheight: { get: function () { return this.el.scrollHeight / stageScale(); } },
    desiredlayoutwidth: { get: function () { return this.el.scrollWidth / stageScale(); } },
    desiredlayoutheight: { get: function () { return this.el.scrollHeight / stageScale(); } },
    actualuiscale_x: { get: function () { return UI_SCALE; } },
    actualuiscale_y: { get: function () { return UI_SCALE; } },
    // PARENT-relative, as in the game. These used to answer in window space, which made popups
    // placed from them draw under their control here and in the top-left corner of the game.
    // Window space is GetPositionWithinWindow()'s job.
    actualxoffset: { get: function () { return parentOffset(this, 'left'); } },
    actualyoffset: { get: function () { return parentOffset(this, 'top'); } },
    scrolloffset_y: { get: function () { return this.el.scrollTop; }, set: function () {} },
    scrolloffset_x: { get: function () { return this.el.scrollLeft; }, set: function () {} },
    tooltiptext: { get: function () { return this._tip || ''; }, set: function (v) { this._tip = v; } }
  });

  // ---------------------------------------------------------------- driver

  var Shim = {
    timers: new Set(),
    root: null,
    hostEl: null,
    onCommand: null,

    hostRect: function () {
      return Shim.hostEl ? Shim.hostEl.getBoundingClientRect() : { left: 0, top: 0 };
    },

    reset: function (hostEl) {
      Shim.timers.forEach(function (h) { clearTimeout(h); });
      Shim.timers.clear();
      logLines.length = 0;
      commands.length = 0;
      panels.length = 0;
      styleWrites.count = 0;
      styleWrites.byPanel = Object.create(null);
      unknownStyle = {};
      Shim.hostEl = hostEl;
      hostEl.innerHTML = '';
      // The context panel the hook resolves in game: Deadlock's HUD root, which is what
      // $.GetContextPanel() answers and what the workspace builds into.
      var ctx = new Panel('Panel', 'CitadelHudRoot');
      ctx.style.width = '100%';
      ctx.style.height = '100%';
      hostEl.appendChild(ctx.el);
      ctx.el.classList.add('pnl-context');
      Shim.root = ctx;
      return ctx;
    },

    findById: function (id) {
      for (var i = 0; i < panels.length; i++) if (panels[i].id === id) return panels[i];
      return null;
    },

    allPanels: function () { return panels.slice(); },
    styleWrites: function () { return styleWrites; },
    unknownStyleProps: function () { return unknownStyle; },
    logs: function () { return logLines; },
    commands: function () { return commands; }
  };

  // ---------------------------------------------------------------- $

  function $(sel) {
    if (typeof sel === 'string' && sel.charAt(0) === '#') return Shim.findById(sel.slice(1));
    return null;
  }

  $.CreatePanel = function (type, parent, id, props) {
    var p = new Panel(type, id);
    if (parent) p._attach(parent);
    if (props) {
      for (var k in props) {
        if (k === 'class') String(props[k]).split(/\s+/).forEach(function (c) { if (c) p.AddClass(c); });
        else if (k === 'text') p.text = props[k];
        else try { p[k] = props[k]; } catch (e) { /* ignore */ }
      }
    }
    return p;
  };

  $.GetContextPanel = function () { return Shim.root; };

  // THE COMMAND CHANNEL. Not a convenience: it is the only route out of this context, and modelling
  // it exactly is what keeps the mockup honest. A panel that went back to calling a binding that
  // does not exist would show up as a control that issues nothing.
  $.Msg = function () {
    var s = Array.prototype.map.call(arguments, String).join(' ');
    if (s.indexOf(CMD_MARKER) === 0) {
      var cmd = s.slice(CMD_MARKER.length).trim();
      commands.push(cmd);
      if (Shim.onCommand) { try { Shim.onCommand(cmd); } catch (e) { log('[cmd] ' + e); } }
      return;
    }
    log(s.replace(/\n$/, ''));
  };

  // Tracked so a rebuild can cancel them. Several fragments run a self-rescheduling pump, and one
  // left ticking against a torn-down tree shows up as phantom log lines and stray exceptions.
  $.Schedule = function (delay, fn) {
    var h = setTimeout(function () {
      Shim.timers.delete(h);
      try { fn(); } catch (e) { log('[schedule] ' + e); }
    }, (delay || 0) * 1000);
    Shim.timers.add(h);
    return h;
  };
  $.CancelScheduled = function (h) { Shim.timers.delete(h); clearTimeout(h); };

  // Panorama needs the SOURCE argument. Drop it and the event fires with no source, the native
  // onactivate never runs, and the button highlights and does nothing.
  $.DispatchEvent = function (name, panel, source) {
    if (name === 'Activated' && panel && panel._events && panel._events['onactivate']) {
      try { panel._events['onactivate'](); } catch (e) { log('[dispatch] ' + e); }
      return;
    }
    log('[DispatchEvent] ' + name + ' -> ' + (panel && panel.id) + (source ? ' (' + source + ')' : ''));
  };
  $.DispatchEventAsync = function (delay, name, panel, source) {
    setTimeout(function () { $.DispatchEvent(name, panel, source); }, (delay || 0) * 1000);
  };

  $.RegisterEventHandler = function () { return 0; };
  $.RegisterForUnhandledEvent = function () { return 0; };
  $.UnregisterEventHandler = function () {};
  $.Localize = function (t) { return String(t).replace(/^#/, ''); };
  $.LocalizeSafe = $.Localize;
  $.ConstructString = $.Localize;
  $.Language = function () { return 'english'; };
  $.DbgIsReloadingScript = function () { return false; };

  global.$ = $;

  // DELIBERATELY ABSENT, mirroring the real context. Deleting rather than omitting, because a
  // previous page load or another script could have left one lying around -- and a panel that finds
  // one here would work in the browser and be dead in game.
  try { delete global.GameInterfaceAPI; } catch (e) { global.GameInterfaceAPI = undefined; }
  try { delete global.UiToolkitAPI; } catch (e) { global.UiToolkitAPI = undefined; }

  // Deadlock's own replay-bar functions, which its XML binds as onactivate expressions. The
  // Regular Timeline host delegates to these, so they are present and RECORDED -- and the panels
  // must also survive them being absent, which the build test covers separately.
  ['CitadelReplayIncrementSpeedStep', 'CitadelReplaySkipByTime', 'CitadelReplayNextMarker'].forEach(function (n) {
    global[n] = function () {
      var args = Array.prototype.slice.call(arguments).join(', ');
      commands.push('(native) ' + n + '(' + args + ')');
      if (Shim.onCommand) { try { Shim.onCommand('(native) ' + n + '(' + args + ')'); } catch (e) {} }
    };
  });

  global.PanoramaShim = Shim;
  global.PanoramaPanel = Panel;
})(window);
