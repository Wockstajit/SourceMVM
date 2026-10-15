# Contributing to SOURCE:MVM

Thanks for helping. SOURCE:MVM is a Windows-only toolkit for offline Counter-Strike 2 and Deadlock
movie work. Fixes, features, documentation, and tests are all welcome, including AI-assisted ones.

## Before you start

- For a meaningful feature, regression, or refactor, open or find a tracking issue first. Small typo
  and documentation fixes do not need one.
- Check the scope. Keep one change to one goal. Split unrelated work into separate PRs.
- Read [the GitHub workflow policy](Docs/setup/git-workflow.md). It covers branch names, PR rules,
  required checks, and releases.

## Set up

- [Collaborator setup](Docs/setup/collaborator-setup.md) explains how to clone, build, and launch.
- [BUILDING.md](BUILDING.md) lists prerequisites and environment overrides.
- The compiled FX pack, ragdoll animations, and other third-party media are not in the repository.
  A clone builds and runs without them.

## Make the change

1. Branch from an up-to-date `main`. Use a prefix such as `feat/`, `fix/`, `perf/`, `refactor/`,
   `docs/`, or `chore/`.
2. Keep the diff focused. Do not reformat unrelated files or rewrite nearby code.
3. Add or update tests where the behavior is testable. Engine-free math belongs in a `*Math.h` with
   its own tests. See [CLAUDE.md](CLAUDE.md) for the test commands.
4. Update the documentation the change affects. The docs integrity test checks every Markdown link:

   ```batch
   node --test Automation/tests/documentation-integrity-tests.mjs
   ```

5. Open a draft PR early. Describe what is done, what is not, and what you verified. Fill in the PR
   template completely, and do not tick a box for a test you did not run.

## What counts as verified

- **Built:** the changed target compiles in the configuration you name.
- **Tested:** the automated checks you ran passed. Name them.
- **Runtime verified:** the change was checked in a real CS2 or Deadlock session. A mockup, a unit
  test, or a screenshot from a different build does not count.
- **Deployed verified:** a published build that contains the change was installed and checked.

Report each of these separately. Never claim a runtime result you did not observe.

## AI-assisted contributions

AI-assisted contributions are welcome. Reviewers judge a PR on correctness, security, licensing,
maintainability, documentation, and reproducible testing. They do not judge it on whether a tool
helped write it.

If you used AI in a way that materially shaped the code or the design, say so in the PR. A short
factual note is enough, for example "AI-assisted with a refactor of the camera interpolation module.
Reviewed and tested with `CameraMathTests` and a live offline demo." If you did not use AI, you can
write "No AI assistance used."

You are responsible for the change. You must be able to explain how it works, why it is safe, and how
to revert it. Never send secrets, private demos, private user data, or unreleased third-party source to
an external AI tool. Never present AI-generated claims about tests, screenshots, or benchmarks as real
evidence.

## Security and privacy

- Report vulnerabilities privately through
  [GitHub Security Advisories](https://github.com/Wockstajit/SourceMVM/security/advisories/new). Do not
  open a public issue. See [SECURITY.md](SECURITY.md).
- Never commit or paste secrets, webhook URLs, access tokens, personal machine paths, or private demo
  files.
- Keep the tool to offline demo and movie work. Never use it on VAC-protected or online servers.

## Licensing and attribution

SOURCE:MVM is MIT-licensed ([LICENSE](LICENSE)). Keep the upstream HLAE and advancedfx authorship and
every third-party license intact. Credit is listed in [CREDITS.md](CREDITS.md). Do not add game-derived
assets, audio, or models unless you have the right to redistribute them.

## Questions

Use the issue forms for bugs and feature requests. Discussions are not enabled for this repository.
Search open and closed issues before you open a new one.
