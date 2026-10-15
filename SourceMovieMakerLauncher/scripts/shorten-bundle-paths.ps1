<#
.SYNOPSIS
    Keeps every bundled file path short enough for the Windows installer and uninstaller.

.DESCRIPTION
    The NSIS setup and uninstaller are not long-path aware, so Windows holds them to 260
    characters. An upgrade runs the previous version's uninstaller, which moves every installed
    file into %TEMP%\nsXXXX.tmp\old-install\. If one path is too long the move fails, the
    uninstaller exits with code 2, and setup reports "cannot be closed" and then "Failed to
    uninstall old application files".

    Bundled media sets can carry very long folder and definition file names. Their names are
    not read by anything (sets are matched by their contents), so a folder or definition file
    longer than the limit gets a short prefix plus a hash of the original name. Other files in a
    set are referenced by name from inside it and are never renamed.

    Afterwards every file must fit in -MaxAppRelative characters, measured from the install
    folder (resources\bundle\...), or the build fails.
#>
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [int]$MaxAppRelative = 140,
    [int]$MaxSetName = 42,
    [int]$MaxDefinitionName = 44
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\')

function Get-ShortName([string]$name, [int]$keep) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $hash = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($name)))).Replace('-', '').Substring(0, 8).ToLowerInvariant()
    $prefix = $name.Substring(0, [Math]::Min($keep, $name.Length)).TrimEnd(' ', '.', '-', '_')
    "$prefix ~$hash"
}
function Long([string]$path) { '\\?\' + $path }

$renamed = 0
$sets = Join-Path $Root 'runtime\osu\Songs'
if (Test-Path -LiteralPath $sets) {
    foreach ($set in @(Get-ChildItem -LiteralPath $sets -Directory)) {
        $dir = $set.FullName
        foreach ($file in @(Get-ChildItem -LiteralPath (Long $dir) -File -Filter '*.osu')) {
            if ($file.Name.Length -le $MaxDefinitionName) { continue }
            $short = (Get-ShortName $file.BaseName ($MaxDefinitionName - 14)) + $file.Extension
            [IO.File]::Move((Long (Join-Path $dir $file.Name)), (Long (Join-Path $dir $short)))
            $renamed++
        }
        if ($set.Name.Length -gt $MaxSetName) {
            $short = Get-ShortName $set.Name ($MaxSetName - 10)
            [IO.Directory]::Move((Long $dir), (Long (Join-Path $sets $short)))
            $renamed++
        }
    }
}

$tooLong = @(Get-ChildItem -LiteralPath (Long $Root) -Recurse -File | ForEach-Object {
    $relative = 'resources\bundle\' + $_.FullName.Substring((Long $Root).Length + 1)
    if ($relative.Length -gt $MaxAppRelative) { "$($relative.Length)  $relative" }
})
if ($tooLong.Count) {
    throw "These bundled paths are longer than $MaxAppRelative characters inside the install folder, which breaks upgrades:`n  $($tooLong -join "`n  ")"
}
Write-Host "Bundle paths fit in $MaxAppRelative characters ($renamed names shortened)."
