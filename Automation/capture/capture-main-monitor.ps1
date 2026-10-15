# Captures the complete Windows primary monitor to PNG.
# This intentionally captures the monitor rather than the CS2 window so the
# entire game and every visible UI surface are represented without cropping.
param(
    [Parameter(Mandatory = $true)]
    [string]$Out
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class MonitorCaptureDpi {
  [DllImport("user32.dll")]
  private static extern IntPtr SetThreadDpiAwarenessContext(IntPtr dpiContext);

  public static void EnablePerMonitorV2() {
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4.
    SetThreadDpiAwarenessContext(new IntPtr(-4));
  }
}
'@

[MonitorCaptureDpi]::EnablePerMonitorV2()
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
if ($bounds.Width -le 0 -or $bounds.Height -le 0) {
    throw "Primary monitor has invalid bounds: $bounds"
}

$parent = Split-Path -Parent $Out
if ($parent) {
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
}

$bitmap = [System.Drawing.Bitmap]::new(
    $bounds.Width,
    $bounds.Height,
    [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
try {
    $graphics.CopyFromScreen(
        $bounds.X,
        $bounds.Y,
        0,
        0,
        $bounds.Size,
        [System.Drawing.CopyPixelOperation]::SourceCopy
    )
    $bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
} finally {
    $graphics.Dispose()
    $bitmap.Dispose()
}

$file = Get-Item -LiteralPath $Out
$kb = [math]::Round($file.Length / 1kb)
Write-Host "Captured primary monitor ($($bounds.Width)x$($bounds.Height), $kb KB) -> $($file.FullName)"
