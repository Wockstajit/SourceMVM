# Read startup diagnostics without sending input or changing any window/process state.
# Only CS2-owned standard error dialogs and their Static text controls are inspected.
if (-not ('SourceMvm.Automation.Cs2StartupWindows' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace SourceMvm.Automation {
    public sealed class Cs2StartupDialog {
        public long WindowHandle;
        public string ClassName;
        public string Title;
        public string[] StaticText;
    }

    public static class Cs2StartupWindows {
        private delegate bool EnumCallback(IntPtr window, IntPtr parameter);
        [DllImport("user32.dll")]
        private static extern bool EnumWindows(EnumCallback callback, IntPtr parameter);
        [DllImport("user32.dll")]
        private static extern bool EnumChildWindows(IntPtr parent, EnumCallback callback, IntPtr parameter);
        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetClassName(IntPtr window, StringBuilder text, int capacity);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetWindowText(IntPtr window, StringBuilder text, int capacity);
        [DllImport("user32.dll")]
        private static extern bool IsWindowVisible(IntPtr window);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr SendMessageTimeout(IntPtr window, uint message, UIntPtr length,
            StringBuilder text, uint flags, uint timeoutMilliseconds, out UIntPtr result);

        public static bool IsStartupDialogTitle(string title) {
            if (title == null) return false;
            string[] titles = { "Launcher Error", "Engine Error", "Fatal Error", "Error",
                "Counter-Strike 2", "Counter-Strike 2 Error", "CS2", "CS2 Error" };
            foreach (string candidate in titles)
                if (String.Equals(title.Trim(), candidate, StringComparison.OrdinalIgnoreCase)) return true;
            return false;
        }

        private static string ClassName(IntPtr window) {
            StringBuilder text = new StringBuilder(128);
            return GetClassName(window, text, text.Capacity) > 0 ? text.ToString() : String.Empty;
        }

        private static string StaticText(IntPtr window) {
            const int capacity = 8192;
            StringBuilder text = new StringBuilder(capacity);
            UIntPtr copied;
            // WM_GETTEXT is read-only. SMTO_BLOCK | SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT
            // bounds a stuck or disappearing control to 100 ms instead of hanging launch.
            IntPtr result = SendMessageTimeout(window, 0x000D, new UIntPtr((uint)capacity),
                text, 0x0023, 100, out copied);
            if (result == IntPtr.Zero || copied.ToUInt64() >= capacity - 1) return null;
            return text.ToString();
        }

        public static Cs2StartupDialog[] ReadDialogs(uint processId) {
            List<Cs2StartupDialog> dialogs = new List<Cs2StartupDialog>();
            if (processId == 0) return dialogs.ToArray();
            EnumWindows(delegate(IntPtr window, IntPtr parameter) {
                uint owner;
                GetWindowThreadProcessId(window, out owner);
                if (owner != processId || !IsWindowVisible(window) || ClassName(window) != "#32770") return true;
                StringBuilder title = new StringBuilder(512);
                GetWindowText(window, title, title.Capacity);
                if (!IsStartupDialogTitle(title.ToString())) return true;
                List<KeyValuePair<long, string>> messages = new List<KeyValuePair<long, string>>();
                int controlsRead = 0;
                EnumChildWindows(window, delegate(IntPtr child, IntPtr childParameter) {
                    uint childOwner;
                    GetWindowThreadProcessId(child, out childOwner);
                    if (childOwner != processId || !IsWindowVisible(child) || ClassName(child) != "Static") return true;
                    ++controlsRead;
                    string message = StaticText(child);
                    if (!String.IsNullOrWhiteSpace(message))
                        messages.Add(new KeyValuePair<long, string>(child.ToInt64(), message));
                    return controlsRead < 16;
                }, IntPtr.Zero);
                messages.Sort(delegate(KeyValuePair<long, string> first, KeyValuePair<long, string> second) {
                    return first.Key.CompareTo(second.Key);
                });
                List<string> text = new List<string>();
                foreach (KeyValuePair<long, string> message in messages) text.Add(message.Value);
                dialogs.Add(new Cs2StartupDialog { WindowHandle = window.ToInt64(), ClassName = "#32770",
                    Title = title.ToString(), StaticText = text.ToArray() });
                return dialogs.Count < 4;
            }, IntPtr.Zero);
            return dialogs.ToArray();
        }
    }
}
'@
}

# Pure classification also accepts plain objects, allowing fixtures without a running game.
function Get-Cs2StartupFailureMessage {
    param([AllowNull()][AllowEmptyCollection()][object[]]$Dialogs)
    foreach ($dialog in @($Dialogs | Sort-Object WindowHandle)) {
        if ($null -eq $dialog -or $dialog.ClassName -ne '#32770' -or
            -not [SourceMvm.Automation.Cs2StartupWindows]::IsStartupDialogTitle([string]$dialog.Title)) { continue }
        $text = (@($dialog.StaticText | Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) }) -join [Environment]::NewLine).Trim()
        if ($text -match '(?im)^\s*FATAL ERROR\s*:' -or
            $text -match '(?i)Failed to initialize the Steamworks SDK') { return $text }
    }
    return $null
}

function Get-Cs2StartupFailure {
    param([Parameter(Mandatory = $true)][ValidateRange(1, 2147483647)][int]$ProcessId)
    # Do not inspect unrelated apps if a stale PID was supplied or reused.
    try {
        $process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
        if ($null -eq $process -or $process.ProcessName -ine 'cs2') { return $null }
        $dialogs = [SourceMvm.Automation.Cs2StartupWindows]::ReadDialogs([uint32]$ProcessId)
    } catch {
        # The process/dialog may disappear during a probe. The launcher's existing
        # process-exit and readiness checks remain authoritative when no text can be read.
        return $null
    }
    Get-Cs2StartupFailureMessage -Dialogs $dialogs
}
