import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const testDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(testDirectory, "..", "..");
const commandSource = fs.readFileSync(
  path.join(repositoryRoot, "AfxHookSource2", "Filmmaker", "Render", "RenderCommand.cpp"),
  "utf8",
);
const guardHeader = fs.readFileSync(
  path.join(repositoryRoot, "AfxHookSource2", "Filmmaker", "Render", "RenderStateGuard.h"),
  "utf8",
);
const guardSource = fs.readFileSync(
  path.join(repositoryRoot, "AfxHookSource2", "Filmmaker", "Render", "RenderStateGuard.cpp"),
  "utf8",
);

test("opening the Render workspace always hands the mouse to Panorama", () => {
  const begin = commandSource.indexOf("void Render_SetModalOpen(bool open)");
  const end = commandSource.indexOf("bool Render_IsCancelRequested", begin);
  const body = commandSource.slice(begin, end);
  assert.ok(begin >= 0 && end > begin);
  assert.match(body, /if \(open && CameraEditor_Active\(\)\)/);
  assert.match(body, /CameraEditor_SetCursorMode\(true\)/);
});

test("render state restoration preserves the pre-job UI cursor", () => {
  assert.match(guardHeader, /bool m_uiCursor/);
  assert.match(guardSource, /m_uiCursor\s*=\s*CameraTimelineHudRef\(\)\.Cursor\(\)/);
  assert.match(guardSource, /MovieModeRef\(\)\.SetMode\([^;]+\/\*keepCursor\*\/\s*true\)/);
  assert.match(guardSource, /CameraEditor_SetCursorMode\(m_uiCursor\)/);
});
