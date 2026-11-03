using System;
using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;

namespace SourceMovieMakerHost
{
    internal static class LaunchDiagnostics
    {
        internal static Exception CreateProcessError(int error, string programPath)
        {
            string message = "Windows could not start the game (CreateProcessW error " + error
                + ": " + new Win32Exception(error).Message + ").";
            if (error == 5)
                message += " Check the game's file permissions and Windows Security Protection history."
                    + " Check whether Steam or the game has a Run as administrator compatibility setting;"
                    + " start Steam and SOURCE:MVM normally under the same Windows account.";
            else if (error == 740)
                message += " The game requires elevation. Check its Run as administrator compatibility setting;"
                    + " start Steam and SOURCE:MVM normally under the same Windows account.";
            return new InvalidOperationException(message + Environment.NewLine + "Game: " + programPath);
        }

        internal static void ObserveStartup(IntPtr process, uint processId, uint milliseconds)
        {
            DateTime startedUtc = DateTime.UtcNow.AddSeconds(-5);
            uint result = WaitForSingleObject(process, milliseconds);
            if (result == 0x00000102) return; // WAIT_TIMEOUT: still running.
            if (result == 0xffffffff)
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not monitor game startup.");
            uint exitCode;
            if (!GetExitCodeProcess(process, out exitCode))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not read the game's startup exit code.");
            string message = "The game exited during startup (PID " + processId
                + ", exit code 0x" + exitCode.ToString("X8") + ").";
            // The hook writes a crash report even this early, but the "CS2 crashed last time" prompt
            // only shows in the main menu, which a game that dies at every start never reaches. Name
            // the report here, so the player has something to send (a player sent only this message
            // and launch.log, which say nothing about where it crashed).
            string summary;
            string report = FindCrashReport(startedUtc, out summary);
            if (report != null)
                message += " SOURCE:MVM saved a crash report (" + summary + "). Please send it with your bug report: "
                    + report + (File.Exists(Path.ChangeExtension(report, ".dmp")) ? " and the .dmp file beside it." : ".")
                    + " If an NVIDIA overlay or Ansel (photo mode) is on, try once with it off.";
            else
                message += " Check that Steam is signed in under the same Windows account and verify the game files in Steam.";
            throw new InvalidOperationException(message + " If DLSS 5 is enabled, try once with it off.");
        }

        // When the crash happened, from the name the hook gives the report
        // (SourceMVM-crash-2026-11-03_08-43-59, local time). Not the file's write time: a later
        // session rebuilds an earlier crash's .pending into a fresh .txt, and that old crash must not
        // be named as this start's failure.
        internal static DateTime? CrashTime(string fileName)
        {
            const string prefix = "SourceMVM-crash-";
            string stamp = Path.GetFileNameWithoutExtension(fileName);
            DateTime at;
            if (!stamp.StartsWith(prefix, StringComparison.Ordinal)
                || !DateTime.TryParseExact(stamp.Substring(prefix.Length), "yyyy-MM-dd_HH-mm-ss",
                    CultureInfo.InvariantCulture, DateTimeStyles.AssumeLocal, out at))
                return null;
            return at;
        }

        // The newest SourceMVM-crash-*.txt from a crash since `sinceUtc`, and its Summary line.
        internal static string FindCrashReport(DateTime sinceUtc, out string summary)
        {
            summary = "";
            try
            {
                // The hook's own choice before its session starts: %APPDATA% from the environment.
                string appData = Environment.GetEnvironmentVariable("APPDATA");
                if (string.IsNullOrEmpty(appData)) appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
                string folder = Path.Combine(appData, "HLAE", "bugreports");
                if (!Directory.Exists(folder)) return null;
                // The full report, else the header the crash handler writes first (<base>.pending):
                // when the report itself cannot be finished, only a later session that reaches the
                // menu rebuilds it, and a game that dies at every start never gets there.
                DateTime sinceLocal = sinceUtc.ToLocalTime();
                FileInfo newest = null;
                foreach (string pattern in new[] { "SourceMVM-crash-*.txt", "SourceMVM-crash-*.pending" })
                {
                    newest = new DirectoryInfo(folder).GetFiles(pattern)
                        .Select(f => new { File = f, At = CrashTime(f.Name) })
                        .Where(c => c.At.HasValue && c.At.Value >= sinceLocal)
                        .OrderByDescending(c => c.At.Value)
                        .Select(c => c.File)
                        .FirstOrDefault();
                    if (newest != null) break;
                }
                if (newest == null) return null;
                foreach (string line in File.ReadLines(newest.FullName).Take(40))
                {
                    if (!line.StartsWith("Summary", StringComparison.Ordinal)) continue;
                    int colon = line.IndexOf(':');
                    summary = colon >= 0 ? line.Substring(colon + 1).Trim() : line.Trim();
                    break;
                }
                if (summary.Length == 0) summary = "crash";
                return newest.FullName;
            }
            catch (Exception)
            {
                return null; // a diagnostic extra: never let it hide the startup failure itself
            }
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);
    }
}
