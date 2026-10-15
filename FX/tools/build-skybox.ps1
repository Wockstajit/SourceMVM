<#
.SYNOPSIS
    Turns an ordinary image into a CS2 skybox, compiled and ready to hot-swap in-game.

.DESCRIPTION
    STANDALONE BY DESIGN. This is its own tool with its own inputs, outputs and entry point --
    it must never be folded into FX/tools/convert-fx-source1.ps1 (the particle-FX
    converter) or into build.bat. Skybox import is a separate capability; the repo rule is that
    a new capability gets its own thing. It shares only the CS2 resourcecompiler, which is a
    tool, not shared build logic.

    WHAT IT DOES
      1. Stages a Source 2 content tree containing your image plus a generated sky material.
      2. Runs CS2's own resourcecompiler.exe over it.
      3. Drops the compiled .vmat_c / .vtex_c into the game's own search path, under a
         filmmaker/ subfolder that only this tool writes to.
      4. Writes a downscaled preview PNG and a library entry so the in-game Weather & Sky tab
         can show it as a clickable card.

    The result is loadable at runtime with no game restart -- that is what makes the swap
    instant: `mirv_sky material <path>` resolves it through the normal resource system.

    IMAGE FORMAT
      A skybox wants an EQUIRECTANGULAR (2:1) panorama. HDR sources (.exr / .hdr) give the best
      result because the sky drives scene lighting; .png/.jpg/.tga work but are low dynamic
      range, so very bright skies will clip. A non-2:1 image is accepted and reported, since it
      will look stretched rather than fail.

.EXAMPLE
    pwsh FX\tools\build-skybox.ps1 -Image "C:\hdris\sunset.exr" -Name sunset

.EXAMPLE
    pwsh FX\tools\build-skybox.ps1 -Image "C:\photos\clouds.jpg" -Name clouds -ExposureBias 0.5
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Image,
    # Library name. Becomes the material filename, so it is sanitised to [a-z0-9_].
    [Parameter(Mandatory = $true)][string]$Name,
    # Exposure offset baked into the material. Positive = brighter sky.
    [double]$ExposureBias = 0.0,
    # Cubemap face resolution. Valve ships 512 for CS2's own skies; 1024 is the default here
    # because a filmmaking backdrop is often on screen at full frame, where 512 shows its edges.
    [ValidateSet(256, 512, 1024, 2048)]
    [int]$FaceSize = 1024,
    # Yaw the sky, in degrees, so you can put the sun (or the interesting clouds) where the shot
    # needs them. Applied by rolling the equirectangular source horizontally before it is
    # reprojected, because sky.vfx exposes no rotation parameter to do it live -- the shipped
    # sky materials carry only exposure-bias and solar-position parameters.
    [double]$Rotation = 0.0,
    # Mirror the sky left-to-right. Cheap way to get a different look out of the same panorama,
    # and the only way to move the sun to the other side without a second HDRI.
    [switch]$Flip,
    [string]$Cs2Dir = '',
    [string]$ResourceCompiler = '',
    # Where the library index + previews live. The in-game UI reads this.
    [string]$LibraryDir = "$env:APPDATA\HLAE\filmmaker\skyboxes",
    # Source2Viewer CLI, used ONLY to turn the compiled sky texture back into a thumbnail.
    # Optional: without it you get a name-only card instead of a preview, never a failure.
    [string]$Vrf = "$env:USERPROFILE\Documents\Github Projects\ValveResourceFormat\CLI\bin\Release\Source2Viewer-CLI.exe",
    [switch]$NoCompile
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

# ---------------------------------------------------------------- inputs ----
if (-not (Test-Path -LiteralPath $Image)) { Fail "Image not found: $Image" }
$Image = (Resolve-Path -LiteralPath $Image).Path
$ext = [IO.Path]::GetExtension($Image).ToLowerInvariant()
# What resourcecompiler ACTUALLY reads, established by probing it rather than assumed:
#   .exr .png .jpg .tga .psd .pfm  -> load fine
#   .hdr                           -> "HelperLoadFbmByFileExtension: Unknown file type"
# .hdr is nevertheless the format most HDRI sites (Poly Haven, HDRI Haven) hand you by default,
# so instead of rejecting it we decode it ourselves to .pfm, which the compiler does accept and
# which is likewise 32-bit float -- so no dynamic range is lost in the hand-off.
$supported = @('.exr', '.hdr', '.png', '.jpg', '.jpeg', '.tga', '.bmp', '.pfm')
if ($supported -notcontains $ext) {
    Fail "Unsupported image type '$ext'. Supported: $($supported -join ', ')"
}

# Sanitise: this becomes a filename inside the game's material tree.
$safeName = ($Name.ToLowerInvariant() -replace '[^a-z0-9_]', '_').Trim('_')
if ([string]::IsNullOrWhiteSpace($safeName)) { Fail "Name '$Name' has no usable characters." }

if ([string]::IsNullOrWhiteSpace($ResourceCompiler)) {
    $ResourceCompiler = Join-Path $Cs2Dir 'game\bin\win64\resourcecompiler.exe'
}
if (-not $NoCompile -and -not (Test-Path -LiteralPath $ResourceCompiler)) {
    Fail "resourcecompiler.exe not found at '$ResourceCompiler'. It ships with the CS2 workshop tools; install those, or pass -ResourceCompiler."
}

$gameCsgo = Join-Path $Cs2Dir 'game\csgo'
if (-not (Test-Path -LiteralPath $gameCsgo)) { Fail "CS2 game\csgo not found under '$Cs2Dir'." }

# ------------------------------------------------------------- staging ------
# Content MUST live under <Cs2Dir>\content\csgo\ -- resourcecompiler maps content/<mod> onto
# game/<mod> by that convention, and staging elsewhere (e.g. inside the repo) makes it fail with
# "Tried to FileExists NULL filename", which is what happened on the first attempt.
#
# `filmmaker/` is OUR subtree and nothing else writes to it, so a rebuild can never clobber
# Valve content or the particle-FX pack.
$contentDir = Join-Path $Cs2Dir 'content\csgo\materials\skybox\filmmaker'
New-Item -ItemType Directory -Force -Path $contentDir | Out-Null

# Inline C# because a per-pixel loop in PowerShell over a 2K panorama (2 million pixels) takes
# minutes; this takes well under a second. csc ships with .NET, so it adds no dependency. Loaded
# on demand -- an ordinary .png import never pays for it.
function Use-MvmImaging {
    if ('MvmRadiance' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Text;

public static class MvmRadiance
{
    // Radiance .hdr (RGBE) -> Portable Float Map. Both are linear 32-bit float, so this is a
    // container change, not a conversion: no range is clipped and no tone mapping happens.
    public static void HdrToPfm(string srcPath, string dstPath)
    {
        byte[] d = File.ReadAllBytes(srcPath);
        int p = 0;

        string first = ReadLine(d, ref p);
        if (first == null || !first.StartsWith("#?"))
            throw new Exception("Not a Radiance .hdr file (missing '#?RADIANCE' signature).");

        // Header runs until a blank line; the resolution line follows it.
        string ln;
        while ((ln = ReadLine(d, ref p)) != null && ln.Length > 0) { }

        string res = ReadLine(d, ref p);
        if (res == null) throw new Exception("Truncated .hdr: no resolution line.");
        string[] tok = res.Split(new char[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
        // Only the standard top-down/left-right orientation is handled. Every .hdr produced by
        // the usual tools uses it; anything else would silently come out mirrored, so it throws.
        if (tok.Length != 4 || tok[0] != "-Y" || tok[2] != "+X")
            throw new Exception("Unsupported .hdr orientation '" + res + "' (expected '-Y <h> +X <w>').");
        int h = int.Parse(tok[1]);
        int w = int.Parse(tok[3]);
        if (w <= 0 || h <= 0) throw new Exception("Bad .hdr dimensions " + w + "x" + h + ".");

        float[] pix = new float[w * h * 3];
        byte[] scan = new byte[w * 4];

        for (int y = 0; y < h; y++)
        {
            if (p + 4 > d.Length) throw new Exception("Truncated .hdr at scanline " + y + ".");
            byte b0 = d[p], b1 = d[p + 1], b2 = d[p + 2], b3 = d[p + 3];

            if (b0 == 2 && b1 == 2 && ((b2 << 8) | b3) == w && w >= 8 && w < 32768)
            {
                // Adaptive RLE: four separate component planes, each run-length coded.
                p += 4;
                for (int c = 0; c < 4; c++)
                {
                    int x = 0;
                    while (x < w)
                    {
                        if (p >= d.Length) throw new Exception("Truncated RLE at scanline " + y + ".");
                        int cnt = d[p++];
                        if (cnt > 128)
                        {
                            cnt -= 128;
                            byte v = d[p++];
                            if (x + cnt > w) throw new Exception("RLE run overruns scanline " + y + ".");
                            for (int i = 0; i < cnt; i++) scan[c * w + x++] = v;
                        }
                        else
                        {
                            if (cnt == 0 || x + cnt > w) throw new Exception("Bad RLE literal at scanline " + y + ".");
                            for (int i = 0; i < cnt; i++) scan[c * w + x++] = d[p++];
                        }
                    }
                }
                for (int x = 0; x < w; x++)
                    Store(pix, y, x, w, scan[x], scan[w + x], scan[2 * w + x], scan[3 * w + x]);
            }
            else
            {
                // Flat scanline, possibly with old-style RLE: (1,1,1,n) repeats the previous pixel.
                byte pr = 0, pg = 0, pb = 0, pe = 0;
                int shift = 0;
                for (int x = 0; x < w; )
                {
                    if (p + 4 > d.Length) throw new Exception("Truncated flat scanline " + y + ".");
                    byte r = d[p], g = d[p + 1], bb = d[p + 2], e = d[p + 3];
                    p += 4;
                    if (r == 1 && g == 1 && bb == 1 && x > 0)
                    {
                        int rep = e << (shift * 8);
                        shift++;
                        for (int i = 0; i < rep && x < w; i++) Store(pix, y, x++, w, pr, pg, pb, pe);
                    }
                    else
                    {
                        pr = r; pg = g; pb = bb; pe = e; shift = 0;
                        Store(pix, y, x++, w, r, g, bb, e);
                    }
                }
            }
        }

        using (FileStream fs = new FileStream(dstPath, FileMode.Create, FileAccess.Write))
        {
            // "PF" = colour, and a NEGATIVE scale means little-endian samples.
            byte[] hdr = Encoding.ASCII.GetBytes("PF\n" + w + " " + h + "\n-1.0\n");
            fs.Write(hdr, 0, hdr.Length);
            // PFM rows run bottom-to-top; .hdr with "-Y" runs top-to-bottom. Hence the reversal --
            // skip it and the sky renders upside down.
            byte[] row = new byte[w * 3 * 4];
            for (int y = h - 1; y >= 0; y--)
            {
                Buffer.BlockCopy(pix, y * w * 3 * 4, row, 0, row.Length);
                fs.Write(row, 0, row.Length);
            }
        }
    }

    private static void Store(float[] pix, int y, int x, int w, byte r, byte g, byte b, byte e)
    {
        int i = (y * w + x) * 3;
        if (e == 0) { pix[i] = 0f; pix[i + 1] = 0f; pix[i + 2] = 0f; return; }
        // RGBE: the shared exponent is biased by 128, and the mantissa is a 0..255 fraction,
        // hence the extra 8.
        float f = (float)Math.Pow(2.0, e - 136);
        pix[i] = r * f; pix[i + 1] = g * f; pix[i + 2] = b * f;
    }

    private static string ReadLine(byte[] d, ref int p)
    {
        if (p >= d.Length) return null;
        StringBuilder sb = new StringBuilder();
        while (p < d.Length && d[p] != (byte)'\n') { sb.Append((char)d[p]); p++; }
        p++; // consume the newline
        return sb.ToString().TrimEnd('\r');
    }

    // Rolls an equirectangular PFM horizontally, wrapping around. On a 360-degree panorama a
    // horizontal shift IS a yaw, so this rotates the finished sky about the vertical axis.
    public static void RollPfm(string path, double degrees)
    {
        byte[] d = File.ReadAllBytes(path);
        int p = 0;
        string magic = ReadLine(d, ref p);
        if (magic != "PF") throw new Exception("Not a colour PFM: '" + magic + "'.");
        string[] dim = ReadLine(d, ref p).Split(new char[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
        int w = int.Parse(dim[0]), h = int.Parse(dim[1]);
        string scale = ReadLine(d, ref p);
        int dataStart = p;

        int shift = (int)Math.Round(degrees / 360.0 * w) % w;
        if (shift < 0) shift += w;
        if (shift == 0) return;

        int stride = w * 3 * 4;
        byte[] row = new byte[stride];
        using (FileStream fs = new FileStream(path, FileMode.Create, FileAccess.Write))
        {
            byte[] hdr = Encoding.ASCII.GetBytes("PF\n" + w + " " + h + "\n" + scale + "\n");
            fs.Write(hdr, 0, hdr.Length);
            for (int y = 0; y < h; y++)
            {
                int src = dataStart + y * stride;
                if (src + stride > d.Length) throw new Exception("Truncated PFM data.");
                // Copy the tail first, then the head: that is the wrap.
                int cut = shift * 3 * 4;
                Buffer.BlockCopy(d, src + cut, row, 0, stride - cut);
                Buffer.BlockCopy(d, src, row, stride - cut, cut);
                fs.Write(row, 0, stride);
            }
        }
    }

    // Mirrors an equirectangular PFM left-to-right.
    public static void FlipPfm(string path)
    {
        byte[] d = File.ReadAllBytes(path);
        int p = 0;
        string magic = ReadLine(d, ref p);
        if (magic != "PF") throw new Exception("Not a colour PFM: '" + magic + "'.");
        string[] dim = ReadLine(d, ref p).Split(new char[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
        int w = int.Parse(dim[0]), h = int.Parse(dim[1]);
        string scale = ReadLine(d, ref p);
        int dataStart = p;

        int stride = w * 3 * 4;
        byte[] row = new byte[stride];
        using (FileStream fs = new FileStream(path, FileMode.Create, FileAccess.Write))
        {
            byte[] hdr = Encoding.ASCII.GetBytes("PF\n" + w + " " + h + "\n" + scale + "\n");
            fs.Write(hdr, 0, hdr.Length);
            for (int y = 0; y < h; y++)
            {
                int src = dataStart + y * stride;
                if (src + stride > d.Length) throw new Exception("Truncated PFM data.");
                // Reverse pixel order, keeping each pixel's three floats in RGB order.
                for (int x = 0; x < w; x++)
                    Buffer.BlockCopy(d, src + (w - 1 - x) * 12, row, x * 12, 12);
                fs.Write(row, 0, stride);
            }
        }
    }
}
'@ -ErrorAction Stop
}

# Wraps an LDR image horizontally. Same idea as RollPfm, but System.Drawing can do it directly.
function Invoke-LdrRoll([string]$path, [double]$degrees) {
    Add-Type -AssemblyName System.Drawing
    $src = [System.Drawing.Image]::FromFile($path)
    try {
        $w = $src.Width; $h = $src.Height
        $shift = [int][Math]::Round($degrees / 360.0 * $w) % $w
        if ($shift -lt 0) { $shift += $w }
        if ($shift -eq 0) { return }
        $bmp = New-Object System.Drawing.Bitmap $w, $h
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.DrawImage($src, -$shift, 0, $w, $h)
        $g.DrawImage($src, $w - $shift, 0, $w, $h)
        $g.Dispose()
        $out = $bmp.Clone()
        $bmp.Dispose()
        $src.Dispose()
        $out.Save($path)
        $out.Dispose()
    } catch { $src.Dispose(); throw }
}

function Invoke-LdrFlip([string]$path) {
    Add-Type -AssemblyName System.Drawing
    $src = [System.Drawing.Image]::FromFile($path)
    try {
        $src.RotateFlip([System.Drawing.RotateFlipType]::RotateNoneFlipX)
        $out = New-Object System.Drawing.Bitmap $src
    } finally { $src.Dispose() }
    $out.Save($path)
    $out.Dispose()
}

if ($ext -eq '.hdr') {
    Use-MvmImaging
    Write-Host "Decoding Radiance .hdr to .pfm (CS2's compiler cannot read .hdr)..." -ForegroundColor Cyan
    $imgLeaf   = "$safeName.pfm"
    $stagedImg = Join-Path $contentDir $imgLeaf
    [MvmRadiance]::HdrToPfm($Image, $stagedImg)
    $ext = '.pfm'
}
else {
    $imgLeaf   = "$safeName$ext"
    $stagedImg = Join-Path $contentDir $imgLeaf
    Copy-Item -LiteralPath $Image -Destination $stagedImg -Force
}

$wantRotate = [Math]::Abs($Rotation) -gt 0.0001
if ($wantRotate -or $Flip) {
    $ldrExts = @('.png', '.jpg', '.jpeg', '.bmp')
    if ($ext -eq '.pfm') {
        Use-MvmImaging
        if ($wantRotate) { [MvmRadiance]::RollPfm($stagedImg, $Rotation) }
        if ($Flip)       { [MvmRadiance]::FlipPfm($stagedImg) }
    }
    elseif ($ldrExts -contains $ext) {
        if ($wantRotate) { Invoke-LdrRoll $stagedImg $Rotation }
        if ($Flip)       { Invoke-LdrFlip $stagedImg }
    }
    else {
        # .exr and .tga cannot be decoded here, and silently ignoring the request would be worse
        # than refusing: you would ship a shot with the sun in the wrong place.
        Fail "-Rotation/-Flip cannot be applied to '$ext'. They need a source this tool can decode: use the .hdr version of the panorama (most HDRI sites offer both), or a .png/.jpg."
    }
    $did = @()
    if ($wantRotate) { $did += "rotated $Rotation deg" }
    if ($Flip)       { $did += 'mirrored' }
    Write-Host ("Sky " + ($did -join ' and ') + ".") -ForegroundColor Cyan
}

# A .vtex TEXTURE DEFINITION. The material cannot point at a raw .png/.exr -- resourcecompiler
# rejects that outright ("An output texture can only point to a .vtex_c file"). Shipped sky
# materials reference a generated `<name>_<ext>_<hash>.vtex`; we generate our own, plainly named.
#
# HDR sources get BC6H (a float format, so bright sky keeps its range and drives lighting
# correctly); LDR sources get DXT1 in sRGB. Picking BC6H for an 8-bit png would just waste space
# without adding range.
$isHdr = @('.exr', '.pfm') -contains $ext   # .hdr has already become .pfm by this point
$vtexFormat     = if ($isHdr) { 'BC6H' } else { 'DXT1' }
$vtexColorSpace = if ($isHdr) { 'linear' } else { 'srgb' }

# Writes a .vtex TEXTURE DEFINITION.
#
# A .vtex is DMX keyvalues2, NOT KV3 -- a KV3-shaped file fails with "Cannot determine encoding
# from file". This structure is copied from a shipped example
# (content/csgo/materials/decals/sprays/workshop/spray_example_nodrips.vtex).
#
# One is needed for BOTH textures this tool produces, because resourcecompiler has no implicit
# image compiler: handing it a bare .png fails with "Failed to find compiler for file". Every
# image needs an authored .vtex naming it.
function Write-Vtex {
    param(
        [string]$Path,        # .vtex to write
        [string]$RelImage,    # game-relative path of the source image
        [string]$Format,      # BC6H for float sources, DXT1 for 8-bit
        [string]$ColorSpace,  # linear for float sources, srgb for 8-bit
        [int]$MaxDimension,
        # "2D" or "Cube". Setting BOTH to Cube is what makes resourcecompiler reproject a single
        # equirectangular image into a six-face cubemap -- see the sky call site.
        [string]$TypeString = '2D',
        [string]$OutputType = '2D',
        # Output colour space, separate from the input's. They differ for the thumbnail, which
        # reads a LINEAR float source and must write an sRGB-encoded 8-bit texture.
        [string]$OutputColorSpace = '',
        # 1 for UI textures. Valve's own panorama/images textures all carry VTEX_FLAG_NO_LOD.
        [int]$NoLod = 0
    )
    if ([string]::IsNullOrWhiteSpace($OutputColorSpace)) { $OutputColorSpace = $ColorSpace }
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
			"m_typeString" "string" "$TypeString"
			"m_imageProcessorArray" "element_array"
			[
			]
		}
	]
	"m_outputTypeString" "string" "$OutputType"
	"m_outputFormat" "string" "$Format"
	"m_outputClearColor" "vector4" "0 0 0 0"
	"m_nOutputMinDimension" "int" "0"
	"m_nOutputMaxDimension" "int" "$MaxDimension"
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
			"m_outputColorSpace" "string" "$OutputColorSpace"
		}
	]
	"m_vClamp" "vector3" "0 0 0"
	"m_bNoLod" "bool" "$NoLod"
}
"@
    # No BOM: the DMX header comment must be the very first bytes or the encoding sniff fails.
    [IO.File]::WriteAllText($Path, $body, (New-Object Text.UTF8Encoding($false)))
}

# THE SKY TEXTURE MUST BE A CUBEMAP, not the 2D panorama you fed in.
#
# sky.vfx samples g_tSkyTexture as a cube. Handing it a 2D texture is not a quality compromise,
# it does not render at all: the game logs
#     Bad texture dimension type 1 ... wanted 4 ... returning error texture
# and draws the magenta error texture instead -- which looks enough like "a custom sky loaded"
# to fool a before/after screenshot. It did exactly that here before this was caught.
#
# Setting m_typeString AND m_outputTypeString to "Cube" makes resourcecompiler reproject the
# equirectangular source onto six faces by itself; there is no lat-long image processor to
# invoke. Verified against Valve's own materials/skybox/sky_de_mirage_exr_*.vtex_c, which is a
# 512x512 BC6H texture carrying VTEX_FLAG_CUBE_TEXTURE and is built from a single .exr the same
# way. With type Cube, m_nOutputMaxDimension is the FACE size, not the panorama width.
$vtexPath = Join-Path $contentDir "$safeName.vtex"
Write-Vtex -Path $vtexPath -RelImage "materials/skybox/filmmaker/$imgLeaf" `
           -Format $vtexFormat -ColorSpace $vtexColorSpace -MaxDimension $FaceSize `
           -TypeString 'Cube' -OutputType 'Cube'

# The sky material. Shader and texture parameter were read off a shipped CS2 sky material
# (materials/skybox/sky_de_mirage.vmat_c, decompiled 2026-08-04): shader `sky.vfx`, texture
# parameter `g_tSkyTexture`, plus an exposure bias.
$vmatPath = Join-Path $contentDir "$safeName.vmat"
$vmatBody = @"
// Generated by FX/tools/build-skybox.ps1 -- do not hand-edit; re-run the tool instead.
// Source image: $([IO.Path]::GetFileName($Image))
Layer0
{
	shader "sky.vfx"

	g_tSkyTexture "materials/skybox/filmmaker/$safeName.vtex_c"
	g_flBrightnessExposureBias "$([string]::Format([Globalization.CultureInfo]::InvariantCulture,'{0:0.000}',$ExposureBias))"

	// A PROCEDURAL SUN, declared so the in-game sun controls have something to drive.
	//
	// sky.vfx supports these, but a material only exposes the parameters it actually SETS --
	// verified at runtime: sky_de_mirage and sky_de_dust2 omit them and MaterialFindParam then
	// returns null for all three, so `world sun azimuth/size/brightness` can do nothing on those
	// maps. Declaring them here is what makes an imported skybox's sun movable at all.
	//
	// Defaults copied from Valve's own sky_de_nuke:
	//   SolarRadius 0.51        the real sun's angular diameter, in DEGREES
	//   SolarIrradiance         RGB brightness, very slightly warm
	//   SolarPosition           UNIT direction vector; this one is elevation 45, azimuth 0
	SolarRadius "0.510"
	SolarIrradiance "[2.107187 2.123156 1.794319 0.000000]"
	SolarPosition "[0.707107 0.000000 0.707107 0.000000]"
}
"@
Set-Content -LiteralPath $vmatPath -Value $vmatBody -Encoding UTF8

Write-Host "Staged content:" -ForegroundColor Cyan
Write-Host "  $vmatPath"
Write-Host "  $vtexPath"
Write-Host "  $stagedImg"

# ------------------------------------------------------------- compile ------
if (-not $NoCompile) {
    Write-Host "Compiling with resourcecompiler..." -ForegroundColor Cyan
    # Same invocation shape the particle-FX pipeline uses and which is known to work:
    #   -i <input>  -r (recurse)  -game <game dir>  -nop4  -fshallow2
    # Texture first, then the material that references it -- the material compile validates that
    # the .vtex_c already exists.
    & $ResourceCompiler -i $vtexPath -r -game $gameCsgo -nop4 -fshallow2
    if ($LASTEXITCODE -ne 0) { Fail "resourcecompiler.exe failed compiling the texture (exit $LASTEXITCODE)." }
    & $ResourceCompiler -i $vmatPath -r -game $gameCsgo -nop4 -fshallow2
    if ($LASTEXITCODE -ne 0) {
        Fail "resourcecompiler.exe failed (exit $LASTEXITCODE). The most likely cause is a shader parameter name that changed in a CS2 update -- decompile a shipped materials/skybox/sky_*.vmat_c and compare."
    }

    $compiled = Join-Path $gameCsgo "materials\skybox\filmmaker\$safeName.vmat_c"
    if (-not (Test-Path -LiteralPath $compiled)) {
        Fail "Compile reported success but '$compiled' is missing."
    }
    Write-Host "Compiled: $compiled" -ForegroundColor Green
}

# ------------------------------------------------------------- preview ------
# The thumbnail the in-game card shows.
#
# It is a SECOND, FLAT 2D texture compiled from the same source image -- not a view of the sky
# texture built above. The sky texture is a cubemap, and a cubemap decodes to six separate faces:
# useless as a recognisable thumbnail. Compiling the equirect source again as plain 2D keeps the
# familiar panorama shape.
#
# resourcecompiler does the format conversion, so this works for every input format including
# .exr and .pfm, which System.Drawing cannot read at all. Input colour space follows the source
# (linear for float formats) while output is always sRGB, so an HDR sky is encoded down to 8-bit
# correctly rather than coming out black.
#
# Panorama cannot load a loose .png off disk -- it needs a compiled texture addressed over
# s2r:// -- which is the whole reason this is a compile rather than a resize.
New-Item -ItemType Directory -Force -Path $LibraryDir | Out-Null
$previewPath  = Join-Path $LibraryDir "$safeName.png"
$havePreview  = $false
$previewImage = ''

if (-not $NoCompile) {
    if (-not (Test-Path -LiteralPath $Vrf)) {
        Write-Warning "Source2Viewer-CLI not found at '$Vrf'; this skybox gets a letter tile instead of a thumbnail. Pass -Vrf to enable previews."
    }
    else {
        try {
            $uiContent = Join-Path $Cs2Dir 'content\csgo\panorama\images\filmmaker\skyboxes'
            New-Item -ItemType Directory -Force -Path $uiContent | Out-Null
            $tmp = Join-Path ([IO.Path]::GetTempPath()) ("mvmsky_" + [Guid]::NewGuid().ToString('N'))
            New-Item -ItemType Directory -Force -Path $tmp | Out-Null
            try {
                # STEP 1: compile the source small, keeping it FLOAT (BC6H, linear).
                #
                # Going straight from an HDR source to DXT1 with an sRGB output colour space is
                # what the first version did, and it produced near-WHITE tiles. An sRGB transfer
                # curve is not a tone map: sky luminance in an HDRI routinely exceeds 1.0, so
                # encoding then clamping blows the whole image out. The float intermediate keeps
                # the range so the next step can tone-map it properly.
                $tmpVtex = Join-Path $uiContent "$($safeName)_hdrtmp.vtex"
                Write-Vtex -Path $tmpVtex -RelImage "materials/skybox/filmmaker/$imgLeaf" `
                           -Format ($isHdr ? 'BC6H' : 'DXT1') -ColorSpace $vtexColorSpace `
                           -MaxDimension 256 -TypeString '2D' -OutputType '2D'
                & $ResourceCompiler -i $tmpVtex -r -game $gameCsgo -nop4 -fshallow2 2>&1 | Out-Null
                $tmpCompiled = Join-Path $gameCsgo "panorama\images\filmmaker\skyboxes\$($safeName)_hdrtmp.vtex_c"

                # STEP 2: decode it with ForceLDR, which DOES tone-map, to an 8-bit PNG.
                if (Test-Path -LiteralPath $tmpCompiled) {
                    & $Vrf -i $tmpCompiled -o (Join-Path $tmp 'dump') -d --texture_decode_flags ForceLDR 2>&1 | Out-Null
                }
                $decoded = Get-ChildItem -LiteralPath $tmp -Filter *.png -Recurse -ErrorAction SilentlyContinue |
                           Select-Object -First 1 -ExpandProperty FullName

                # The float intermediate exists only to be tone-mapped; leaving it behind would
                # double every skybox's footprint in the game tree.
                Remove-Item -LiteralPath $tmpVtex -Force -ErrorAction SilentlyContinue
                Remove-Item -LiteralPath $tmpCompiled -Force -ErrorAction SilentlyContinue

                if (-not $decoded) {
                    Write-Warning "Could not tone-map a thumbnail; this skybox gets a letter tile."
                }
                else {
                    Copy-Item -LiteralPath $decoded -Destination $previewPath -Force
                    # STEP 3: compile the tone-mapped PNG as the UI texture. sRGB in AND out now,
                    # because the PNG is already display-referred -- no transfer to apply.
                    #
                    # RGBA8888 + NoLod, NOT DXT1. Panorama's UI path does not sample a compressed
                    # sRGB texture correctly -- a DXT1 thumbnail renders washed out to near-white
                    # with a pink cast, even though the texture decodes perfectly outside the
                    # game. Valve's own panorama/images textures are all uncompressed RGBA8888
                    # with VTEX_FLAG_NO_LOD, which is what this matches.
                    Copy-Item -LiteralPath $decoded -Destination (Join-Path $uiContent "$safeName.png") -Force
                    $uiVtex = Join-Path $uiContent "$safeName.vtex"
                    Write-Vtex -Path $uiVtex -RelImage "panorama/images/filmmaker/skyboxes/$safeName.png" `
                               -Format 'RGBA8888' -ColorSpace 'srgb' -OutputColorSpace 'srgb' `
                               -MaxDimension 256 -TypeString '2D' -OutputType '2D' -NoLod 1
                    & $ResourceCompiler -i $uiVtex -r -game $gameCsgo -nop4 -fshallow2 2>&1 | Out-Null

                    $uiCompiled = Join-Path $gameCsgo "panorama\images\filmmaker\skyboxes\$safeName.vtex_c"
                    if (Test-Path -LiteralPath $uiCompiled) {
                        $previewImage = "s2r://panorama/images/filmmaker/skyboxes/$safeName.vtex"
                        $havePreview = $true
                    } else {
                        Write-Warning "Thumbnail compile produced no '$uiCompiled'; this skybox gets a letter tile."
                    }
                }
            } finally {
                Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
            }
        } catch {
            Write-Warning "Could not build the thumbnail: $($_.Exception.Message)"
        }
    }
}

# ------------------------------------------------------------- library ------
# One JSON index the in-game Weather & Sky tab reads. Rewritten wholesale each run, with this
# entry replaced rather than duplicated.
$indexPath = Join-Path $LibraryDir 'skyboxes.json'
$entries = @()
if (Test-Path -LiteralPath $indexPath) {
    try {
        $existing = Get-Content -LiteralPath $indexPath -Raw | ConvertFrom-Json
        # Both @() wrappers are load-bearing. ConvertFrom-Json unwraps a one-element array to a
        # bare object, and Where-Object unwraps a one-result pipeline the same way -- either alone
        # leaves $entries a PSObject, and the `+=` below then fails with "does not contain a method
        # named 'op_Addition'". Hit for real on the second skybox ever built.
        if ($null -ne $existing) { $entries = @(@($existing) | Where-Object { $_.name -ne $safeName }) }
    } catch {
        Write-Warning "skyboxes.json was unreadable and is being rebuilt: $($_.Exception.Message)"
        $entries = @()
    }
}

$entries += [pscustomobject]@{
    name     = $safeName
    label    = $Name
    material = "materials/skybox/filmmaker/$safeName.vmat"
    preview  = if ($havePreview) { "$safeName.png" } else { '' }
    # What the Panorama card actually sets on its <Image>. Empty means "no thumbnail", and the
    # UI is required to fall back to a name-only card rather than showing a broken image.
    image    = $previewImage
    source   = [IO.Path]::GetFileName($Image)
    # FULL path, not just the leaf: the in-game Skybox Settings tab rebuilds a skybox with a new
    # rotation/flip by re-running this tool against its original image, and it can only do that
    # if it knows where that image is.
    sourcePath = $Image
    exposure = $ExposureBias
    rotation = $Rotation
    flipped  = [bool]$Flip
    created  = (Get-Date).ToString('s')
}

# -AsArray is load-bearing: with exactly one entry ConvertTo-Json emits a bare object instead of
# a one-element array, and the in-game reader would then see no skyboxes at all on a fresh
# install -- the single case a first-time user hits first.
($entries | ConvertTo-Json -Depth 4 -AsArray) | Set-Content -LiteralPath $indexPath -Encoding UTF8

Write-Host ""
Write-Host "Skybox '$safeName' is ready." -ForegroundColor Green
Write-Host "  library : $indexPath"
if ($havePreview) { Write-Host "  preview : $previewPath" }
Write-Host ""
Write-Host "Apply it in-game with:" -ForegroundColor Cyan
Write-Host "  mirv_sky material materials/skybox/filmmaker/$safeName.vmat"
Write-Host "...or pick it from CONFIG -> EFFECTS -> Weather & Sky."
