<#
.SYNOPSIS
  Drive a hooked Deadlock over netcon, then capture its window to PNG.

.DESCRIPTION
  A screenshot of a game is only evidence if you know what the game was doing when it was taken.
  This script owns that pairing: it sends the setup commands, waits for the frames they need to
  land, captures, and writes a sidecar recording exactly which commands produced the image.

  Its own entry point rather than a flag on the CS2 verifiers, per the repository rule -- a Deadlock
  session and a CS2 session are unrelated events and neither script should be able to drive the
  wrong game. The pixel-grabbing itself is NOT duplicated: capture-game-window.ps1 already takes
  -Process and is game-neutral, so this calls it.

  THE PART THAT IS EASY TO GET WRONG. A console command sent over netcon is queued and executed by
  the MAIN-THREAD PUMP, so nothing it asks for exists until the game reaches its next frame -- and
  a seek takes far longer than one. Capturing immediately after sending photographs the state
  BEFORE the command. -SettleSeconds is that wait, and it defaults high enough to survive a seek.

.PARAMETER Out
  Where the PNG goes. A directory gets a timestamped name; a .png path is used verbatim.

.PARAMETER Setup
  Commands to send BEFORE capturing, separated by ';;'. The usual shape is a pause, a seek, and
  whichever workspace you want in the shot.

.PARAMETER After
  Commands to send AFTER capturing, separated by ';;'. Use it to put the session back.

.PARAMETER Pause
  Pause the replay before capturing. On by default: a moving world makes two captures of the same
  thing look different for reasons that have nothing to do with what you changed.

.PARAMETER HideUi
  Close the workspace before capturing, for a clean plate of the game itself.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File Automation\verify\verify-deadlock-screenshot.ps1 `
      -Out Automation\runs\deadlock-ui -Setup "mirv_deadlock workspace camera"

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File Automation\verify\verify-deadlock-screenshot.ps1 `
      -Out shot.png -Setup "demo_gototick 5700;;mirv_deadlock workspace render" -SettleSeconds 6
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Setup = '',
    [string]$After = '',
    [int]$Port = 29020,
    [string]$Process = 'deadlock',
    [ValidateSet('client', 'window')][string]$Mode = 'client',
    # Generous on purpose: a netcon command is executed by the main-thread pump, and a seek can
    # take several seconds to replay its packets. Capturing early photographs the previous state.
    [double]$SettleSeconds = 3.0,
    [switch]$NoPause,
    [switch]$HideUi,
    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$netcon = Join-Path $repoRoot 'Automation\netcon\deadlock-netcon.ps1'
$capture = Join-Path $repoRoot 'Automation\capture\capture-game-window.ps1'

foreach ($tool in @($netcon, $capture)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "missing helper: $tool" }
}

function Write-Note([string]$text) {
    if (-not $Quiet) { Write-Host $text }
}

# The window has to exist before anything else is worth trying. Saying so here, by name, beats a
# capture that fails later with a message about a null handle.
$proc = Get-Process -Name $Process -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) {
    throw "No '$Process' process with a visible window. Launch it with Automation\launch\deadlock.bat first."
}

function Send-Batch([string]$batch) {
    if (-not $batch) { return '' }
    $reply = & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass `
        -File $netcon -Port $Port -CommandBatch $batch -Quiet 2>&1 | Out-String
    return $reply
}

# --- resolve the output path ----------------------------------------------------------------
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ($Out -match '\.png$') {
    $outPath = $Out
} else {
    if (-not (Test-Path -LiteralPath $Out)) { New-Item -ItemType Directory -Force -Path $Out | Out-Null }
    $outPath = Join-Path $Out "deadlock-$stamp.png"
}
$outDir = Split-Path -Parent $outPath
if ($outDir -and -not (Test-Path -LiteralPath $outDir)) {
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
}

# --- setup ------------------------------------------------------------------------------------
$steps = New-Object System.Collections.Generic.List[string]
if (-not $NoPause) { $steps.Add('demo_pause') }
if ($HideUi) { $steps.Add('mirv_deadlock workspace off') }
if ($Setup) { foreach ($c in ($Setup -split ';;')) { if ($c.Trim()) { $steps.Add($c.Trim()) } } }

$setupReply = ''
if ($steps.Count -gt 0) {
    $batch = ($steps -join ';;')
    Write-Note "setup: $batch"
    $setupReply = Send-Batch $batch
}

# Let the main-thread pump actually run the commands before photographing the result.
Start-Sleep -Milliseconds ([int]($SettleSeconds * 1000))

# --- capture ------------------------------------------------------------------------------------
$captureReply = & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass `
    -File $capture -Out $outPath -Process $Process -Mode $Mode 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) { throw "capture failed: $captureReply" }

if (-not (Test-Path -LiteralPath $outPath)) { throw "capture produced no file: $outPath" }
$png = Get-Item -LiteralPath $outPath

# A PrintWindow capture can succeed and still hand back an all-black frame when the swapchain is
# not composited the way it expected. A few hundred bytes of PNG is the tell, and catching it here
# is the difference between a failed verification and a verification that silently proved nothing.
if ($png.Length -lt 8192) {
    Write-Warning "the capture is only $($png.Length) bytes -- probably a blank frame. Check that the window is not minimised."
}

# --- state alongside the image -------------------------------------------------------------------
# The health block is the sidecar's whole point: an image on its own cannot say which build, which
# tick, or whether the preview was actually scaling when it was taken.
$health = Send-Batch 'mirv_deadlock health'

$sidecar = [System.IO.Path]::ChangeExtension($outPath, '.txt')
@(
    "captured : $stamp"
    "process  : $Process (pid $($proc.Id))"
    "mode     : $Mode"
    "png      : $outPath ($($png.Length) bytes)"
    "capture  : $($captureReply.Trim())"
    "setup    : $(if ($steps.Count) { $steps -join ' ;; ' } else { '(none)' })"
    ''
    '--- setup reply ---'
    $setupReply.TrimEnd()
    ''
    '--- mirv_deadlock health ---'
    $health.TrimEnd()
) | Set-Content -LiteralPath $sidecar -Encoding UTF8

if ($After) {
    Write-Note "after: $After"
    Send-Batch $After | Out-Null
}

Write-Note "wrote $outPath"
Write-Note "wrote $sidecar"
Write-Output $outPath
