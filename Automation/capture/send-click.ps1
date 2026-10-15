# Sends a REAL left click to the CS2 window at a point given in PANORAMA surface coordinates
# (the space GetPositionWithinWindow reports in).
#
#   pwsh Automation\capture\send-click.ps1 -X 40 -Y 1166 -SurfaceW 1600 -SurfaceH 1200
#
# Companion to send-wheel.ps1, and it carries the same two traps:
#
#   COORDINATES. Panorama's surface is NOT the Win32 client rect -- a render scale of 1.25 has been
#   measured here (client 1280x960, Panorama 1600x1200). Pass -SurfaceW/-SurfaceH and the point is
#   rescaled to the real client rect first; omit them only when X/Y are already client pixels.
#
#   DELIVERY. PostMessage lands the click in the window's own queue, where it reaches the hooked
#   WndProc regardless of focus. SendInput goes to whatever window has focus AND competes with the
#   game's raw-input path, so a synthetic click routinely never arrives. Posting is the default
#   here for the same reason send-wheel.ps1 documents -Post as "the one that works"; -SendInput picks
#   the SendInput path for the cases that genuinely need a real cursor.
#
# CS2 must be in UI cursor mode for a click to reach a Panorama panel at all.
param(
    [Parameter(Mandatory = $true)][int]$X,
    [Parameter(Mandatory = $true)][int]$Y,
    [int]$SurfaceW = 0,
    [int]$SurfaceH = 0,
    [int]$HoldMs = 60,
    [string]$Process = 'cs2',
    [switch]$SendInput
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Win32Click {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, int data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
    public const uint WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202;
    public const uint MOUSEEVENTF_LEFTDOWN = 0x0002, MOUSEEVENTF_LEFTUP = 0x0004;
}
'@

$proc = Get-Process -Name $Process -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
if (-not $proc) { throw "send-click: no '$Process' process with a main window is running." }
$hwnd = $proc.MainWindowHandle
[void][Win32Click]::SetThreadDpiAwarenessContext([IntPtr](-4))

$cli = New-Object Win32Click+RECT
[void][Win32Click]::GetClientRect($hwnd, [ref]$cli)
$cw = $cli.R - $cli.L
$ch = $cli.B - $cli.T
if ($cw -le 0 -or $ch -le 0) { throw 'send-click: the game window reported an empty client area.' }

$cx = $X; $cy = $Y
if ($SurfaceW -gt 0 -and $SurfaceH -gt 0) {
    $cx = [int][Math]::Round($X * $cw / $SurfaceW)
    $cy = [int][Math]::Round($Y * $ch / $SurfaceH)
}
if ($cx -lt 0 -or $cy -lt 0 -or $cx -ge $cw -or $cy -ge $ch) {
    throw "send-click: point ($cx,$cy) is outside the ${cw}x${ch} client area."
}
$lparam = [IntPtr](($cy -shl 16) -bor ($cx -band 0xFFFF))

if ($SendInput) {
    $pt = New-Object Win32Click+POINT
    $pt.X = $cx; $pt.Y = $cy
    [void][Win32Click]::ClientToScreen($hwnd, [ref]$pt)
    [void][Win32Click]::SetCursorPos($pt.X, $pt.Y)
    Start-Sleep -Milliseconds 40
    [Win32Click]::mouse_event([Win32Click]::MOUSEEVENTF_LEFTDOWN, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds $HoldMs
    [Win32Click]::mouse_event([Win32Click]::MOUSEEVENTF_LEFTUP, 0, 0, 0, [UIntPtr]::Zero)
    Write-Host "send-click: SendInput left click at client ($cx,$cy) of ${cw}x${ch}"
} else {
    # A move first: Panorama resolves hover (and therefore the panel under the cursor) from the
    # last motion it saw, so a bare down/up on a panel it thinks is not hovered can be discarded.
    [void][Win32Click]::PostMessage($hwnd, [Win32Click]::WM_MOUSEMOVE, [IntPtr]::Zero, $lparam)
    Start-Sleep -Milliseconds 40
    [void][Win32Click]::PostMessage($hwnd, [Win32Click]::WM_LBUTTONDOWN, [IntPtr]1, $lparam)
    Start-Sleep -Milliseconds $HoldMs
    [void][Win32Click]::PostMessage($hwnd, [Win32Click]::WM_LBUTTONUP, [IntPtr]::Zero, $lparam)
    Write-Host "send-click: posted left click at client ($cx,$cy) of ${cw}x${ch}"
}
