// The fake per-frame state feed.
//
// In game, C++ serialises this object and pushes it onto the root panel before calling render().
// Here the mockup owns it instead, so every panel populates exactly as it would mid-demo. Field
// names are the real contract -- they were lifted from the `st.*` reads across all the extracted
// scripts and from the C++ builders (ConfigHud::BuildStateJson, CameraTimelineHud::BuildStateJson,
// CameraEditorHud::BuildStateJson) -- so a rename on the C++ side shows up here as a dead control
// rather than as a silently wrong mockup.

(function (global) {
  'use strict';

  var TICK_RATE = 64;

  // The five camera keys every surface shares. Declared up front because the graph editor needs
  // the SAME keys expressed as per-channel curves, and two independent copies would drift.
  var MARKERS = [
    { tick: 41900, x: -410.2, y: 1200.5, z: 128.0, pitch: 4.5, yaw: 91.2, roll: 0, fov: 90, ease: 2, speedMul: 1.0 },
    { tick: 42250, x: -260.8, y: 1310.1, z: 141.5, pitch: 2.0, yaw: 74.6, roll: -1.5, fov: 86, ease: 2, speedMul: 1.0 },
    { tick: 42600, x: -95.4, y: 1402.7, z: 155.2, pitch: -1.2, yaw: 58.9, roll: -2.0, fov: 82, ease: 2, speedMul: 1.0 },
    { tick: 43100, x: 140.6, y: 1488.3, z: 168.9, pitch: -3.4, yaw: 41.0, roll: 0, fov: 78, ease: 3, speedMul: 1.25 },
    { tick: 43700, x: 402.1, y: 1550.0, z: 172.4, pitch: -2.1, yaw: 25.7, roll: 0, fov: 75, ease: 1, speedMul: 1.0 }
  ];

  // Graph-editor model: one entry per curve, each carrying its own value range and its keys as
  // {id, t, v, sel}. Built from MARKERS so the graph and the timeline always agree.
  // Channel ids are NUMERIC (0..6) — GraphEditorJs looks them up with chById(c), matching C++.
  var CHANNEL_NAMES = ['Pos X', 'Pos Y', 'Pos Z', 'Pitch', 'Yaw', 'Roll', 'FOV'];
  var CHANNEL_COLORS = ['#ff8885', '#80d680', '#69a7ff', '#fd96d7', '#c89cff', '#25c8ff', '#ff9a5a'];
  var CHANNEL_KEYS = ['x', 'y', 'z', 'pitch', 'yaw', 'roll', 'fov'];
  var CHANNEL_RANGE = {
    x: [-600, 600], y: [1100, 1650], z: [80, 220],
    pitch: [-45, 45], yaw: [0, 180], roll: [-30, 30], fov: [40, 120]
  };
  function makeChannels(selectedIndex, markers) {
    markers = markers || MARKERS;
    return CHANNEL_KEYS.map(function (name, idx) {
      return {
        ch: idx, name: CHANNEL_NAMES[idx], color: CHANNEL_COLORS[idx],
        vis: true, solo: false, edit: true,
        min: CHANNEL_RANGE[name][0], max: CHANNEL_RANGE[name][1],
        keys: markers.map(function (m, i) {
          return { id: i, t: m.tick, v: m[name], sel: i === selectedIndex };
        })
      };
    });
  }

  var SCENE_CATALOG = global.MvmSceneCatalog;
  var SCENE_BASELINE = global.MvmSceneKeys.baseline();
  function rebuildSceneDerived(sk) { global.MvmSceneKeys.sync(sk, sk.tick || 0); }
  function syncSceneResolved(sk,tick) { sk.tick=tick;global.MvmSceneKeys.sync(sk,tick); }
  function makeSceneKeys() { return global.MvmSceneKeys.create(); }

  function clone(o) { return JSON.parse(JSON.stringify(o)); }

  function makeState() {
    var st = {
      // ---- navigation truth ---------------------------------------------------
      // The HUD drivers do not share one feed: ConfigHud, CameraEditorHud, GraphExp and
      // FxDebugHud each publish their own `enabled`, MovieHud and CameraTimeline both have a
      // `visible`/`open`, MovieHud and MarkerHud both have a `mode`. One flat model cannot hold
      // both meanings of a key, so the colliding ones live under the owner they belong to and
      // app.js projects them back out per script (stateFor). Everything else stays flat because
      // it means the same thing to every reader.
      //
      // `nav` is what the engine keeps in the HUD objects' m_enabled flags plus Render_IsModalOpen
      // and HudBlackout_Active: which workspace is up. The mockup boots CLOSED, like the game
      // after a demo loads.
      nav: {
        context: 'ingame',      // ingame | menu-demos | menu-video (which Panorama context is up)
        configOpen: false,      // ConfigHud::m_enabled
        editorOpen: false,      // CameraEditorHud::m_enabled
        renderOpen: false,      // Render_IsModalOpen (a modal ON the editor)
        customizeOpen: false,   // the Customize Player modal was asked for
        blackout: false         // HudBlackout_Active (F8)
      },
      // Movie/TimelineDock.cpp: the tab is remembered across closes; host/hosted/visible are what
      // Claim/Release/Apply write into CameraTimelineHud and GraphEditorExperimentHud.
      dock: {
        tab: 'native',          // native | timeline  (TimelineDock g_tab)
        host: 'none',           // none | editor | config   (TimelineDock g_host)
        hosted: false,          // CameraTimelineHud::m_editorHosted
        tlVisible: false,       // CameraTimelineHud::m_visible (the docked camera timeline)
        graphEnabled: false     // GraphEditorExperimentHud::m_enabled
      },
      // MovieHud: the F8-era director card. Visible by default (MovieHud.h), hidden by the
      // workspaces while they are open and restored on exit.
      movie: { visible: true, mode: 'First person', prev: null },
      // MarkerHud: editing | reposition | previewArmed | previewPlaying, plus its card and notice.
      marker: { mode: 'editing', menuOpen: true, notice: '' },
      // AimCrosshairHud. Off at rest, like the game; values are the C++ defaults
      // (AimCrosshairHud.h: size 7, gap 4, thickness 2, rgb 0,255,120, dot on, centre off).
      aim: { on: false, valid: true, front: true, sx: 0.5, sy: 0.5, size: 7, gap: 4, thick: 2,
             col: 'rgba(0,255,120,1)', dot: true, centre: false, view: 0, debugbg: false },
      // FxDebugHud: `mirv_filmmaker fx debughud on` -- a diagnostic, off until asked for.
      fxdbg: { enabled: false, litMuzzle: 0, litTracer: 0, litOnSmoke: 0, litModSmoke: 0 },
      // EasterEggs_BuildOverlayJson: modes, the cursor feed, and one-shot event ids.
      eggs: { hidden: false, egirl: false, cx: 0.5, cy: 0.5, cvis: true, click: 0,
              hint: { id: 0 }, vegas: 0, clippy: { id: 0, show: false },
              disco: false, bay: 0, wes: { on: false, chapter: 0, title: '' } },
      // Onboarding.cpp's OnboardingState: off until `mvm_onboarding` opens it.
      onboarding: { on: false, closed: false, voice: true, route: '', step: 0, rev: 0, px: -1, py: -1 },

      // ---- session / context ------------------------------------------------
      demo: true,              // demo playback (not a live mvm_test map)
      cursor: true,            // UI owns the mouse
      cursorForced: false,
      povReady: true,
      mvmDebug: false,
      hudView: 'full',
      hudScale: 1.0,
      // The inspector column's width, as InspectorWidth.h publishes it every frame. The panels
      // each keep a live copy for the drag and sync back to this number, so it has to be in the
      // feed from the first frame or they disagree about where the column's edge is.
      inspW: 640,
      winW: 1920, winH: 1080,
      previewHudHidden: false,
      previewRect: { x: 0, y: 0, w: 1420, h: 800 },

      // ---- mouse pipe (drives shift-drag on every slider) --------------------
      mx: 0, my: 0, lmb: false, rmb: false, shift: false, mseq: 0,

      // ---- demo clock -------------------------------------------------------
      tick: 42350, tickMin: 0, tickMax: 115200,
      demoStartTick: 0, demoEndTick: 115200,
      playing: false, scrubbing: false, scrubTick: 42350,
      timescale: 1.0, time: 661.7,
      // constSpeed is a MULTIPLIER the marker card formats with toFixed(1), not a flag.
      speedMode: 'Manual', speedMul: 1.0, constSpeed: 1.0, autoSnap: true,
      interp: true, interpDegraded: false,

      // ---- playback loop ----------------------------------------------------
      loopOn: true, loopReady: true, loopIn: 41800, loopOut: 43900,

      // ---- camera path / timeline ------------------------------------------
      pathLive: false,
      graphDrive: false,
      canUndo: true, canRedo: false,
      // `count` is the camera-key total the timeline header and the marker card both read;
      // `selCount` is the same number under the name the inspectors use.
      count: 5, selected: 2, selCount: 5,
      selEase: 2, selSpeedMul: 1.0, selIsLast: false,
      // The key editor reads the selected key's full transform off `sel` (x/y/z + pitch/yaw/roll),
      // not just its tick and lens, so mirror the whole marker rather than a subset.
      sel: Object.assign({}, MARKERS[2], { isLast: false }),
      markers: clone(MARKERS),
      // Every camera key's tick, in order: the Path rail lists the cameras with their ticks.
      keyTicks: MARKERS.map(function (m) { return m.tick; }),
      // Per-keyframe shake is an ARRAY of {i, s} (marker index + style key), not a map -- the
      // timeline builds its "has a shake" dots by mapping over it.
      shakes: [{ i: 3, s: 'handheld' }],

      // Live camera readout on the Movie HUD director card.
      ox: -95.4, oy: 1402.7, oz: 155.2, pitch: -1.2, yaw: 58.9, roll: -2.0, fov: 82,
      // CameraPath::TimingName(): 'Live' | 'Freeze' (the timeline's Live / Freeze pair).
      timing: 'Live',

      // ---- follow / attach --------------------------------------------------
      follow: { enabled: false, type: 'player' },
      obsTarget: 'donk',

      // ---- surface visibility gates ----------------------------------------
      // captureHide blanks every Source MVM panel while a render job is capturing, so it must be
      // false here or the editor, marker card and timeline all build and then vanish. The
      // per-owner gates (dock open, movie card visible, marker mode) live under nav/dock/movie/
      // marker above.
      captureHide: false,
      hudHidden: false,
      editorPlay: false,

      // ---- render -----------------------------------------------------------
      // render.out is the OUTPUT SETTINGS OBJECT the workspace reads field by field (fps, codec,
      // resolution, folder...), not a path string.
      render: {
        // `avail` is what each range option is allowed to offer, not a boolean; `resOk` plus
        // resStart/resEnd are what make "Full Demo" and "Current Round" selectable instead of
        // reading "Demo bounds unavailable".
        avail: { pathKeys: 5, pathSel: 2, followReady: true },
        resOk: true, resStart: 41800, resEnd: 43900,
        // Indices, as RenderCommand.cpp Render_BuildStateJson writes them: rangeMode is
        // RenderRangeMode, cameraSource RenderCameraSource, extension CameraPathExtension.
        rangeMode: 4, tab: 0,
        startTick: 41800, endTickExclusive: 43900, demoStart: 0, demoEnd: 115200,
        cameraSource: 4, extension: 0,
        sessionState: 0, prog: {}, warnCount: 0, fatalCount: 0, firstFatal: '',
        captureHide: false, jobActive: false, open: false,
        pass: {
          hud: 0, cameraData: true, blood: true, key: [0, 255, 0], depthNear: 8, depthFar: 4096, preview: 'all',
          list: [{ type: 'beauty', elements: 'all', stream: 'beauty' }], rendersPerFrame: 1
        },
        out: {
          folder: 'C:/Users/ExampleUser/Videos/SourceMVM', folderPending: false,
          project: 'match730_de_mirage_r12',
          format: 'mp4', formatName: 'MP4 (H.264)', formatOk: true, formatNote: '',
          // encoder RenderEncoder (0 CPU, 1 GPU, 2 Auto), speed RenderEncodeSpeed, nvenc the
          // RenderEncoderDetect state (2 = proven working, 3 = unavailable).
          encoder: 2, nvenc: 2, nvencNote: 'NVIDIA NVENC available',
          fps: 60, captureFps: 60, speed: 0, timescale: 1,
          width: 1920, height: 1080,
          crf: 18, bitrateKbps: 0,
          length: 32.8, drops: 0, join: true, push: false
        }
      },
      renderJobActive: false,
      renderRangeActive: true, renderRangeMode: 'loop',
      renderInTick: 41800, renderOutTickExclusive: 43900,

      // ---- Smooth View ------------------------------------------------------
      svOn: true,
      svBreathX: 0.24, svBreathY: 0.17, svStandAmp: 0.35, svWalkAmp: 0.6, svRunAmp: 0.9,
      svTiltEnabled: true, svTilt: 40, svTiltSpeed: 6.0, svTiltInv: false,
      svFalling: true, svJumping: true, svHealthSway: false,
      smoothViewDefault: false,
      fisheye: false,
      swayPct: 45, deadzonePct: 30, dzXhair: false, floatTight: 55, floatAmt: 40,

      // ---- Handheld ---------------------------------------------------------
      handheldOn: true, handheldDefault: false,
      hhSpeed: 55, hhIdle: 40, hhWalk: 65, hhRun: 85, hhDrift: 30, hhDriftDir: 0,
      hhPeriod: 2.4, hhVariance: 35, hhTilt: 25, hhCurve: 2, hhRegrip: 18,
      hhCenterLock: 40, hhCenterLockOn: true, hhVmDecoupled: true,

      // ---- Weapon Recoil ----------------------------------------------------
      wrOn: true, weaponRecoilDefault: false,
      wrAmount: 62, wrPunch: 48, wrRecovery: 55, wrSnap: 35,
      wrRot: 40, wrCamRot: 30, wrCamPos: 25, wrCamRoll: 15,
      wrPivot: 'grip', wrPivotOrb: false, wrStyle: 'style1',
      wrScope: 'type', wrScopeKey: 'rifle', wrScopeOwn: true, wrScopeCovers: true,
      wrLive: 'weapon_ak47', wrLiveSlot: 'primary', wrLiveType: 'rifle',
      wrOverrides: [
        { scope: 'weapon', key: 'weapon_ak47' },
        { scope: 'type', key: 'sniper' },
        { scope: 'slot', key: 'secondary' }
      ],

      // ---- Particle FX ------------------------------------------------------
      fxOn: true, fxReady: true, fxMoneyshot: false, fxMollyLights: false, fxHeLights: false, fxFlashLights: false,
      fx_impacts: 'on', fx_tracers: 'modern', fx_weaponfx: 'modern',
      fx_explosions: 'less', fx_bombfx: 'on', fx_molotov: 'on',
      fxKeep: true, keepCamera: true, keepRagdolls: false,
      camFovOn: true, camFov: 100,
      vmPerPlayer: true, vmFov: 68, vmX: 0, vmY: 0, vmZ: 0, vmLeftHanded: false,
      viewmodelDefault: true,

      // ---- Ragdolls ---------------------------------------------------------
      ragdollOn: true,
      rp_gravity: 100, rp_friction: 55, rp_collide: true, rp_limit: 12, rp_minage: 2,

      // ---- Lighting ---------------------------------------------------------
      exposure: -1,            // -1 = engine auto-exposure
      lightOk: true, lightUniform: true, lightRotationOk: true,
      lightR: 255, lightG: 240, lightB: 214, lightAmount: 65,
      engineLights: true, bakedLighting: true, bakedLightingOk: true,
      shadows: true, shadowsOk: true, bakedShadows: true, dynamicShadows: true,
      shaderShadows: false, shaderShadowSteps: 16, shaderShadowStepPixels: 4,
      shaderShadowStrength: 70, shaderShadowThickness: 30, customShadowStub: false,
      mod_world: '#8fa4b8', mod_world_on: false, mod_strength: 45,

      // ---- Atmosphere -------------------------------------------------------
      smokeT: 'default', smokeCT: 'default',
      smokeAppearance: false, smokeAppearanceReady: true,
      smoke_density: 0.85, smoke_wispy: 0, smoke_noise: 0.8, smoke_speed: 1.5,
      smoke_scale_low: 3, smoke_scale_high: 7, smoke_strength_low: 0.5, smoke_strength_high: 0.35,
      smoke_boil: -0.3, smoke_ray_step: 6,
      smoke_spread: 1, smoke_resolution: 32, smoke_voxel_size: 20, smoke_voxel_count: 32768,

      // ---- Weather & Sky ----------------------------------------------------
      skyCurrent: 'sky_dust2_02',
      skyList: [
        { name: 'sky_dust2_02', label: 'Dust II Day', image: '', rotation: 0, flipped: false, editable: false, builtin: true },
        { name: 'sky_cs15_daylight02_hdr', label: 'Daylight Clear', image: '', rotation: 0, flipped: false, editable: false, builtin: true },
        { name: 'sky_vertigoblue_hdr', label: 'Vertigo Blue', image: '', rotation: 15, flipped: false, editable: false, builtin: true },
        { name: 'mvm_dusk_custom', label: 'Dusk (custom)', image: '', rotation: 40, flipped: true, editable: true, builtin: false }
      ],
      skyEnts: 2, skyMirror: false, skyMoveSky: true, skyMoveFog: true,
      skyOrientOk: true, skyYaw: 40, skyPitch: 0,
      skyBuildState: 'idle', skyBuildBusy: false, skyBuildCan: true, skyBuildMsg: '',
      cloudMats: 3, cloudDensity: 55, cloudBright: 70, cloudDrift: 25, cloudSize: 60,
      sunVisible: true, sunYaw: 210, sunPitch: 32, sunGlowSize: 45,

      // ---- Environment (the added renderer) ---------------------------------
      // Mirrors EnvironmentDocument.cpp SerializeScene(): `climate` carries one field per entry in
      // ClimateSpecs(), so a key added there and not here shows up as a dead tile rather than as a
      // silently plausible mockup.
      envMap: 'de_dust2', envSelected: 0, envEditing: 0, envDragging: false, envEditRevision: 0, envGizmo: null,
      envOcclusionReady: true,
      envOcclusionPreparing: false,
      envBuildBusy: false,
      envBuildStatus: '',
      envOcclusionStatus: 'Map shadow geometry ready (de_dust2, 184k triangles).',
      envRender: 'Compositing 6 passes at half resolution',
      envSaved: 'Dusk storm', envDirty: true, envMessage: '',
      envAssets: ['barrel', 'crate_stack'],
      envTextures: ['concrete_albedo', 'concrete_normal'],
      envPresets: ['Dusk storm', 'Overcast morning'],
      // Filmmaker/Heatmap: RoundHeatmap.cpp AppendStateJson. players -1 = the grid is still building.
      heatmap: { on: false, radius: 1500, teams: 'all', state: 'off', round: 4, rounds: 18, players: 10, error: '' },
      // Filmmaker/Props: Props_AppendStateJson. total -1 = catalog not loaded until the first search.
      props: { revision: 0, map: 'de_dust2', message: '', selected: 0, placed: [
        { id: 1, model: 'models/props/de_dust/hr_dust/dust_cart/dust_cart.vmdl', name: 'dust cart', folder: 'props/de_dust/hr_dust/dust_cart', pos: [0, 0, 0], ang: [0, 0, 0], scale: 1, visible: true, live: true, group: 'Market stalls', render: true, locked: false },
        { id: 2, model: 'models/props/de_dust/hr_dust/dust_crates/dust_crate_style_01_32x32x32.vmdl', name: 'dust crate style 01 32x32x32', folder: 'props/de_dust/hr_dust/dust_crates', pos: [0, 0, 0], ang: [0, 0, 0], scale: 1, visible: true, live: true, group: 'Market stalls', render: true, locked: false },
        { id: 3, model: 'models/anubis/foliage/palm_tree_01.vmdl', name: 'palm tree 01', folder: 'anubis/foliage', pos: [0, 0, 0], ang: [0, 0, 0], scale: 1, visible: true, live: true, group: 'Palm row', render: true, locked: true },
        { id: 4, model: 'models/cs_italy/foliage/fern/fern_plant_1.vmdl', name: 'fern plant 1', folder: 'cs_italy/foliage/fern', pos: [0, 0, 0], ang: [0, 0, 0], scale: 2, visible: false, live: false, group: '', render: true, locked: false }],
        // Props_AppendStateJson's icons over the view: `color` is the light's own, `accent` its
        // kind's colour (SceneMarkerMath.h, the in-world markers' palette).
        markers: [
          { id: 2, kind: 'point', name: 'Point light 2', selected: false, color: '#ffc98a', accent: '#ff7d2d', at: [0.34, 0.44, true] },
          { id: 5, kind: 'spot', name: 'Spot light 5', selected: true, color: '#ffffff', accent: '#ff5096', at: [0.5, 0.36, true] },
          { id: 6, kind: 'area', name: 'Area light 6', selected: false, color: '#a0d8ff', accent: '#afeb3c', at: [0.62, 0.52, true] },
          { id: 7, kind: 'camera', name: 'Camera 7', selected: false, color: '#ffffff', accent: '#28e1af', at: [0.44, 0.6, true] },
          { id: 1, kind: 'mesh', name: 'box', selected: false, color: '#ccc7b8', at: [0.27, 0.62, true] }
        ],
        query: '', category: 'all', offset: 0, total: -1, results: [] },
      environment: {
        version: 14, enabled: true, map: 'de_dust2',
        exposure: 1.1, ambient: 0.6,
        fogColor: [0.45, 0.5, 0.6], tint: [1, 0.98, 0.94],
        climate: {
          sky: 1, clouds: 1, coverage: 0.45, density: 1, cloudBase: 2400, cloudDepth: 2600,
          cloudScale: 0.0018, windSpeed: 40, sunAzimuth: 210, sunElevation: 32, sunPower: 3,
          haze: 0.6, stars: 0.5,
          fogDensity: 0.35, fogStart: 400, fogHeight: 600, fogBase: 0, fogNoise: 0.4, fogRange: 4000,
          scatter: 0.4, lightShafts: 1, localFogRadius: 0, fogX: 0, fogY: 0, fogZ: 0,
          snow: 0.35, wet: 0.5, puddles: 0.4, puddleHeight: 0, puddleDepth: 40,
          surfaceRadius: 0, surfaceX: 0, surfaceY: 0, maskScale: 0.02, ripple: 0.5,
          reflectionRoughness: 0.3,
          relight: 0.35, ambient: 0.6, sunRelight: 1, bakedSuppressed: 0, bakedFill: 0.6, contactShadows: 1,
          relighting: 1, shadowBias: 2, shadowRange: 3000,
          mapLightPower: 1, sunAngle: 0.53, lampAngle: 1, shadowSoftness: 1, ao: 1, skyOcclusion: 1,
          particleDetail: 0.5, turbulence: 1.5, particleOpacity: 0.7, softness: 8,
          streak: 12, particleLight: 1,
          bloom: 0.35, bloomThreshold: 1.2, saturation: 1.05, contrast: 1.1, vignette: 0.2,
          grain: 0.012, chromatic: 0,
          enabled: 1, storm: 0, stormStrength: 2, stormInterval: 9, stormHold: 0, puddleNative: 0,
          ssr: 0, ssrRange: 1800, ssrThickness: 12, gi: 0, giRange: 200,
          worldGi: 1, bounce: 1, bounceSteps: 1, bounceSize: 2048, bounceX: 0, bounceY: 0, bounceZ: 0,
          bounceAlbedo: 0.4, sceneReflections: 0.5
        },
        weather: {
          type: 2, count: 1800, seed: 7, radius: 1200, height: 1400, speed: 420, size: 1.5,
          windX: 60, windY: 0, follow: true, shelter: true,
          center: [0, 0, 0], color: [0.65, 0.75, 0.9]
        },
        objects: [
          { id: 1, kind: 'box', asset: '', enabled: true, position: [512.0, -220.5, 64.0],
            angles: [0, 35, 0], scale: [48, 48, 96], color: [0.8, 0.78, 0.72],
            intensity: 1, range: 512, cone: 45, width: 64, height: 64, sourceRadius: 4,
            shadows: 1, pattern: 0, profile: 0, patternScale: 1, cue: 0, rate: 1, amount: 0.5,
            phase: 0, materialEnabled: true, cameraVisible: true,
            roughness: 0.6, metallic: 0, coat: 0, coatRoughness: 0.2, anisotropy: 0,
            emission: 0, normalStrength: 1, uvScale: 1,
            albedoTexture: 'concrete_albedo', normalTexture: '', roughnessTexture: '' },
          { id: 2, kind: 'point', asset: '', enabled: true, position: [180.0, 420.0, 260.0],
            angles: [-38, 120, 0], scale: [1, 1, 1], color: [1, 0.86, 0.62],
            intensity: 4.5, range: 1400, cone: 55, width: 64, height: 64, sourceRadius: 6,
            shadows: 1, pattern: 1, profile: 1, patternScale: 4, cue: 0, rate: 1, amount: 0.5,
            phase: 0, materialEnabled: false, cameraVisible: true,
            roughness: 0.5, metallic: 0, coat: 0, coatRoughness: 0.2, anisotropy: 0,
            emission: 0, normalStrength: 1, uvScale: 1,
            albedoTexture: '', normalTexture: '', roughnessTexture: '' }
        ]
      },

      // ---- Customize Player -------------------------------------------------
      // The modal reads the SELECTED target, not the roster, so it needs its own copy or it
      // opens onto an empty loadout. Shape: CosmeticUiQueries.cpp BuildCustomizeTargetJson /
      // BuildCustomizePlayersJson (pawn index keys). The per-player death animation picker lists
      // these players too (shim/commands.js deathAnimPlayers).
      customizeTarget: {
        pawnIndex: 3, controllerIndex: 12, key: 'steam:76561199063068840', steamId: '76561199063068840', name: 'donk', team: 2,
        activeWeaponIndex: 214, activeWeaponDefIndex: 7, activeWeaponOwnerSteamId: '', activeWeaponOwnerName: '', activeWeaponPickup: false,
        weapons: {
          primary: { entityIndex: 214, defIndex: 7, paintKit: 675, wear: 0.06, pickup: false, ownerSteamId: '', ownerName: '' },
          secondary: { entityIndex: 215, defIndex: 61, paintKit: 38, wear: 0.02, pickup: false, ownerSteamId: '', ownerName: '' },
          knife: { entityIndex: 216, defIndex: 42, paintKit: 0, wear: 0, pickup: false, ownerSteamId: '', ownerName: '' },
          gloves: { defIndex: 5027, paintKit: 10037, wear: 0.15 }
        },
        agentModel: 'agents/models/tm_phoenix/tm_phoenix.vmdl'
      },
      customizePlayers: {
        '3': {
          pawnIndex: 3, controllerIndex: 12, key: 'steam:76561199063068840', steamId: '76561199063068840', name: 'donk', team: 2,
          activeWeaponIndex: 214, activeWeaponDefIndex: 7, activeWeaponOwnerSteamId: '', activeWeaponOwnerName: '', activeWeaponPickup: false,
          weapons: {
            primary: { entityIndex: 214, defIndex: 7, paintKit: 675, wear: 0.06, pickup: false, ownerSteamId: '', ownerName: '' },
            secondary: { entityIndex: 215, defIndex: 61, paintKit: 38, wear: 0.02, pickup: false, ownerSteamId: '', ownerName: '' },
            knife: { entityIndex: 216, defIndex: 42, paintKit: 0, wear: 0, pickup: false, ownerSteamId: '', ownerName: '' },
            gloves: { defIndex: 5027, paintKit: 10037, wear: 0.15 }
          },
          agentModel: 'agents/models/tm_phoenix/tm_phoenix.vmdl'
        },
        '5': {
          pawnIndex: 5, controllerIndex: 14, key: 'steam:76561198386265483', steamId: '76561198386265483', name: 'm0NESY', team: 3,
          activeWeaponIndex: 233, activeWeaponDefIndex: 9, activeWeaponOwnerSteamId: '', activeWeaponOwnerName: '', activeWeaponPickup: false,
          weapons: {
            primary: { entityIndex: 233, defIndex: 9, paintKit: 344, wear: 0.11, pickup: false, ownerSteamId: '', ownerName: '' },
            secondary: { entityIndex: 234, defIndex: 1, paintKit: 0, wear: 0, pickup: false, ownerSteamId: '', ownerName: '' },
            knife: { entityIndex: 235, defIndex: 42, paintKit: 0, wear: 0, pickup: false, ownerSteamId: '', ownerName: '' },
            gloves: { defIndex: 5030, paintKit: 10047, wear: 0.22 }
          },
          agentModel: 'agents/models/ctm_sas/ctm_sas.vmdl'
        }
      },

      // ---- persisted UI state (tabs + collapsible cards) --------------------
      uiConfigTab: 'camera',
      uiConfigFxTab: 'vfx',
      uiConfigSmoothTab: 'breathing',
      uiConfigSmoothOpen: true,
      uiConfigHandheldOpen: true,
      // The engine hydrates the editor to PATH CAMERA, so the mockup must too -- opening on
      // FOLLOW made every follow-only panel report a live rect where the game reports none, and
      // the whole Path inspector was missing from a like-for-like screenshot.
      uiEditorInspector: 'path',
      uiEditorAdvanced: false,
      ui: {
        'fx.gunfire': '1', 'fx.damage': '1', 'fx.explosives': '1', 'fx.world': '1',
        'config.section.smoothview': '1', 'config.section.handheld': '1',
        'config.section.recoil': '1', 'config.section.stepbob': '0',
        'config.section.lighting': '1', 'config.section.atmosphere': '1',
        'config.section.weather': '1', 'config.section.ragdolls': '1'
      },

      // ---- Lens: depth of field and lens flare ---------------------------------------------
      // The CAMERA tab's DEPTH OF FIELD and LENS FLARE cards read st.lens (Lens/LensFx.cpp,
      // LensFx_AppendStateJson). Without this block the cards render unsynced (every slider at its own
      // minimum). Defaults are the parameter table's in Lens/LensFxMath.h; both effects ship OFF.
      // dofDefault / flareDefault dim each card's Reset.
      lens: {
        'dof.on': 0, 'dof.mode': 0, 'dof.auto': 0, 'dof.focusSpeed': 6, 'dof.focus': 400, 'dof.fstop': 2, 'dof.range': 0, 'dof.maxBlur': 40,
        'dof.nearStart': 60, 'dof.nearEnd': 180, 'dof.farStart': 600, 'dof.farEnd': 2400,
        'dof.shape': 0, 'dof.blades': 6, 'dof.rotation': 0, 'dof.roundness': 0.12, 'dof.squeeze': 1,
        'dof.edge': 0, 'dof.gamma': 6, 'dof.quality': 1, 'dof.weapon': 0, 'dof.debug': 0,
        'flare.on': 0, 'flare.intensity': 1, 'flare.threshold': 0.85, 'flare.ghosts': 5, 'flare.halo': 0.5,
        'flare.streak': 0.8, 'flare.streakLen': 0.6, 'flare.star': 0.5, 'flare.blades': 6, 'flare.glow': 0.5, 'flare.sun': 1,
        'flare.dirt': 0.2, 'flare.tint': 0, 'flare.streakTint': 0, 'flare.debug': 0,
        dofDefault: true, flareDefault: true, irisMask: 'diamond', dirtLoaded: false, status: 'Off', frames: 0
      },

      // ---- misc flags read by the HUD surfaces ------------------------------
      dbg: false, debug: false, debugbg: false, flashToken: 0, valid: true,
      // Graph editor: per-channel curves plus the index of the selected channel (a NUMBER --
      // the graph indexes into `channels` with it, it is not a channel name).
      channels: makeChannels(2),
      cameraChannels: null,
      selCh: 1, playhead: 42350, viewT0: 41700, viewT1: 44100,
      // The graph's overview navigator maps the WHOLE demo, so the feed carries the demo bounds
      // as the domain (C++ reads Render_GetDemoBounds). Wheel modifiers are mirrored so
      // Ctrl/Shift+wheel over the dock can be exercised in the browser.
      domainT0: 0, domainT1: 115200, ctrl: false, shift: false, alt: false, viewEpoch: 0,
      // Time snap (editor.graph.snap): "off" | "auto" | a tick count. One shared value.
      snap: 'off',
      // SCENE KEYS state: the timeline feed publishes it under "scene" next to the camera channels.
      sceneKeys: makeSceneKeys()
    };
    st.cameraChannels = st.channels;
    return st;
  }

  // Advance the fake demo clock. One render frame at 60fps moves the tick by 64/60 when playing,
  // and the loop range wraps exactly the way PlaybackLoop does in game.
  function advance(st, dtSeconds) {
    // `playing` is not one field in the engine: MovieHud publishes playing=true to mean a demo is
    // LOADED (it sends paused=true in the same breath) while CamTimeline publishes playing=false
    // to mean the clock is stopped. Merging the roots into one model let MovieHud's meaning win,
    // so a captured-paused demo ran anyway. `paused` says only one thing, so it decides.
    if (st.sceneKeys) syncSceneResolved(st.sceneKeys, st.tick);
    // Mirrors CameraPath::InterpDegraded: the cubic needs three keys to bend at all, so a
    // two-key Smooth path only eases its timing and the Smooth label dims.
    st.interpDegraded = !!st.interp && (st.count || 0) < 3;
    if (!st.playing || st.paused) return;
    st.tick += TICK_RATE * dtSeconds * (st.timescale || 1);
    if (st.loopOn && st.loopReady && st.tick >= st.loopOut) st.tick = st.loopIn;
    // Wrap on the DEMO's bounds. tickMin/tickMax are the camera path's first and last key
    // (17660..17857 in a 138839-tick demo), so wrapping on them yanked the playhead inside the
    // path the moment the demo tick sat outside it -- which is exactly where it normally sits,
    // and it left the graph editor reading its channel values at the wrong tick.
    var lo = isFinite(st.demoStartTick) ? st.demoStartTick : st.tickMin;
    var hi = isFinite(st.demoEndTick) ? st.demoEndTick : st.tickMax;
    if (st.tick > hi) st.tick = lo;
    st.tick = Math.round(st.tick);
    st.scrubTick = st.tick;
    st.playhead = st.tick;
    st.time = st.tick / TICK_RATE;
    if (st.sceneKeys) syncSceneResolved(st.sceneKeys, st.tick);
  }

  global.MvmState = {
    make: makeState,
    advance: advance,
    TICK_RATE: TICK_RATE,
    rebuildSceneDerived: rebuildSceneDerived,
    syncSceneResolved: syncSceneResolved,
    sceneFieldMeta: global.MvmSceneKeys.meta,
    makeCameraChannels: makeChannels,
    SCENE_CATALOG: SCENE_CATALOG,
    SCENE_BASELINE: SCENE_BASELINE
  };
})(window);
