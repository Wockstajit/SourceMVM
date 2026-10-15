using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace SourceMovieMakerHost
{
    internal static class LaunchDiagnostics
    {
        // Detect an immediate Steam/loader/crash exit; this is not a game-readiness probe.
        internal const uint StartupObservationMilliseconds = 8000;

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
            uint result = WaitForSingleObject(process, milliseconds);
            if (result == 0x00000102) return; // WAIT_TIMEOUT: still running.
            if (result == 0xffffffff)
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not monitor game startup.");
            uint exitCode;
            if (!GetExitCodeProcess(process, out exitCode))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not read the game's startup exit code.");
            throw new InvalidOperationException("The game exited during startup (PID " + processId
                + ", exit code 0x" + exitCode.ToString("X8") + ")."
                + " Check that Steam is signed in under the same Windows account and verify the game files in Steam."
                + " If DLSS 5 is enabled, try once with it off.");
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);
    }
}
