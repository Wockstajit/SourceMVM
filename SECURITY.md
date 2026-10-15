# Security policy

## Supported versions

Only the latest release on the current `0.1.x` line receives security fixes. Older builds do not.
Update the launcher through the in-app updater, or download the newest installer from the
[project website](https://wockstajit.github.io/SourceMVM/).

## Reporting a vulnerability

Report security problems privately through
[GitHub Security Advisories](https://github.com/Wockstajit/SourceMVM/security/advisories/new).
Do not open a public issue or post exploit details in a pull request.

Include:

- the affected version and the Windows build;
- the steps that trigger the problem;
- what an attacker could do, and what they need first;
- any proof of concept, kept out of public places.

We will reply as soon as we can, and we will keep you informed while we fix the issue.

## Scope

In scope:

- the Electron launcher, the injection host, and the installer;
- the injected CS2 and Deadlock DLLs;
- the in-app updater and the public release downloads;
- the project website.

Out of scope:

- issues in the upstream HLAE and advancedfx code. Report those to the upstream project.
- game bugs or anti-cheat behavior. The tool is for offline demo and movie work only.

## Secrets

If a secret was exposed, such as a webhook URL, token, or private key, revoke or rotate it first.
Then report it through the private channel above. Removing it from a later commit does not remove it
from the Git history.
