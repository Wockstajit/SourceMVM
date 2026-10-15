// Where the Source2Viewer CLI and the CS2 VPK live, shared by fetch-assets.mjs (one-off setup) and
// the preview server's on-demand agent model export. The CLI is a HELPER, never a build dependency
// of this repo. It is a ValveResourceFormat checkout next to this repository unless
// SOURCEMVM_VRF_CLI says otherwise; the VPK comes from CS2_PAK or the Steam library CS2 is in.
import { execFile, execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');

export const CLI = process.env.SOURCEMVM_VRF_CLI
  || path.join(path.dirname(REPO), 'ValveResourceFormat', 'CLI', 'bin', 'Release', 'Source2Viewer-CLI.exe');

// Automation/lib/Resolve-Cs2Dir.ps1 reads Steam's libraryfolders.vdf, like every other launcher here.
function cs2Dir() {
  try {
    return execFileSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
      '-File', path.join(REPO, 'Automation', 'lib', 'Resolve-Cs2Dir.ps1')], { encoding: 'utf8', windowsHide: true }).trim();
  } catch {
    return '';
  }
}
export const PAK = process.env.CS2_PAK || (() => {
  const dir = cs2Dir();
  return dir ? path.join(dir, 'game', 'csgo', 'pak01_dir.vpk') : '';
})();

// assets/models/<vmdl path without extension>.glb -> the agent model, skinned (the animation flag is
// what makes the CLI write the skeleton; the filter matches no clip, so none are exported), with its
// materials and textures beside it. The 3D preview stand-in (shim/preview3d.js) poses that skeleton.
const AGENT_GLB = /^assets\/models\/(agents\/models\/[A-Za-z0-9_/]+)\.glb$/;
const pending = new Map();

export function agentModelExporter(outDir) {
  return (rel) => {
    const m = AGENT_GLB.exec(rel);
    if (!m || !fs.existsSync(CLI) || !PAK || !fs.existsSync(PAK)) return null;
    if (!pending.has(m[1])) {
      pending.set(m[1], new Promise((resolve) => {
        execFile(CLI, ['-i', PAK, '-o', outDir, '-d', '--gltf_export_format', 'glb', '--gltf_export_materials',
          '--gltf_textures_adapt', '--gltf_export_animations', '--gltf_animation_list', 'mvm_no_animations',
          '-f', `${m[1]}.vmdl_c`], { windowsHide: true, timeout: 120000 }, () => {
          pending.delete(m[1]);
          resolve();
        });
      }));
    }
    return pending.get(m[1]).then(() => path.join(outDir, `${m[1]}.glb`));
  };
}
