[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ProcessName,
    [ValidateRange(0, 16)]
    [int]$Monitor = 0,
    [int]$TimeoutSeconds = 20,
    [string]$WindowTitle = ''
)

# Dev-only window placement. This changes no CS2/game settings and is deliberately separate from
# the render system so shipped gameplay never depends on a monitor ordinal. `-Monitor 3` targets
# Windows' `\\.\DISPLAY3` device, not list order. Monitor 0 selects the current primary display.
$ErrorActionPreference = 'Stop'

if (-not ('MvmWindowPlacement.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace MvmWindowPlacement {
    public struct Rect {
        public int Left, Top, Right, Bottom;
        public int Width { get { return Right - Left; } }
        public int Height { get { return Bottom - Top; } }
    }

    public struct MonitorArea {
        public string Device;
        public bool Primary;
        public Rect Work;
    }

    public static class Native {
        private const uint MONITOR_DEFAULTTONEAREST = 2;
        private const uint SWP_NOZORDER = 0x0004;
        private const uint SWP_NOACTIVATE = 0x0010;
        private const uint SWP_SHOWWINDOW = 0x0040;
        private const int SW_RESTORE = 9;

        [StructLayout(LayoutKind.Sequential)]
        private struct RECT { public int Left, Top, Right, Bottom; }
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct MONITORINFOEX {
            public int cbSize; public RECT rcMonitor, rcWork; public uint dwFlags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szDevice;
        }
        private delegate bool MonitorEnumProc(IntPtr hMonitor, IntPtr hdc, IntPtr clip, IntPtr data);

        [DllImport("user32.dll")] private static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, MonitorEnumProc callback, IntPtr data);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool GetMonitorInfo(IntPtr hMonitor, ref MONITORINFOEX info);
        [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
        [DllImport("user32.dll")] private static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint flags);
        [DllImport("user32.dll")] private static extern bool SetWindowPos(IntPtr hwnd, IntPtr insertAfter, int x, int y, int cx, int cy, uint flags);
        [DllImport("user32.dll")] private static extern bool ShowWindow(IntPtr hwnd, int command);

        public static List<MonitorArea> EnumerateWorkAreas() {
            var result = new List<MonitorArea>();
            EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, delegate(IntPtr h, IntPtr dc, IntPtr clip, IntPtr data) {
                var info = new MONITORINFOEX(); info.cbSize = Marshal.SizeOf(typeof(MONITORINFOEX));
                if (GetMonitorInfo(h, ref info)) result.Add(new MonitorArea {
                    Device = info.szDevice,
                    Primary = (info.dwFlags & 1) != 0,
                    Work = new Rect {
                        Left = info.rcWork.Left, Top = info.rcWork.Top,
                        Right = info.rcWork.Right, Bottom = info.rcWork.Bottom
                    }
                });
                return true;
            }, IntPtr.Zero);
            result.Sort(delegate(MonitorArea a, MonitorArea b) {
                int x = a.Work.Left.CompareTo(b.Work.Left);
                return x != 0 ? x : a.Work.Top.CompareTo(b.Work.Top);
            });
            return result;
        }

        public static bool Move(IntPtr hwnd, Rect target) {
            RECT current;
            if (!GetWindowRect(hwnd, out current)) return false;
            int width = Math.Max(64, current.Right - current.Left);
            int height = Math.Max(64, current.Bottom - current.Top);
            width = Math.Min(width, Math.Max(64, target.Width));
            height = Math.Min(height, Math.Max(64, target.Height));
            int x = target.Left + Math.Max(0, (target.Width - width) / 2);
            int y = target.Top + Math.Max(0, (target.Height - height) / 2);
            ShowWindow(hwnd, SW_RESTORE);
            return SetWindowPos(hwnd, IntPtr.Zero, x, y, width, height,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }
}
'@
}

$deadline = (Get-Date).AddSeconds([Math]::Max(1, $TimeoutSeconds))
$target = $null
while ((Get-Date) -lt $deadline) {
    $candidates = @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 -and
            ([string]::IsNullOrWhiteSpace($WindowTitle) -or $_.MainWindowTitle -like "*$WindowTitle*") } |
        Sort-Object Id)
    if ($candidates.Count -gt 0) { $target = $candidates[0]; break }
    Start-Sleep -Milliseconds 250
}
if (-not $target) { throw "No visible window for process '$ProcessName' within $TimeoutSeconds seconds." }

$areas = [MvmWindowPlacement.Native]::EnumerateWorkAreas()
$deviceName = "\\.\DISPLAY$Monitor"
$area = $areas | Where-Object {
    if ($Monitor -eq 0) { $_.Primary } else { $_.Device -ieq $deviceName }
} | Select-Object -First 1
if ($Monitor -eq 0 -and $area) { $deviceName = $area.Device }
if (-not $area) {
    $reported = ($areas | ForEach-Object { $_.Device }) -join ', '
    throw "Monitor $Monitor ($deviceName) is not available; Windows reported: $reported"
}
if (-not [MvmWindowPlacement.Native]::Move($target.MainWindowHandle, $area.Work)) {
    throw "SetWindowPos failed for $ProcessName (PID $($target.Id))."
}
Write-Host "Moved $ProcessName (PID $($target.Id)) to monitor $Monitor ($deviceName; $($areas.Count) display(s))."
