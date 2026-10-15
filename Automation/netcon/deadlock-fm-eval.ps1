<#
.SYNOPSIS
  Runs Panorama JavaScript inside a hooked Deadlock.

.DESCRIPTION
  The Deadlock counterpart of Automation/netcon/fm-eval.ps1: wraps `mirv_deadlock ui_eval "<js>"`.

  Two things to know about the JS you pass:

  1. Use SINGLE quotes only. Double quotes are the command-argument delimiter, so a double-quoted
     string inside the JS terminates the argument early.
  2. Unlike CS2's engine console, there is no 256-byte truncation here. AfxHookDeadlock hosts its
     own netcon channel rather than using the engine's, and that limit was the engine's -- which is
     why the CS2 UI has to be driven through short registered helpers instead of inline JS. Longer
     snippets are fine.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File Automation\netcon\deadlock-fm-eval.ps1 `
      -Js "$.Msg('markers: ' + $.MvmWorkspace.root().GetAttributeString('state','{}'))"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Js,
    [int]$Port = 29020,
    [double]$ReadSeconds = 1.5
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($Js -match '"') {
    Write-Host "The JS contains a double quote, which ends the command argument early. Use single quotes." -ForegroundColor Yellow
    exit 2
}

& (Join-Path $PSScriptRoot 'deadlock-netcon.ps1') `
    -CommandBatch ("mirv_deadlock ui_eval `"$Js`"") -Port $Port -ReadSeconds $ReadSeconds
exit $LASTEXITCODE
