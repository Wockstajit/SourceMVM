// Unit tests for Automation/verify/check-repo-policy.mjs. Run: node --test Automation/tests/repo-policy-tests.mjs
// Fixtures are assembled at runtime so this file does not trip the scanner it tests.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import {
  MAX_TRACKED_BYTES,
  scanText,
  forbiddenTrackedPath,
  isBlockedBinary,
  evaluateChangedFile,
  parseAllowlist,
  changedFileNames,
  unexemptedScanFindings,
} from "../verify/check-repo-policy.mjs";

const webhookToken = "a".repeat(68);
const realLookingWebhook = "https://discord.com/api/webhooks/123456789/" + webhookToken;
const shortFixtureWebhook = "https://discord.com/api/webhooks/123456789/" + "x".repeat(11);
const userPath = "C:" + "\\Users\\" + "Example" + "User" + "\\AppData";

test("scanText flags a webhook URL with a real-length token", () => {
  assert.deepEqual(scanText(`url = ${realLookingWebhook}`), ["Discord webhook URL"]);
});

test("scanText ignores a short format fixture webhook", () => {
  assert.deepEqual(scanText(shortFixtureWebhook), []);
});

test("scanText flags GitHub tokens, private keys, and private user paths", () => {
  assert.deepEqual(scanText("ghp_" + "b".repeat(36)), ["GitHub token"]);
  // Split so this fixture does not contain a literal key header that the scanner would flag.
  assert.deepEqual(scanText("-----BEGIN " + "RSA PRIVATE KEY-----"), ["private key block"]);
  assert.deepEqual(scanText(userPath), ["private Windows user path"]);
});

test("scanText ignores placeholders and ordinary text", () => {
  assert.deepEqual(scanText("%USERPROFILE%\\AppData and Public docs"), []);
  assert.deepEqual(scanText("mirv_filmmaker health"), []);
});

test("forbiddenTrackedPath blocks generated, donor, and personal paths", () => {
  assert.ok(forbiddenTrackedPath("FX/compiled/pack.vpcf_c"));
  assert.ok(forbiddenTrackedPath("FX\\sources\\x.vmap"));
  assert.ok(forbiddenTrackedPath("FX/death-animations/clip-tags.json"));
  assert.ok(forbiddenTrackedPath("build/source2-release/AfxHookSource2.dll"));
  assert.ok(forbiddenTrackedPath(".claude/settings.local.json"));
  assert.ok(forbiddenTrackedPath("Automation/runs/session.json"));
  assert.ok(forbiddenTrackedPath("debug.log"));
});

test("forbiddenTrackedPath allows ordinary source and docs", () => {
  assert.equal(forbiddenTrackedPath("Docs/setup/git-workflow.md"), null);
  assert.equal(forbiddenTrackedPath("AfxHookSource2/Filmmaker/Community/BugReportMath.h"), null);
  assert.equal(forbiddenTrackedPath(".github/pull_request_template.md"), null);
});

test("isBlockedBinary matches binary types case-insensitively", () => {
  assert.equal(isBlockedBinary("Installer/setup.EXE"), true);
  assert.equal(isBlockedBinary("Docs/setup/git-workflow.md"), false);
});

test("evaluateChangedFile rejects an oversized non-LFS file and accepts it through LFS", () => {
  const big = MAX_TRACKED_BYTES + 1;
  assert.equal(evaluateChangedFile({ path: "Assets/clip.bin", lfs: false, size: big }).length, 2);
  assert.deepEqual(evaluateChangedFile({ path: "Assets/clip.bin", lfs: true, size: big }).length, 0);
});

test("evaluateChangedFile lets the allowlist exempt binary and size rules only", () => {
  const exempt = evaluateChangedFile({ path: "Tools/helper.exe", lfs: false, size: 10, allowlisted: true });
  assert.deepEqual(exempt, []);
  const forbidden = evaluateChangedFile({
    path: "FX/compiled/pack.vpcf_c",
    lfs: false,
    size: 10,
    allowlisted: true,
  });
  assert.equal(forbidden.length, 1);
});

test("parseAllowlist reads binary and scan entries and skips comments", () => {
  const parsed = parseAllowlist(
    "# note\n\nbinary Tools/helper.exe\nscan Resources/x.txt | private Windows user path\n",
  );
  assert.ok(parsed.binary.has("Tools/helper.exe"));
  assert.ok(parsed.scan.get("Resources/x.txt").has("private Windows user path"));
});

test("parseAllowlist rejects malformed entries", () => {
  assert.throws(() => parseAllowlist("delete everything"));
  assert.throws(() => parseAllowlist("scan Resources/x.txt"));
});

test("changedFileNames skips submodule pointers and keeps paths with spaces", () => {
  const raw = [
    ":100644 100644 aaaa bbbb M\tDocs/my doc.md",
    ":000000 160000 0000 cccc A\tDeps/submodule",
    ":100644 100644 aaaa bbbb M\tAfxHookSource2/a.h",
    "",
  ].join("\n");
  assert.deepEqual(changedFileNames(raw), ["Docs/my doc.md", "AfxHookSource2/a.h"]);
});

test("unexemptedScanFindings drops only the rules a file is exempt from", () => {
  const allowlist = parseAllowlist("scan Resources/x.txt | private Windows user path\n");
  assert.deepEqual(
    unexemptedScanFindings("Resources/x.txt", ["private Windows user path", "GitHub token"], allowlist),
    ["Resources/x.txt: GitHub token"],
  );
  assert.deepEqual(
    unexemptedScanFindings("Other/y.txt", ["private Windows user path"], allowlist),
    ["Other/y.txt: private Windows user path"],
  );
});

test("the committed allowlist parses", () => {
  const allowlistFile = fileURLToPath(new URL("../verify/repo-policy-allowlist.txt", import.meta.url));
  assert.doesNotThrow(() => parseAllowlist(readFileSync(allowlistFile, "utf8")));
});
