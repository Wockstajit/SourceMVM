// Static regression checks for how precipitation particles are placed. They read the shader sources,
// so they run without CS2.
//
// The rule they protect: a rain drop, snowflake or ash fleck's WORLD position is a function of its
// lattice cell and absolute time only. The camera decides which cells are drawn; it must never move a
// particle. A density remap that referenced the camera made snow and ash slide across the world as the
// camera moved (2026-09-28).
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const shaders = readFileSync(new URL('../../AfxHookSource2/Filmmaker/Environment/EnvironmentShaders.h', import.meta.url), 'utf8');
const splash = readFileSync(new URL('../../AfxHookSource2/Filmmaker/Environment/EnvironmentSplashShaders.h', import.meta.url), 'utf8');
const vs = shaders.slice(shaders.indexOf('ParticleOutput WeatherVS'), shaders.indexOf('float4 WeatherPS'));

test('rain, snow and ash share the world-anchored lattice', () => {
  assert.match(vs, /const bool flake=weatherParams\.z==2\|\|weatherParams\.z==3;/);
  assert.match(vs, /if\(weatherParams\.z==1\|\|flake\)\{/);
  assert.match(vs, /p=\(float3\(id\)\+jitter\)\*cell\+motion;/);
});

test('one cell size from the CPU lattice, never from the camera', () => {
  assert.match(vs, /float cell=weatherLattice\.x;/);
  assert.doesNotMatch(vs, /weatherBands|band\*|pow\(3/);
});

test('particles and the veil split one density by complementary weights', () => {
  assert.match(vs, /alpha=\(1-PrecipitationVeilWeight\(p\)\)\*PrecipitationRegion\(p\);/);
  const veil = shaders.slice(shaders.indexOf('float4 WeatherVeilPS'));
  assert.match(veil, /float w=PrecipitationVeilWeight\(p\)\*PrecipitationRegion\(p\);/);
  // Only open-sky stretches of the ray count, so a doorway view matches standing outside.
  assert.match(veil, /w\*=PrecipitationOpenSky\(p\);/);
  // The veil's clumping noise falls with the particles in world space.
  assert.match(veil, /Noise3\(\(p-velocity\*climateClock\.x\)\*scale\)/);
});

test('rain drop sizes follow a long-tailed spectrum and change streak width and length', () => {
  assert.match(vs, /if\(weatherParams\.z==1\)variation=RainDropVariation\(Random01\(n\+7\)\);/);
  assert.match(shaders, /float RainDropVariation\(float u\) \{ return \.25\+\.75\*pow\(u,1\.6\); \}/);
  assert.match(vs, /float width=max\(physical,pixel\*\(\.35\+\.65\*variation\)\);/);
  assert.match(vs, /float streakLength=max\(physical\*2,CL_streak\*weatherParams\.y\*fall\);/);
});

test('roof test and lighting run once per particle, not per pixel', () => {
  assert.match(vs, /open=PrecipitationOpenSky\(p\);/);
  assert.match(vs, /alpha\*=smoothstep\(\.3,\.7,open\);/);
  assert.match(vs, /o\.light=PrecipitationLighting\(p,pixel\);/);
  const ps = shaders.slice(shaders.indexOf('float4 WeatherPS'), shaders.indexOf('float4 WeatherVeilPS'));
  assert.doesNotMatch(ps, /WorldPrecipitationVisible|RainLighting\(/);
});

test('gust particles are roof-tested along their gust but streak along the shared fall axis', () => {
  assert.match(vs, /open=GustOpen\(g,p\);/);
  assert.doesNotMatch(vs, /fallAxis=gustDirection/);
  assert.match(vs, /float3 streak=fallAxis;/);
  // Motion blur: opacity falls with streak length.
  assert.match(vs, /alpha\*=min\(1\.,RAIN_MOTION_BLUR\*physical\/streakLength\);/);
  // Surfaces, splashes and puddles use the graded exposure, not the single-direction cut.
  const surface = readFileSync(new URL('../../AfxHookSource2/Filmmaker/Environment/EnvironmentRainSurfaceShaders.h', import.meta.url), 'utf8');
  assert.match(surface, /PrecipitationExposure\(p\+float3\(0,0,2\)\)/);
  assert.match(splash, /PrecipitationExposure\(i\.hit\+float3\(0,0,2\)\)/);
});

test('no camera-relative remap of particle positions', () => {
  assert.doesNotMatch(vs, /crowd/);
  assert.doesNotMatch(vs, /pow\(abs\([^)]*weatherCenterRadius/);
});

test('flake drift is driven by absolute time and the flake phase only', () => {
  const drift = vs.slice(vs.indexOf('if(flake){'), vs.indexOf('}else{'));
  assert.match(drift, /climateClock\.x/);
  assert.doesNotMatch(drift, /camera\.xyz|weatherCenterRadius/);
});

test('splash cells are world lattice cells and use absolute time', () => {
  assert.match(splash, /int2 id=base\+int2\(cell%SPLASH_GRID,cell\/SPLASH_GRID\);/);
  assert.match(splash, /float2 xy=\(float2\(id\)\+float2\(Random01\(en\+5\),Random01\(en\+6\)\)\)\*cs;/);
  assert.match(splash, /climateClock\.x\/period/);
});

test('splashes refuse player bodies and unconverged columns', () => {
  assert.match(splash, /coatingActors\[i\]\.z>20/);
  assert.match(splash, /k==4/);
});

test('snow is a soft clump, not a star', () => {
  const ps = shaders.slice(shaders.indexOf('float4 WeatherPS'));
  assert.doesNotMatch(ps, /cos\(angle\*6\)\),10\)/);
  assert.match(ps, /SNOW: a soft, slightly lumpy clump/);
});
