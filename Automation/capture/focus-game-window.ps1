# Brings the CS2 window to the foreground so a screen-pixel capture sees the GAME and not
# whatever window happens to be sitting on top of it.
#
# capture-game-window.ps1 only un-minimises; it reads the pixels at the window's screen rect, so
# any overlapping application is captured instead. That is both a wrong screenshot and a privacy
# problem (it silently grabs whatever the user had open), which is why this is a separate,
# explicit step rather than something the capture script does behind your back.
#
# SetForegroundWindow is refused by Windows unless the calling thread owns the foreground or is
# attached to it, so this attaches to the current foreground thread's input queue for the call
# and detaches immediately after.
param(
    [string]$Process = 'cs2',
    [int]$SettleMs = 400
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Win32Focus {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, IntPtr pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
}
'@

$proc = Get-Process -Name $Process -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) { throw "No $Process window found." }
$h = $proc.MainWindowHandle

if ([Win32Focus]::IsIconic($h)) { [void][Win32Focus]::ShowWindow($h, 9) }  # SW_RESTORE

$fg = [Win32Focus]::GetForegroundWindow()
$fgThread = [Win32Focus]::GetWindowThreadProcessId($fg, [IntPtr]::Zero)
$myThread = [Win32Focus]::GetCurrentThreadId()
$attached = $false
if ($fgThread -ne 0 -and $fgThread -ne $myThread) {
    $attached = [Win32Focus]::AttachThreadInput($myThread, $fgThread, $true)
}
[void][Win32Focus]::BringWindowToTop($h)
$ok = [Win32Focus]::SetForegroundWindow($h)
if ($attached) { [void][Win32Focus]::AttachThreadInput($myThread, $fgThread, $false) }

Start-Sleep -Milliseconds $SettleMs
$now = [Win32Focus]::GetForegroundWindow()
if ($now -ne $h) { Write-Warning "CS2 did not take the foreground (another window may be topmost)." }
else { Write-Host "CS2 is foreground; safe to capture." }
