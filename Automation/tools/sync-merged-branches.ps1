<#
.SYNOPSIS
    Retires local branches and worktrees whose pull requests merged, then fast-forwards local main.

.DESCRIPTION
    GitHub squash-merges a pull request and deletes its head branch, but the local branch, its
    worktree and the local main stay behind. A squash commit does not contain the branch's own
    commits, so git never sees those branches as merged, and every later branch built on one of them
    conflicts with main. Whoever merges runs this right after the merge
    (Docs/setup/git-workflow.md, "Branch retirement").

    A local branch other than main is retired when:
    - a merged PR in Wockstajit/SourceMVM has it as the head branch, and the local tip is that PR's
      merged head commit or an ancestor of it, or
    - it has no commits outside origin/main, no open PR and no worktree.
    It is kept, with the reason printed, when it has an open PR, carries commits its merged PR did
    not include, or its worktree has uncommitted changes, untracked files, submodule changes, or is
    the folder this script runs from.

    Before a worktree is removed, its git-ignored files are kept unless they are rebuildable output
    (build folders, compiled shaders, node_modules, caches). Each kept file moves into
    Automation\runs\worktree-leftovers\<branch>\ in the main checkout at its relative path, unless
    the main checkout already holds an identical copy. Handoff notes, captures, review output,
    generated UI drafts, local configs and debug logs survive the cleanup.

    Finally, main is fast-forwarded to origin/main in the checkout that has it, when that checkout
    has no tracked changes.

.PARAMETER DryRun
    Print what would happen without changing anything.
#>
[CmdletBinding()]
param([switch]$DryRun)

$ErrorActionPreference = 'Stop'
$repo = 'Wockstajit/SourceMVM'

# Ignored output the build or the tools recreate. Everything else ignored in a worktree is kept.
$rebuildable = '^(build|out|ipch|packages)/|^build-[^/]*/|^cmake-build-|(^|/)node_modules/|(^|/)__pycache__/|(^|/)(bin|obj|target)/$|^Shaders/build/|^AfxCppCli/build/|^SourceMovieMakerLauncher/(host/build|renderer-dist|release[^/]*|payload-dist|bundle|packaged)/|\.(obj|pdb|ilk|pch|idb|tlog|pyc|pyo)$|^\.vs/'

function Invoke-Git([string]$Dir, [string[]]$GitArgs) {
    $out = & git -C $Dir -c core.quotePath=false @GitArgs
    if ($LASTEXITCODE -ne 0) { throw "git $($GitArgs -join ' ') failed in $Dir" }
    $out
}

# Windows PowerShell turns redirected native stderr into errors, so the preference is relaxed here.
function Test-Git([string]$Dir, [string[]]$GitArgs) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & git -C $Dir @GitArgs *> $null } finally { $ErrorActionPreference = $saved }
    $LASTEXITCODE -eq 0
}

function Write-Result([string]$Action, [string]$Branch, [string]$Reason) {
    $prefix = if ($DryRun -and $Action -ne 'kept') { "[dry run] $Action" } else { $Action }
    Write-Host ('{0,-18} {1}  ({2})' -f $prefix, $Branch, $Reason)
}

# Moves one ignored entry's files out of a worktree, skipping files the main checkout already has.
function Save-Leftover([string]$Worktree, [string]$Entry, [string]$Destination, [string]$MainRoot) {
    $root = [IO.Path]::GetFullPath($Worktree).TrimEnd('\')
    $source = Join-Path $root $Entry
    if (-not (Test-Path -LiteralPath $source)) { return 0 }
    $files = if (Test-Path -LiteralPath $source -PathType Container) {
        Get-ChildItem -LiteralPath $source -Recurse -File -Force
    } else { Get-Item -LiteralPath $source -Force }
    $moved = 0
    foreach ($file in @($files)) {
        $relative = $file.FullName.Substring($root.Length).TrimStart('\')
        $twin = Join-Path $MainRoot $relative
        if ((Test-Path -LiteralPath $twin -PathType Leaf) -and
            (Get-Item -LiteralPath $twin -Force).Length -eq $file.Length -and
            (Get-FileHash -LiteralPath $twin).Hash -eq (Get-FileHash -LiteralPath $file.FullName).Hash) { continue }
        if (-not $DryRun) {
            $target = Join-Path $Destination $relative
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
            Move-Item -LiteralPath $file.FullName -Destination $target -Force
        }
        ++$moved
    }
    $moved
}

$savedEncoding = [Console]::OutputEncoding
[Console]::OutputEncoding = [Text.Encoding]::UTF8
try {
    $worktrees = @()
    foreach ($line in (Invoke-Git $PSScriptRoot @('worktree', 'list', '--porcelain'))) {
        if ($line -like 'worktree *') {
            $worktrees += [pscustomobject]@{ Path = [IO.Path]::GetFullPath($line.Substring(9)).TrimEnd('\'); Branch = $null }
        } elseif ($line -like 'branch refs/heads/*') {
            $worktrees[-1].Branch = $line.Substring(18)
        }
    }
    $mainRoot = $worktrees[0].Path
    $here = [IO.Path]::GetFullPath((Get-Location).ProviderPath).TrimEnd('\')

    Invoke-Git $mainRoot @('fetch', '--prune', '--quiet', 'origin') | Out-Null

    $merged = @{}
    $open = @{}
    if (Get-Command gh -ErrorAction SilentlyContinue) {
        foreach ($state in 'merged', 'open') {
            $json = & gh pr list --repo $repo --state $state --limit 500 --json number,headRefName,headRefOid
            if ($LASTEXITCODE -ne 0) { throw "gh pr list failed. Check 'gh auth status'." }
            $table = if ($state -eq 'merged') { $merged } else { $open }
            foreach ($pr in (($json -join "`n") | ConvertFrom-Json)) {
                if (-not $table.ContainsKey($pr.headRefName)) { $table[$pr.headRefName] = @() }
                $table[$pr.headRefName] += $pr
            }
        }
    } else {
        Write-Warning 'gh is not installed: only branches with no commits outside origin/main are retired.'
    }

    $branches = Invoke-Git $mainRoot @('for-each-ref', '--format=%(refname:short)', 'refs/heads')
    foreach ($branch in $branches) {
        if ($branch -eq 'main') { continue }
        if ($open[$branch]) { Write-Result 'kept' $branch "open PR #$($open[$branch][0].number)"; continue }

        $tip = (Invoke-Git $mainRoot @('rev-parse', "refs/heads/$branch")).Trim()
        $worktree = $worktrees | Where-Object { $_.Branch -eq $branch } | Select-Object -First 1
        $reason = $null
        foreach ($pr in $(if ($merged.ContainsKey($branch)) { $merged[$branch] } else { @() })) {
            if ($tip -eq $pr.headRefOid -or
                ((Test-Git $mainRoot @('cat-file', '-e', "$($pr.headRefOid)^{commit}")) -and
                 (Test-Git $mainRoot @('merge-base', '--is-ancestor', $tip, $pr.headRefOid)))) {
                $reason = "PR #$($pr.number) merged"
                break
            }
        }
        if (-not $reason) {
            $ahead = [int](Invoke-Git $mainRoot @('rev-list', '--count', "origin/main..refs/heads/$branch"))
            if ($ahead -gt 0) {
                $why = if ($merged[$branch]) { "commits its merged PR did not include" } else { 'not merged' }
                Write-Result 'kept' $branch "$ahead commits outside origin/main, $why"
                continue
            }
            if ($worktree) { Write-Result 'kept' $branch 'no commits yet, but a worktree uses it'; continue }
            $reason = 'no commits outside origin/main'
        }

        if ($worktree) {
            $path = $worktree.Path
            if ($path -eq $mainRoot) { Write-Result 'kept' $branch 'checked out in the main checkout'; continue }
            if ($here -eq $path -or $here.StartsWith("$path\", [StringComparison]::OrdinalIgnoreCase)) {
                Write-Result 'kept' $branch "this shell is inside $path; rerun from the main checkout"
                continue
            }
            if (Invoke-Git $path @('status', '--porcelain', '--ignore-submodules=none')) {
                Write-Result 'kept' $branch "uncommitted or untracked files in $path"
                continue
            }
            $submodules = Invoke-Git $path @('submodule', 'foreach', '--quiet', '--recursive',
                'git status --porcelain; git log --oneline --branches --not --remotes')
            if ($submodules) { Write-Result 'kept' $branch "unsaved submodule work in $path"; continue }

            $destination = Join-Path $mainRoot ("Automation\runs\worktree-leftovers\" + ($branch -replace '[\\/]', '-'))
            $moved = 0
            foreach ($entry in (Invoke-Git $path @('ls-files', '--others', '--ignored', '--exclude-standard', '--directory'))) {
                if ($entry -notmatch $rebuildable) { $moved += Save-Leftover $path $entry $destination $mainRoot }
            }
            if ($moved) { Write-Host "  $moved ignored file(s) from $path -> $destination" }
            # --force: worktrees with initialized submodules refuse a plain remove. Both checks above passed.
            if (-not $DryRun) { Invoke-Git $mainRoot @('worktree', 'remove', '--force', $path) | Out-Null }
        }
        if (-not $DryRun) { Invoke-Git $mainRoot @('branch', '-D', '--quiet', $branch) | Out-Null }
        Write-Result 'retired' $branch $reason
    }
    if (-not $DryRun) { Invoke-Git $mainRoot @('worktree', 'prune') | Out-Null }

    $mainTree = $worktrees | Where-Object { $_.Branch -eq 'main' } | Select-Object -First 1
    $behind = [int](Invoke-Git $mainRoot @('rev-list', '--count', 'refs/heads/main..origin/main'))
    if ($behind -eq 0) {
        Write-Result 'kept' 'main' 'already at origin/main'
    } elseif ($mainTree -and (Invoke-Git $mainTree.Path @('status', '--porcelain', '--untracked-files=no'))) {
        Write-Result 'kept' 'main' "tracked changes in $($mainTree.Path); not fast-forwarded"
    } elseif ($DryRun) {
        Write-Result 'fast-forwarded' 'main' "$behind commits from origin/main"
    } else {
        $ok = if ($mainTree) { Test-Git $mainTree.Path @('merge', '--ff-only', '--quiet', 'origin/main') }
              else { Test-Git $mainRoot @('fetch', '--quiet', 'origin', 'main:main') }
        if ($ok) { Write-Result 'fast-forwarded' 'main' "$behind commits from origin/main" }
        else { Write-Result 'kept' 'main' 'not a fast-forward (local commits, or untracked files in the way)' }
    }
} finally {
    [Console]::OutputEncoding = $savedEncoding
}
