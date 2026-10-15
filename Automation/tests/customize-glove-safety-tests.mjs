import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const testDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(testDirectory, "..", "..");
const queries = fs.readFileSync(path.join(
  repositoryRoot,
  "AfxHookSource2",
  "Filmmaker",
  "Cosmetics",
  "CosmeticUiQueries.cpp",
), "utf8");

test("customize glove polling contains transient Source 2 entity read faults", () => {
  const reader = queries.slice(
    queries.indexOf("bool ReadGloveInfo"),
    queries.indexOf("void WriteWeaponSlotBody"),
  );
  assert.match(reader, /m_iItemDefinitionIndex == 0/);
  assert.match(reader, /__try/);
  assert.match(reader, /__except \(1\)/);
  assert.match(reader, /MvmDebugLog_LinefAlways\("customize\.gloves"/);
});
