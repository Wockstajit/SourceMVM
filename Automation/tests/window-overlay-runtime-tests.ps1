$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
# Requires an already running offline CS2 session with the hook and netcon on port 29010.
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class OverlayProbe {
 public delegate bool EnumProc(IntPtr h, IntPtr p);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb,IntPtr p);
 [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h,StringBuilder b,int n);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 public static int VisibleCount() {int count=0; EnumWindows((h,p)=>{var b=new StringBuilder(256);GetClassName(h,b,256);if(b.ToString()=="SourceMvmWindowSizeOverlay" && IsWindowVisible(h))count++;return true;},IntPtr.Zero);return count;}
}
"@
& powershell.exe -NoProfile -File (Join-Path $repo 'Automation/netcon/cs2-netcon.ps1') -CommandBatch 'mirv_filmmaker window overlay test' -ReadSeconds 0.1 | Out-Null
$shown = [OverlayProbe]::VisibleCount()
Start-Sleep -Milliseconds 2300
$expired = [OverlayProbe]::VisibleCount()
"visible after show=$shown after deadline=$expired"
if ($shown -ne 1 -or $expired -ne 0) {throw 'Overlay timeout failed'}
