// MapPlayerPreviewPanel stand-in: the 3D agent the Customize Player modal and the per-player death
// animation picker show, rendered in the browser.
//
// In game both scripts create CS2's native MapPlayerPreviewPanel (a live engine scene) and drive it
// with SetPlayerModel / EquipPlayerWithItem / SetReadyForDisplay. The death picker additionally sends
// `mirv_filmmaker deathanim view <clip>`, and Panorama/DeathAnimPreview.cpp poses every preview pawn
// from the clip. This file is that pair for the browser:
//
//   * the agent is the game's own model, exported from pak01 by the preview server on first use
//     (assets/models/agents/models/<agent>/<agent>.glb, see UiMockup/lib/vrf.mjs);
//   * the clip is the game's own baked library (assets/ragdoll/ragdoll_animations.mvmanim),
//     parsed here exactly as Movie/DeathAnimations.cpp ParseLibrary reads it;
//   * the pose is DeathAnimPreview.cpp PosePawn ported line for line: SolvePlacement against the
//     idle pose, Place, pelvis drift removed, FollowDriven for every undriven bone, loop with a
//     0.8 s hold on the last frame.
//
// What it cannot be: the vanity scene's lighting, the animgraph idle, the held weapon (the gun is
// composited from the item's paint kit at runtime) and the limb clearance track
// (DeathAnimationsMath BuildClearanceTrack). With no clip the agent stands in the first frame of a
// standing clip. If the model cannot be exported (no CLI, a static server) the agent's inventory
// portrait stands in, the same picture the AGENT row shows.
(function (global) {
  'use strict';

  var THREE_URL = 'three', LOADER_URL = 'three/addons/loaders/GLTFLoader.js';
  var LOOP_HOLD = 0.8;                       // DeathAnimPreview.cpp kLoopHold
  var BODIES = 15, PELVIS = 5, THIGH_L = 0, THIGH_R = 2;
  var BODY_BONE = ['leg_upper_l', 'leg_lower_l', 'leg_upper_r', 'leg_lower_r', 'head_0', 'pelvis', 'spine_2',
    'arm_upper_l', 'arm_lower_l', 'arm_upper_r', 'arm_lower_r', 'hand_l', 'hand_r', 'ankle_l', 'ankle_r'];
  // Movie/DeathAnimations.cpp name tables, in enum order.
  var CATEGORY = ['generic', 'head', 'neck', 'torso', 'leftarm', 'rightarm', 'leftleg', 'rightleg',
    'explosion', 'moving', 'fire', 'melee', 'manual'];
  var WEAPON = ['any', 'shotgun', 'sniper', 'heavy'];
  var SIDE = ['any', 'front', 'back', 'left', 'right'];
  var STANCE = ['standing', 'crouched', 'prone', 'running'];
  // DeathAnimationsMath::Source order, as the file's explicit source field stores it.
  var SOURCE = ['MW2019', 'Classic CoD', 'Extra'];
  var PACK_FILE = 'assets/ragdoll/ragdoll_animations.mvmanim';

  var Preview3D = global.MvmPreview3D = {
    clip: '',            // `deathanim view` -- '' poses the idle
    clipStart: 0,
    status: 'idle',
    panels: []
  };

  // ---------------------------------------------------------------- quaternion helpers (xyzw)

  function qmul(a, b) {
    return [a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]];
  }
  function qconj(q) { return [-q[0], -q[1], -q[2], q[3]]; }
  function qnorm(q) {
    var l = Math.hypot(q[0], q[1], q[2], q[3]);
    return l > 1e-12 ? [q[0] / l, q[1] / l, q[2] / l, q[3] / l] : [0, 0, 0, 1];
  }
  function qrot(q, v) {
    var r = qmul(qmul(q, [v[0], v[1], v[2], 0]), qconj(q));
    return [r[0], r[1], r[2]];
  }
  // DeathAnimationsMath QuatBlend: normalized lerp along the short arc.
  function qblend(a, b, t) {
    var dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], s = dot < 0 ? -1 : 1;
    return qnorm([a[0] * (1 - t) + b[0] * s * t, a[1] * (1 - t) + b[1] * s * t,
                  a[2] * (1 - t) + b[2] * s * t, a[3] * (1 - t) + b[3] * s * t]);
  }

  // The CLI writes glTF the standard way: Source's Z-up inches become Y-up meters, a -90 degree turn
  // about X applied to every bone's world frame. Source (x, y, z) -> glTF (x, z, -y) * 0.0254.
  var INCH = 0.0254, C = [-Math.SQRT1_2, 0, 0, Math.SQRT1_2], CINV = qconj(C);
  function toGltf(p) { return [p[0] * INCH, p[2] * INCH, -p[1] * INCH]; }
  function fromGltf(p) { return [p[0] / INCH, -p[2] / INCH, p[1] / INCH]; }

  // ------------------------------------------------------- the death animation clip library

  var packsPromise = null;
  // Movie/DeathAnimations.cpp ParseLibrary, v9: header magic[8] version count bodies fps (+8
  // spare), then per clip name[32] source category frames offset side stance weapon base[45],
  // then the payload.
  function parsePack(buf, lib) {
    var dv = new DataView(buf), HEADER = 28, ENTRY = 32 + 28 + BODIES * 12;
    var magic = String.fromCharCode.apply(null, new Uint8Array(buf, 0, 8));
    if (magic !== 'MVMDANIM' || dv.getUint32(8, true) !== 9) throw new Error(PACK_FILE + ': not a v9 ragdoll animation file');
    var count = dv.getUint32(12, true), fps = dv.getFloat32(20, true);
    var table = HEADER + count * ENTRY;
    var payload = new Float32Array(buf.slice(table));
    lib.fps = fps;
    for (var i = 0; i < count; i++) {
      var e = HEADER + i * ENTRY, name = '';
      for (var k = 0; k < 32; k++) { var ch = dv.getUint8(e + k); if (!ch) break; name += String.fromCharCode(ch); }
      var base = new Float32Array(BODIES * 3);
      for (var j = 0; j < BODIES * 3; j++) base[j] = dv.getFloat32(e + 60 + j * 4, true);
      lib.clips.push({
        name: name, source: SOURCE[dv.getUint32(e + 32, true)], category: CATEGORY[dv.getUint32(e + 36, true)],
        frames: dv.getUint32(e + 40, true), offset: dv.getUint32(e + 44, true) / 4,
        side: SIDE[dv.getUint32(e + 48, true)], stance: STANCE[dv.getUint32(e + 52, true)],
        weapon: WEAPON[dv.getUint32(e + 56, true)], base: base, payload: payload
      });
    }
  }
  Preview3D.loadPacks = function () {
    if (!packsPromise) {
      var lib = { fps: 30, clips: [], byName: {} };
      packsPromise = fetch(PACK_FILE).then(function (r) {
        if (!r.ok) throw new Error(PACK_FILE + ': HTTP ' + r.status);
        return r.arrayBuffer();
      }).then(function (buf) {
        parsePack(buf, lib);
        lib.clips.forEach(function (c) { lib.byName[c.name.toLowerCase()] = c; });
        return lib;
      });
      packsPromise.catch(function (e) { Preview3D.status = 'clips: ' + e.message; packsPromise = null; });
    }
    return packsPromise;
  };
  // DeathAnimations.cpp CatalogScript: [name, source, stance, area, side, weapon, frames] per clip.
  Preview3D.catalogRows = function (lib) {
    return lib.clips.map(function (c) { return [c.name, c.source, c.stance, c.category, c.side, c.weapon, c.frames]; });
  };

  // DeathAnimationsMath Sample: base + per-frame displacement, orientation blended between frames.
  function sample(lib, clip, seconds, body) {
    var f = seconds * lib.fps;
    if (!(f > 0)) f = 0;
    if (f > clip.frames - 1) f = clip.frames - 1;
    var a = Math.floor(f), b = a + 1 < clip.frames ? a + 1 : a, t = f - a;
    var pa = clip.offset + (a * BODIES + body) * 7, pb = clip.offset + (b * BODIES + body) * 7, P = clip.payload;
    var p = [0, 1, 2].map(function (k) { return clip.base[body * 3 + k] + P[pa + k] * (1 - t) + P[pb + k] * t; });
    return { p: p, q: qblend([P[pa + 3], P[pa + 4], P[pa + 5], P[pa + 6]], [P[pb + 3], P[pb + 4], P[pb + 5], P[pb + 6]], t) };
  }
  function duration(lib, clip) { return clip.frames > 1 ? (clip.frames - 1) / lib.fps : 0; }

  // DeathAnimationsMath SolvePlacement / Place.
  function fitWeight(i) { return (i === 7 || i === 9) ? 0.35 : (i === 8 || i === 10) ? 0.15 : (i === 11 || i === 12) ? 0.10 : 1.0; }
  function facingFromHips(a, ai, b, bi) {
    var lx = a[ai] - b[bi], ly = a[ai + 1] - b[bi + 1], l = Math.hypot(lx, ly);
    return l > 1e-3 ? [ly / l, -lx / l] : null;
  }
  function solvePlacement(clipBase, death) {
    var cc = [0, 0], pc = [0, 0], wsum = 0, i;
    for (i = 0; i < BODIES; i++) {
      var w = fitWeight(i); wsum += w;
      cc[0] += clipBase[i * 3] * w; cc[1] += clipBase[i * 3 + 1] * w;
      pc[0] += death[i * 3] * w; pc[1] += death[i * 3 + 1] * w;
    }
    cc[0] /= wsum; cc[1] /= wsum; pc[0] /= wsum; pc[1] /= wsum;
    var num = 0, den = 0;
    for (i = 0; i < BODIES; i++) {
      var ww = fitWeight(i), cx = clipBase[i * 3] - cc[0], cy = clipBase[i * 3 + 1] - cc[1];
      var px = death[i * 3] - pc[0], py = death[i * 3 + 1] - pc[1];
      num += (cx * py - cy * px) * ww; den += (cx * px + cy * py) * ww;
    }
    var yaw = 0, cf = facingFromHips(clipBase, THIGH_L * 3, clipBase, THIGH_R * 3), df = facingFromHips(death, THIGH_L * 3, death, THIGH_R * 3);
    if (cf && df) yaw = Math.atan2(df[1], df[0]) - Math.atan2(cf[1], cf[0]);
    else if (Math.hypot(num, den) > 1e-3) yaw = Math.atan2(num, den);
    var c = Math.cos(yaw), s = Math.sin(yaw), cp = PELVIS * 3;
    return { c: c, s: s, yaw: yaw, off: [death[cp] - (clipBase[cp] * c - clipBase[cp + 1] * s),
      death[cp + 1] - (clipBase[cp] * s + clipBase[cp + 1] * c), 0] };
  }
  function place(pl, pose) {
    var p = pose.p;
    return { p: [p[0] * pl.c - p[1] * pl.s + pl.off[0], p[0] * pl.s + p[1] * pl.c + pl.off[1], p[2] + pl.off[2]],
             q: qnorm(qmul([0, 0, Math.sin(pl.yaw / 2), Math.cos(pl.yaw / 2)], pose.q)) };
  }

  // ---------------------------------------------------------------- three.js

  var threePromise = null;
  function loadThree() {
    if (!threePromise) {
      threePromise = Promise.all([import(THREE_URL), import(LOADER_URL)]).then(function (m) {
        return { THREE: m[0], GLTFLoader: m[1].GLTFLoader };
      });
      threePromise.catch(function (e) { Preview3D.status = 'three.js: ' + e.message; });
    }
    return threePromise;
  }
  var modelCache = {};
  function modelUrl(model) {
    var m = /^(agents\/models\/[A-Za-z0-9_\/]+)\.vmdl$/.exec(String(model || '').replace(/\\/g, '/'));
    return m ? 'assets/models/' + m[1] + '.glb' : '';
  }
  function portraitUrl(model) {
    var m = /([A-Za-z0-9_]+)\.vmdl$/.exec(String(model || ''));
    return m ? 'assets/panorama/images/econ/characters/customplayer_' + m[1] + '_png.png' : '';
  }
  function loadModel(three, url) {
    if (!modelCache[url]) {
      modelCache[url] = new Promise(function (resolve, reject) {
        new three.GLTFLoader().load(url, function (g) { resolve(g); }, undefined, reject);
      });
      modelCache[url].catch(function () { delete modelCache[url]; });
    }
    return modelCache[url];
  }

  // The skeleton in parent-before-child order with Source-space idle world poses, which is the shape
  // DeathAnimPreview.cpp's Skeleton + PawnState hold.
  function buildRig(THREE, root) {
    var bones = [], seen = new Set();
    root.traverse(function (o) { if (o.isBone && !seen.has(o)) { seen.add(o); bones.push(o); } });
    root.updateMatrixWorld(true);
    var inv = new THREE.Matrix4().copy(root.matrixWorld).invert();
    var index = new Map(); bones.forEach(function (b, i) { index.set(b, i); });
    var rig = { bones: bones, parents: [], driven: [], rest: [], bodyBone: [], weapon: [] };
    var m = new THREE.Matrix4(), pos = new THREE.Vector3(), q = new THREE.Quaternion(), scl = new THREE.Vector3();
    bones.forEach(function (b, i) {
      rig.parents[i] = b.parent && index.has(b.parent) ? index.get(b.parent) : -1;
      // Case-insensitive, first match wins: DeathAnimPreview.cpp NameEquals (the skeleton spells
      // arm_upper_L, the pack's table arm_upper_l).
      var body = BODY_BONE.indexOf(b.name.toLowerCase());
      rig.driven[i] = body >= 0 && rig.bodyBone[body] == null ? body : -1;
      if (rig.driven[i] >= 0) rig.bodyBone[body] = i;
      rig.weapon[i] = /^weapon/i.test(b.name);
      m.multiplyMatrices(inv, b.matrixWorld).decompose(pos, q, scl);
      rig.rest[i] = { p: fromGltf([pos.x, pos.y, pos.z]), q: qnorm(qmul(CINV, [q.x, q.y, q.z, q.w])) };
    });
    for (var k = 0; k < BODIES; k++) if (rig.bodyBone[k] == null) return null;
    return rig;
  }

  // DeathAnimPreview.cpp PosePawn, then the world -> parent-local write into the three.js bones.
  function poseRig(THREE, rig, lib, clip, seconds) {
    var restBodies = new Float32Array(BODIES * 3), b, k;
    for (b = 0; b < BODIES; b++) for (k = 0; k < 3; k++) restBodies[b * 3 + k] = rig.rest[rig.bodyBone[b]].p[k];
    var pl = solvePlacement(clip.base, restBodies);
    var bodies = [];
    for (b = 0; b < BODIES; b++) bodies[b] = place(pl, sample(lib, clip, seconds, b));
    var start = place(pl, { p: [clip.base[PELVIS * 3], clip.base[PELVIS * 3 + 1], clip.base[PELVIS * 3 + 2]], q: [0, 0, 0, 1] });
    var dx = bodies[PELVIS].p[0] - start.p[0], dy = bodies[PELVIS].p[1] - start.p[1];
    for (b = 0; b < BODIES; b++) { bodies[b].p[0] -= dx; bodies[b].p[1] -= dy; }
    // DeathAnimationsMath FollowDriven.
    var world = [];
    for (b = 0; b < rig.bones.length; b++) {
      if (rig.driven[b] >= 0) { world[b] = bodies[rig.driven[b]]; continue; }
      var a = rig.parents[b];
      while (a >= 0 && rig.driven[a] < 0) a = rig.parents[a];
      if (a < 0) { world[b] = rig.rest[b]; continue; }
      var delta = qnorm(qmul(world[a].q, qconj(rig.rest[a].q)));
      var rel = [rig.rest[b].p[0] - rig.rest[a].p[0], rig.rest[b].p[1] - rig.rest[a].p[1], rig.rest[b].p[2] - rig.rest[a].p[2]];
      var rot = qrot(delta, rel);
      world[b] = { p: [world[a].p[0] + rot[0], world[a].p[1] + rot[1], world[a].p[2] + rot[2]], q: qnorm(qmul(delta, rig.rest[b].q)) };
    }
    for (b = 0; b < rig.bones.length; b++) {
      var wg = { p: toGltf(world[b].p), q: qmul(C, world[b].q) }, local;
      var par = rig.parents[b];
      if (par < 0) {
        local = wg;
      } else {
        var pg = { p: toGltf(world[par].p), q: qmul(C, world[par].q) }, pinv = qconj(pg.q);
        local = { p: qrot(pinv, [wg.p[0] - pg.p[0], wg.p[1] - pg.p[1], wg.p[2] - pg.p[2]]), q: qnorm(qmul(pinv, wg.q)) };
      }
      // No gun in a death: the C++ sinks the weapon bones under the floor. Nothing is bone-merged to
      // them here, so leaving them is equivalent.
      var bone = rig.bones[b];
      if (par < 0) continue;           // the root keeps its glTF transform
      bone.position.set(local.p[0], local.p[1], local.p[2]);
      bone.quaternion.set(local.q[0], local.q[1], local.q[2], local.q[3]);
    }
  }

  // ---------------------------------------------------------------- the panel

  function attach(panel) {
    var el = panel.el;
    var host = document.createElement('div');
    host.className = 'mvm-preview3d';
    el.appendChild(host);
    var view = { panel: panel, host: host, model: '', key: '', state: 'waiting', renderer: null, rig: null, frame: 0 };
    Preview3D.panels.push(view);

    panel.SetPlayerModel = function (model) { setModel(view, model); };
    panel.SetPlayerCharacterItemID = function () {};
    panel.SetActiveCharacter = function () {};
    panel.TransitionToCamera = function () {};
    panel.EquipPlayerWithItem = function () {};
    panel.SetReadyForDisplay = function () {};
    panel.SetItemItemId = function () {};
    panel.SetActiveItem = function () {};
    panel.StartWeaponLookat = function () {};
    panel.SetHideStaticGeometry = function () {};
    // The scripts create it with `playermodel` among the creation attributes.
    setModel(view, panel.playermodel || '');
    return view;
  }

  function showPortrait(view, why) {
    view.state = 'portrait (' + why + ')';
    disposeRenderer(view);
    var url = portraitUrl(view.model);
    view.host.innerHTML = '';
    if (!url) return;
    var img = document.createElement('img');
    img.className = 'mvm-preview3d-portrait';
    img.src = url;
    img.onerror = function () { img.remove(); };
    view.host.appendChild(img);
  }

  function disposeRenderer(view) {
    if (view.raf) cancelAnimationFrame(view.raf);
    view.raf = 0;
    if (view.renderer) { view.renderer.dispose(); view.renderer.forceContextLoss && view.renderer.forceContextLoss(); }
    view.renderer = null; view.rig = null;
  }

  function setModel(view, model) {
    model = String(model || '');
    if (model === view.model && view.state !== 'waiting') return;
    view.model = model;
    var key = view.key = model + '#' + Math.random();
    var url = modelUrl(model);
    if (!url) { showPortrait(view, 'not an agent model'); return; }
    view.state = 'loading';
    Promise.all([loadThree(), Preview3D.loadPacks()]).then(function (r) {
      var three = r[0], lib = r[1];
      return loadModel(three, url).then(function (gltf) {
        if (view.key !== key) return;
        build(view, three.THREE, gltf, lib);
      });
    }).catch(function (e) {
      if (view.key !== key) return;
      showPortrait(view, e && e.message ? e.message : 'model unavailable');
    });
  }

  // First frame of the first standing, generic MW2019 clip: a natural standing pose to show while
  // no clip plays (the vanity scene's animgraph idle is not reproducible here).
  function idleClip(lib) {
    for (var i = 0; i < lib.clips.length; i++) {
      var c = lib.clips[i];
      if (c.source === 'MW2019' && c.stance === 'standing' && c.category === 'torso') return c;
    }
    return lib.clips[0];
  }

  function build(view, THREE, gltf, lib) {
    disposeRenderer(view);
    view.host.innerHTML = '';
    // One clone per panel: the pose is written into the bones every frame.
    var root = cloneSkinned(THREE, gltf.scene);
    root.traverse(function (o) {
      if (o.isMesh && /firstperson|defusekit/i.test(o.name)) o.visible = false;
      if (o.isMesh) o.frustumCulled = false;
    });
    var rig = buildRig(THREE, root);
    if (!rig) { showPortrait(view, 'ragdoll bones missing'); return; }

    var renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true, preserveDrawingBuffer: true });
    renderer.setClearColor(0x000000, 0);
    renderer.outputColorSpace = THREE.SRGBColorSpace;
    renderer.toneMapping = THREE.ACESFilmicToneMapping;
    renderer.toneMappingExposure = 1.05;
    view.host.appendChild(renderer.domElement);

    var scene = new THREE.Scene();
    scene.add(new THREE.HemisphereLight(0xdde6f5, 0x1c1a18, 1.15));
    var key = new THREE.DirectionalLight(0xfff1de, 2.4); key.position.set(1.6, 2.6, 2.4); scene.add(key);
    var rim = new THREE.DirectionalLight(0xbcd2ff, 1.6); rim.position.set(-2.2, 2.0, -2.6); scene.add(rim);
    var fill = new THREE.DirectionalLight(0xffffff, 0.45); fill.position.set(-1.5, 0.6, 2.0); scene.add(fill);

    // The exported agent faces glTF +Z, i.e. the camera; turned a little off-axis the way the
    // loadout camera sees it.
    var turn = new THREE.Group();
    turn.rotation.y = 0.2;
    turn.add(root);
    scene.add(turn);

    // cam_loadoutmenu_ct is a full-body camera; pin-fov is vertical. The script then scales the panel
    // by PREVIEW_FIT_SCALE and offsets it (CameraEditorCustomizePreviewJs.h), as it does in game.
    // Framing measured off game captures of both pickers (2026-10-04, 2560x1351): unscaled, the floor
    // sits at 85% of the box height and a standing agent's head at 10%, centred. With Customize's
    // 1.3 scale and +31px offset that puts the head near the top and the feet on the bottom edge,
    // as in game; the death picker (unscaled) shows the body lying small near the foot of the box.
    var camera = new THREE.PerspectiveCamera(24, 1, 0.1, 50);
    camera.position.set(0, 0.84, 5.64);
    camera.lookAt(0, 0.84, 0);

    view.renderer = renderer; view.rig = rig; view.state = 'live';
    var idle = idleClip(lib);
    function frame() {
      view.raf = requestAnimationFrame(frame);
      draw();
    }
    // `at`: seconds into the clip, for the test hook (rAF does not run in a hidden tab).
    function draw(at) {
      if (!view.host.isConnected) { disposeRenderer(view); return; }
      var w = view.host.clientWidth, h = view.host.clientHeight;
      if (w < 2 || h < 2) return;
      var dpr = Math.min(2, (global.devicePixelRatio || 1) * Math.max(1, (global.MvmMockup && global.MvmMockup.stageScale) || 1));
      if (view.w !== w || view.h !== h || view.dpr !== dpr) {
        view.w = w; view.h = h; view.dpr = dpr;
        renderer.setPixelRatio(dpr);
        renderer.setSize(w, h, false);
        camera.aspect = w / h; camera.updateProjectionMatrix();
      }
      var clip = Preview3D.clip && lib.byName[Preview3D.clip.toLowerCase()];
      if (clip) {
        var cycle = duration(lib, clip) + LOOP_HOLD;
        var t = at != null ? at : (performance.now() - Preview3D.clipStart) / 1000;
        if (cycle > 0) t = t - cycle * Math.floor(t / cycle);
        poseRig(THREE, rig, lib, clip, t);
      } else if (idle) {
        poseRig(THREE, rig, lib, idle, 0);
      }
      renderer.render(scene, camera);
    }
    view.draw = draw;
    frame();
  }

  // SkeletonUtils.clone without the extra import: copy the graph, then rebind each skinned mesh to the
  // copied bones by name.
  function cloneSkinned(THREE, source) {
    var clone = source.clone(true), byName = {};
    clone.traverse(function (o) { if (o.isBone) byName[o.name] = o; });
    var srcMeshes = [], dstMeshes = [];
    source.traverse(function (o) { if (o.isSkinnedMesh) srcMeshes.push(o); });
    clone.traverse(function (o) { if (o.isSkinnedMesh) dstMeshes.push(o); });
    dstMeshes.forEach(function (m, i) {
      var sk = srcMeshes[i].skeleton;
      var bones = sk.bones.map(function (b) { return byName[b.name]; });
      m.bind(new THREE.Skeleton(bones, sk.boneInverses), srcMeshes[i].bindMatrix);
    });
    return clone;
  }

  // ---------------------------------------------------------------- the engine side

  // `mirv_filmmaker deathanim view <clip|off>` (DeathAnimPreview_Set): every preview pawn plays it.
  Preview3D.setClip = function (name) {
    var on = name && String(name).toLowerCase() !== 'off';
    Preview3D.clip = on ? String(name) : '';
    Preview3D.clipStart = performance.now();
    Preview3D.status = on ? 'playing ' + name : 'idle';
  };
  Preview3D.handles = function (type) { return type === 'MapPlayerPreviewPanel'; };
  Preview3D.attach = attach;
  // Test hook: draw every live preview at `seconds` into the current clip.
  Preview3D.drawAt = function (seconds) {
    Preview3D.panels.forEach(function (v) { if (v.draw && v.host.isConnected) v.draw(seconds); });
  };
  Preview3D.debug = function () {
    return Preview3D.panels.filter(function (v) { return v.host.isConnected; })
      .map(function (v) { return v.model + ' ' + v.state; }).concat(['clip=' + (Preview3D.clip || '-'), Preview3D.status]);
  };
})(window);
