# Captures ONLY the CS2 game window to PNG (not the whole desktop).
#
# Why: a full-monitor grab makes the game a small slice of a big image, so fine
# UI detail (tile colors, navbar icons, text) is hard to read. Cropping to the
# window -- by default the client area, i.e. the game's render surface with no
# title bar / borders / desktop -- yields a tight, full-resolution image.
#
# Capture method: PrintWindow with PW_RENDERFULLCONTENT, which reads the window's
# own DWM-composited surface -- so the capture is correct even when another window,
# a folder, or a tooltip is sitting on top of the game. The old CopyFromScreen
# screen-crop (which photographed whatever was on top) is kept only as a fallback
# for the rare case PrintWindow fails or returns an all-black frame.
#
#   powershell -ExecutionPolicy Bypass -File Automation\capture\capture-game-window.ps1 -Out shot.png
#   powershell -ExecutionPolicy Bypass -File Automation\capture\capture-game-window.ps1 -Out shot.png -Mode window
#   powershell -ExecutionPolicy Bypass -File Automation\capture\capture-game-window.ps1 -Out shot.png -Process cs2
#
# -Mode client (default) : just the game viewport (recommended for reading UI).
# -Mode window           : the whole OS window including title bar + borders.
param(
    [Parameter(Mandatory = $true)]
    [string]$Out,
    [string]$Process = 'cs2',
    [ValidateSet('client', 'window')]
    [string]$Mode = 'client'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Win32WindowCapture {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT p);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
  [DllImport("user32.dll")] private static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
  // DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS=9) gives the true visible
  // frame (GetWindowRect includes invisible drop-shadow margins on Win10/11).
  [DllImport("dwmapi.dll")] private static extern int DwmGetWindowAttribute(IntPtr hWnd, int attr, out RECT r, int size);

  public static void EnablePerMonitorV2() { SetThreadDpiAwarenessContext(new IntPtr(-4)); }

  public static RECT ExtendedFrame(IntPtr hWnd) {
    RECT r;
    if (DwmGetWindowAttribute(hWnd, 9, out r, Marshal.SizeOf(typeof(RECT))) == 0) return r;
    GetWindowRect(hWnd, out r); return r;
  }
}
'@

[Win32WindowCapture]::EnablePerMonitorV2()

# Locate the target window (first process of -Process with a real top-level window).
$proc = Get-Process -Name $Process -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) { throw "No '$Process' process with a visible window. Is the game running?" }
$h = $proc.MainWindowHandle

# Un-minimize if needed (a minimized window has no capturable pixels). SW_RESTORE = 9.
if ([Win32WindowCapture]::IsIconic($h)) { [void][Win32WindowCapture]::ShowWindow($h, 9); Start-Sleep -Milliseconds 250 }

# Geometry. PrintWindow renders the FULL window (frame included) at (0,0) of the
# destination DC, so for client mode we render the whole window and crop the client
# rect out of it using the client origin's offset inside the window rect.
$wr = New-Object Win32WindowCapture+RECT
[void][Win32WindowCapture]::GetWindowRect($h, [ref]$wr)
$winW = $wr.Right - $wr.Left; $winH = $wr.Bottom - $wr.Top

$rc = New-Object Win32WindowCapture+RECT
[void][Win32WindowCapture]::GetClientRect($h, [ref]$rc)
$tl = New-Object Win32WindowCapture+POINT; $tl.X = 0; $tl.Y = 0
[void][Win32WindowCapture]::ClientToScreen($h, [ref]$tl)
$cliX = $tl.X - $wr.Left; $cliY = $tl.Y - $wr.Top
$cliW = $rc.Right - $rc.Left; $cliH = $rc.Bottom - $rc.Top

if ($winW -le 0 -or $winH -le 0 -or ($Mode -eq 'client' -and ($cliW -le 0 -or $cliH -le 0))) {
    throw "Window '$Process' has invalid bounds (win ${winW}x${winH}, client ${cliW}x${cliH}); it may be minimized or hidden."
}

$parent = Split-Path -Parent $Out
if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }

function Test-MostlyBlack([System.Drawing.Bitmap]$bmp) {
    # Sample a sparse grid; if every sample is pure black the capture failed.
    $stepX = [Math]::Max(1, [int]($bmp.Width / 16)); $stepY = [Math]::Max(1, [int]($bmp.Height / 16))
    for ($py = 0; $py -lt $bmp.Height; $py += $stepY) {
        for ($px = 0; $px -lt $bmp.Width; $px += $stepX) {
            $c = $bmp.GetPixel($px, $py)
            if ($c.R -ne 0 -or $c.G -ne 0 -or $c.B -ne 0) { return $false }
        }
    }
    return $true
}

$method = 'PrintWindow'
$full = [System.Drawing.Bitmap]::new($winW, $winH, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($full)
$ok = $false
try {
    $hdc = $g.GetHdc()
    try {
        # PW_CLIENTONLY(1) is deliberately NOT used: with FULLCONTENT it yields blank
        # frames on some drivers. Render the whole window, crop below.
        $ok = [Win32WindowCapture]::PrintWindow($h, $hdc, 2) # PW_RENDERFULLCONTENT
    } finally { $g.ReleaseHdc($hdc) }
} finally { $g.Dispose() }

if (-not $ok -or (Test-MostlyBlack $full)) {
    # Fallback: legacy screen crop (photographs whatever is on top of the game).
    $method = 'CopyFromScreen (fallback; capture may include occluding windows)'
    $full.Dispose()
    $full = [System.Drawing.Bitmap]::new($winW, $winH, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($full)
    try {
        $g.CopyFromScreen($wr.Left, $wr.Top, 0, 0, (New-Object System.Drawing.Size($winW, $winH)), [System.Drawing.CopyPixelOperation]::SourceCopy)
    } finally { $g.Dispose() }
}

try {
    if ($Mode -eq 'client') {
        $crop = New-Object System.Drawing.Rectangle($cliX, $cliY, $cliW, $cliH)
        $clientBmp = $full.Clone($crop, $full.PixelFormat)
        try { $clientBmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png) } finally { $clientBmp.Dispose() }
        $w = $cliW; $hgt = $cliH
    } else {
        $full.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
        $w = $winW; $hgt = $winH
    }
} finally { $full.Dispose() }

$file = Get-Item -LiteralPath $Out
$kb = [math]::Round($file.Length / 1kb)
Write-Host "Captured $Process $Mode via $method (${w}x${hgt}, $kb KB) -> $($file.FullName)"
