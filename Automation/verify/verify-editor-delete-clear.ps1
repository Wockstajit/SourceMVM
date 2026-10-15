param()

$ErrorActionPreference = 'Stop'

$automationRoot = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $automationRoot
$movieMode = Join-Path $root 'AfxHookSource2\Filmmaker\Movie\MovieMode.cpp'
$cameraEditorJs = Join-Path $root 'AfxHookSource2\Filmmaker\Panorama\CameraEditorJs.h'
$graphEditorJs = Join-Path $root 'AfxHookSource2\Filmmaker\Panorama\GraphEditorJs.h'
$cameraPath = Join-Path $root 'AfxHookSource2\Filmmaker\Movie\CameraPath.cpp'
# grapheditor grammar moved to its own dispatcher TU (file-per-feature split)
$command = Join-Path $root 'AfxHookSource2\Filmmaker\GraphEditorCommand.cpp'

$mm = Get-Content -Raw -Path $movieMode
$cej = Get-Content -Raw -Path $cameraEditorJs
$gej = Get-Content -Raw -Path $graphEditorJs
$cp = Get-Content -Raw -Path $cameraPath
$fc = Get-Content -Raw -Path $command

if ($mm -notmatch 'kVK_DELETE' -or $mm -notmatch 'mirv_filmmaker grapheditor delsel') {
    throw 'Delete/Backspace must route graph selected-key deletion.'
}

if ($cej -notmatch 'CLEAR ALL CAMERA PATHS\?' -or $cej -notmatch 'confirmOverlay' -or $cej -notmatch 'mirv_filmmaker camtl clear') {
    throw 'Camera editor must have a full-screen clear-all confirmation overlay wired to camtl clear.'
}

if (-not $gej.Contains("'Delete'") -or -not $gej.Contains("em === 'delete'") -or -not $gej.Contains("cmd('delsel')")) {
    throw 'Graph right-click menu must expose Delete and dispatch delsel.'
}

if ($cp -notmatch 'GraphEditorExperimentHudRef\(\)\.CmdReseed\(\)' -or $cp -notmatch 'GraphEditorExperimentHudRef\(\)\.CmdClear\(\)') {
    throw 'CameraPath delete/delete-all must keep graph model in sync.'
}

if ($fc -notmatch '"clear"' -or $fc -notmatch 'ge\.CmdClear\(\)') {
    throw 'GraphEditor command dispatch must expose grapheditor clear.'
}

Write-Host 'editor delete/clear regression checks passed'




