# Capture CS2 thread contexts and stacks for an offline render stall. No input, quit, or upload.
# Run outside the sandbox as the same Windows user who launched CS2.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [ValidateRange(0, 600)][int]$WaitForStallSeconds = 0,
    [ValidateRange(1000, 60000)][int]$MinimumFrameAgeMs = 2000,
    [string]$MapName = ''
)
$ErrorActionPreference = 'Stop'
$game = @(Get-Process -Name cs2 -ErrorAction SilentlyContinue)
if ($game.Count -ne 1) { throw 'Expected exactly one CS2 process.' }
$destination = [System.IO.Path]::GetFullPath($Out)
if ([System.IO.File]::Exists($destination) -or [System.IO.File]::Exists($destination + '.heartbeat.txt')) {
    throw 'Choose a new output path; existing capture evidence is never overwritten.'
}
$parent = Split-Path -Parent $destination
[System.IO.Directory]::CreateDirectory($parent) | Out-Null
if (-not ('MvmHangCapture.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
namespace MvmHangCapture {
    public static class Native {
        [DllImport("kernel32.dll", SetLastError=true)]
        static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
        [DllImport("dbghelp.dll", SetLastError=true)]
        static extern bool MiniDumpWriteDump(IntPtr process, uint pid, SafeFileHandle file,
            uint type, IntPtr exception, IntPtr userStreams, IntPtr callback);
        public static void Capture(uint pid, string path) {
            IntPtr process = OpenProcess(0x0410, false, pid); // QUERY_INFORMATION | VM_READ
            if (process == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
            try {
                using (var file = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None)) {
                    // Default stack/context capture + unloaded modules, memory information and
                    // thread information. Deliberately avoid full process memory / handles.
                    if (!MiniDumpWriteDump(process, pid, file.SafeFileHandle, 0x1820,
                        IntPtr.Zero, IntPtr.Zero, IntPtr.Zero))
                        throw new Win32Exception(Marshal.GetLastWin32Error());
                }
            } finally { CloseHandle(process); }
        }
    }
}
'@
}
if ($WaitForStallSeconds -gt 0) {
    $history = Join-Path $env:APPDATA 'HLAE/bugreports/session.log'
    $deadline = (Get-Date).AddSeconds($WaitForStallSeconds)
    $stalled = $false
    Write-Output "Waiting up to $WaitForStallSeconds seconds for a stale CS2 render pump."
    while ((Get-Date) -lt $deadline) {
        $game[0].Refresh()
        if ($game[0].HasExited) { throw 'CS2 exited before a stall could be captured.' }
        $line = Get-Content -LiteralPath $history -Tail 256 -ErrorAction SilentlyContinue |
            Where-Object { $_ -match '\[health\] heartbeat ' } | Select-Object -Last 1
        if ($line -match '^(\d\d:\d\d:\d\d\.\d{3}).*heartbeat map=(.*?) \|.*render phase=\S+ age=\d+ ms frameAge=(\d+) ms started=1') {
            $sampleTime = [datetime]::ParseExact($Matches[1], 'HH:mm:ss.fff', [cultureinfo]::InvariantCulture)
            $sampleMap = $Matches[2]
            $frameAge = [uint64]$Matches[3]
            $sampleAge = ((Get-Date) - $sampleTime).TotalSeconds
            if ($sampleAge -ge 0 -and $sampleAge -lt 7 -and $frameAge -ge $MinimumFrameAgeMs -and
                (-not $MapName -or $sampleMap -eq $MapName)) {
                $stalled = $true
                [System.IO.File]::WriteAllText($destination + '.heartbeat.txt', $line)
                break
            }
        }
        Start-Sleep -Milliseconds 250
    }
    if (-not $stalled) { Write-Output 'No matching render stall observed; no dump captured.'; return }
}
[MvmHangCapture.Native]::Capture([uint32]$game[0].Id, $destination)
Write-Output "Captured CS2 PID $($game[0].Id) thread dump: $destination"
