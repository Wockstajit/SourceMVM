param()

$ErrorActionPreference = 'Stop'

$automationRoot = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $automationRoot
$filmmaker = Join-Path $root 'AfxHookSource2\Filmmaker\Filmmaker.cpp'
$playingDemoPath = Join-Path $root 'AfxHookSource2\Filmmaker\Demo\PlayingDemoPath.cpp'

$fm = Get-Content -Raw -Path $filmmaker
$pd = Get-Content -Raw -Path $playingDemoPath

if ($fm -notmatch 'if \(!playing\)\s*\r?\n\s*return L"";') {
    throw 'PlayingDemoPath() must return empty when no demo is playing; stale Watch() fallback would leak old cam paths.'
}

if ($pd -match 'if \(demo == s_lastDemo\) return s_lastPath;') {
    throw 'ResolvePlayingDemoPath() must not return the pointer cache before sampling the engine path getter.'
}

if ($pd -notmatch 'SafeGetEngineDemoPath\(ebuf, \(int\)sizeof\(ebuf\)\)') {
    throw 'ResolvePlayingDemoPath() must sample the engine demo path getter.'
}

if ($pd -notmatch 'if \(!resolved\.empty\(\)\)\s*\{[\s\S]*?s_lastPath = resolved;[\s\S]*?return s_lastPath;[\s\S]*?\}') {
    throw 'ResolvePlayingDemoPath() must update the cached path when the engine getter returns a path.'
}

Write-Host 'demo transition guard regression checks passed'




