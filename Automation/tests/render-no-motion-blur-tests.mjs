import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const testDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(testDirectory, "..", "..");
const read = (...parts) => fs.readFileSync(path.join(repositoryRoot, ...parts), "utf8");

const renderUi = read(
  "AfxHookSource2", "Filmmaker", "Panorama", "CameraEditorRenderJs.h",
);
const renderCommand = read(
  "AfxHookSource2", "Filmmaker", "Render", "RenderCommand.cpp",
);
const renderOutput = read(
  "AfxHookSource2", "Filmmaker", "Render", "RenderOutput.h",
);
const renderDriver = read(
  "AfxHookSource2", "Filmmaker", "Render", "RenderSessionDriver.cpp",
);
const renderManifest = read(
  "AfxHookSource2", "Filmmaker", "Render", "RenderManifest.cpp",
);

test("Render workspace exposes no unfinished motion-blur controls or commands", () => {
  assert.doesNotMatch(renderUi, /motion blur|blurSubframes|blurShutterDeg|render output blur|shutter/i);
  assert.doesNotMatch(renderCommand, /blurSubframes|blurShutterDeg|render output blur|output shutter/i);
});

test("Render jobs cannot enable motion blur through hidden state or sampling", () => {
  assert.doesNotMatch(renderOutput, /blurSubframes|blurShutterDeg/i);
  assert.doesNotMatch(renderManifest, /blurSubframes|blurShutterDeg/i);
  assert.doesNotMatch(renderDriver, /mvmRenderSampler|SetSamplerSetting|s_subframes|s_subframeCounter/i);
  // captureFps == out.fps; one recorder frame stays one output frame.
  assert.match(renderDriver, /AfxStreams_SetOverrideFps\(true, \(float\)s_job\.captureFps\)/);
});
