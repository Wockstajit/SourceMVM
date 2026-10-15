// In-world camera-path gizmos must never appear in a captured frame.
//
// CCampathDrawer paints the keyframe camera icons, the trajectory poly-line and the standalone
// follow-camera marker straight into the scene render target (OnRenderThread_Draw runs off the
// world pass, BEFORE the UI passes). That places them upstream of every capture stage we use, so
// a recorded frame gets them regardless of HUD mode, camera source or stream: POV, free cam,
// path camera, beauty, depth and the isolation passes are all affected.
//
// The suppression has two independent triggers and two halves, and each one is load-bearing:
//   * trigger 1 -- the Filmmaker render job, for its whole lifetime (not just the capturing
//     states), so pre-roll and warm-up frames are clean too;
//   * trigger 2 -- AfxStreams_IsRcording(), so a bare `mirv_streams record` driven from the
//     console outside the Render workspace is covered as well;
//   * producer half -- OnEngineThread_SetupViewDone() queues no functor, which is what keeps
//     this leak-free (nothing piles up behind the render thread);
//   * consumer half -- OnRenderThread_Draw() discards an already-queued functor, closing the
//     one-frame window when a recording starts mid-frame.
//
// Restoration must lift only the suppression, never the user's own path-draw toggle.

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const testDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(testDirectory, "..", "..");
const read = (...parts) => fs.readFileSync(path.join(repositoryRoot, ...parts), "utf8");

const drawerHeader = read("AfxHookSource2", "CampathDrawer.h");
const drawer = read("AfxHookSource2", "CampathDrawer.cpp");
const bridgeHeader = read("AfxHookSource2", "Filmmaker", "Movie", "CameraBridge.h");
const bridge = read("AfxHookSource2", "Filmmaker", "Movie", "CameraBridge.cpp");
const driver = read("AfxHookSource2", "Filmmaker", "Render", "RenderSessionDriver.cpp");

test("the drawer carries a capture-suppression flag separate from the user's draw toggle", () => {
  assert.match(drawerHeader, /void\s+SuppressForCapture_set\(bool value\)/);
  assert.match(drawerHeader, /m_SuppressForCapture/);
  // Written from the render/main thread, read from the engine and render threads.
  assert.match(drawerHeader, /std::atomic_bool\s+m_SuppressForCapture/);
  // Draw_set stays the user's own control and must not be repurposed as the capture gate.
  assert.match(drawerHeader, /void\s+Draw_set\(bool value\)/);
});

test("the producer refuses to queue gizmo work while capturing", () => {
  const producer = drawer.slice(
    drawer.indexOf("void CCampathDrawer::OnEngineThread_SetupViewDone"),
    drawer.indexOf("void CCampathDrawer::OnEngineThread_EndFrame"),
  );
  assert.notEqual(producer.length, 0, "OnEngineThread_SetupViewDone not found");
  assert.match(producer, /m_SuppressForCapture\.load/);
  assert.match(producer, /AfxStreams_IsRcording\(\)/);
  // The gate must precede the CPU-expensive rebuild and the functor push, or it saves nothing.
  const gate = producer.search(/m_SuppressForCapture\.load/);
  assert.ok(gate < producer.indexOf("new CLessDynamicProperties"), "gate runs after the rebuild");
  assert.ok(gate < producer.indexOf("new CCampathDrawerFunctor"), "gate runs after the functor push");
});

test("the consumer discards a functor queued just before a mid-frame record start", () => {
  const consumer = drawer.slice(
    drawer.indexOf("void CCampathDrawer::OnRenderThread_Draw"),
    drawer.indexOf("void CCampathDrawer::OnRenderThread_Present"),
  );
  assert.notEqual(consumer.length, 0, "OnRenderThread_Draw not found");
  assert.match(consumer, /m_SuppressForCapture\.load/);
  assert.match(consumer, /AfxStreams_IsRcording\(\)/);
  // Discarding must delete, not leak, and must happen before the draw call.
  const guard = consumer.search(/m_SuppressForCapture\.load/);
  // The discard may do MORE cleanup than the delete (it also drops the retained functor the
  // pivot marker keeps alive for late redraws -- otherwise that one keeps drawing into the
  // capture). Assert the delete and the early return, not the exact statements between them.
  assert.match(consumer.slice(guard), /delete pFunctor;[\s\S]{0,200}?return;/);
  assert.ok(guard < consumer.indexOf("pFunctor->operator()"), "suppression runs after drawing");
});

test("Filmmaker reaches the drawer through the existing CameraBridge seam", () => {
  assert.match(bridgeHeader, /void CameraBridge_SetPathDrawSuppressed\(bool suppressed\);/);
  assert.match(bridge, /CameraBridge_SetPathDrawSuppressed[\s\S]{0,160}SuppressForCapture_set\(suppressed\)/);
});

test("a render job suppresses for its whole lifetime and restores on every exit path", () => {
  // Armed at start, next to the job-active latch -- not inside the capturing states, so the
  // pre-roll and warm-up frames are already clean.
  assert.match(driver, /Render_SetJobActive\(true\);[\s\S]{0,400}CameraBridge_SetPathDrawSuppressed\(true\)/);

  // RestoreEverything() is the single idempotent teardown for success, cancel and failure.
  const restore = driver.slice(
    driver.indexOf("void RestoreEverything()"),
    driver.indexOf("void FinishJob("),
  );
  assert.notEqual(restore.length, 0, "RestoreEverything not found");
  assert.match(restore, /CameraBridge_SetPathDrawSuppressed\(false\)/);

  // Restoring must not touch the user's own gizmo toggle.
  assert.doesNotMatch(restore, /CameraBridge_SetPathDrawEnabled/);
});
