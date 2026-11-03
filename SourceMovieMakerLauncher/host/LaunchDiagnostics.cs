using System;
using System.ComponentModel;
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

        // The newest SourceMVM-crash-*.txt the hook wrote since `sinceUtc`, and its Summary line.
        private static string FindCrashReport(DateTime sinceUtc, out string summary)
        {
            summary = "";
            try
            {
                // The hook's own choice before its session starts: %APPDATA% from the environment.
                string appData = Environment.GetEnvironmentVariable("APPDATA");
                if (string.IsNullOrEmpty(appData)) appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
                string folder = Path.Combine(appData, "HLAE", "bugreports");
                if (!Directory.Exists(folder)) return null;
                FileInfo newest = new DirectoryInfo(folder).GetFiles("SourceMVM-crash-*.txt")
                    .Where(f => f.LastWriteTimeUtc >= sinceUtc)
                    .OrderByDescending(f => f.LastWriteTimeUtc)
                    .FirstOrDefault();
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
