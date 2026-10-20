import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");

function read(rel) {
  return fs.readFileSync(path.join(root, rel), "utf8");
}

function functionBody(source, name) {
  const signature = `${name}(`;
  const start = source.indexOf(signature);
  assert.notEqual(start, -1, `${name} must exist`);
  const open = source.indexOf("{", start);
  let depth = 0;
  for (let index = open; index < source.length; index += 1) {
    if (source[index] === "{") depth += 1;
    if (source[index] === "}") {
      depth -= 1;
      if (depth === 0) return source.slice(open + 1, index);
    }
  }
  throw new Error(`${name} has no closing brace`);
}

test("a scrub seek neither resumes nor toggles pause", () => {
  const source = read("AfxHookSource2/Filmmaker/Movie/DemoScrub.cpp");
  const at = source.indexOf("case ScrubAction::Seek:");
  assert.notEqual(at, -1);
  const seek = source.slice(at, source.indexOf("break;", at));
  assert.match(seek, /"demo_gototick %d"/);
  assert.doesNotMatch(
    seek,
    /demo_resume/,
    "a paused seek lands paused; resuming for it played the world on past every click (2026-10-19)",
  );
  assert.doesNotMatch(seek, /demo_pause/, "demo_pause toggles: sent while paused it starts playback");
  const frame = functionBody(source, "void DemoScrub_RunFrame");
  assert.match(
    frame,
    /there && !paused && !g_repaused/,
    "the one pause is sent only after the seek is observed playing",
  );
});

test("a skin play-out pauses only after resume is observed", () => {
  const source = read("AfxHookSource2/Filmmaker/Cosmetics/CosmeticDemoSync.cpp");
  const abort = functionBody(source, "CosmeticOverrideSystem::AbortActiveTickNudge");
  assert.match(abort, /m_nudgeWasPaused && m_nudgeSawResume && !DemoIsPaused\(\)/);
  const nudge = functionBody(source, "CosmeticOverrideSystem::MaybeFireTickNudge");
  assert.match(nudge, /if \(!paused\) m_nudgeSawResume = true/);
  assert.match(nudge, /m_nudgeWasPaused && !paused && g_pEngineToClient/);
  assert.doesNotMatch(
    nudge,
    /if \(m_nudgeWasPaused && g_pEngineToClient\)\s*g_pEngineToClient->ExecuteClientCmd\(0, "demo_pause"/,
    "phase 1 must not toggle pause before it has seen the demo playing",
  );
});

test("a scheduled button repaint checks the panel is still alive", () => {
  for (const rel of [
    "AfxHookSource2/Filmmaker/Panorama/CameraEditorWidgetsJs.h",
    "AfxHookSource2/Filmmaker/Panorama/GraphEditorJs.h",
    "AfxHookSource2/Filmmaker/Panorama/CameraTimelineJs.h",
  ]) {
    const source = read(rel);
    assert.match(source, /function panelAlive\(b\)/);
    assert.match(source, /if \(!panelAlive\(b\)\) return/);
    assert.match(
      source,
      /Schedule\(0\.11, function \(\) \{ if \(!panelAlive\(b\)\) return;/,
      `${rel} still paints a button after the workspace deleted it`,
    );
  }
});
