<#
.SYNOPSIS
    Generates thumbnails for CS2's OWN skyboxes, so the in-game skybox picker shows a preview
    for the stock skies instead of a letter tile.

.DESCRIPTION
    STANDALONE BY DESIGN, and deliberately NOT part of FX/tools/build-skybox.ps1. That tool
    imports an image YOU supply; this one mines thumbnails out of the shipped game. Different
    inputs, different preconditions (this one needs Source2Viewer-CLI and the game VPK), and
    different cadence -- you run this once, and build-skybox.ps1 every time you make a sky. The
    repo rule is that a separate capability gets a separate entry point, so the small amount of
    .vtex boilerplate below is duplicated on purpose rather than shared.

    HOW IT WORKS
      1. Reads each stock sky material out of pak01_dir.vpk to find its sky texture.
      2. Extracts that texture and decodes it. It is a CUBEMAP, and Source2Viewer reprojects a
         cubemap back to a 2048x1024 equirectangular .exr -- which is exactly the shape a
         thumbnail wants.
      3. Compiles that .exr down to a 256px 2D texture in our own panorama/images subtree.

    Note step 2 must run WITHOUT --texture_decode_flags ForceLDR. With it, the faces decode to
    Bgra8888 first and the cube assembly then fails with "Cubemap faces must be RgbaF32 format".

    The output names are what AfxHookSource2/Filmmaker/World/SkyboxLibrary.cpp expects:
    builtin_<name>.vtex, addressed as s2r://panorama/images/filmmaker/skyboxes/builtin_<name>.vtex.
    Skies this script cannot process simply have no texture, and the UI falls back to a letter
    tile -- a missing thumbnail is never an error.

.EXAMPLE
    pwsh FX\tools\build-builtin-sky-previews.ps1
#>
[CmdletBinding()]
param(
    [string]$Cs2Dir = '',
    [string]$Vrf = "$env:USERPROFILE\Documents\Github Projects\ValveResourceFormat\CLI\bin\Release\Source2Viewer-CLI.exe",
    [string]$ResourceCompiler = '',
    # Restrict to specific skies (by the SkyboxLibrary name). Default: all of them.
    [string[]]$Only = @()
)

# CS2 install root. Empty means auto-detect (Steam registry + libraryfolders.vdf). This
# used to default to the author's own Steam library, a path no other machine has.
# Override with the parameter or $env:SOURCEMVM_CS2_DIR.
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) { $Cs2Dir = & (Join-Path $PSScriptRoot '..\..\Automation\lib\Resolve-Cs2Dir.ps1') }
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) {
    throw 'Counter-Strike 2 was not found. Pass the install root explicitly or set SOURCEMVM_CS2_DIR.'
}

$ErrorActionPreference = 'Stop'
function Fail([string]$m) { Write-Error $m; exit 1 }

# MUST match kBuiltins in AfxHookSource2/Filmmaker/World/SkyboxLibrary.cpp. If you add a sky
# there, add it here; a name present in one and not the other just means no thumbnail.
$BUILTINS = @(
    'sky_de_mirage', 'sky_de_dust2', 'sky_de_nuke', 'sky_de_vertigo', 'sky_de_overpass_01',
    'sky_de_annubis', 'sky_de_train03', 'sky_hr_aztec_02', 'sky_ar_baggage_01',
    'sky_cs15_daylight01_hdr', 'sky_csgo_cloudy01', 'sky_overcast_01', 'sky_rain_night_01',
    'sky_black'
)
if ($Only.Count) { $BUILTINS = $BUILTINS | Where-Object { $Only -contains $_ } }

if ([string]::IsNullOrWhiteSpace($ResourceCompiler)) {
    $ResourceCompiler = Join-Path $Cs2Dir 'game\bin\win64\resourcecompiler.exe'
}
foreach ($p in @($Vrf, $ResourceCompiler)) {
    if (-not (Test-Path -LiteralPath $p)) { Fail "Required tool not found: $p" }
}
$gameCsgo = Join-Path $Cs2Dir 'game\csgo'
$vpk = Join-Path $gameCsgo 'pak01_dir.vpk'
if (-not (Test-Path -LiteralPath $vpk)) { Fail "CS2 VPK not found: $vpk" }

$uiContent = Join-Path $Cs2Dir 'content\csgo\panorama\images\filmmaker\skyboxes'
New-Item -ItemType Directory -Force -Path $uiContent | Out-Null

function Write-PreviewVtex([string]$Path, [string]$RelImage, [string]$Format, [string]$ColorSpace,
                          [int]$NoLod = 0) {
    # A .vtex is DMX keyvalues2, NOT KV3.
    $body = @"
<!-- dmx encoding keyvalues2_noids 1 format vtex 1 -->
"CDmeVtex"
{
	"m_inputTextureArray" "element_array"
	[
		"CDmeInputTexture"
		{
			"m_name" "string" "InputTexture"
			"m_fileName" "string" "$RelImage"
			"m_colorSpace" "string" "$ColorSpace"
			"m_typeString" "string" "2D"
			"m_imageProcessorArray" "element_array"
			[
			]
		}
	]
	"m_outputTypeString" "string" "2D"
	"m_outputFormat" "string" "$Format"
	"m_outputClearColor" "vector4" "0 0 0 0"
	"m_nOutputMinDimension" "int" "0"
	"m_nOutputMaxDimension" "int" "256"
	"m_textureOutputChannelArray" "element_array"
	[
		"CDmeTextureOutputChannel"
		{
			"m_inputTextureArray" "string_array"
			[
				"InputTexture"
			]
			"m_srcChannels" "string" "rgba"
			"m_dstChannels" "string" "rgba"
			"m_mipAlgorithm" "CDmeImageProcessor"
			{
				"m_algorithm" "string" ""
				"m_stringArg" "string" ""
				"m_vFloat4Arg" "vector4" "0 0 0 0"
			}
			"m_outputColorSpace" "string" "srgb"
		}
	]
	"m_vClamp" "vector3" "0 0 0"
	"m_bNoLod" "bool" "$NoLod"
}
"@
    [IO.File]::WriteAllText($Path, $body, (New-Object Text.UTF8Encoding($false)))
}

$made = 0; $skipped = @()

foreach ($sky in $BUILTINS) {
    Write-Host "-- $sky" -ForegroundColor Cyan
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("mvmskyprev_" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    try {
        # 1. Which texture does this material sample?
        $matDump = & $Vrf -i $vpk -f "materials/skybox/$sky.vmat_c" -e vmat_c -b DATA 2>&1 | Out-String
        $m = [regex]::Match($matDump, 'resource:"(materials/skybox/[^"]+\.vtex)"')
        if (-not $m.Success) { $skipped += "$sky (no sky texture found in material)"; continue }
        $texRel = $m.Groups[1].Value                       # materials/skybox/foo_exr_1234.vtex
        $texLeaf = [IO.Path]::GetFileNameWithoutExtension($texRel)

        # 2. Extract, then decode. Decoding straight out of the VPK throws a NullReferenceException,
        #    so the file is pulled to disk first and decoded standalone.
        & $Vrf -i $vpk -o (Join-Path $tmp 'ext') -f $texRel -e vtex_c 2>&1 | Out-Null
        $texFile = Get-ChildItem -LiteralPath $tmp -Filter "$texLeaf.vtex_c" -Recurse -ErrorAction SilentlyContinue |
                   Select-Object -First 1 -ExpandProperty FullName
        if (-not $texFile) { $skipped += "$sky (could not extract $texRel)"; continue }

        # No ForceLDR here -- see the note at the top of this file.
        & $Vrf -i $texFile -o (Join-Path $tmp 'dec') -d 2>&1 | Out-Null
        $exr = Get-ChildItem -LiteralPath $tmp -Filter '*.exr' -Recurse -ErrorAction SilentlyContinue |
               Select-Object -First 1 -ExpandProperty FullName
        if (-not $exr) { $skipped += "$sky (cubemap did not decode to an .exr)"; continue }

        # 3. Compile the equirect to a small FLOAT texture, tone-map it, then compile that.
        #
        # The float intermediate is not an optimisation, it is the whole point. Compiling the .exr
        # straight to DXT1 with an sRGB output colour space applies a transfer curve, not a tone
        # map, and sky luminance above 1.0 then clamps -- every thumbnail comes out white. Only
        # Source2Viewer's ForceLDR decode actually tone-maps.
        $leaf = "builtin_$sky"
        Copy-Item -LiteralPath $exr -Destination (Join-Path $uiContent "$leaf.exr") -Force

        $tmpVtex = Join-Path $uiContent "$($leaf)_hdrtmp.vtex"
        Write-PreviewVtex -Path $tmpVtex -RelImage "panorama/images/filmmaker/skyboxes/$leaf.exr" `
                          -Format 'BC6H' -ColorSpace 'linear'
        & $ResourceCompiler -i $tmpVtex -r -game $gameCsgo -nop4 -fshallow2 2>&1 | Out-Null
        $tmpCompiled = Join-Path $gameCsgo "panorama\images\filmmaker\skyboxes\$($leaf)_hdrtmp.vtex_c"
        if (Test-Path -LiteralPath $tmpCompiled) {
            & $Vrf -i $tmpCompiled -o (Join-Path $tmp 'ldr') -d --texture_decode_flags ForceLDR 2>&1 | Out-Null
        }
        $png = Get-ChildItem -LiteralPath $tmp -Filter '*.png' -Recurse -ErrorAction SilentlyContinue |
               Select-Object -First 1 -ExpandProperty FullName
        Remove-Item -LiteralPath $tmpVtex -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $tmpCompiled -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath (Join-Path $uiContent "$leaf.exr") -Force -ErrorAction SilentlyContinue
        if (-not $png) { $skipped += "$sky (could not tone-map the thumbnail)"; continue }

        # RGBA8888 + NoLod, NOT DXT1: Panorama's UI path renders a compressed sRGB texture washed
        # out to near-white, even though it decodes correctly outside the game. Valve's own
        # panorama/images textures are uncompressed RGBA8888 with VTEX_FLAG_NO_LOD.
        Copy-Item -LiteralPath $png -Destination (Join-Path $uiContent "$leaf.png") -Force
        $vtex = Join-Path $uiContent "$leaf.vtex"
        Write-PreviewVtex -Path $vtex -RelImage "panorama/images/filmmaker/skyboxes/$leaf.png" `
                          -Format 'RGBA8888' -ColorSpace 'srgb' -NoLod 1
        & $ResourceCompiler -i $vtex -r -game $gameCsgo -nop4 -fshallow2 2>&1 | Out-Null

        $compiled = Join-Path $gameCsgo "panorama\images\filmmaker\skyboxes\$leaf.vtex_c"
        if (Test-Path -LiteralPath $compiled) {
            $made++
            Write-Host "   ok -> $leaf.vtex_c" -ForegroundColor Green
        } else {
            $skipped += "$sky (thumbnail compile produced nothing)"
        }
    } catch {
        $skipped += "$sky ($($_.Exception.Message))"
    } finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ""
Write-Host "Built $made built-in skybox thumbnail(s)." -ForegroundColor Green
if ($skipped.Count) {
    # Named, not silent: a stock sky quietly losing its thumbnail after a CS2 update should be
    # visible here rather than discovered by noticing a letter tile in the panel.
    Write-Host "Skipped (these keep a letter tile in the picker):" -ForegroundColor Yellow
    $skipped | ForEach-Object { Write-Host "  $_" }
}
