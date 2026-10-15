# Creates a private executable/DLL tree. Shared game assets remain junctions.
[CmdletBinding()]
param([string]$Cs2Dir = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $Cs2Dir) { $Cs2Dir = & (Join-Path $repo 'Automation/lib/Resolve-Cs2Dir.ps1') }
if (-not $Cs2Dir) { throw 'CS2 was not found.' }
$source = (Resolve-Path -LiteralPath $Cs2Dir).Path
$target = Join-Path $repo 'build/dlss5-cs2'
$marker = Join-Path $target 'source-mvm-isolation.json'
foreach ($directory in @($target,(Join-Path $target 'game'),(Join-Path $target 'game/bin'),(Join-Path $target 'game/bin/win64'))) {
    if ((Test-Path -LiteralPath $directory) -and
        ((Get-Item -LiteralPath $directory).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Refusing a redirected private binary directory: $directory"
    }
}
if (Test-Path -LiteralPath $target) {
    if (-not (Test-Path -LiteralPath $marker)) { throw "Refusing an unowned folder: $target" }
    $previous = Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json
    if ($previous.source -ne $source) { throw 'The existing private tree belongs to a different CS2 install.' }
}
foreach ($process in Get-CimInstance Win32_Process -Filter "Name='cs2.exe'") {
    if ($process.ExecutablePath -and $process.ExecutablePath.StartsWith($target + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Close the private DLSS CS2 session before refreshing its binaries.'
    }
}
$sourceBin = Join-Path $source 'game/bin'
if (-not (Test-Path -LiteralPath (Join-Path $sourceBin 'win64/cs2.exe'))) { throw 'Invalid CS2 install.' }
# Refuse a source that already auto-loads graphics mods. Never propagate those silently.
foreach ($name in @('d3d11.dll','dxgi.dll','version.dll','dinput8.dll')) {
    if (Test-Path -LiteralPath (Join-Path $sourceBin "win64/$name")) {
        throw "Source bin contains $name. Review the existing proxy before preparing isolation."
    }
}
New-Item -ItemType Directory -Path $target -Force | Out-Null
@{ source=$source; preparedAt=(Get-Date).ToString('o'); sharedAssets=$true } |
    ConvertTo-Json | Set-Content -LiteralPath $marker -Encoding UTF8
$game = Join-Path $target 'game'
New-Item -ItemType Directory -Path $game -Force | Out-Null
# No hardlinks: DLL writes in the private tree must never modify Steam's files.
$privateBin = Join-Path $game 'bin'
if ((Test-Path -LiteralPath $privateBin) -and
    ((Get-Item -LiteralPath $privateBin).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw 'Private bin must be a real directory.'
}
& robocopy.exe $sourceBin $privateBin /E /COPY:DAT /DCOPY:DAT /R:1 /W:1 /XJ /NFL /NDL /NJH /NJS /NP
if ($LASTEXITCODE -ge 8) { throw "Binary copy failed: $LASTEXITCODE" }
foreach ($entry in Get-ChildItem -LiteralPath (Join-Path $source 'game')) {
    if ($entry.Name -eq 'bin') { continue }
    $destination = Join-Path $game $entry.Name
    if ($entry.PSIsContainer) {
        if (-not (Test-Path -LiteralPath $destination)) {
            New-Item -ItemType Junction -Path $destination -Target $entry.FullName | Out-Null
        } elseif ((Get-Item -LiteralPath $destination).Target -ne $entry.FullName) {
            throw "Unexpected asset junction: $destination"
        }
    } else { Copy-Item -LiteralPath $entry.FullName -Destination $destination -Force }
}
foreach ($entry in Get-ChildItem -LiteralPath $source -File) {
    Copy-Item -LiteralPath $entry.FullName -Destination (Join-Path $target $entry.Name) -Force
}
Write-Host "Private CS2 executable tree prepared: $target"
Write-Host 'DLSS is not installed by this step. Assets and Steam settings are still shared.'
exit 0
