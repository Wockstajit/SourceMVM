param(
    [Parameter(Mandatory = $true)]
    [string]$TakeDir,

    [double]$MinimumSeconds = 10
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $TakeDir -PathType Container)) {
    throw "Take directory does not exist: $TakeDir"
}

$manifestPath = Join-Path $TakeDir 'manifest.json'
$framesPath = Join-Path $TakeDir 'frames.csv'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Missing manifest.json: $manifestPath"
}
if (-not (Test-Path -LiteralPath $framesPath -PathType Leaf)) {
    throw "Missing frames.csv: $framesPath"
}

$manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
if ($manifest.status -ne 'completed') {
    throw "Render is not completed (status=$($manifest.status))."
}

$expected = [int]$manifest.framesExpected
$written = [int]$manifest.framesWritten
$fps = [double]$manifest.outputFps
if ($expected -le 0 -or $written -le 0) {
    throw "Manifest has invalid frame counts (expected=$expected written=$written)."
}
if ($expected -ne $written) {
    throw "Manifest frame count mismatch (expected=$expected written=$written)."
}
if ($fps -le 0) {
    throw "Manifest has invalid output FPS: $fps"
}
$minimumFrames = [int][Math]::Ceiling($fps * $MinimumSeconds)
if ($expected -lt $minimumFrames) {
    throw "Capture is shorter than requested: $expected frames at $fps fps (need at least $minimumFrames for $MinimumSeconds seconds)."
}

$frameRows = @(Import-Csv -LiteralPath $framesPath)
if ($frameRows.Count -ne $written) {
    throw "frames.csv row count mismatch (rows=$($frameRows.Count) manifest=$written)."
}
for ($i = 0; $i -lt $frameRows.Count; $i++) {
    $index = [int]$frameRows[$i].frame_index
    if ($index -ne $i) {
        throw "frames.csv frame_index is not contiguous at row $i (found $index)."
    }
    if ($i -gt 0 -and [int]$frameRows[$i].demo_tick -lt [int]$frameRows[$i - 1].demo_tick) {
        throw "frames.csv demo_tick moves backwards at frame $i."
    }
}

$ffprobe = Get-Command ffprobe -ErrorAction SilentlyContinue
if (-not $ffprobe) {
    throw 'ffprobe is required by this verifier to prove output resolution. Install FFmpeg or add ffprobe.exe to PATH.'
}

function Get-ImageSize([string]$Path) {
    $raw = (& $ffprobe.Source '-v' 'error' '-select_streams' 'v:0' '-show_entries' 'stream=width,height' '-of' 'csv=p=0:s=x' $Path 2>$null | Out-String).Trim()
    if ($raw -notmatch '^\d+x\d+$') {
        throw "Unable to read image resolution from $Path (ffprobe returned '$raw')."
    }
    return $raw
}

# The take folder holds one folder per pass type; an image sequence keeps each variant's frames
# in <type>\<variant>\ (manifest streams[].folder).
$streamResults = @()
foreach ($stream in @($manifest.streams)) {
    if (-not $stream.folder) {
        throw "Manifest stream '$($stream.name)' has no output folder (a take from before the flat layout?)."
    }
    $streamDir = Join-Path $TakeDir ([string]$stream.folder)
    $streamFiles = @()
    if (Test-Path -LiteralPath $streamDir -PathType Container) {
        $streamFiles = @(Get-ChildItem -LiteralPath $streamDir -File | Where-Object {
            $_.Extension.ToLowerInvariant() -in @('.tga', '.png', '.jpg', '.jpeg', '.exr', '.bmp', '.tiff')
        })
    }
    $streamFiles = @($streamFiles | Sort-Object FullName)
    if ($streamFiles.Count -ne $expected) {
        throw "Stream '$($stream.name)' has $($streamFiles.Count) images; expected $expected."
    }

    $names = @($streamFiles | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_.Name) })
    # The classic writer uses 00000.. while FFmpeg image-sequence presets use
    # frame_000001... Accept either convention, but prove that the numeric suffix is
    # contiguous and that the non-numeric prefix/width never changes.
    if ($names[0] -notmatch '^(.*?)(\d+)$') {
        throw "Stream '$($stream.name)' has an unrecognized first frame name '$($names[0])'."
    }
    $namePrefix = $Matches[1]
    $nameWidth = $Matches[2].Length
    $firstNumber = [int]$Matches[2]
    if ($firstNumber -notin @(0, 1)) {
        throw "Stream '$($stream.name)' starts at unexpected frame number $firstNumber."
    }
    for ($i = 0; $i -lt $names.Count; $i++) {
        if ($names[$i] -notmatch '^(.*?)(\d+)$' -or $Matches[1] -ne $namePrefix -or
            $Matches[2].Length -ne $nameWidth -or [int]$Matches[2] -ne ($firstNumber + $i)) {
            throw "Stream '$($stream.name)' frame name is not contiguous at index $i ('$($names[$i])')."
        }
    }
    $firstSize = Get-ImageSize $streamFiles[0].FullName
    $lastSize = Get-ImageSize $streamFiles[$streamFiles.Count - 1].FullName
    if ($firstSize -ne $lastSize) {
        throw "Stream '$($stream.name)' changes resolution ($firstSize -> $lastSize)."
    }

    $streamResults += [pscustomobject]@{
        Token = $stream.name
        Scope = $stream.elements
        Frames = $streamFiles.Count
        Resolution = $firstSize
        First = $streamFiles[0].FullName
        Last = $streamFiles[$streamFiles.Count - 1].FullName
        Names = $names
    }
}

if ($streamResults.Count -lt 1) {
    throw 'Manifest has no selected streams.'
}
$referenceNames = $streamResults[0].Names
foreach ($result in $streamResults | Select-Object -Skip 1) {
    if ($result.Names.Count -ne $referenceNames.Count) {
        throw "Stream '$($result.Token)' is not frame aligned with '$($streamResults[0].Token)'."
    }
    for ($i = 0; $i -lt $referenceNames.Count; $i++) {
        if ($result.Names[$i] -ne $referenceNames[$i]) {
            throw "Stream '$($result.Token)' frame $i is '$($result.Names[$i])'; expected '$($referenceNames[$i])'."
        }
    }
}

$resolutions = @($streamResults | Select-Object -ExpandProperty Resolution -Unique)
if ($resolutions.Count -ne 1) {
    throw "Selected streams do not share one resolution: $($resolutions -join ', ')"
}

Write-Host "render stream verification passed"
Write-Host ("  frames={0} fps={1} duration={2:N3}s resolution={3}" -f $expected, $fps, ($expected / $fps), $resolutions[0])
foreach ($result in $streamResults) {
    Write-Host ("  {0} [{1}] frames={2} folder={3}" -f $result.Token, $result.Scope, $result.Frames, (Split-Path -Leaf (Split-Path -Parent $result.First)))
    Write-Host ("    proofFirst={0}" -f $result.First)
    Write-Host ("    proofLast={0}" -f $result.Last)
}
