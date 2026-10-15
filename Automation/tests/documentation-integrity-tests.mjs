import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const testDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(testDirectory, "..", "..");
const repositoryFiles = execFileSync(
  "git",
  ["ls-files", "-co", "--exclude-standard", "-z"],
  {
    cwd: repositoryRoot,
    encoding: "utf8",
  },
)
  .split("\0")
  .filter(Boolean)
  .map((file) => file.replaceAll("\\", "/"));
// A documentation cleanup commonly runs before its deletions are staged. Validate the
// intended working-tree result, including new docs, while excluding ignored build output.
const activeFiles = repositoryFiles.filter((file) =>
  fs.existsSync(path.join(repositoryRoot, file)),
);
const activeSet = new Set(activeFiles);
const markdownFiles = activeFiles.filter((file) => file.toLowerCase().endsWith(".md"));

function sourceWithoutFencedCode(source) {
  return source.replace(/```[\s\S]*?```/g, "");
}

function markdownTargets(source) {
  const targets = [];
  const pattern = /!?\[[^\]]*]\(([^)\r\n]+)\)/g;
  for (const match of sourceWithoutFencedCode(source).matchAll(pattern)) {
    targets.push(match[1].trim());
  }
  return targets;
}

function normalizeLinkTarget(rawTarget) {
  let target = rawTarget;
  if (target.startsWith("<") && target.endsWith(">")) {
    target = target.slice(1, -1);
  }
  if (
    !target ||
    /^(?:https?:|mailto:|data:|#)/i.test(target) ||
    target.startsWith("/")
  ) {
    return null;
  }

  // A few research documents contain JavaScript-like "[value](this, 0)" expressions outside a
  // fence. They are not Markdown file links.
  if (target.includes(",") || /^[A-Za-z_$][\w$]*=/.test(target)) {
    return null;
  }

  target = target.split("#", 1)[0].split("?", 1)[0];
  target = target.replace(/:\d+(?:-\d+)?$/, "");
  try {
    return decodeURIComponent(target);
  } catch {
    return target;
  }
}

test("all repository Markdown links resolve inside the repository", () => {
  const failures = [];

  for (const file of markdownFiles) {
    const absoluteFile = path.join(repositoryRoot, file);
    const source = fs.readFileSync(absoluteFile, "utf8");
    for (const rawTarget of markdownTargets(source)) {
      const target = normalizeLinkTarget(rawTarget);
      if (target === null) continue;

      const resolved = path.resolve(path.dirname(absoluteFile), target);
      if (!fs.existsSync(resolved)) {
        failures.push(`${file}: ${rawTarget}`);
      }
    }
  }

  assert.deepEqual(
    failures,
    [],
    `Broken repository-relative Markdown links:\n${failures.join("\n")}`,
  );
});

test("maintained docs do not depend on private agent state", () => {
  const maintained = markdownFiles.filter(
    (file) =>
      !file.startsWith("Deps/") &&
      !file.startsWith("Resources/") &&
      !file.startsWith("Docs/archive/") &&
      !file.startsWith("Docs/notes_"),
  );
  const forbidden = [
    { pattern: /\[memory:/i, label: "private memory reference" },
    { pattern: /[A-Za-z]:\\Users\\/i, label: "private absolute Windows path" },
  ];
  const failures = [];

  for (const file of maintained) {
    const source = fs.readFileSync(path.join(repositoryRoot, file), "utf8");
    for (const rule of forbidden) {
      if (rule.pattern.test(source)) failures.push(`${file}: ${rule.label}`);
    }
  }

  assert.deepEqual(
    failures,
    [],
    `Maintained documentation contains non-portable state:\n${failures.join("\n")}`,
  );
});

test("retired handoffs and generated build artifacts stay out of source control", () => {
  const retiredDocs = [
    "Docs/customize-feature-handoff.md",
    "Docs/follow-camera/qa-automation-handoff.md",
    "Docs/render-system/HANDOFF.md",
    "CURSOR_STYLE_AI_INSTRUMENTATION_DEBUGGING_SKILL.md",
    "DESIGN.md",
    "PRODUCT.md",
    "UiMockup/native-menu-brief.md",
    "Docs/effects/HANDOFF-sky-rotation.md",
    "Docs/effects/lighting-experiment-handoff.md",
  ];
  const retiredDirectories = [
    "Docs/handoff/",
    "Docs/deadlock/handoff-deadlock/",
    "Misc/design_handoff_config_camera_editor_rail/",
    "Misc/design_handoff_customize_deathanims/",
  ];
  const generatedPatterns = [
    /^FilmmakerDemoInfoGo\/(?:FilmmakerDemoInfo\.exe|out\.json|err\.txt)$/i,
    /^FilmmakerDemoRepairGo\/FilmmakerDemoRepair\.exe$/i,
    /^particles\//i,
    /^Automation\/(?:.*\/)?(?:runs|output)\//i,
    /^(?:build(?:-[^/]+)?|cmake-build-[^/]+|out|packages)\//i,
    /(?:^|\/)(?:node_modules|__pycache__|\.pytest_cache)\//i,
    /^SourceMovieMakerLauncher\/(?:renderer-dist|release(?:-[^/]+)?|payload-dist|bundle|packaged|bin|obj|host\/(?:build|bin|obj))\//i,
    /^[^/]+\.(?:log|pdb|obj|tmp)$/i,
    /(?:^|\/)steam_appid\.txt$/i,
  ];

  for (const file of retiredDocs) {
    assert.equal(activeSet.has(file), false, `${file} is retired; fold durable facts into its feature doc`);
  }
  for (const file of activeFiles) {
    assert.equal(
      generatedPatterns.some((pattern) => pattern.test(file)) ||
        retiredDirectories.some((directory) => file.startsWith(directory)),
      false,
      `${file} is generated output and must not be tracked`,
    );
  }
});

test("runtime output paths remain ignored by Git", () => {
  const runtimePaths = [
    "Automation/runs/artifact-guard/session.json",
    "Automation/output/artifact-guard/test.png",
    "Automation/verify/runs/artifact-guard/session.json",
    "Automation/verify/output/artifact-guard/test.png",
    "build/artifact-guard/test.dll",
    "SourceMovieMakerLauncher/renderer-dist/artifact-guard.js",
    "SourceMovieMakerLauncher/host/build/artifact-guard.exe",
  ];
  const ignored = execFileSync("git", ["check-ignore", "--no-index", "--", ...runtimePaths], {
    cwd: repositoryRoot,
    encoding: "utf8",
  }).trim().split(/\r?\n/);
  assert.deepEqual(ignored, runtimePaths);
});

test("documented demo-info schema versions agree", () => {
  const goSource = fs.readFileSync(
    path.join(repositoryRoot, "FilmmakerDemoInfoGo", "main.go"),
    "utf8",
  );
  const cppSource = fs.readFileSync(
    path.join(
      repositoryRoot,
      "AfxHookSource2",
      "Filmmaker",
      "Demo",
      "DemoInfoHelper.cpp",
    ),
    "utf8",
  );
  const goVersion = goSource.match(/const\s+schemaVersion\s*=\s*(\d+)/)?.[1];
  const cppVersion = cppSource.match(/constexpr\s+int\s+kSchemaVersion\s*=\s*(\d+)/)?.[1];

  assert.ok(goVersion, "FilmmakerDemoInfoGo schemaVersion was not found");
  assert.ok(cppVersion, "DemoInfoHelper kSchemaVersion was not found");
  assert.equal(goVersion, cppVersion, "demo helper and cache reader schema versions must match");
});
