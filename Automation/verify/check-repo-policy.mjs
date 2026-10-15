#!/usr/bin/env node
// Repository policy checks used by the CI jobs `security-license-scan` and `changed-file-policy`.
// See Docs/setup/git-workflow.md (section 5).
//
//   node Automation/verify/check-repo-policy.mjs --scan
//       Scan every tracked file for secrets, private paths, and forbidden tracked files.
//   node Automation/verify/check-repo-policy.mjs --changed <base-ref> <head-ref>
//       Check the files a change adds or modifies: binary types, file size, forbidden paths, secrets.
//
// Exemptions live in Automation/verify/repo-policy-allowlist.txt. The script prints file paths and
// rule names only. It never prints matched secret text.

import { spawnSync } from "node:child_process";
import { existsSync, readFileSync, statSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = fileURLToPath(new URL("../../", import.meta.url));
const allowlistPath = path.join(repoRoot, "Automation", "verify", "repo-policy-allowlist.txt");

export const MAX_TRACKED_BYTES = 2 * 1024 * 1024;

export const SECRET_PATTERNS = [
  {
    // Discord webhook tokens are about 68 characters. Shorter test fixtures do not match.
    name: "Discord webhook URL",
    re: /https:\/\/(?:ptb\.|canary\.)?discord(?:app)?\.com\/api\/webhooks\/\d+\/[\w-]{40,}/i,
  },
  { name: "GitHub token", re: /\bgh[pousr]_[A-Za-z0-9]{36,}\b/ },
  { name: "AWS access key ID", re: /\bAKIA[0-9A-Z]{16}\b/ },
  { name: "private key block", re: /-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----/ },
  { name: "private Windows user path", re: /[A-Za-z]:\\Users\\[^\\\s"'`<>|]+/ },
];

const BLOCKED_EXTENSIONS = new Set([
  ".exe", ".dll", ".lib", ".obj", ".pdb", ".zip", ".7z", ".rar", ".msi", ".blockmap",
  ".vpk", ".dem", ".mp3", ".ogg", ".wav", ".mp4", ".mkv", ".webm", ".vtex_c", ".vpcf_c",
  ".vmat_c", ".vmdl_c", ".vcd", ".cso", ".bin",
]);

const FORBIDDEN_PATH_RULES = [
  [/^FX\/(sources|compiled)\//, "donor-game or compiled FX pack"],
  [/^FX\/death-animations\//, "ragdoll animation library"],
  [/^build\//, "generated build output"],
  [/^Automation\/(runs|symbols)\//, "generated run output or symbol files"],
  [/^\.claude\//, "personal agent settings"],
  [/(^|\/)\.env(\..*)?$/, "environment file"],
  [/\.(pem|pfx|key|log)$/i, "key material or log file"],
];

const normalizePath = (filePath) => filePath.replace(/\\/g, "/");

export function scanText(text) {
  return SECRET_PATTERNS.filter(({ re }) => re.test(text)).map(({ name }) => name);
}

export function forbiddenTrackedPath(filePath) {
  const normalized = normalizePath(filePath);
  const rule = FORBIDDEN_PATH_RULES.find(([re]) => re.test(normalized));
  return rule ? rule[1] : null;
}

export function isBlockedBinary(filePath) {
  return BLOCKED_EXTENSIONS.has(path.extname(filePath).toLowerCase());
}

// Returns the problems for one added or modified file. `allowlisted` exempts only the binary and
// size rules. Forbidden paths are never exempt.
export function evaluateChangedFile({ path: filePath, lfs, size, allowlisted = false }) {
  const problems = [];
  const forbidden = forbiddenTrackedPath(filePath);
  if (forbidden) problems.push(`forbidden in the repository: ${forbidden}`);
  if (!lfs && !allowlisted && isBlockedBinary(filePath)) {
    problems.push("binary type must go through Git LFS or the allowlist");
  }
  if (!lfs && !allowlisted && size > MAX_TRACKED_BYTES) {
    problems.push(`file is ${size} bytes, over the ${MAX_TRACKED_BYTES} byte limit without Git LFS`);
  }
  return problems;
}

// Allowlist format, one entry per line ("#" starts a comment):
//   binary <path>                  exempts one path from the binary and size rules
//   scan <path> | <rule name>      exempts one path from one secret or private-path rule
export function parseAllowlist(text) {
  const binary = new Set();
  const scan = new Map();
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith("#")) continue;
    if (line.startsWith("binary ")) {
      binary.add(normalizePath(line.slice("binary ".length).trim()));
    } else if (line.startsWith("scan ")) {
      const [file, rule] = line.slice("scan ".length).split("|").map((part) => part.trim());
      if (!file || !rule) throw new Error(`bad allowlist entry: ${line}`);
      const key = normalizePath(file);
      if (!scan.has(key)) scan.set(key, new Set());
      scan.get(key).add(rule);
    } else {
      throw new Error(`bad allowlist entry: ${line}`);
    }
  }
  return { binary, scan };
}

// Parses `git diff --raw` output and skips submodule pointers (mode 160000), which are not files.
export function changedFileNames(rawDiff) {
  return rawDiff
    .split("\n")
    .filter((line) => line.startsWith(":"))
    .filter((line) => line.split(/\s+/)[1] !== "160000")
    .map((line) => line.split("\t").slice(1).join("\t"))
    .filter(Boolean);
}

export function unexemptedScanFindings(file, rules, allowlist) {
  const exempt = allowlist.scan.get(normalizePath(file)) ?? new Set();
  return rules.filter((rule) => !exempt.has(rule)).map((rule) => `${file}: ${rule}`);
}

function git(args) {
  const result = spawnSync("git", args, {
    cwd: repoRoot,
    encoding: "utf8",
    maxBuffer: 512 * 1024 * 1024,
  });
  if (result.status !== 0) {
    throw new Error(`git ${args.join(" ")} failed: ${result.stderr.trim()}`);
  }
  return result.stdout;
}

function readAllowlist() {
  return existsSync(allowlistPath)
    ? parseAllowlist(readFileSync(allowlistPath, "utf8"))
    : { binary: new Set(), scan: new Map() };
}

function runScan() {
  const allowlist = readAllowlist();
  const failures = [];
  const files = git(["ls-files", "-z"]).split("\0").filter(Boolean);
  for (const file of files) {
    const forbidden = forbiddenTrackedPath(file);
    if (forbidden) failures.push(`${file}: forbidden in the repository: ${forbidden}`);

    const absolute = path.join(repoRoot, file);
    if (!existsSync(absolute) || !statSync(absolute).isFile() || isBlockedBinary(file)) continue;
    const buffer = readFileSync(absolute);
    if (buffer.length > MAX_TRACKED_BYTES || buffer.includes(0)) continue;
    failures.push(...unexemptedScanFindings(file, scanText(buffer.toString("utf8")), allowlist));
  }
  return failures;
}

function runChanged(base, head) {
  const names = changedFileNames(git(["diff", "--raw", "--diff-filter=ACMR", `${base}...${head}`]));
  if (names.length === 0) return [];

  const lfsPaths = new Set(
    git(["check-attr", "filter", "--", ...names])
      .split("\n")
      .filter((line) => line.endsWith(": filter: lfs"))
      .map((line) => line.slice(0, -": filter: lfs".length)),
  );
  const allowlist = readAllowlist();

  const failures = [];
  for (const name of names) {
    const size = Number(git(["cat-file", "-s", `${head}:${name}`]).trim());
    const problems = evaluateChangedFile({
      path: name,
      lfs: lfsPaths.has(name),
      size,
      allowlisted: allowlist.binary.has(normalizePath(name)),
    });
    for (const problem of problems) failures.push(`${name}: ${problem}`);

    if (isBlockedBinary(name) || size > MAX_TRACKED_BYTES) continue;
    failures.push(...unexemptedScanFindings(name, scanText(git(["show", `${head}:${name}`])), allowlist));
  }
  return failures;
}

function main(argv) {
  let failures;
  if (argv.length === 1 && argv[0] === "--scan") {
    failures = runScan();
  } else if (argv.length === 3 && argv[0] === "--changed") {
    failures = runChanged(argv[1], argv[2]);
  } else {
    console.error("usage: check-repo-policy.mjs --scan | --changed <base-ref> <head-ref>");
    return 2;
  }
  if (failures.length === 0) {
    console.log("repository policy: no findings");
    return 0;
  }
  console.error(`repository policy: ${failures.length} finding(s)`);
  for (const failure of failures) console.error(`  ${failure}`);
  return 1;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  process.exitCode = main(process.argv.slice(2));
}
