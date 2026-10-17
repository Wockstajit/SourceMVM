// No angle is wrapped by a loop that walks 360 degrees at a time.
//
// A camera path saved a yaw of 4.7e10 degrees (2026-10-17). Every `while (a > 180) a -= 360` that
// met it ran for ~130 million steps a call, every frame, so the game froze the moment the path was
// evaluated -- and on every later load of that demo. In float, `a -= 360` stops changing `a` at all
// past ~1e10 and the loop never ends. Wraps are one step now (std::remainder / std::fmod), and
// camera markers are kept within limits (SanitizeMarker in CamMarkers.h).
//
// Static check over the SOURCE:MVM hook, so it runs without CS2.
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';
import assert from 'node:assert/strict';
import { fileURLToPath } from 'node:url';

const root = new URL('../../AfxHookSource2/', import.meta.url);
const rootDir = fileURLToPath(root);

function* sources(dir) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) yield* sources(full);
    else if (/\.(h|cpp)$/.test(entry.name)) yield full;
  }
}

test('no angle wrap loops by 360 or 180', () => {
  const loop = /while\s*\([^)]*[<>]=?\s*-?\s*(?:180|360)(?:\.0*)?f?\s*\)\s*[^;{]*[-+]=\s*360/;
  const offenders = [];
  for (const file of sources(rootDir)) {
    fs.readFileSync(file, 'utf8').split('\n').forEach((line, i) => {
      if (loop.test(line)) offenders.push(`${path.relative(rootDir, file)}:${i + 1}: ${line.trim()}`);
    });
  }
  assert.deepEqual(offenders, []);
});

test('camera markers are sanitized wherever they enter the list', () => {
  const read = (name) => fs.readFileSync(new URL('Filmmaker/Movie/' + name, root), 'utf8');
  const markers = read('CamMarkers.cpp');
  assert.match(markers, /m_markers\.insert\(m_markers\.begin\(\) \+ idx, m\);\s*SanitizeMarker\(m_markers\[idx\]\);/);
  assert.match(markers, /for \(CamMarker& m : m_markers\) SanitizeMarker\(m\);/);
  assert.match(markers, /if \(SanitizeMarker\(m\)\)/);
  assert.match(read('CameraPath.cpp'), /\*target = v;\s*SanitizeMarker\(m\);/);
});
