# Runs Panorama JS in the Filmmaker UI context via the netconport REPL
# (mirv_filmmaker ui_eval). The JS is sent as a single double-quoted console
# arg, so use SINGLE quotes inside your JS (no double quotes).
#
#   pwsh Automation\netcon\fm-eval.ps1 -Js "$.Msg('hi ' + (typeof $.GetContextPanel().BLoadLayout))"
param(
    [Parameter(Mandatory=$true)][string]$Js,
    [int]$Port = 29010,
    [double]$ReadSeconds = 2.0
)
$cmd = 'mirv_filmmaker ui_eval "' + $Js + '"'
& (Join-Path $PSScriptRoot 'cs2-netcon.ps1') -Port $Port -Commands $cmd -ReadSeconds $ReadSeconds
