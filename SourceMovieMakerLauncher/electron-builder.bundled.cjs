// The SELF-CONTAINED installer: one exe carrying the launcher, the hook runtime, the FX pack and
// the DLSS 5 payload (ReShade included). Built by `npm run package:bundled`, which stages
// bundle\ first (scripts/build-bundle.ps1). Everything else comes from package.json's "build".
//
// Complete releases use the same public update channel. A new setup replaces the app, then
// installBundled installs that version's matching runtime; it never downloads an older payload.
// Publishing remains a separate, explicitly invoked release step.
const base = require('./package.json').build;

module.exports = {
  ...base,
  extraResources: [
    ...base.extraResources,
    { from: 'bundle', to: 'bundle' },
  ],
  win: {
    ...base.win,
    // One installer exe. A portable exe would unpack ~0.5 GB to %TEMP% on every start.
    target: [{ target: 'nsis', arch: ['x64'] }],
  },
  nsis: {
    ...base.nsis,
    artifactName: 'SourceMovieMaker-Complete-Setup-${version}.${ext}',
  },
  publish: base.publish,
};
