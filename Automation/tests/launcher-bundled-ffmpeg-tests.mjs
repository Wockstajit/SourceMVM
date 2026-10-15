import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import { test } from 'node:test';

const source = readFileSync(new URL('../../SourceMovieMakerLauncher/electron/ffmpeg-tool.cjs', import.meta.url), 'utf8');
function fixture({ configured = '', bundledWorks = true } = {}) {
  const probes = [], bundle = path.join('clean-user', 'payload', 'runtime', 'ffmpeg', 'bin', 'ffmpeg.exe');
  const context = vm.createContext({ module: { exports: {} }, process: { env: { PATH: '' } },
    require: name => {
      if (name === 'electron') return { app: { getPath: () => 'clean-user' }, net: { fetch() { throw new Error('No download should run'); } } };
      if (name === './runtime-payload.cjs') return { installedRuntimeDir: () => path.join('clean-user', 'payload', 'runtime') };
      if (name === 'node:fs') return { existsSync: file => file === bundle || file === configured, readdirSync: () => [] };
      if (name === 'node:child_process') return { execFile: (file, args, opts, callback) => {
        probes.push(file);
        callback(file === bundle && !bundledWorks ? new Error('Broken encoder') : null, 'ffmpeg version fixture', '');
      } };
      if (name === 'node:path') return path;
      if (name === 'node:fs/promises' || name === 'node:os' || name === 'node:crypto') return {};
      throw new Error('Unexpected module ' + name);
    } });
  vm.runInContext(source, context);
  return { detect: () => context.module.exports.detect(configured), probes, bundle };
}
test('a clean machine resolves bundled FFmpeg without PATH or a separate download', async () => {
  const f = fixture();
  const result = await f.detect();
  assert.equal(result.found, true);
  assert.equal(result.path, f.bundle);
  assert.deepEqual(f.probes, [f.bundle]);
});
test('an explicitly chosen encoder still takes priority over the bundled one', async () => {
  const f = fixture({ configured: 'chosen-encoder.exe' });
  assert.equal((await f.detect()).path, 'chosen-encoder.exe');
  assert.deepEqual(f.probes, ['chosen-encoder.exe']);
});
test('a bundled encoder that cannot execute is not reported as usable', async () => {
  const f = fixture({ bundledWorks: false });
  assert.equal((await f.detect()).found, false);
});
