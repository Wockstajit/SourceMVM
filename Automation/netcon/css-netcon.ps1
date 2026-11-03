<#
.SYNOPSIS
  Sends console commands to a hooked Counter-Strike: Source and prints what comes back.

.DESCRIPTION
  CS:S 64-bit has no engine netcon, so AfxHookSource serves one itself when launched with
  -mvmnetconport (launch-css.ps1 does this), on port 29030 by default. CS2 uses 29010 and Deadlock
  29020, so all three can be driven side by side.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File Automation\netcon\css-netcon.ps1 -CommandBatch "mirv_filmmaker health"
#>
[CmdletBinding()]
param(
    [string[]]$Commands = @(),
    [string]$CommandBatch = '',
    [int]$Port = 29030,
    [double]$ReadSeconds = 3.0,
    [switch]$Quiet
)

& (Join-Path $PSScriptRoot 'hook-netcon.ps1') -Port $Port -Commands $Commands -CommandBatch $CommandBatch `
    -ReadSeconds $ReadSeconds -Quiet:$Quiet `
    -NotListeningHint 'Launch CS:S with Automation\launch\launch-css.ps1, which starts the hook port.'
exit $LASTEXITCODE
