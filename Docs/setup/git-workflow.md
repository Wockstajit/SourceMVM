# GitHub workflow and contribution policy

This is the canonical policy for branches, pull requests, AI-assisted contributions, checks, releases,
and issues in SOURCE:MVM. It applies to maintainers, contributors, GitHub Actions, and coding agents.
[CLAUDE.md](../../CLAUDE.md) carries the short version for agents. [CONTRIBUTING.md](../../CONTRIBUTING.md)
is the public version for contributors.

> **Before any public source merge:** follow
> [GitHub's guide to removing sensitive data from a repository](https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/removing-sensitive-data-from-a-repository).
> Check the full history, Git LFS objects, build artifacts, third-party assets, and release packages.
> `.gitignore` does not remove content that was already tracked, and it does not clean history. This
> policy does not authorize publication or a history rewrite.

## 1. Principles

1. `main` is the default branch and the primary integration branch. It is always current.
2. Merging to `main` does not publish a release. Releases are separate, deliberate decisions.
3. Nontrivial changes use a short-lived branch and a pull request. Direct commits to protected branches
   are reserved for documented emergencies or a one-time approved migration.
4. CI catches repeatable problems. Runs in the real CS2 and Deadlock games establish runtime behavior.
   A passing compile does not prove that an in-game fix works.
5. AI-assisted development is welcome. It needs transparent disclosure, maintainable code, human
   accountability, and test evidence.
6. Keep the original HLAE attribution and every third-party license. Never commit or publish secrets,
   private machine data, or game-derived assets without a license to redistribute them.
7. Do not create a branch, tag, issue, or PR just to make the repository look active.

## 2. Branch model

| Pattern | Example | Purpose | Lifetime | Merges into |
| --- | --- | --- | --- | --- |
| `main` | `main` | Always-current development and integration | Permanent, protected | N/A |
| `release/<major.minor>` | `release/0.1` | Stabilized release family, maintenance, and hotfixes | While the line is supported | Selected fixes back to `main` |
| `feat/<topic>` | `feat/environment-lighting` | New features | Temporary | `main` |
| `fix/<topic>` | `fix/camera-path-snap` | Bug fixes | Temporary | `main`, or the active release line for urgent patches |
| `perf/<topic>` | `perf/shadow-culling` | Optimization, with benchmarks | Temporary | `main` |
| `refactor/<topic>` | `refactor/resource-loader` | Restructuring without intended behavior changes | Temporary | `main` |
| `docs/<topic>` | `docs/build-instructions` | Documentation | Temporary | `main` |
| `chore/<topic>` | `chore/upgrade-actions` | Maintenance and dependencies | Temporary | `main` |
| `security/<topic>` | `security/dependency-audit` | Security and privacy hardening. Keep sensitive details out of the name. | Temporary | `main` or a patched release |
| `upstream/<version>` | `upstream/hlae-2.192.4` | Controlled HLAE upstream integration | Temporary | `main` |
| `experiment/<topic>` | `experiment/renderdoc-bridge` | Unfinished experiments, usually as a draft PR | Temporary | `main`, only after acceptance |

Rules:

- Do not create a persistent `active` or `develop` branch.
- Do not name feature branches after release tags.
- Do not keep a permanent experimental branch unless a continuous experimental build has a real audience.
- Keep several supported release branches only when there is a clear compatibility need.
- GitHub's Active and Stale tabs show branch recency only. They do not identify the supported release.
  A stale release branch can still be the correct historical record. A continuously moving preview
  build is a release channel, not a required special branch.

### Typical flow

```text
main (development)
  ├─ feat/new-camera-feature   -> draft PR -> tests -> review -> merge to main -> delete branch
  ├─ fix/render-crash          -> PR       -> tests -> review -> merge to main -> delete branch
  └─ upstream/hlae-2.192.4     -> PR       -> compatibility tests -> merge -> delete branch

release/0.1 (supported beta line)
  └─ fix/0.1-render-crash      -> targeted fix -> tests -> PR to release/0.1
                                -> immutable version tag and release, if required
                                -> same fix ported back to main
```

Feature branches come off `main`. When a supported release series needs maintenance, branch from a
reviewed commit such as `release/0.1` and allow only release-specific fixes. Fix on `main` first where
possible, then backport only what the release needs. If an emergency fix starts on a release branch,
forward-port it to `main`. Never merge an old release branch over newer work.

### Branch retirement

- After a merge, delete the branch. Turn on GitHub's automatic deletion of head branches.
- After 30 days without changes, triage an open working branch or PR as active, paused, blocked, or
  abandoned. This is a maintenance cadence, not GitHub's Stale threshold.
- After 60 to 90 days, close abandoned draft PRs with an explanation. Delete a branch only after you
  confirm it has no unmerged commits you need, and tell its owner.
- Never auto-delete `main`, supported `release/*` branches, immutable release tags, or branches needed
  for an open security or history migration.
- No routine force-pushes. An author may update an unshared working branch after telling reviewers.
  Never force-push protected or shared release branches.

## 3. Pull requests

PRs are the normal path into `main` and `release/*`. This applies to maintainers and coding agents too.
Keep each PR small enough to review, with one goal. Split unrelated changes, even when one session
produced them.

### Required workflow

1. Create or find a tracking issue for meaningful features, regressions, security fixes, and significant
   refactors. Trivial typo and doc fixes can skip the issue.
2. Branch from an up-to-date base. Never build unrelated features directly on `main`.
3. Keep the changes scoped, and add tests alongside the code. Keep generated binaries, logs,
   credentials, local paths, and restricted assets out of the commit.
4. Open a draft PR early for work in progress. Describe the tradeoffs, the limits, and the verification
   still missing.
5. Run the local checks and GitHub CI. Update docs, tests, and release notes where they apply.
6. Disclose meaningful AI assistance, and make sure a responsible maintainer understands the code.
   AI use is not a reason to reject a PR.
7. Request review, resolve the comments, and rerun the checks. With no second maintainer, do a documented
   self-review. Do not present a self-review as an independent approval.
8. Squash merge by default, with a concise final message and the issue reference. Use rebase merge only
   for commits you mean to keep. Disallow merge commits if the project enforces linear history.
9. Confirm the merged commit and the linked issue's state, then delete the branch. Merging is not
   publishing.

### Draft or ready

Keep a PR as Draft while core work is incomplete, security review is open, CI is failing, or required
in-game verification is missing. A PR can be ready for code review before runtime validation is done,
but it must say what remains. It must not be described as runtime verified.

### Titles

Use an imperative sentence under 72 characters with a type prefix:

```text
feat(camera): add smooth follow transitions
fix(render): prevent VRAM leak after repeated export
perf(lighting): avoid duplicate shadow submissions
docs(setup): clarify fresh Windows build requirements
security(launcher): validate update manifest checksums
upstream(hlae): integrate 2.192.4 changes
```

Put issue references in the PR body or the merge message. Avoid noisy commits such as `fix stuff`,
`updates`, or repeated `WIP` commits on a protected branch.

### PR body template

The repository template lives at [`.github/pull_request_template.md`](../../.github/pull_request_template.md).

Do not check a box for a test you did not run. The evidence still belongs in the PR, even when GitHub
does not keep the plain-text template exactly.

## 4. AI-assisted contributions

### Policy statement

AI-assisted contributions are welcome. SOURCE:MVM judges a contribution by its correctness, security,
licensing, maintainability, documentation, and reproducible testing. It does not judge a contribution by
whether an AI assistant was involved. Contributors must understand, review, and own their changes. AI
use that materially shaped the implementation or design must be disclosed. AI-generated claims about
tests, screenshots, benchmarks, or code review are never presented as verification.

### Requirements for every contributor

- **Transparency.** Disclose significant AI-generated or AI-assisted code in the PR. A short, factual note
  is enough. Do not paste chat transcripts or confidential prompts.
- **Accountability.** The PR author explains how the code works, why it is safe, and how to revert it.
  AI-generated code gets the same review as any other code.
- **Tests.** Write meaningful unit or integration tests where possible. Build the changed platforms for
  real. Validate engine hooks, rendering, camera behavior, and game-state features live in CS2 or
  Deadlock. Report untested areas plainly.
- **Minimal scope.** Do not silently rewrite nearby code, vendor packages, change licenses, touch
  generated output, or change resources outside the task.
- **Provenance.** Check dependency licenses, keep upstream attribution, and do not add unlicensed FX,
  audio, models, or other game-derived content. AI output is not automatically free of third-party rights.
- **Security.** Never send secrets, private user data, private game or demo files, unreleased proprietary
  source, or nonconsensual telemetry to an external AI tool or a PR. Do not hide credentials with
  obfuscation and call it protection.
- **Evidence.** Include exact test commands, observed results, limits, the performance method, and proof
  of runtime behavior where it applies. Do not invent measurements or call anything verified without evidence.
- **Reviewability.** Answer reviewer questions, and follow the project's formatting, architecture, safety,
  and license rules.

### Disclosure examples

- "AI-assisted with a refactor of the camera interpolation module. Manually reviewed and tested with
  `CameraMathTests` plus a live offline demo."
- "No AI assistance used."

## 5. Checks and runtime evidence

Each required check has a distinct job name, so branch protection can identify it. A check counts only
when it runs on the real codebase.

| Check | What it verifies | Release gate? |
| --- | --- | --- |
| `docs-integrity` | Relative links and documentation consistency (`Automation/tests/documentation-integrity-tests.mjs`) | Yes when docs change |
| `panorama-tests` | Panorama source and UI interaction tests | Yes for UI changes |
| `changes` | Decides which jobs a PR or push needs from its changed files ([CI](ci.md)). Merge queue groups run every job. Nightly, manual and release-line runs also run the shipping build | Yes on all PRs |
| `native-build-windows` | CS2 and Deadlock hook build (`x64-deadlock-ninja-release`, Ninja with sccache and cached shader and cargo builds) followed by the CTest suites, including state and math, map extraction, and crash handling. A Deadlock-only change builds and tests only the Deadlock targets. The four slow D3D11 render fixtures (`EnvironmentAtmosphereEdgeTests`, `EnvironmentRenderTests`, `RelightProbeGpuTests`, `EnvironmentVolumeCacheTests`) are excluded because the runner has no GPU, so run them locally | Yes for native changes |
| `native-full-validation` | The shipping configuration (`x64-deadlock-release`, Visual Studio generator) from a clean tree without compile caches, with the same CTest suites. Runs nightly, on manual runs and for `release/*` | Yes for release lines |
| `launcher-build` | Node install, launcher typecheck, and renderer build | Yes for launcher changes |
| `security-license-scan` | Secrets, private paths, and forbidden tracked files (`Automation/verify/check-repo-policy.mjs --scan`) | Yes on all PRs |
| `changed-file-policy` | No accidental binaries, large files, or private artifacts in the PR diff (`check-repo-policy.mjs --changed`) | Yes on all PRs |
| `ci-gate` | Aggregates the checks above. Fails if any of them fails or is cancelled, or if a check the change needed did not run | Yes on all PRs |

The inherited HLAE multi-build (Source 1, GoldSrc, win32) is not part of the PR path. `legacy-native.yml` runs it nightly and on demand from the Actions tab.

Mark a job as a required status check only after it exists, runs reliably on PRs, and has passed at least
once. A conditional check needs an always-running aggregator, or another predictable gate. Otherwise a
protected branch can deadlock. Security scans must not print discovered secrets into public logs.

### Runtime evidence

Changes to injected DLLs, camera paths, rendering, offsets, ragdolls, live lighting, cosmetics, and other
engine-coupled behavior need documented offline game-session tests before release. A browser mockup or a
test fixture does not replace in-game validation. The labels `status: built`, `status: tested`,
`status: runtime-verified`, and `status: deployed-verified` must match the evidence.

### CI security

- Use least-privilege `permissions` in GitHub Actions workflows.
- Builds from fork PRs run without privileged secrets.
- Never run untrusted PR code in a privileged `pull_request_target` workflow.
- Never publish release artifacts from an untrusted PR job.

## 6. Repository settings

These settings are applied in GitHub after the publication and security audit is complete. The exact
options depend on the plan and the repository visibility.

| Setting | Proposed configuration |
| --- | --- |
| Default branch | `main` |
| Protected branches | `main` and `release/*` |
| Require a PR before merging | Yes |
| Required approvals | One independent approval once a real second reviewer exists. Until then, the single-maintainer fallback is PR, CI, and documented owner review. |
| Required status checks | `ci-gate` (and the individual jobs it aggregates), with up-to-date checks where practical |
| Require resolved review conversations | Yes |
| Force pushes and deletion on protected branches | Disallowed |
| Merge method | Squash by default. Rebase is optional. Disallow merge commits if linear history is enforced. |
| Delete head branches after merge | On. Protected release branches are excluded. |
| Branch exemptions | Narrow, documented, time-bound emergency process only |
| Code owners | `.github/CODEOWNERS`. Require owner review once a second maintainer exists. |
| Dependabot and security alerts | Enabled, with alert noise reviewed |

A required approval from another person is impossible without a second authorized reviewer. Do not
install a fake reviewer, do not present AI review as an independent approval, and do not configure a rule
that nobody can satisfy.

GitHub protections depend on the plan and on repository visibility. Do not describe a ruleset as active
until GitHub confirms it. Where the platform cannot enforce a rule, enforce it through CI and PR discipline.

Useful GitHub documentation:

- [About protected branches](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-protected-branches/about-protected-branches)
- [Managing a branch protection rule](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-protected-branches/managing-a-branch-protection-rule)
- [Available rules for rulesets](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-rulesets/available-rules-for-rulesets)
- [Securely using `pull_request_target`](https://docs.github.com/en/actions/reference/security/securely-using-pull_request_target)

## 7. Releases, previews, and hotfixes

`main` is the development line, not the stable release branch. GitHub Releases and immutable tags identify
shipped versions. The public `SourceMVM` releases and their installer and updater URLs stay intact.

| Channel | Source | Artifact and tag rules | Intended users |
| --- | --- | --- | --- |
| Development | `main` | CI build artifacts only, not an automatic public release | Developers |
| Preview | A reviewed `main` commit | Opt-in prerelease with a unique, immutable version such as `v0.2.0-preview.1`, created after tests | Testers |
| Stable beta and stable | `release/0.1` or the current supported line | Immutable tags such as `v0.1.10` and `v0.2.0`, with reviewed release notes and a complete installer | Public |
| Runtime hotfix | A supported released version | Follows the existing hotfix serial and manifest scheme. Never move an existing stable version tag. | Existing users |
| Experimental | An explicit experiment branch or commit | Manual artifact only, unless a clearly labeled test channel is configured | Developers and invited testers |

Do not copy moving tags. SOURCE:MVM uses an Electron updater and public release manifests. Adopt moving
preview tags only after you test the tag, the checksum, `latest.yml`, prerelease handling, and updater
behavior end to end. Unique, immutable prerelease tags are the safer default.

The step-by-step packaging and upload procedure is in [publishing releases](publishing-releases.md).

### Release approval

1. Select a reviewed, tested commit or the current supported release branch. Confirm the right to
   redistribute every bundled file.
2. Update the version and changelog. Reconcile upstream HLAE changes, and check source and installer
   compatibility.
3. Build from a clean checkout. Make sure generated payloads do not come from developer-local leftovers.
4. Run CI, the relevant native and launcher tests, real offline CS2 tests, and upgrade and downgrade checks.
5. Check packaged binaries for embedded webhooks, credentials, private developer paths, tracking,
   unlicensed audio or FX, and unexpected outbound requests.
6. Produce checksums. Verify the executable and the complete installer contents. One VirusTotal scan does
   not certify the whole distribution.
7. Create an immutable version tag and a draft GitHub Release. Verify asset names, hashes, manifests,
   download links, and the expected updater behavior.
8. Publish only after explicit maintainer authorization. Link the release notes to tracked changes, state
   the known limitations, and keep the website current.
9. After deployment, test installation and updates on a clean machine. Only then set
   `status: deployed-verified`.

No agent may publish a release, create a version tag, change updater feeds, or merge a protected branch
just because a code change is finished. Those actions need an explicit request for a release or a
promotion. A push is not a deployment.

Maintain release lines responsibly. Keep only the supported branches, plus any archived historical refs
you intend to keep. Backport critical fixes when needed, with explicit review of conflicts. Never delete
a release or tag just because GitHub marks it stale.

## 8. Issues, reviews, and communication

- **Bug reports.** Include the launcher version, the Windows build, the CS2 or Deadlock version, the
  reproduction steps, the expected and actual behavior, safe logs, screenshots, and whether a recent game
  update may be the cause. Ask users to remove sensitive data before posting.
- **Feature requests.** Describe the user problem, the proposal, a sample workflow, the tradeoffs, and
  whether an existing feature already covers it.
- **Security issues.** Report them privately through
  [GitHub Security Advisories](https://github.com/Wockstajit/SourceMVM/security/advisories/new), not in a
  public issue. Keep exploit details and secrets out of public tickets. See [SECURITY.md](../../SECURITY.md).
- **PR reviews.** Focus on evidence and behavior. Critique the code, not the contributor.
- **Scope.** Avoid unrelated changes. Apply labels consistently. Close duplicates with a reference. Never
  mark work complete just to tidy a project board.
- **Commit and issue links.** Use `Closes #123` only when merging to the default branch actually resolves
  the issue. On an unmerged branch, describe the work as pending.
- **Attribution.** Keep upstream authorship, copyright notices, submodule links, and third-party licenses.
  Do not claim sole ownership of HLAE-derived code.

Keep the label families already defined in [How we finish work](finishing-work.md) (`type:`, `area:`,
`status:`). Revise them instead of creating a competing scheme. Use GitHub Milestones only for real
release goals. Write changelog entries for user-facing changes. Keep detailed technical evidence in
issues and PRs for engineers.
