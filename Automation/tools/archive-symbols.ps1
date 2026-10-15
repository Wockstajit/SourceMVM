<#
.SYNOPSIS
    Archives shipped hook DLLs and their PDBs in a Microsoft-layout symbol store.

.DESCRIPTION
    Crash reports carry frames as "AfxHookSource2.dll+0x4e24e0" plus the DLL's symbol-store key
    ("Hook ID", or "id" in the build line). Resolving that to a function needs the PDB of that
    exact link, and releases never ship PDBs. The packaging scripts call this before they strip
    them, so every shipped build stays resolvable:

        <store>\AfxHookSource2.dll\<link stamp><image size>\AfxHookSource2.dll
        <store>\AfxHookSource2.pdb\<pdb guid><age>\AfxHookSource2.pdb

    That is the layout DbgHelp reads through "srv*<store>", which is how
    Automation\tools\discord-reports.py resolves report stacks. The store is git-ignored; keep it
    (or point SOURCEMVM_SYMBOL_STORE at a backed-up folder): a lost PDB cannot be rebuilt.

.PARAMETER BinDir
    Folder holding the DLLs to archive (the staged bin\x64).

.PARAMETER PdbDir
    Folder holding their PDBs (the staged pdb\x64).

.PARAMETER Names
    DLL file names to archive. Default: the hook.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BinDir,
    [Parameter(Mandatory)][string]$PdbDir,
    [string[]]$Names = @('AfxHookSource2.dll'),
    [string]$Store
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Store) { $Store = $env:SOURCEMVM_SYMBOL_STORE }
if (-not $Store) { $Store = Join-Path $repoRoot 'Automation\symbols' }

# Link stamp, image size and CodeView (RSDS) record of a PE32+ image.
function Read-PeIdentity([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $pe = [BitConverter]::ToInt32($bytes, 0x3C)
    if ([BitConverter]::ToUInt32($bytes, $pe) -ne 0x00004550) { throw "Not a PE image: $Path" }
    $stamp = [BitConverter]::ToUInt32($bytes, $pe + 8)
    $optional = $pe + 24
    if ([BitConverter]::ToUInt16($bytes, $optional) -ne 0x20B) { throw "Not a 64-bit image: $Path" }
    $sizeOfImage = [BitConverter]::ToUInt32($bytes, $optional + 56)
    # Data directory 6 (debug) of a PE32+ optional header sits at offset 112 + 6 * 8.
    $debugRva = [BitConverter]::ToUInt32($bytes, $optional + 112 + 48)
    $debugSize = [BitConverter]::ToUInt32($bytes, $optional + 112 + 52)
    $sections = $optional + [BitConverter]::ToUInt16($bytes, $pe + 20)
    $sectionCount = [BitConverter]::ToUInt16($bytes, $pe + 6)
    $rvaToFile = {
        param([uint32]$rva)
        for ($i = 0; $i -lt $sectionCount; ++$i) {
            $s = $sections + $i * 40
            $va = [BitConverter]::ToUInt32($bytes, $s + 12)
            $raw = [BitConverter]::ToUInt32($bytes, $s + 16)
            $rawPtr = [BitConverter]::ToUInt32($bytes, $s + 20)
            if ($rva -ge $va -and $rva -lt $va + [Math]::Max($raw, 1)) { return [int]($rva - $va + $rawPtr) }
        }
        return -1
    }
    $guid = $null; $age = 0; $pdbName = $null
    $dir = & $rvaToFile $debugRva
    for ($entry = 0; $dir -ge 0 -and $entry -lt [int]($debugSize / 28); ++$entry) {
        $e = $dir + $entry * 28
        if ([BitConverter]::ToUInt32($bytes, $e + 12) -ne 2) { continue } # IMAGE_DEBUG_TYPE_CODEVIEW
        $cv = [BitConverter]::ToInt32($bytes, $e + 24)
        if ([BitConverter]::ToUInt32($bytes, $cv) -ne 0x53445352) { continue } # 'RSDS'
        $guidBytes = New-Object byte[] 16
        [Array]::Copy($bytes, $cv + 4, $guidBytes, 0, 16)
        $guid = $guidBytes
        $age = [BitConverter]::ToUInt32($bytes, $cv + 20)
        $end = [Array]::IndexOf($bytes, [byte]0, $cv + 24)
        $pdbName = [IO.Path]::GetFileName([Text.Encoding]::UTF8.GetString($bytes, $cv + 24, $end - $cv - 24))
        break
    }
    if (-not $guid) { throw "No CodeView record (built without /DEBUG?): $Path" }
    [pscustomobject]@{
        ImageKey = ('{0:X8}{1:x}' -f $stamp, $sizeOfImage)
        PdbKey   = ((New-Object Guid (, $guid)).ToString('N').ToUpperInvariant() + ('{0:X}' -f $age))
        PdbName  = $pdbName
        Guid     = $guid
    }
}

foreach ($name in $Names) {
    $dll = Join-Path $BinDir $name
    if (-not (Test-Path -LiteralPath $dll)) { throw "Not in the staged build: $dll" }
    $id = Read-PeIdentity $dll
    $pdb = Join-Path $PdbDir $id.PdbName
    if (-not (Test-Path -LiteralPath $pdb)) { throw "No PDB beside the staged build: $pdb" }
    # A PDB from another link would archive cleanly and then resolve every frame to the wrong
    # function. Its info stream holds the same GUID the DLL's CodeView record names.
    $pdbText = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes($pdb))
    $guidText = [Text.Encoding]::GetEncoding(28591).GetString($id.Guid)
    if ($pdbText.IndexOf($guidText, [StringComparison]::Ordinal) -lt 0) {
        throw "$pdb does not belong to $dll (its GUID is not in the PDB). Rebuild and install both together."
    }
    $dllTarget = Join-Path $Store "$name\$($id.ImageKey)"
    $pdbTarget = Join-Path $Store "$($id.PdbName)\$($id.PdbKey)"
    New-Item -ItemType Directory -Path $dllTarget, $pdbTarget -Force | Out-Null
    Copy-Item -LiteralPath $dll -Destination (Join-Path $dllTarget $name) -Force
    Copy-Item -LiteralPath $pdb -Destination (Join-Path $pdbTarget $id.PdbName) -Force
    Write-Host "Symbols archived: $name id $($id.ImageKey) -> $Store"
}
