[CmdletBinding()]
param(
    # Poses separated by ';', each "x,y,z,pitch,yaw,roll". One string so it survives powershell.exe -File.
    [Parameter(Mandatory = $true)][string]$Poses,
    # Variant names separated by ','; each needs a command in -VariantCommands.
    [string]$Variants = 'native,live',
    # "name=cmd;;cmd" entries separated by '|'.
    [string]$VariantCommands = 'native=mirv_relight off|live=mirv_relight on',
    [string]$Tag = 'shot',
    [string]$OutputDir = 'Automation/output/lighting-live',
    [int]$Port = 29010,
    [int]$SettleMs = 900,
    # 'setpos' drives an offline map (noclip camera); 'mirv' drives a demo's free camera.
    [ValidateSet('setpos', 'mirv')][string]$Camera = 'setpos'
)
# Free-cam A/B capture for lighting work. For every pose it sends the pose to the running game and
# grabs the client window once per variant with the demo HUD hidden, so paired images differ only
# in the lighting variant. Files are <OutputDir>/<Tag>-<pose#>-<variant>.png.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$net = Join-Path $repo 'Automation/netcon/cs2-netcon.ps1'
$cap = Join-Path $repo 'Automation/capture/capture-game-window.ps1'
$out = Join-Path $repo $OutputDir
New-Item -ItemType Directory -Force -Path $out | Out-Null
$byName = @{}
foreach ($v in $VariantCommands -split '\|') { $i = $v.IndexOf('='); $byName[$v.Substring(0, $i)] = $v.Substring($i + 1) }
function Net([string]$batch) { & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $net -Port $Port -CommandBatch $batch | Out-Null }
Net 'cl_drawhud 0;;r_drawviewmodel 0;;mirv_filmmaker hud off'
$n = 0
foreach ($pose in $Poses -split ';') {
    $n++
    $p = $pose -split ','
    if ($p.Count -ne 6) { throw "Pose '$pose' needs x,y,z,pitch,yaw,roll" }
    if ($Camera -eq 'mirv') { Net ("mirv_input position {0} {1} {2};;mirv_input angles {3} {4} {5}" -f $p) }
    else { Net ("setpos_exact {0} {1} {2};;setang_exact {3} {4} {5}" -f $p); Start-Sleep -Milliseconds 400 }
    foreach ($variant in $Variants -split ',') {
        if (-not $byName.ContainsKey($variant)) { throw "No command for variant '$variant'" }
        Net $byName[$variant]
        Start-Sleep -Milliseconds $SettleMs
        $file = Join-Path $out ("{0}-{1:00}-{2}.png" -f $Tag, $n, $variant)
        & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $cap -Out $file | Out-Null
        Write-Host $file
    }
}
