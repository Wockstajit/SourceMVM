# BURST-CAPTURE the CS2 client surface: N consecutive frames as PNGs, optionally firing a console
# command part-way through.
#
# WHY. capture-game-window.ps1 answers "what does it look like"; a video answers "what does it look
# like over time" but you cannot read a single frame out of it. Neither answers "what did the two
# frames right after that click look like" -- and a one-frame UI glitch (a panel that un-docks and
# re-docks while a workspace swaps, a preview that snaps full-screen and back) only exists in
# exactly those frames. This grabs them as separate, full-resolution, inspectable images.
#
#   powershell -ExecutionPolicy Bypass -File Automation\capture\capture-game-burst.ps1 `
#       -OutDir build\burst -Frames 24 -PreFrames 3 -Command "mirv_filmmaker editor on"
#
# The command is sent over netcon (port 29010 by default) on an ALREADY-OPEN socket, from inside
# the capture loop, right after frame -PreFrames. Connecting is what costs milliseconds, so it is
# done before the loop starts; the send itself is one write on a NoDelay socket.
#
# CAPTURE RATE. PrintWindow(PW_RENDERFULLCONTENT) reads the window's own DWM-composited surface, so
# the capture is correct even when another window covers the game -- but it is not free, and at
# 1600x1200 it lands around 12-25 ms per frame. That is roughly frame-for-frame at 60 fps and
# strictly slower than the game; a burst samples consecutive PRESENTED frames, it does not
# guarantee every one of them. The per-frame wall clock is written to frames.csv so the actual
# spacing is visible rather than assumed. Encoding is deferred to the end (frames are held as raw
# bitmaps) so PNG compression never lands between two samples.
param(
    [Parameter(Mandatory = $true)][string]$OutDir,
    [int]$Frames = 20,
    [int]$PreFrames = 3,
    [string]$Command = '',
    [string]$EndCommand = '',
    [int]$EndFrame = 18,
    [int]$Port = 29010,
    [string]$Process = 'cs2',
    [string]$Prefix = 'f'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if ($Frames -lt 1 -or $Frames -gt 240) { throw "capture-game-burst: -Frames must be 1..240 (got $Frames)" }
if ($PreFrames -lt 0 -or $PreFrames -ge $Frames) { throw "capture-game-burst: -PreFrames must be 0..$($Frames - 1)" }
if ($EndCommand -and (-not $Command -or $EndFrame -le $PreFrames -or $EndFrame -ge $Frames)) {
    throw 'capture-game-burst: EndFrame must follow PreFrames and precede Frames, with a start Command.'
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class BurstWin32 {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT p);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
}
'@

$proc = Get-Process -Name $Process -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
if (-not $proc) { throw "capture-game-burst: no '$Process' process with a main window is running." }
$hwnd = $proc.MainWindowHandle

# Per-monitor DPI awareness, so the client rect we crop is in the same pixels PrintWindow renders.
[void][BurstWin32]::SetThreadDpiAwarenessContext([IntPtr](-4))

$win = New-Object BurstWin32+RECT
$cli = New-Object BurstWin32+RECT
[void][BurstWin32]::GetWindowRect($hwnd, [ref]$win)
[void][BurstWin32]::GetClientRect($hwnd, [ref]$cli)
$origin = New-Object BurstWin32+POINT
[void][BurstWin32]::ClientToScreen($hwnd, [ref]$origin)
$w = $cli.Right - $cli.Left
$h = $cli.Bottom - $cli.Top
if ($w -le 0 -or $h -le 0) { throw 'capture-game-burst: the game window reported an empty client area.' }
$region = New-Object System.Drawing.Rectangle (($origin.X - $win.Left), ($origin.Y - $win.Top), $w, $h)

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# Open the netcon socket BEFORE the loop: connecting mid-burst would cost more than the frame it
# is meant to land on.
$client = $null; $stream = $null
if ($Command) {
    $client = New-Object System.Net.Sockets.TcpClient
    $async = $client.BeginConnect('127.0.0.1', $Port, $null, $null)
    if (-not $async.AsyncWaitHandle.WaitOne(3000)) { $client.Close(); throw "capture-game-burst: netcon connect to 127.0.0.1:$Port timed out." }
    $client.EndConnect($async)
    $client.NoDelay = $true
    $stream = $client.GetStream()
}

$full = New-Object System.Drawing.Bitmap (($win.Right - $win.Left)), (($win.Bottom - $win.Top)), ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$gfx = [System.Drawing.Graphics]::FromImage($full)
$shots = New-Object System.Collections.Generic.List[object]
$clock = [System.Diagnostics.Stopwatch]::StartNew()
$sentAt = -1.0
$endedAt = -1.0

try {
    for ($i = 0; $i -lt $Frames; $i++) {
        if ($Command -and $i -eq $PreFrames) {
            $bytes = [System.Text.Encoding]::ASCII.GetBytes($Command + "`n")
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush()
            $sentAt = $clock.Elapsed.TotalMilliseconds
        }
        if ($EndCommand -and $i -eq $EndFrame) {
            $bytes = [System.Text.Encoding]::ASCII.GetBytes($EndCommand + "`n")
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush()
            $endedAt = $clock.Elapsed.TotalMilliseconds
        }
        $hdc = $gfx.GetHdc()
        try { $ok = [BurstWin32]::PrintWindow($hwnd, $hdc, 2) } finally { $gfx.ReleaseHdc($hdc) }
        if (-not $ok) { throw "capture-game-burst: PrintWindow failed on frame $i." }
        $t = $clock.Elapsed.TotalMilliseconds
        # Clone only the client region, and defer PNG encoding: compressing here would put tens of
        # milliseconds between consecutive samples and hide the very frames we are hunting.
        $shots.Add([pscustomobject]@{ Index = $i; Ms = $t; Bmp = $full.Clone($region, $full.PixelFormat) })
    }
} finally {
    # Release held input even if capture fails before the scheduled end frame.
    if ($EndCommand -and $stream) {
        $bytes = [System.Text.Encoding]::ASCII.GetBytes($EndCommand + "`n")
        try { $stream.Write($bytes, 0, $bytes.Length); $stream.Flush() } catch { }
    }
    $gfx.Dispose(); $full.Dispose()
    if ($client) { $client.Close() }
}

$csv = New-Object System.Collections.Generic.List[string]
$csv.Add('frame,wall_ms,delta_ms,after_command_ms,file')
$prev = $null
foreach ($s in $shots) {
    $name = ('{0}{1:d3}.png' -f $Prefix, $s.Index)
    $path = Join-Path $OutDir $name
    $s.Bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $s.Bmp.Dispose()
    $delta = if ($null -eq $prev) { 0 } else { $s.Ms - $prev }
    $after = if ($sentAt -lt 0) { '' } else { ('{0:F1}' -f ($s.Ms - $sentAt)) }
    $csv.Add(('{0},{1:F1},{2:F1},{3},{4}' -f $s.Index, $s.Ms, $delta, $after, $name))
    $prev = $s.Ms
}
$csvPath = Join-Path $OutDir 'frames.csv'
[System.IO.File]::WriteAllLines($csvPath, $csv)

$span = $shots[$shots.Count - 1].Ms - $shots[0].Ms
$rate = if ($span -gt 0) { ($shots.Count - 1) * 1000.0 / $span } else { 0 }
Write-Host ("capture-game-burst: {0} frames of {1}x{2} over {3:F0} ms ({4:F1} captures/s) -> {5}" -f `
    $shots.Count, $w, $h, $span, $rate, $OutDir)
if ($Command) { Write-Host ("  sent '{0}' at {1:F1} ms (just before frame {2})" -f $Command, $sentAt, $PreFrames) }
if ($EndCommand) { Write-Host ("  sent '{0}' at {1:F1} ms (just before frame {2})" -f $EndCommand, $endedAt, $EndFrame) }
Write-Host "  timings: $csvPath"
