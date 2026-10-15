// Panorama -> DOM emulation layer.
//
// The extracted in-game scripts run against this UNCHANGED, so the mockup shows the real UI
// rather than a copy of it. Everything here exists to make CS2's Panorama API behave the way
// the scripts expect in a browser.
//
// The two layout rules that matter most, because they are what makes a Panorama panel tree look
// the way it does in game:
//
//   1. A panel with NO flowChildren stacks its children at its own origin, all overlapping, and
//      sizes itself to the largest one. That is CSS grid with every child in cell 1/1 -- not
//      absolute positioning, which would collapse the parent to zero.
//   2. A panel with flowChildren 'right'/'down' lays them out in a line and never SHRINKS them
//      (Panorama clips or squishes instead). That is flex with flex-shrink:0 on every child.
//
// horizontalAlign/verticalAlign then act as the cross-axis alignment inside a flowing parent and
// as both-axis alignment inside a stacking one.

(function (global) {
  'use strict';

  // The stage is laid out at a true 1920x1080 and then CSS-scaled to fit the browser window.
  // getBoundingClientRect reports POST-transform pixels, so every measurement handed back to the
  // scripts is divided by this to keep them in the same logical space the game uses. Panorama's
  // own uiscale is reported as 1.0 for the same reason: in logical space there is no extra scale,
  // and probes like textIsClipped() normalise by it.
  var stageScale = function () { return (global.MvmMockup && global.MvmMockup.stageScale) || 1; };
  var UI_SCALE = 1.0;
  var panels = [];              // every live panel, for id lookup
  var unknownStyle = {};        // style props we did not map, surfaced in the dev overlay
  var logLines = [];

  function log(s) {
    logLines.push(s);
    if (logLines.length > 400) logLines.shift();
    if (global.MvmMockup && global.MvmMockup.onLog) global.MvmMockup.onLog(s);
  }

  // ---------------------------------------------------------------- colours & values

  // Panorama writes #rrggbbaa, which CSS understands natively. gradient() does not exist in CSS,
  // so it is rewritten into linear-gradient()/radial-gradient().
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

  function gradientToCss(v) {
    var m = /^gradient\s*\(([\s\S]*)\)\s*$/i.exec(v.trim());
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
    var list = stops.map(function (s) { return s.color + ' ' + (s.pos * 100).toFixed(1) + '%'; }).join(', ');

    if (kind === 'radial') {
      var cx = geom[0] ? geom[0].split(/\s+/)[0] : '50%';
      var cy = geom[0] ? (geom[0].split(/\s+/)[1] || '50%') : '50%';
      return 'radial-gradient(ellipse at ' + cx + ' ' + cy + ', ' + list + ')';
    }
    // linear: geom[0] = "x1 y1", geom[1] = "x2 y2"
    var a0 = (geom[0] || '0% 0%').split(/\s+/), a1 = (geom[1] || '0% 100%').split(/\s+/);
    var x1 = parseFloat(a0[0]), y1 = parseFloat(a0[1]);
    var x2 = parseFloat(a1[0]), y2 = parseFloat(a1[1]);
    var dir = 'to bottom';
    if (Math.abs(x2 - x1) >= Math.abs(y2 - y1)) dir = (x2 >= x1) ? 'to right' : 'to left';
    else dir = (y2 >= y1) ? 'to bottom' : 'to top';
    return 'linear-gradient(' + dir + ', ' + list + ')';
  }

  function colorValue(v) {
    if (typeof v !== 'string') return v;
    if (/^gradient\s*\(/i.test(v.trim())) return gradientToCss(v);
    return v;
  }

  // Panorama textShadow is "x y blur strength color"; CSS has no strength component.
  function textShadowToCss(v) {
    var p = String(v).trim().split(/\s+/);
    if (p.length >= 5) return p[0] + ' ' + p[1] + ' ' + p[2] + ' ' + p[4];
    return v;
  }

  // Panorama asset refs point into the game's own files (s2r://panorama/..., file://{images}/...,
  // file://{resources}/...). Those exact assets are pulled into UiMockup/assets by
  // fetch-assets.mjs, so the reference is rewritten to the real file rather than replaced with a
  // placeholder -- the dotted panel background, the fade masks, the icons and the animated panel
  // video are Valve's, and stand-ins would not look right.
  //
  // Compiled resources change extension on extraction (.vtex -> .png, .vsvg -> .svg); videos are
  // loose files copied as-is. {resources} resolves to csgo/panorama/, {images} to its images/ dir.
  function resolveAsset(v) {
    var str = String(v);
    var m = /url\(\s*["']?([^"')]+)["']?\s*\)/i.exec(str);
    var raw = m ? m[1] : str;
    if (/^(https?:|data:|\.|\/|assets\/)/i.test(raw)) return { kind: 'image', url: raw };

    var path = null;
    if (/^s2r:\/\//i.test(raw)) path = raw.replace(/^s2r:\/\//i, '');
    else if (/^file:\/\/\{images\}\//i.test(raw)) path = 'panorama/images/' + raw.replace(/^file:\/\/\{images\}\//i, '');
    else if (/^file:\/\/\{resources\}\//i.test(raw)) path = raw.replace(/^file:\/\/\{resources\}\//i, '');
    if (!path) return null;

    path = path.replace(/\.vtex$/i, '.png').replace(/\.vsvg$/i, '.svg').replace(/\.vsvg_c$/i, '.svg');
    if (/\.(webm|mp4)$/i.test(path)) return { kind: 'video', url: 'assets/' + path };
    if (/\.(png|svg|jpe?g|webp)$/i.test(path)) return { kind: 'image', url: 'assets/' + path };
    return null;   // layouts and other non-visual {resources} paths
  }

  var OVERFLOW = { noclip: 'visible', clip: 'hidden', scroll: 'auto', squish: 'hidden' };

  // ---------------------------------------------------------------- Panel

  var HALIGN_GRID = { left: 'start', center: 'center', right: 'end' };
  var VALIGN_GRID = { top: 'start', center: 'center', bottom: 'end' };
  var HALIGN_FLEX = { left: 'flex-start', center: 'center', right: 'flex-end' };
  var VALIGN_FLEX = { top: 'flex-start', center: 'center', bottom: 'flex-end' };

  var TEXT_TYPES = { Label: 1, Button: 1, TextEntry: 1 };

  function Panel(type, id) {
    this.type = type || 'Panel';
    this.id = id || '';
    this.children = [];
    this.parent = null;
    this._sty = Object.create(null);
    this._attrs = Object.create(null);
    this._dialogVars = Object.create(null);
    this._events = Object.create(null);
    this._visible = true;
    this._hittest = true;
    this._hittestchildren = true;
    this._enabled = true;
    this._deleted = false;
    this._text = '';
    // Colour / image / image-alpha are kept apart because CSS cannot express Panorama's
    // per-layer image alpha directly; _applyBackground() recomposites all three.
    this._bg = { color: '', image: '', imgAlpha: 1, wash: '' };

    var el = document.createElement('div');
    el.className = 'pnl pnl-' + this.type;
    el.__panel = this;
    this.el = el;
    // Carry the Panorama panel id onto the DOM node. Without it the browser tree is 1500 anonymous
    // divs, so neither a geometry diff against the game nor a person in devtools can say WHICH
    // panel they are looking at -- and the point of this mockup is that an edit here can be traced
    // back to the panel in AfxHookSource2/Filmmaker/Panorama/*Js.h. The real id is set too, but
    // only while it is still free: a duplicate id would break every getElementById lookup.
    if (this.id) {
      el.setAttribute('data-pid', this.id);
      if (!document.getElementById(this.id)) el.id = this.id;
    }

    // A Label's text lives in its own node so children (rare, but legal) still lay out.
    if (TEXT_TYPES[this.type]) {
      this.textEl = document.createElement('span');
      this.textEl.className = 'pnl-text';
      el.appendChild(this.textEl);
    }
    if (this.type === 'Slider') this._buildSlider();
    if (this.type === 'TextEntry') this._buildTextEntry();
    if (this.type === 'ToggleButton' || this.type === 'RadioButton') this._checked = false;

    var self = this;
    this.style = new Proxy(Object.create(null), {
      set: function (t, k, v) { t[k] = v; self._setStyle(k, v); return true; },
      get: function (t, k) { return t[k]; },
      deleteProperty: function (t, k) { delete t[k]; self._setStyle(k, null); return true; }
    });

    panels.push(this);
    this._applyLayout();
    this._applyPlacement();
  }

  // Paint order, matching Panorama: background colour, then the image at background-img-opacity.
  // The veil layer is the base colour again at (1 - alpha), which composites an opaque texture
  // to exactly the same result the engine produces, and degrades sensibly for a translucent one.
  var RGB_RE = /^rgba?\(\s*([\d.]+)[,\s]+([\d.]+)[,\s]+([\d.]+)/i;
  Panel.prototype._applyBackground = function () {
    var el = this.el;
    var bg = this._bg;
    var color = bg.color || '';
    var img = bg.image || '';
    var alpha = (typeof bg.imgAlpha === 'number' && isFinite(bg.imgAlpha)) ? bg.imgAlpha : 1;

    // A video layer is still the background IMAGE layer, so it takes background-img-opacity, and
    // a wash has to multiply THAT layer rather than the whole panel. Applying the wash as a panel
    // overlay painted flat colour across the panel's transparent area as well, which is how the
    // render workspace's JOB and RENDER columns came out as grey slabs instead of a dim contour
    // texture. Both go on the media wrapper, which covers exactly the video and nothing else.
    if (this._videoWrap) {
      this._videoWrap.style.opacity = (alpha >= 0.999) ? '' : String(alpha);
      if (bg.wash) {
        this._videoWrap.style.setProperty('--wash', bg.wash);
        this._videoWrap.classList.add('pnl-washmedia');
      } else {
        this._videoWrap.classList.remove('pnl-washmedia');
      }
    }

    // Panorama's background-color accepts a gradient(); CSS background-color does not, and writing
    // one there silently drops the fill. A gradient has to ride in the image stack instead, as the
    // BOTTOM layer -- CSS paints the first layer on top, so it goes last.
    var colorIsGradient = /gradient\s*\(/i.test(color);
    var layers = [];

    if (img && alpha < 0.999 && alpha > 0.001 && !colorIsGradient) {
      // Per-layer image alpha does not exist in CSS. Veiling the image with (1 - alpha) of the base
      // colour reproduces Panorama's blend exactly for an opaque texture, which is what every use
      // of background-img-opacity in this UI is.
      var m = RGB_RE.exec(color);
      if (m) {
        var veil = 'rgba(' + m[1] + ',' + m[2] + ',' + m[3] + ',' + (1 - alpha).toFixed(3) + ')';
        layers.push('linear-gradient(' + veil + ',' + veil + ')');
      }
    }
    if (img && alpha > 0.001) layers.push(img);
    if (colorIsGradient) layers.push(color);

    // A washed IMAGE is a solid fill masked by the icon: the same pixels the engine produces, and
    // unlike an overlay it leaves the icon's transparent margin alone.
    if (bg.wash && img) {
      el.classList.remove('pnl-wash');
      el.style.backgroundImage = '';
      el.style.backgroundColor = bg.wash;
      el.style.webkitMaskImage = img; el.style.maskImage = img;
      el.style.webkitMaskSize = 'contain'; el.style.maskSize = 'contain';
      el.style.webkitMaskRepeat = 'no-repeat'; el.style.maskRepeat = 'no-repeat';
      el.style.webkitMaskPosition = 'center'; el.style.maskPosition = 'center';
      return;
    }
    // The panel-wide overlay is only right when the content it tints is text or child panels.
    if (bg.wash && !this._videoWrap) el.classList.add('pnl-wash');

    el.style.backgroundImage = layers.length ? layers.join(',') : '';
    el.style.backgroundColor = colorIsGradient ? '' : color;
  };
  Panel.prototype._buildSlider = function () {
    var self = this;
    this.min = 0; this.max = 1; this._value = 0;
    var input = document.createElement('input');
    input.type = 'range'; input.className = 'pnl-slider';
    input.min = '0'; input.max = '1'; input.step = 'any'; input.value = '0';
    input.addEventListener('input', function () {
      self._value = parseFloat(input.value);
      Shim.fireEvent('SliderValueChanged', self, self._value);
    });
    input.addEventListener('change', function () { Shim.fireEvent('SliderReleased', self, self._value); });
    this.el.appendChild(input);
    this.inputEl = input;
  };

  Panel.prototype._buildTextEntry = function () {
    var self = this;
    var input = document.createElement('input');
    input.type = 'text'; input.className = 'pnl-entry';
    input.addEventListener('input', function () {
      // Panorama keeps TextEntry.text synchronized with what the user typed. Mirror that here so
      // submit handlers read the current value instead of the value assigned when the panel built.
      self._text = input.value;
      Shim.fireEvent('TextEntryChanged', self, input.value);
      var h = self._events['ontextentrychange'] || self._events['ontextchanged'];
      if (h) h();
    });
    input.addEventListener('keydown', function (e) {
      if (e.key === 'Enter' && self._events['oninputsubmit']) self._events['oninputsubmit']();
      if (e.key === 'Escape' && self._events['oncancel']) self._events['oncancel']();
    });
    input.addEventListener('blur', function () { if (self._events['onblur']) self._events['onblur'](); });
    this.el.appendChild(input);
    this.inputEl = input;
  };

  // ---- style ------------------------------------------------------------------

  // Properties whose effect depends on the parent's flow, so they are recomputed rather than
  // written straight through.
  var LAYOUT_PROPS = {
    width: 1, height: 1, horizontalAlign: 1, verticalAlign: 1, flowChildren: 1,
    ignoreParentFlow: 1, position: 1, transform: 1, x: 1, y: 1, align: 1
  };

  var DIRECT = {
    backgroundColor: 'backgroundColor', color: 'color', opacity: 'opacity', zIndex: 'zIndex',
    marginTop: 'marginTop', marginBottom: 'marginBottom', marginLeft: 'marginLeft',
    marginRight: 'marginRight', margin: 'margin',
    paddingTop: 'paddingTop', paddingBottom: 'paddingBottom', paddingLeft: 'paddingLeft',
    paddingRight: 'paddingRight', padding: 'padding',
    border: 'border', borderTop: 'borderTop', borderBottom: 'borderBottom',
    borderLeft: 'borderLeft', borderRight: 'borderRight',
    borderRadius: 'borderRadius', borderTopLeftRadius: 'borderTopLeftRadius',
    borderTopRightRadius: 'borderTopRightRadius', borderBottomLeftRadius: 'borderBottomLeftRadius',
    borderBottomRightRadius: 'borderBottomRightRadius',
    fontSize: 'fontSize', fontWeight: 'fontWeight', fontFamily: 'fontFamily',
    fontStyle: 'fontStyle', letterSpacing: 'letterSpacing', lineHeight: 'lineHeight',
    textAlign: 'textAlign', textTransform: 'textTransform', textOverflow: 'textOverflow',
    whiteSpace: 'whiteSpace', textDecoration: 'textDecoration',
    boxShadow: 'boxShadow', minWidth: 'minWidth', maxWidth: 'maxWidth',
    minHeight: 'minHeight', maxHeight: 'maxHeight',
    transitionDuration: 'transitionDuration', transitionProperty: 'transitionProperty',
    transitionTimingFunction: 'transitionTimingFunction', transitionDelay: 'transitionDelay',
    transition: 'transition', transformOrigin: 'transformOrigin',
    backgroundSize: 'backgroundSize', backgroundPosition: 'backgroundPosition',
    backgroundRepeat: 'backgroundRepeat', visibility: 'visibility'
  };

  // Every write the SCRIPT makes to the DOM style is followed by a snapshot of the result, so a
  // change made afterwards by hand (devtools, the Elements panel) can be told apart from what the
  // panel code asked for. That difference is what compare.js turns into Panorama style lines.
  Panel.prototype._stamp = function () { this.el.__authored = this.el.style.cssText; };
  Panel.prototype._setStyle = function (k, v) { this._setStyleImpl(k, v); this._stamp(); };
  // Untransformed layout size in design px. offsetWidth rounds, so the exact rect is preferred when
  // no transform is in play (rect / stage scale agrees with offsetWidth to within a pixel then).
  function layoutSize(el, axis) {
    var off = axis === 'width' ? el.offsetWidth : el.offsetHeight;
    var rect = el.getBoundingClientRect()[axis] / stageScale();
    return Math.abs(rect - off) < 1 ? rect : off;
  }
  var posSeq = 0;
  Panel.prototype._setStyleImpl = function (k, v) {
    this._sty[k] = v;
    if (k === 'position' || k === 'x' || k === 'y') (this._posSeq = this._posSeq || {})[k] = ++posSeq;
    var el = this.el;
    if (v === null || v === undefined) {
      // Clearing a background property has to go through the compositor too, or the panel keeps
      // painting the layer that was just removed.
      if (k === 'backgroundColor') { this._bg.color = ''; this._applyBackground(); return; }
      if (k === 'backgroundImage') { this._bg.image = ''; this._clearVideo(); this._applyBackground(); return; }
      if (k === 'backgroundImgOpacity') { this._bg.imgAlpha = 1; this._applyBackground(); return; }
      if (DIRECT[k]) el.style[DIRECT[k]] = '';
      return;
    }

    if (LAYOUT_PROPS[k]) { this._applyPlacement(); if (k === 'flowChildren') this._applyLayout(); return; }

    switch (k) {
      case 'backgroundColor': this._bg.color = colorValue(v); this._applyBackground(); return;
      case 'textShadow': el.style.textShadow = textShadowToCss(v); return;
      // Panorama only trims a label when it is told to; an unconstrained one just takes the width
      // it needs. The shim used to clip every label unconditionally, so anywhere Chrome measured
      // text a few px wider than Panorama did the last characters vanished (REGULAR TIMELINE lost
      // its E). Clip only where the source asked for it.
      case 'textOverflow':
        el.classList.toggle('pnl-cliptext', v === 'ellipsis' || v === 'shrink' || v === 'clip');
        el.style.textOverflow = (v === 'shrink') ? 'ellipsis' : v;
        return;
      case 'overflow': {
        var p = String(v).trim().split(/\s+/);
        var ox = OVERFLOW[p[0]] || 'visible';
        var oy = OVERFLOW[p[1] || p[0]] || 'visible';
        el.style.overflowX = ox; el.style.overflowY = oy;
        return;
      }
      case 'backgroundImage': {
        var a = resolveAsset(v);
        if (a && a.kind === 'video') {
          // CSS cannot play a video as a background-image, so the animated panel surfaces get a
          // real <video> layer underneath their content. Muted + playsinline so it autoplays.
          this._setVideo(a.url);
          this._bg.image = '';
          el.classList.remove('pnl-noimg');
        } else {
          this._clearVideo();
          if (a) { this._bg.image = 'url("' + a.url + '")'; el.classList.remove('pnl-noimg'); }
          else { this._bg.image = ''; el.classList.add('pnl-noimg'); }
        }
        this._applyBackground();
        return;
      }
      case 'backgroundImgOpacity': this._bg.imgAlpha = parseFloat(v); this._applyBackground(); return;
      case 'backgroundSize':
        el.style.backgroundSize = v;
        if (this._videoEl) this._videoEl.style.objectFit = (v === 'contain') ? 'contain' : 'cover';
        return;
      // brightness and blur both land in CSS `filter`, so they are composed rather than written
      // over each other. Panorama's blur is `gaussian(h, v, passes)` and can be directional
      // (a horizontal motion smear); CSS blur is isotropic, so the mockup approximates it with a
      // radius weighted toward the larger axis. Fonts and the exact smear need a game capture.
      case 'brightness': this._filt = this._filt || {}; this._filt.brightness = v; this._applyFilter(); return;
      case 'blur': {
        this._filt = this._filt || {};
        var gm = /gaussian\(\s*([\d.]+)\s*(?:,\s*([\d.]+))?/i.exec(String(v));
        var gh = gm ? parseFloat(gm[1]) : 0, gv = gm && gm[2] !== undefined ? parseFloat(gm[2]) : gh;
        this._filt.blur = gm ? (Math.max(gh, gv) * 0.32 + Math.min(gh, gv) * 0.6) : 0;
        this._applyFilter();
        return;
      }
      case 'washColor': {
        // Panorama multiplies panel CONTENT by this colour, so a monochrome glyph comes out tinted
        // and the transparent margin around it stays transparent. An overlay div cannot express
        // that -- it painted a solid square wherever the icon was not, which is how the demo bar's
        // kill/death pips turned into grey blocks. _applyBackground() masks a solid fill with the
        // icon instead; the overlay stays for panels whose content is text or children.
        this._bg.wash = colorValue(v);
        el.style.setProperty('--wash', this._bg.wash);
        this._applyBackground();
        return;
      }
      // Panorama's opacity mask is a CSS mask. Dropping it (as this did) removed every soft
      // edge in the UI -- the Customize modal's bottom fade, the timeline's end ramps -- and
      // replaced them with a hard cut, which reads as a different design rather than a subtle
      // one. Gradient masks are passed through verbatim; an asset mask is resolved like any
      // other image.
      case 'opacityMask': case 'opacityMaskScrollUp': case 'opacityMaskScrollDown': {
        var mv = String(v).trim();
        var mcss = /^(gradient|linear-gradient|radial-gradient)/i.test(mv)
          ? colorValue(mv)
          : (function () { var ma = resolveAsset(mv); return ma && ma.kind !== 'video' ? 'url("' + ma.url + '")' : ''; })();
        if (!mcss) return;
        el.style.webkitMaskImage = mcss; el.style.maskImage = mcss;
        el.style.webkitMaskSize = '100% 100%'; el.style.maskSize = '100% 100%';
        el.style.webkitMaskRepeat = 'no-repeat'; el.style.maskRepeat = 'no-repeat';
        return;
      }
      case 'sound': case 'soundOut': return;
      default:
        if (DIRECT[k]) { el.style[DIRECT[k]] = colorValue(v); return; }
        unknownStyle[k] = (unknownStyle[k] || 0) + 1;
        try { el.style[k] = v; } catch (e) { /* ignore */ }
    }
  };

  Panel.prototype._setVideo = function (url) {
    if (this._videoEl && this._videoEl.__src === url) return;
    this._clearVideo();
    var v = document.createElement('video');
    v.className = 'pnl-video';
    v.src = url; v.__src = url;
    v.autoplay = true; v.loop = true; v.muted = true; v.playsInline = true;
    v.setAttribute('muted', '');            // Safari/Chrome autoplay policy wants the attribute too
    var fit = this._sty.backgroundSize;
    v.style.objectFit = (fit === 'contain') ? 'contain' : 'cover';
    // The video sits in a wrapper, not straight in the panel: background-img-opacity and
    // wash-color both belong to the image LAYER, and applying them to the panel would fade or
    // tint its children and its transparent margin along with the texture.
    var wrap = document.createElement('div');
    wrap.className = 'pnl-videowrap';
    wrap.appendChild(v);
    this.el.insertBefore(wrap, this.el.firstChild);
    this._videoEl = v;
    this._videoWrap = wrap;
    var play = v.play();
    if (play && play.catch) play.catch(function () { /* autoplay blocked; poster frame still shows */ });
  };
  Panel.prototype._clearVideo = function () {
    if (!this._videoEl) return;
    var node = this._videoWrap || this._videoEl;
    if (node.parentNode) node.parentNode.removeChild(node);
    this._videoEl = null;
    this._videoWrap = null;
  };

  // How THIS panel arranges its own children.
  Panel.prototype._applyLayout = function () {
    var flow = this._sty.flowChildren;
    var d;
    this.el.style.flexWrap = '';
    if (flow === 'right') { d = 'flex'; this.el.style.flexDirection = 'row'; }
    // Panorama's `flow-children: right-wrap` (CS2's own stylesheets use it for chip rows).
    else if (flow === 'right-wrap') { d = 'flex'; this.el.style.flexDirection = 'row'; this.el.style.flexWrap = 'wrap'; }
    else if (flow === 'down') { d = 'flex'; this.el.style.flexDirection = 'column'; }
    else if (flow === 'left') { d = 'flex'; this.el.style.flexDirection = 'row-reverse'; }
    else if (flow === 'up') { d = 'flex'; this.el.style.flexDirection = 'column-reverse'; }
    else { d = 'grid'; this.el.style.flexDirection = ''; }
    this._display = d;
    this.el.style.display = this._visible ? d : 'none';
    for (var i = 0; i < this.children.length; i++) this.children[i]._applyPlacement();
    this._stamp();
  };

  function parseSize(val) {
    if (val === null || val === undefined) return null;
    var s = String(val).trim();
    var m = /^fill-parent-flow\(\s*([\d.]+)\s*\)$/.exec(s);
    if (m) return { fill: parseFloat(m[1]) };
    if (s === 'fit-children') return { css: 'max-content' };
    return { css: s };
  }

  // How this panel sits inside its parent.
  Panel.prototype._applyPlacement = function () {
    var el = this.el, s = this._sty;
    var pflow = this.parent ? this.parent._sty.flowChildren : null;
    var isRow = pflow === 'right' || pflow === 'left' || pflow === 'right-wrap';
    var isCol = pflow === 'down' || pflow === 'up';
    var stacking = !isRow && !isCol;
    var abs = s.ignoreParentFlow === true || s.ignoreParentFlow === 'true';

    var w = parseSize(s.width), h = parseSize(s.height);

    el.style.flexGrow = ''; el.style.flexShrink = '0'; el.style.flexBasis = '';
    el.style.gridArea = ''; el.style.justifySelf = ''; el.style.alignSelf = '';
    el.style.position = abs ? 'absolute' : 'relative';
    el.style.top = ''; el.style.left = ''; el.style.right = ''; el.style.bottom = '';

    // width / height
    if (w && w.fill) {
      if (isRow && !abs) { el.style.flexGrow = String(w.fill); el.style.flexBasis = '0'; el.style.minWidth = '0'; el.style.width = ''; }
      else el.style.width = '100%';
    } else el.style.width = w ? w.css : '';

    if (h && h.fill) {
      if (isCol && !abs) { el.style.flexGrow = String(h.fill); el.style.flexBasis = '0'; el.style.minHeight = '0'; el.style.height = ''; }
      else el.style.height = '100%';
    } else el.style.height = h ? h.css : '';

    var ha = s.horizontalAlign, va = s.verticalAlign;
    var shift = [];

    if (abs) {
      if (ha === 'center') { el.style.left = '50%'; shift.push('translateX(-50%)'); }
      else if (ha === 'right') el.style.right = '0';
      else el.style.left = '0';
      if (va === 'center') { el.style.top = '50%'; shift.push('translateY(-50%)'); }
      else if (va === 'bottom') el.style.bottom = '0';
      else el.style.top = '0';
    } else if (stacking) {
      el.style.gridArea = '1 / 1';
      // width:100% + horizontal margins. In CSS that is 100% of the parent PLUS the margins, so
      // the panel hangs over the edge; Panorama's default overflow is `squish` (the real CS2
      // stylesheets say so by having to write `overflow: noclip` 110 times to opt out), which
      // compresses it back inside. `stretch` reproduces that: the panel fills what is left after
      // its own margins. Without this the demo bar's round-marker strip -- authored 100% wide
      // with 0px 10px margins, straight out of huddemocontroller.css -- ran 10px off the screen
      // and its last segment was cut.
      // Deliberately narrow: only width, and only when a horizontal margin is what pushes the
      // panel out. Stretching every 100% panel also stretched ones whose height:100% then covered
      // their siblings -- the editor's tab ribbon grew over LOOP and CONFIG and swallowed the
      // clicks. The margin case is the one the game visibly squishes.
      var sideMargin = /(^|\s)(?!0px)(\d|\.)/.test(String(s.margin || '').split(/\s+/)[1] || '')
        || parseFloat(s.marginLeft) > 0 || parseFloat(s.marginRight) > 0;
      var fills = (w && !w.fill && w.css === '100%' && sideMargin);
      el.style.justifySelf = fills ? 'stretch' : (HALIGN_GRID[ha] || 'start');
      el.style.alignSelf = VALIGN_GRID[va] || 'start';
      if (fills) el.style.width = 'auto';
    } else if (isRow) {
      el.style.alignSelf = VALIGN_FLEX[va] || 'flex-start';
    } else {
      el.style.alignSelf = HALIGN_FLEX[ha] || 'flex-start';
    }

    this._anchorShift = shift.join(' ');
    this._composeTransform();
    this._stamp();
  };

  Panel.prototype._applyFilter = function () {
    var f = this._filt || {}, parts = [];
    if (f.brightness !== undefined && f.brightness !== null) parts.push('brightness(' + f.brightness + ')');
    if (f.blur > 0.05) parts.push('blur(' + f.blur.toFixed(2) + 'px)');
    this.el.style.filter = parts.join(' ');
  };

  Panel.prototype._composeTransform = function () {
    var s = this._sty, parts = [];
    if (this._anchorShift) parts.push(this._anchorShift);
    // `x` / `y` are the components of `position`, not a second offset: whichever was written last
    // wins per axis. Stacking them moved the Customize modal's 3D preview clip (which writes both)
    // twice as far as the game does, off its frame.
    var px = '', py = '', seq = this._posSeq || {};
    if (s.position) {
      var p = String(s.position).trim().split(/\s+/);
      px = p[0] || '0px'; py = p[1] || '0px';
    }
    if (s.x && !((seq.position || 0) > (seq.x || 0))) px = s.x;
    if (s.y && !((seq.position || 0) > (seq.y || 0))) py = s.y;
    if (px || py) parts.push('translate3d(' + (px || '0px') + ', ' + (py || '0px') + ', 0)');
    if (s.transform) parts.push(s.transform);
    this.el.style.transform = parts.join(' ');
    this._stamp();
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

  // ---- classes / attributes ---------------------------------------------------

  Panel.prototype.AddClass = function (c) { this.el.classList.add('c-' + c); };
  Panel.prototype.RemoveClass = function (c) { this.el.classList.remove('c-' + c); };
  Panel.prototype.BHasClass = function (c) { return this.el.classList.contains('c-' + c); };
  Panel.prototype.SetHasClass = function (c, on) { this.el.classList.toggle('c-' + c, !!on); };
  Panel.prototype.ToggleClass = function (c) { this.el.classList.toggle('c-' + c); };

  // 'state' is the per-frame JSON the C++ side pushes before calling render(). app.js supplies the
  // owning HUD's projection immediately before each render, matching the game's separate feeds;
  // after the frame it restores the persistent shared model for the comparison/dev harness.
  Panel.prototype.GetAttributeString = function (name, def) {
    if (name === 'state' && Shim.stateJson !== null) return Shim.stateJson;
    return (name in this._attrs) ? String(this._attrs[name]) : (def === undefined ? '' : def);
  };
  Panel.prototype.SetAttributeString = function (name, v) { this._attrs[name] = v; };
  Panel.prototype.GetAttributeInt = function (n, d) { var v = parseInt(this._attrs[n], 10); return isFinite(v) ? v : d; };
  Panel.prototype.SetAttributeInt = function (n, v) { this._attrs[n] = v; };
  Panel.prototype.SetDialogVariable = function (n, v) {
    this._dialogVars[n] = v;
    this._renderText();
  };
  Panel.prototype.SetDialogVariableInt = Panel.prototype.SetDialogVariable;

  Panel.prototype.SetPanelEvent = function (name, fn) {
    this._events[name] = fn;
    this._wireEvent(name);
  };
  Panel.prototype.ClearPanelEvent = function (name) { delete this._events[name]; };

  var DOM_EVENT = {
    onactivate: 'click', onmouseover: 'mouseenter', onmouseout: 'mouseleave',
    oncontextmenu: 'contextmenu', onmousedown: 'mousedown', onmouseup: 'mouseup',
    ondblclick: 'dblclick', onfocus: 'focus', onblur: 'blur'
  };

  Panel.prototype._wireEvent = function (name) {
    var self = this;
    var dom = DOM_EVENT[name];
    if (!dom || this['_wired_' + name]) return;
    this['_wired_' + name] = true;
    this.el.addEventListener(dom, function (e) {
      if (!self._hittest || !self._enabled) return;
      if (name === 'oncontextmenu') e.preventDefault();
      if (name === 'onactivate') e.stopPropagation();
      var fn = self._events[name];
      if (fn) { try { fn(); } catch (err) { log('[panel ' + name + '] ' + err); } }
    });
  };

  Panel.prototype.SetFocus = function () { if (this.inputEl) this.inputEl.focus(); return true; };
  Panel.prototype.SetAcceptsFocus = function () {};
  Panel.prototype.ScrollToTop = function () { this.el.scrollTop = 0; };
  Panel.prototype.ScrollToBottom = function () { this.el.scrollTop = this.el.scrollHeight; };
  Panel.prototype.ScrollParentToMakePanelFit = function () {
    if (this.el.scrollIntoView) this.el.scrollIntoView({ block: 'nearest' });
  };
  Panel.prototype.GetPositionWithinWindow = function () {
    var r = this.el.getBoundingClientRect();
    var host = Shim.hostRect();
    var k = stageScale();
    return { x: (r.left - host.left) / k, y: (r.top - host.top) / k };
  };
  Panel.prototype.BLoadLayoutSnippet = function () { return false; };
  // SetImage takes a bare path, not a url(); route it through the same rewrite as the style so a
  // programmatically-set icon resolves to the extracted asset too.
  Panel.prototype.SetImage = function (src) { this._setStyle('backgroundImage', 'url("' + src + '")'); };
  // Image scaling modes. Without this the Customize modal's inventory thumbnails (512x384, drawn
  // into a 92x44 swatch with stretch-to-fit-preserve-aspect) showed only their empty top-left corner.
  var SCALING = {
    'stretch-to-fit-preserve-aspect': 'contain', 'stretch-to-cover-preserve-aspect': 'cover',
    'stretch-to-fit-x-preserve-aspect': '100% auto', 'stretch-to-fit-y-preserve-aspect': 'auto 100%',
    'stretch': '100% 100%', 'none': 'auto'
  };
  Panel.prototype.SetScaling = function (mode) {
    var size = SCALING[String(mode)];
    if (!size) return;
    this.el.style.backgroundSize = size;
    this.el.style.backgroundPosition = 'center';
    this.el.style.backgroundRepeat = 'no-repeat';
  };

  Panel.prototype._renderText = function () {
    if (!this.textEl) return;
    var t = String(this._text === null || this._text === undefined ? '' : this._text);
    // {s:name} / {d:name} dialog-variable substitution, same as Panorama's own.
    var self = this;
    t = t.replace(/\{[sdif]:([A-Za-z0-9_]+)\}/g, function (m, n) {
      return (n in self._dialogVars) ? String(self._dialogVars[n]) : m;
    });
    // Panorama Labels honour \n only when html is off; newline -> <br> keeps multi-line cards.
    this.textEl.textContent = t;
  };

  // ---- reflected properties ---------------------------------------------------

  Object.defineProperties(Panel.prototype, {
    visible: {
      get: function () { return this._visible; },
      set: function (v) {
        this._visible = !!v;
        this.el.style.display = this._visible ? (this._display || 'grid') : 'none'; this._stamp();
      }
    },
    hittest: {
      get: function () { return this._hittest; },
      // Panorama's hittest=false makes THIS panel transparent to the mouse and says nothing about
      // its children -- that is what hittestchildren is for. CSS pointer-events:none also kills
      // every descendant, so a full-screen root with hittest=false (CamEditorRoot, ConfigHudRoot,
      // the rebuilt demo bar) took the whole panel out of reach: 58 controls in the Camera Editor
      // measured dead, every one of them. The inline 'none' stays for the panel itself; the
      // pnl-nohit rule hands its direct children back their clicks (and a child that is itself
      // hittest=false keeps its own inline none, which outranks that rule).
      set: function (v) {
        this._hittest = !!v;
        this.el.style.pointerEvents = v ? 'auto' : 'none';
        this.el.classList.toggle('pnl-nohit', !v);
        this._stamp();
      }
    },
    hittestchildren: {
      get: function () { return this._hittestchildren; },
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
    html: { get: function () { return false; }, set: function () {} },
    value: {
      get: function () {
        if (this.type === 'Slider') return this._value;
        return this.inputEl ? this.inputEl.value : this._value;
      },
      set: function (v) {
        this._value = v;
        if (this.type === 'Slider' && this.inputEl) this.inputEl.value = String(v);
        else if (this.inputEl) this.inputEl.value = (v === null || v === undefined) ? '' : String(v);
      }
    },
    min: {
      get: function () { return this._min; },
      set: function (v) { this._min = v; if (this.inputEl && this.type === 'Slider') this.inputEl.min = String(v); }
    },
    max: {
      get: function () { return this._max; },
      set: function (v) { this._max = v; if (this.inputEl && this.type === 'Slider') this.inputEl.max = String(v); }
    },
    checked: {
      get: function () { return !!this._checked; },
      set: function (v) { this._checked = !!v; this.el.classList.toggle('pnl-checked', !!v); }
    },
    // The LAYOUT box, which a transform does not change -- in Panorama a scale3d() changes what is
    // drawn, not actuallayoutwidth. The bounding rect folded every ancestor transform in, so the
    // Customize modal (custWin is scaled 0.765) shrank its 3D preview clip twice. offsetWidth is
    // already in design px (the stage scale is a transform too); fractional sizes keep their rect.
    actuallayoutwidth: { get: function () { return layoutSize(this.el, 'width'); } },
    actuallayoutheight: { get: function () { return layoutSize(this.el, 'height'); } },
    contentwidth: { get: function () { return this.el.scrollWidth / stageScale(); } },
    contentheight: { get: function () { return this.el.scrollHeight / stageScale(); } },
    desiredlayoutwidth: { get: function () { return this.el.scrollWidth / stageScale(); } },
    desiredlayoutheight: { get: function () { return this.el.scrollHeight / stageScale(); } },
    actualuiscale_x: { get: function () { return UI_SCALE; } },
    actualuiscale_y: { get: function () { return UI_SCALE; } },
    actualxoffset: { get: function () { return this.el.getBoundingClientRect().left; } },
    actualyoffset: { get: function () { return this.el.getBoundingClientRect().top; } },
    scrolloffset_y: { get: function () { return this.el.scrollTop; }, set: function () {} },
    scrolloffset_x: { get: function () { return this.el.scrollLeft; }, set: function () {} },
    tooltiptext: { get: function () { return this._tip || ''; }, set: function (v) { this._tip = v; } }
  });

  // ---------------------------------------------------------------- shim driver

  var Shim = {
    timers: new Set(),
    stateJson: null,
    root: null,          // context panel for the running surface
    handlers: [],        // $.RegisterEventHandler registrations
    commandLog: [],
    onCommand: null,
    hostEl: null,

    hostRect: function () {
      return Shim.hostEl ? Shim.hostEl.getBoundingClientRect() : { left: 0, top: 0 };
    },

    fireEvent: function (name, panel, a, b) {
      for (var i = 0; i < Shim.handlers.length; i++) {
        var h = Shim.handlers[i];
        if (h.event !== name) continue;
        if (h.panel && h.panel !== panel) continue;
        try { h.fn(panel, a, b); } catch (e) { log('[' + name + '] ' + e); }
      }
    },

    reset: function (hostEl) {
      Shim.timers.forEach(function (h) { clearTimeout(h); });
      Shim.timers.clear();
      logLines.length = 0;
      panels.length = 0;
      Shim.handlers.length = 0;
      Shim.hostEl = hostEl;
      hostEl.innerHTML = '';
      var ctx = new Panel('Panel', 'MockupContext');
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

    // Chrome will not autoplay silent, video-only media -- it answers play() with
    // "video-only background media was paused to save power" and does it whether the video is at
    // 7% opacity or fully visible and on screen. The animated panel textures (linemap.webm) were
    // therefore frozen on frame 0 in the browser while they animate in game. Seeking is still
    // permitted, so the frames are advanced by hand from the mockup's own render loop. If the
    // browser does decide to play a video on its own, this leaves it alone.
    tickVideos: function (dt) {
      if (!(dt > 0)) return;
      for (var i = 0; i < panels.length; i++) {
        var v = panels[i]._videoEl;
        if (!v || !v.paused || v.readyState < 2) continue;
        var d = v.duration;
        if (!(d > 0) || !isFinite(d)) continue;
        try { v.currentTime = (v.currentTime + dt) % d; } catch (e) { /* not seekable yet */ }
      }
    },

    unknownStyleProps: function () { return unknownStyle; },
    logs: function () { return logLines; }
  };

  // ---------------------------------------------------------------- $ and APIs

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
        else if (k === 'style') p.el.setAttribute('style', p.el.getAttribute('style') + ';' + props[k]);
        else if (k === 'src') p._setStyle('backgroundImage', 'url("' + props[k] + '")');
        else try { p[k] = props[k]; } catch (e) { /* ignore */ }
      }
    }
    // The native 3D character panel: shim/preview3d.js renders the agent (and the death clip the
    // `deathanim view` command selects) where the engine would composite its scene.
    if (global.MvmPreview3D && global.MvmPreview3D.handles(type)) global.MvmPreview3D.attach(p);
    return p;
  };
  $.GetContextPanel = function () { return Shim.root; };
  $.Msg = function () {
    var s = Array.prototype.map.call(arguments, String).join(' ');
    log(s.replace(/\n$/, ''));
  };
  // Several surfaces run a self-rescheduling $.Schedule pump. Those handles are tracked so a
  // surface switch can cancel them; otherwise the previous surface keeps ticking against a panel
  // tree that has been torn down, which shows up as phantom log lines and stray exceptions.
  $.Schedule = function (delay, fn) {
    var h = setTimeout(function () {
      Shim.timers.delete(h);
      try { fn(); } catch (e) { log('[schedule] ' + e); }
    }, (delay || 0) * 1000);
    Shim.timers.add(h);
    return h;
  };
  $.CancelScheduled = function (h) { Shim.timers.delete(h); clearTimeout(h); };
  $.RegisterEventHandler = function (event, panel, fn) {
    Shim.handlers.push({ event: event, panel: panel, fn: fn });
    return Shim.handlers.length - 1;
  };
  $.RegisterForUnhandledEvent = function (event, fn) { Shim.handlers.push({ event: event, panel: null, fn: fn }); };
  $.UnregisterEventHandler = function () {};
  $.DispatchEvent = function (name, panel) {
    if (name === 'Activated' && panel && panel._events['onactivate']) {
      try { panel._events['onactivate'](); } catch (e) { log('[dispatch] ' + e); }
      return;
    }
    Shim.fireEvent(name, panel);
  };
  $.DispatchEventAsync = function (delay, name, panel) {
    setTimeout(function () { $.DispatchEvent(name, panel); }, (delay || 0) * 1000);
  };
  $.Localize = function (t) { return String(t).replace(/^#/, ''); };
  $.ConstructString = function (t) { return $.Localize(t); };
  $.LocalizeSafe = $.Localize;
  $.HasKey = function () { return false; };
  $.GetKeyBindingHelp = function () { return ''; };
  $.Language = function () { return 'english'; };
  $.DbgIsReloadingScript = function () { return false; };

  var GameInterfaceAPI = {
    ConsoleCommand: function (c) {
      Shim.commandLog.push(c);
      if (Shim.onCommand) { try { Shim.onCommand(c); } catch (e) { log('[cmd] ' + e); } }
    },
    GetSettingString: function (n) { return (global.MvmFakeCvars && n in global.MvmFakeCvars) ? String(global.MvmFakeCvars[n]) : '0'; },
    SetSettingString: function (n, v) { (global.MvmFakeCvars = global.MvmFakeCvars || {})[n] = v; },
    GetSettingBool: function (n) { return GameInterfaceAPI.GetSettingString(n) === '1'; },
    GetSettingFloat: function (n) { return parseFloat(GameInterfaceAPI.GetSettingString(n)) || 0; }
  };

  var UiToolkitAPI = {
    ShowTextTooltipOnPanel: function (panel, text) { Shim.showTooltip && Shim.showTooltip(panel, text); },
    ShowTextTooltip: function () {},
    HideTextTooltip: function () { Shim.hideTooltip && Shim.hideTooltip(); },
    ShowGenericPopupOkCancel: function (title, msg, cls, ok) {
      if (global.confirm(title + '\n\n' + msg)) { if (typeof ok === 'function') ok(); }
    },
    ShowGenericPopupOk: function (title, msg) { global.alert(title + '\n\n' + msg); },
    ShowSimpleContextMenu: function () { return null; },
    AddDenyInputFlagsToGame: function () {},
    ReleaseDenyInputFlagsToGame: function () {},
    RegisterJSCallback: function () { return 0; },
    UnregisterJSCallback: function () {}
  };

  global.$ = $;
  global.GameInterfaceAPI = GameInterfaceAPI;
  global.UiToolkitAPI = UiToolkitAPI;
  global.PanoramaShim = Shim;
  global.PanoramaPanel = Panel;
})(window);
