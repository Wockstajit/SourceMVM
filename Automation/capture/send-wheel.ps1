# Sends a REAL mouse wheel event to the CS2 window at a point given in PANORAMA surface
# coordinates (what GetPositionWithinWindow reports).
#
#   pwsh Automation\capture\send-wheel.ps1 -X 1362 -Y 986 -SurfaceW 1600 -SurfaceH 1200 -Clicks -3
#
# COORDINATE TRAP (measured 2026-08-03): Panorama's surface is NOT the Win32 client rect.
# Here the client was 1280x960 while Panorama reported 1600x1200 -- a 1.25x render scale. Feeding
# Panorama pixels straight to ClientToScreen puts the cursor outside the window entirely and the
# wheel silently lands on the desktop. Pass -SurfaceW/-SurfaceH and the point is rescaled to the
# real client rect first; omit them only if X/Y are already client pixels.
#
# CS2 must be foreground and in UI cursor mode, so run focus-game-window.ps1 first.
# Negative -Clicks scrolls DOWN.
#
# -Post uses PostMessage(WM_MOUSEWHEEL) straight to the CS2 window instead of SendInput.
# THIS IS THE ONE THAT WORKS for testing (measured 2026-08-03): SendInput's wheel is delivered
# to whatever window has FOCUS, and CS2 also reads raw input, so a synthetic wheel routinely
# never reaches the game at all. A posted message lands in the window's queue and is dispatched
# to the hooked WndProc (WindowInputHooks.cpp) regardless of focus.
param(
    [Parameter(Mandatory = $true)][int]$X,
    [Parameter(Mandatory = $true)][int]$Y,
    [int]$SurfaceW = 0,
    [int]$SurfaceH = 0,
    [int]$Clicks = -3,
    [int]$StepMs = 120,
    [switch]$Post
)
$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Win32Wheel {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int X, int Y);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT lpPoint);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern void mouse_event(uint dwFlags, int dx, int dy, int dwData, UIntPtr dwExtraInfo);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
    public const uint WM_MOUSEWHEEL = 0x020A;
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public const uint MOUSEEVENTF_WHEEL = 0x0800;
}
'@

$proc = Get-Process cs2 -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) { throw 'send-wheel: cs2 is not running (or has no window)' }
$hwnd = $proc.MainWindowHandle

$cr = New-Object Win32Wheel+RECT
[void][Win32Wheel]::GetClientRect($hwnd, [ref]$cr)
$clientW = $cr.R; $clientH = $cr.B
if ($clientW -le 0 -or $clientH -le 0) { throw 'send-wheel: client rect is empty' }

$cx = $X; $cy = $Y
if ($SurfaceW -gt 0 -and $SurfaceH -gt 0) {
    $cx = [int][Math]::Round($X * ($clientW / [double]$SurfaceW))
    $cy = [int][Math]::Round($Y * ($clientH / [double]$SurfaceH))
}
if ($cx -lt 0 -or $cy -lt 0 -or $cx -ge $clientW -or $cy -ge $clientH) {
    throw "send-wheel: point ($cx,$cy) is outside the client area ${clientW}x${clientH} -- check -SurfaceW/-SurfaceH"
}

$fg = [Win32Wheel]::GetForegroundWindow()
if ($fg -ne $hwnd) { Write-Warning "send-wheel: cs2 is NOT foreground (run focus-game-window.ps1) -- the wheel may go elsewhere." }

$pt = New-Object Win32Wheel+POINT
$pt.X = $cx; $pt.Y = $cy
[void][Win32Wheel]::ClientToScreen($hwnd, [ref]$pt)
Write-Output "send-wheel: surface($X,$Y) -> client($cx,$cy) of ${clientW}x${clientH} -> screen($($pt.X),$($pt.Y))"

$step = if ($Clicks -lt 0) { -120 } else { 120 }

if ($Post) {
    # lParam carries SCREEN coordinates (the WndProc hook does ScreenToClient itself).
    $lParam = [IntPtr](($pt.Y -shl 16) -bor ($pt.X -band 0xFFFF))
    for ($i = 0; $i -lt [Math]::Abs($Clicks); $i++) {
        $wParam = [IntPtr]($step -shl 16)
        [void][Win32Wheel]::PostMessage($hwnd, [Win32Wheel]::WM_MOUSEWHEEL, $wParam, $lParam)
        Start-Sleep -Milliseconds $StepMs
    }
    Write-Output "send-wheel: POSTED $([Math]::Abs($Clicks)) notches ($step each) to hwnd=$hwnd at screen($($pt.X),$($pt.Y))"
    return
}

[void][Win32Wheel]::SetCursorPos($pt.X, $pt.Y)
Start-Sleep -Milliseconds 300

for ($i = 0; $i -lt [Math]::Abs($Clicks); $i++) {
    [Win32Wheel]::mouse_event([Win32Wheel]::MOUSEEVENTF_WHEEL, 0, 0, $step, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds $StepMs
}
Write-Output "send-wheel: sent $([Math]::Abs($Clicks)) notches ($step each) at screen($($pt.X),$($pt.Y))"
