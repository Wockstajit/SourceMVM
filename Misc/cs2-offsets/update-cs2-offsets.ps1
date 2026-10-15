# Keeps Misc/cs2-offsets/CS2-OFFSETS-main in sync with https://github.com/sezzyaep/CS2-OFFSETS.
#
# WHY THIS EXISTS. That folder is a NAME/schema reference the agent docs lean on
# (Docs/agent/cs2-update-recovery.md, Docs/effects/update-resilience.md), and every one of them
# warns that it goes stale on each CS2 update. It was previously refreshed by hand, which means in
# practice it was refreshed rarely and silently drifted. Upstream republishes within hours of a CS2
# patch, so a daily check is enough to keep it honest.
#
# WHAT IT IS NOT. These dumps are reference only -- schema field names and their offsets. They are
# NOT a source of byte patterns or hook addresses, and nothing in the build reads them. Refreshing
# this folder can therefore never break a build; it only changes what the agent reads.
#
# The payload is gitignored (Misc/cs2-offsets/CS2-OFFSETS-*/), so this script only ever touches
# untracked files.
#
#   pwsh Misc\cs2-offsets\update-cs2-offsets.ps1            # update if upstream moved
#   pwsh Misc\cs2-offsets\update-cs2-offsets.ps1 -Check     # report only, never write
#   pwsh Misc\cs2-offsets\update-cs2-offsets.ps1 -Force     # re-download even if unchanged
#
# Exit codes: 0 = up to date or updated, 1 = error, 2 = (-Check only) update available.

[CmdletBinding()]
param(
    [switch]$Check,
    [switch]$Force,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Owner   = 'sezzyaep'
$Repo    = 'CS2-OFFSETS'
$Branch  = 'main'

$Root      = Split-Path -Parent $MyInvocation.MyCommand.Path
$PayloadDir = Join-Path $Root "$Repo-$Branch"      # CS2-OFFSETS-main
$MetaPath   = Join-Path $Root 'cs2-offsets-meta.json'
$LogPath    = Join-Path $Root 'cs2-offsets-update.log'

function Write-Line([string]$Message, [string]$Color = 'Gray') {
    $stamp = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')
    Add-Content -LiteralPath $LogPath -Value "[$stamp] $Message" -ErrorAction SilentlyContinue
    if (-not $Quiet) { Write-Host $Message -ForegroundColor $Color }
}

# GitHub rejects API calls without a User-Agent. TLS 1.2 is forced because Windows PowerShell 5.1
# still defaults to SSL3/TLS1.0 on some machines and would fail the handshake.
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$Headers = @{ 'User-Agent' = 'source-mvm-cs2-offsets-updater'; 'Accept' = 'application/vnd.github+json' }

function Get-UpstreamCommit {
    $uri = "https://api.github.com/repos/$Owner/$Repo/commits/$Branch"
    $response = Invoke-RestMethod -Uri $uri -Headers $Headers -TimeoutSec 45
    [pscustomobject]@{
        Sha  = [string]$response.sha
        Date = [string]$response.commit.committer.date
    }
}

function Get-LocalMeta {
    if (-not (Test-Path -LiteralPath $MetaPath)) { return $null }
    try { return Get-Content -LiteralPath $MetaPath -Raw | ConvertFrom-Json } catch { return $null }
}

try {
    $upstream = Get-UpstreamCommit
} catch {
    Write-Line "ERROR: could not reach GitHub: $($_.Exception.Message)" 'Red'
    exit 1
}

$local = Get-LocalMeta
$localSha = if ($local -and $local.PSObject.Properties['commit']) { [string]$local.commit } else { '' }
$havePayload = Test-Path -LiteralPath $PayloadDir

$short = $upstream.Sha.Substring(0, 7)
if ($havePayload -and $localSha -eq $upstream.Sha -and -not $Force) {
    Write-Line "cs2-offsets: up to date (upstream $short, $($upstream.Date))" 'Green'
    exit 0
}

$reason = if (-not $havePayload) { 'no local payload' }
          elseif (-not $localSha) { 'no local version stamp' }
          elseif ($Force)         { '-Force' }
          else                    { "local $($localSha.Substring(0,7)) -> upstream $short" }

if ($Check) {
    Write-Line "cs2-offsets: UPDATE AVAILABLE ($reason), committed $($upstream.Date)" 'Yellow'
    exit 2
}

Write-Line "cs2-offsets: updating ($reason)" 'Cyan'

# Staging dir beside the payload so the final move is a same-volume rename, not a slow cross-disk
# copy, and so a failed download never leaves the real folder half-written.
$stage = Join-Path $Root ".update-stage-$([Guid]::NewGuid().ToString('N').Substring(0,8))"
$zip = Join-Path $stage 'src.zip'
try {
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    $zipUri = "https://github.com/$Owner/$Repo/archive/refs/heads/$Branch.zip"
    Invoke-WebRequest -Uri $zipUri -Headers $Headers -OutFile $zip -TimeoutSec 300
    Expand-Archive -LiteralPath $zip -DestinationPath $stage -Force
    Remove-Item -LiteralPath $zip -Force

    $extracted = Join-Path $stage "$Repo-$Branch"
    if (-not (Test-Path -LiteralPath $extracted)) {
        # Be tolerant if GitHub ever changes the archive's top-level folder name.
        $extracted = (Get-ChildItem -LiteralPath $stage -Directory | Select-Object -First 1).FullName
    }
    if (-not $extracted -or -not (Test-Path -LiteralPath $extracted)) {
        throw 'archive did not contain an extracted folder'
    }

    $fileCount = @(Get-ChildItem -LiteralPath $extracted -File -Recurse).Count
    if ($fileCount -lt 50) {
        # The real payload is ~230 files. Anything tiny means a truncated or error-page download;
        # refuse it rather than replacing a good reference folder with garbage.
        throw "extracted payload has only $fileCount files - refusing to replace the existing copy"
    }

    $backup = "$PayloadDir.old-$([Guid]::NewGuid().ToString('N').Substring(0,6))"
    if (Test-Path -LiteralPath $PayloadDir) { Move-Item -LiteralPath $PayloadDir -Destination $backup }
    try {
        Move-Item -LiteralPath $extracted -Destination $PayloadDir
    } catch {
        if (Test-Path -LiteralPath $backup) { Move-Item -LiteralPath $backup -Destination $PayloadDir -Force }
        throw
    }
    if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Recurse -Force }

    [pscustomobject]@{
        upstream   = "https://github.com/$Owner/$Repo"
        branch     = $Branch
        commit     = $upstream.Sha
        commitDate = $upstream.Date
        fetchedAt  = (Get-Date).ToString('o')
        fileCount  = $fileCount
    } | ConvertTo-Json | Set-Content -LiteralPath $MetaPath -Encoding UTF8

    Write-Line "cs2-offsets: updated to $short ($($upstream.Date)), $fileCount files" 'Green'
    exit 0
} catch {
    Write-Line "ERROR: update failed: $($_.Exception.Message)" 'Red'
    exit 1
} finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue }
}
