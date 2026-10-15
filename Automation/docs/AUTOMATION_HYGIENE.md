# Automation Hygiene

After any automation work, leave the repository organized and easy to review.

## Put outputs in the right place

- Runtime artifacts go under `Automation/runs/<automation-name>/<timestamp>/`.
- Temporary verifier output goes under `Automation/output/<feature-or-test>/`.
- Screenshots, logs, JSON state dumps, CSV exports, and transcripts are outputs, not source files.
- New reusable scripts, configs, and docs go into the matching automation subfolder (`launch/`, `verify/`, `capture/`, `netcon/`, `tools/`, `tests/`, `config/`, or `docs/`).
- Generated source files that are required by the build should live beside the code that consumes them, not inside an output folder.

## Clean up before finishing

- Delete one-off screenshots, logs, failed experiment folders, caches, and stale output that are not needed for review.
- Keep source changes, reusable automation scripts, documentation, and the compact evidence needed
  to review a result. Save before/after comparisons and metrics before removing large intermediates.
- Restrict cleanup to identified generated artifacts; preserve source assets and regression fixtures.
- Do not leave Python `__pycache__`, PowerShell transcripts, ad-hoc root-level PNGs, or random log files in the repo.
- If a result needs to be preserved, put it in a named run folder with a `run.json` or short note explaining what produced it.

## Naming

- Prefer descriptive folder names: `verify-editor-viewport`, `customize-player`, `launch-cs2`.
- Prefer timestamped run folders for repeated runs.
- Avoid dumping files directly into `Automation/`; the root should stay limited to the README, ignore file, and top-level folders.

## Git ignore rules

- Update the root `.gitignore` when a new class of generated artifact appears.
- Keep ignore rules narrow enough that source scripts and docs remain visible to Git.
- Before finishing, run `git status --short` and verify generated output is gone or ignored.
