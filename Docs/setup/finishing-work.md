# How we finish work

SOURCE:MVM source and its tracking issues live in `Wockstajit/SourceMVM` (`origin`). The default
branch is `main`. `upstream` is advancedfx, not our publishing destination. Follow this workflow
for finished work, along with [CLAUDE.md](../../CLAUDE.md).

Git commits cannot carry coloured labels. Put labels on a tracking issue and, when used, on the PR.
Reference that issue from the commit so GitHub links them. The issue is the public report. The
commit records the implementation and its evidence.

## Scope and ownership

A normal finished task is one user request plus its follow-up fixes. It ends in one commit, one
tracking issue and a push. Do not mix unrelated requests. If the user asks for separately reviewed
or pushed fixes, give each finished fix its own commit and issue. Do not manufacture completion
commits or close issues for unfinished experiments.

Run `git status` and inspect the diff before staging. Stage explicit paths or selected hunks only.
Never use `git add .`, `git add -A` or `git commit -a`. Do not stage, revert, stash or commit
someone else's work, unrelated deletions, generated output, captures, large media or secrets. When
files overlap with another agent's work, use an isolated checkout or a reviewed patch that contains
only your changes. Preserve mixed line endings.

## Labels

Before opening an issue, list the existing labels and create only missing ones:

```powershell
gh label list -R Wockstajit/SourceMVM --limit 100
gh label create "type: perf" --color fb8500 --description "Runtime performance or memory improvement" -R Wockstajit/SourceMVM
```

Keep existing labels. Do not rename or remove labels used by older issues. Choose one or two
`type:` labels, every applicable `area:` label, and only truthful `status:` labels. The `Labels:`
line in a commit is a record. GitHub does not interpret it.

| Label | Colour | Description |
| --- | --- | --- |
| `type: feature` | `1f883d` | New user-facing capability |
| `type: fix` | `d1242f` | Correct existing behaviour |
| `type: perf` | `fb8500` | Runtime performance or memory improvement |
| `type: docs` | `0969da` | Maintained documentation or agent guidance |
| `type: tooling` | `57606a` | Build, automation or developer tooling |
| `type: visual` | `8250df` | Rendering appearance or interface design |
| `type: refactor` | `6f42c1` | Restructure implementation without intended behaviour changes |
| `type: test` | `0e8a16` | Test coverage or verification tooling |
| `area: cs2` | `0052cc` | CS2 hook and filmmaker features in AfxHookSource2 |
| `area: lighting` | `d4a017` | Environment, Relight, shadows and irradiance |
| `area: deadlock` | `5319e7` | Deadlock hook and filmmaker integration |
| `area: launcher` | `087f8c` | SourceMovieMakerLauncher host, app and packaging |
| `area: panorama` | `c260b5` | In-game Panorama panels and ui-mockup |
| `area: fx` | `b45f06` | FX conversion pipeline and shipped particle assets |
| `area: shared` | `64748b` | Shared native hooks, utilities and dependencies |
| `area: automation` | `006b75` | automation scripts, tests and capture tooling |
| `area: build` | `7057ff` | CMake, build scripts and GitHub workflows |
| `area: website` | `0ea5e9` | Website and public download presentation |
| `area: docs` | `bfd4f2` | Repository documentation and agent instructions |
| `status: built` | `2ea44f` | Affected build or compile succeeded; record target and configuration |
| `status: tested` | `15803d` | Relevant automated or manual checks passed; name them |
| `status: runtime-verified` | `0e8a16` | Changed build checked in CS2 or Deadlock with runtime evidence |
| `status: not-deployed` | `fbca04` | No deployed release containing this change has been tested |
| `status: deployed-verified` | `1d76db` | Published build containing this change was installed and checked |

Use `status: not-deployed` until a deployed build has actually been tested. Replace it with
`status: deployed-verified` only with that evidence. A Git push, a local DLL stage, a successful
package upload or a clean build is not deployed verification. For docs-only work, report build and
deployment as not applicable. Documentation checks can earn `status: tested`, but never
`status: built` or `status: runtime-verified`.

Commit types map to labels. `feat` maps to `type: feature`. `fix`, `perf`, `docs`, `tooling`,
`visual`, `refactor` and `test` map to the identically named `type:` label.

## Tracking issue and commit

Create the issue before the commit so its number is available. Title it `Area: short plain summary`
(about 70 characters maximum) without a commit prefix. Write the body to a scratch file outside the
checkout, and pass it with `--body-file`:

```powershell
gh issue create -R Wockstajit/SourceMVM --title "Lighting: reuse native material irradiance" --body-file <scratch-file> --label "type: perf" --label "area: cs2" --label "area: lighting" --label "status: not-deployed"
```

Use these sections in order:

1. **Request**: a brief quote of the user's request.
2. **What changed and why**: the problem, root cause and resulting behaviour.
3. **Files**: paths touched by this fix.
4. **Verification**: separate Built, Tested and Deployed lines. Each says yes, no, not tested or
   not applicable, with the actual target, checks and environment.
5. **Known issues / follow-ups**: remaining limitations, including any unverified runtime result.

Link relevant screenshots or videos. Do not commit generated captures or large media. Keep issue
labels, issue verification, commit evidence and the final report consistent.

The commit subject is `<type>(<area>): <imperative summary>`, within 72 characters. Use areas such
as `lighting`, `cs2`, `deadlock`, `launcher`, `panorama`, `fx`, `shared`, `automation`, `build`,
`website` or `docs`. Write the message outside the checkout and use `git commit -F <scratch-file>`.
Include enough detail to review the change alone:

```text
Why: what the user saw or asked for.
Root cause: the mechanism behind the problem.
Changes:
- path/file: what changed and why
Verification:
- Built: yes / no / not applicable; target and configuration
- Tested: actual checks, results and environment
- Deployed: yes / not tested / not applicable; evidence if yes

Closes #<number>
Labels: type: perf, area: lighting, area: cs2, status: built, status: tested, status: not-deployed
Agent: <actual session identifier, or unavailable>
```

## Pushes and pull requests

Routine work goes through a branch and a pull request into `main`. Name the branch with a policy
prefix (`feat/`, `fix/`, `perf/`, `refactor/`, `docs/`, `chore/`, `security/` or `upstream/`).
Push it with `git push -u origin <branch>` on the first push, and keep later fixes on that branch.
Never merge it into `main` locally. A direct push to `main` needs an explicit request from the
repository owner, and the tracking issue records that exception. The full policy is in
[GitHub workflow and contribution policy](git-workflow.md).

If a push is rejected, inspect the remote changes first. In a clean task checkout, fetch and rebase
unpublished work onto the destination branch, then retry a normal push. `git pull --rebase origin <destination>` is appropriate only in that clean checkout. Do not rebase a dirty shared checkout,
auto-stash or force-push. If published branch history must be reconciled, preserve it with a normal
merge or ask about the specific conflict. Do not rewrite other contributors' commits.

Open a draft PR as soon as the branch has a commit, and keep it as a draft while implementation or
runtime checks remain. Target `main`, or the supported `release/*` branch for a release fix. Require
the `ci-gate` check to pass before you merge with squash.
The title and description should explain the final behaviour, link the tracking issues, and state
the validation and remaining gaps. Update the same PR as the series grows. Use the applicable issue
labels on the PR. Do not mark it ready, claim an FPS target, or merge based only on compilation.
Merging and public release publication are separate actions from the requested branch pushes.

After each push, verify that the remote branch tip matches the intended local commit. For a direct
push or merge to `main`, also verify that the tracking issue closed. GitHub closing keywords take
effect when the change reaches the default branch. A feature-branch push leaves its issues open.
Report that state accurately rather than closing issues manually to imitate a merge. Report the
commit SHA and issue URL, plus the PR URL when one exists. See [GitHub's closing-keyword rules](https://docs.github.com/en/issues/tracking-your-work-with-issues/using-issues/linking-a-pull-request-to-an-issue).

## Labels are not release tags

Do not create Git tags for routine fixes. A `launcher-v*` tag triggers the private launcher build
workflow. It does not by itself publish or verify a release. Reserve version tags and public
releases for a requested release, and never move an existing published tag. Follow
[publishing releases](publishing-releases.md), including the existing-release serial scheme for
runtime hotfixes.
