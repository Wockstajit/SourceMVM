// The playback loop must stay independent of the render range.
//
// All three ranges in this tool -- loop, render, export -- are "two ticks and a duration", so the
// tempting simplification is to make them one object. The report asks for the opposite, and the
// reason is asymmetric risk: the loop is something you nudge constantly while reviewing, and the
// render range is the thing you spent time getting exactly right. Share them, and every loop
// tweak silently redefines what a render would produce -- and you find out when the export comes
// back wrong, not when you made the change.
//
// That coupling would be easy to reintroduce by accident (one convenient Render_SetRange call in
// a loop setter, or a Loop button in the timeline that writes the render range because the
// command was nearer to hand). Nothing about it would look wrong in review or fail at runtime, so
// it is checked here.

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, "..", "..");

const loopCpp = fs.readFileSync(
  path.join(repo, "AfxHookSource2", "Filmmaker", "Movie", "PlaybackLoop.cpp"), "utf8");
const timelineJs = fs.readFileSync(
  path.join(repo, "AfxHookSource2", "Filmmaker", "Panorama", "CameraTimelineJs.h"), "utf8");
const timelineRangeJs = fs.readFileSync(
  path.join(repo, "AfxHookSource2", "Filmmaker", "Panorama", "CameraTimelineRangeJs.h"), "utf8");
const graphJs = fs.readFileSync(
  path.join(repo, "AfxHookSource2", "Filmmaker", "Panorama", "GraphEditorJs.h"), "utf8");

// The body of one `if (0 == _stricmp(action, "<name>")) { ... }` block.
function actionBlock(name) {
  const marker = `_stricmp(action, "${name}")`;
  const at = loopCpp.indexOf(marker);
  assert.notEqual(at, -1, `action '${name}' not found in PlaybackLoop.cpp`);
  const open = loopCpp.indexOf("{", at);
  let depth = 0;
  for (let i = open; i < loopCpp.length; i++) {
    if (loopCpp[i] === "{") depth++;
    else if (loopCpp[i] === "}" && --depth === 0) return loopCpp.slice(open, i + 1);
  }
  assert.fail(`action '${name}' block is not terminated`);
}

test("the render range is only written by the explicit torender action", () => {
  const writes = [...loopCpp.matchAll(/Render_SetRange\s*\(/g)].length;
  assert.equal(writes, 1, "Render_SetRange should be called exactly once, from `loop torender`");
  assert.ok(actionBlock("torender").includes("Render_SetRange("),
    "the one Render_SetRange call must be inside the torender action");
});

test("setting the loop region never touches the render range", () => {
  // These are the paths a loop nudge goes through. None of them may reach the render range.
  for (const fn of ["PlaybackLoop_SetIn", "PlaybackLoop_SetOut", "PlaybackLoop_Clear",
    "PlaybackLoop_SetEnabled", "PlaybackLoop_RunFrame"]) {
    const at = loopCpp.indexOf(`${fn}(`);
    assert.notEqual(at, -1, `${fn} not found`);
    const open = loopCpp.indexOf("{", at);
    let depth = 0, end = -1;
    for (let i = open; i < loopCpp.length; i++) {
      if (loopCpp[i] === "{") depth++;
      else if (loopCpp[i] === "}" && --depth === 0) { end = i; break; }
    }
    const body = loopCpp.slice(open, end);
    assert.ok(!body.includes("Render_SetRange"), `${fn} must not write the render range`);
    assert.ok(!body.includes("Render_GetRange"), `${fn} must not read the render range either`);
  }
});

test("the loop persists to its own project.mvm section, not the camera path's", () => {
  // The loop goes through the demo project store with its OWN section tag ("loop" inside the
  // demo's project.mvm); it never touches the camera path's "campath" section.
  assert.match(loopCpp, /ProjectSection::Loop/, "the loop must persist to its own section");
  assert.ok(!/ProjectSection::Campath|campath/.test(loopCpp),
    "the loop must not read or write the camera-path section");
  const fileMath = fs.readFileSync(path.join(path.dirname(fileURLToPath(import.meta.url)), "..", "..", "AfxHookSource2", "Filmmaker", "Demo", "DemoProjectFileMath.h"), "utf8");
  assert.match(fileMath, /case ProjectSection::Loop: return "loop";/, "the loop section tag must be \"loop\"");
});

test("the per-frame loop only seeks under a debounce", () => {
  // demo_gototick replays packets over several frames, during which the tick still reads past
  // Out. An undebounced loop therefore issues a seek EVERY frame until it lands -- a seek storm
  // on the most expensive operation the tool has.
  // Count the COMMAND being built, not the word: the comments above the seek explain it too.
  // Enabling or moving a loop also seeks once via SeekToLoopStartIfOutside. That explicit
  // action is not the per-frame repeat path this guard checks; scope the assertion accordingly.
  const start = loopCpp.indexOf("void PlaybackLoop_RunFrame() {");
  const end = loopCpp.indexOf("// ============================== command", start);
  assert.ok(start >= 0 && end > start, "PlaybackLoop_RunFrame must be found");
  const frame = loopCpp.slice(start, end);
  const seeks = [...frame.matchAll(/"demo_gototick %d"/g)].length;
  assert.equal(seeks, 1, "there should be exactly one seek site");
  assert.ok(frame.includes("s_seeking = true;"), "the seek site must arm the debounce");
  // Anchor on the code site, not the first mention -- the comments above it say the word too.
  const at = frame.indexOf('"demo_gototick %d"');
  const before = frame.slice(0, at);
  assert.ok(before.includes("if (s_seeking) {"), "the debounce must be checked before seeking");
});

test("the explicit loop-start seek arms the same debounce before sending its command", () => {
  const start = loopCpp.indexOf("void SeekToLoopStartIfOutside() {");
  const end = loopCpp.indexOf("} // namespace", start);
  assert.ok(start >= 0 && end > start, "SeekToLoopStartIfOutside must be found");
  const seek = loopCpp.slice(start, end);
  assert.match(seek, /s_seeking = true;[\s\S]*s_seekDeadlineMs = GetTickCount64\(\) \+ kSeekTimeoutMs;[\s\S]*DemoCmd\(cmd\)/);
});

test("only the regular timeline exposes the loop toggle and endpoint controls", () => {
  assert.match(timelineJs, /\$\.CreatePanel\('Panel', host, 'LoopBtn'/);
  assert.match(timelineJs, /lpBtn\.SetPanelEvent\('onactivate', function \(\) \{ cmd\('mirv_filmmaker loop toggle quiet'\); \}\)/);
  assert.equal([...timelineJs.matchAll(/mirv_filmmaker loop toggle/g)].length, 1);
  assert.match(timelineJs, /var lpVis = !\(st && \(st\.open \|\| st\.graphExp\)\)/);
  assert.doesNotMatch(timelineJs, /\b(?:loopBtn|loopReadout|loopWrap|loopBand|loopPending|makeLoopFlag|syncLoopControls|syncLoopFlags|pumpLoopFlags)\b/);
  assert.doesNotMatch(timelineJs, /mirv_filmmaker loop range/);
  assert.doesNotMatch(graphJs, /mirv_filmmaker loop|\bloop(?:On|In|Out|Ready|Btn|Band|Wrap)\b/);
  assert.match(timelineRangeJs, /mirv_filmmaker loop range/);
  assert.match(timelineRangeJs, /!nativeVisible[^\n]*hideRangeOverlay/);
});

test("loop endpoints have an atomic range command", () => {
  assert.match(loopCpp, /PlaybackLoop_SetRange\s*\(/);
  assert.match(loopCpp, /_stricmp\(action,\s*"range"\)/);
});
