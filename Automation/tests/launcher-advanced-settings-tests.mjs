import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, "..", "..");
const app = fs.readFileSync(path.join(repo, "SourceMovieMakerLauncher", "src", "App.tsx"), "utf8");
const css = fs.readFileSync(path.join(repo, "SourceMovieMakerLauncher", "src", "styles.css"), "utf8");

test("Advanced Settings uses Game, Runtime, and Locations tabs", () => {
  assert.match(app, /type AdvancedTab = 'game' \| 'runtime' \| 'locations'/);
  assert.match(app, /role="tablist"/);
  for (const tab of ["GAME", "RUNTIME", "LOCATIONS"]) assert.match(app, new RegExp(`>${tab}<`));
  assert.match(app, /advancedTab === 'game'/);
  assert.match(app, /advancedTab === 'runtime'/);
  assert.match(app, /advancedTab === 'locations'/);
  assert.match(css, /\.advanced-tabs\s*\{/);
  assert.match(css, /grid-template-rows:\s*48px 38px minmax\(0, 1fr\)/);
});

test("Advanced Settings communicates autosave and has no Apply action", () => {
  assert.match(app, /SAVES AUTOMATICALLY/);
  assert.match(app, /saveSettings\(settings\)/);
  const drawer = app.slice(app.indexOf('className={`advanced-drawer'), app.lastIndexOf('</section>'));
  assert.doesNotMatch(drawer, />\s*APPLY\s*</i);
});

test("location actions share one stable trailing column", () => {
  assert.match(app, /className="location-actions"/);
  assert.match(css, /\.location-actions\s*\{/);
  assert.match(css, /\.location-row\s*\{[\s\S]{0,260}grid-template-columns:\s*112px minmax\(0, 1fr\) auto/);
});
