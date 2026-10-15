import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const testDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(testDirectory, "..", "..");
const driver = fs.readFileSync(path.join(
  repositoryRoot, "AfxHookSource2", "Filmmaker", "Render", "RenderSessionDriver.cpp",
), "utf8");
const hooks = fs.readFileSync(path.join(repositoryRoot, "AfxHookSource2", "RenderSystemDX11Hooks.cpp"), "utf8");
const hookHeader = fs.readFileSync(path.join(repositoryRoot, "AfxHookSource2", "RenderSystemDX11Hooks.h"), "utf8");
const workspacePreview = fs.readFileSync(path.join(
  repositoryRoot, "AfxHookSource2", "Filmmaker", "Panorama", "WorkspacePreview.cpp",
), "utf8");

test("capture does not count RecordStart as an emitted source frame", () => {
  const warmup = driver.slice(driver.indexOf("case S::WarmingUp"), driver.indexOf("case S::Capturing"));
  assert.doesNotMatch(warmup, /AppendFrameMetadata/);
  assert.match(warmup, /s_frameIndex\s*=\s*0/);
});

test("capture completion is governed by the requested output-frame count", () => {
  const capturing = driver.slice(driver.indexOf("case S::Capturing"), driver.indexOf("case S::FlushingGpu"));
  assert.match(capturing, /s_frameIndex\s*<\s*s_job\.framesTotal/);
  assert.match(capturing, /s_frameIndex\s*>=\s*s_job\.framesTotal/);
});

test("writer failures prevent a completed manifest", () => {
  assert.match(hookHeader, /AfxStreams_GetWriterFailureCount/);
  assert.match(hooks, /g_AfxCaptureWriterFailureCount\.fetch_add/);
  const post = driver.slice(driver.indexOf("case S::PostProcessing"), driver.indexOf("case S::RestoringState"));
  assert.match(post, /AfxStreams_GetWriterFailureCount\(\)/);
  assert.match(post, /FailJob\(/);
});

// The pass system was removed 2026-08-10: a render job captures exactly ONE image stream through
// the inherited HLAE recorder. World Only (rule 9c) chooses what that one stream CONTAINS; it is a
// subject filter, not a second stream. These tests pin that shape so a multi-stream pass selection
// surface cannot quietly grow back into the job driver.
test("the job registers one recorder stream per pass, before one RecordStart", () => {
  const prep = driver.slice(driver.indexOf("case S::PreparingOutput"), driver.indexOf("case S::SavingState"));
  // Every pass name is cleared first so a crashed job's leftover cannot double-capture.
  assert.match(prep, /AfxStreams_RemoveStream\(PassStreamName\(any\)\.c_str\(\)\)/);
  assert.match(prep, /for \(const PassConfig& pass : s_plan\.passes\)/);
  assert.match(prep, /AfxStreams_AddPassStream\(streamName\.c_str\(\)/);
  assert.doesNotMatch(prep, /AddOverrideStream/);
  assert.doesNotMatch(hookHeader, /AddOverrideStream/);
  assert.doesNotMatch(hookHeader, /AfxStreams_AddDepthStream/);
});

test("only the Beauty + Everything plate can carry the HUD; every other pass is a BeforeUi capture", () => {
  const prep = driver.slice(driver.indexOf("case S::PreparingOutput"), driver.indexOf("case S::SavingState"));
  assert.match(prep, /hudPlate = pass\.type == PassType::Beauty && pass\.elements == VisibleElements::Everything\s*&& s_job\.passes\.hud != RenderHudMode::None/);
  assert.match(prep, /AfxStreams_AddNormalStream\(streamName\.c_str\(\), false/);
  const add = hooks.slice(hooks.indexOf("bool AddPassStream("), hooks.indexOf("bool RemoveStream("));
  assert.match(add, /settings\.Capture = CStreamSettings::Capture_e::BeforeUi;/);
});

test("chrome hiding follows the capture stage, not merely 'a job is running'", () => {
  const chromeGate = driver.slice(
    driver.indexOf("bool RenderSessionDriver_WantsChromeHidden()"),
    driver.indexOf("void RenderSessionDriver_RunFrame()"),
  );
  assert.match(chromeGate, /RenderHudMode::None/);
  assert.match(chromeGate, /return false;/);
  assert.match(chromeGate, /S::WarmingUp/);
  assert.match(chromeGate, /S::Capturing/);
  assert.match(chromeGate, /S::FlushingGpu/);
});

test("the scaled workspace preview cannot be composited into capture streams", () => {
  const commit = workspacePreview.slice(
    workspacePreview.indexOf("void WorkspacePreview::Commit()"),
    workspacePreview.indexOf("bool WorkspacePreview::Active", workspacePreview.indexOf("void WorkspacePreview::Commit()")),
  );
  assert.match(commit, /RenderSessionDriver_WantsChromeHidden\(\)/);
  assert.match(commit, /AfxViewportScaler::SetRequest\(active/);
});

// Recording FPS drives both mux and host_framerate. The timeline dial is preview-only;
// demo_timescale is forced to 1 so leftover dial speed cannot stretch the capture clock.
test("recording FPS sets host_framerate; dial is ignored for capture", () => {
  assert.match(driver, /job\.timescale = 1\.0/);
  assert.match(driver, /job\.captureFps = RenderCaptureFps\(job\.out\.fps\)/);
  assert.match(driver, /FrameCount\(job\.captureFps, job\.tickRate\)/);
  assert.match(driver, /AfxStreams_SetOverrideFps\(true, \(float\)s_job\.captureFps\)/);
  assert.match(driver, /demo_timescale 1\.0/);
  // No second speed knob: the output command surface must not carry its own capture speed.
  const command = fs.readFileSync(path.join(
    repositoryRoot, "AfxHookSource2", "Filmmaker", "Render", "RenderCommand.cpp",
  ), "utf8");
  assert.doesNotMatch(command, /captureSpeed/);
});

// Playback is released in the SAME frame the recorder is armed. Resuming after frame 0 was
// counted put the resume a frame late, so the first two source frames were both captured while
// held -- every clip opened with a duplicated frame. Verified live 2026-08-10: 1 -> 0 duplicate
// frames per take, and frame 0 then landed on the requested start tick exactly.
test("playback is released at arm time, not after frame 0", () => {
  const warm = driver.slice(driver.indexOf("case S::WarmingUp"), driver.indexOf("case S::Capturing"));
  assert.match(warm, /DemoCmd\("demo_resume"\);\s*\r?\n\s*\/\/ RecordStart runs at BeforePresent/);
  const capturing = driver.slice(driver.indexOf("case S::Capturing"), driver.indexOf("case S::FlushingGpu"));
  const firstFrameBlock = capturing.slice(capturing.indexOf("s_awaitFirstFrame"));
  assert.doesNotMatch(firstFrameBlock.slice(0, 400), /DemoCmd\("demo_resume"\)/);
});

test("the recorder accepts the take folder the driver already created", () => {
  // Flat output: the driver creates <root>\<take> first, so the recorder must not fail on "exists".
  assert.match(driver, /CreateDirectoryRecursive\(s_job\.takeDir\)/);
  assert.match(hooks, /CreatePath\(m_TakeDir\.c_str\(\), m_TakeDir, !m_RecordTakeSubfolder\)/);
});
