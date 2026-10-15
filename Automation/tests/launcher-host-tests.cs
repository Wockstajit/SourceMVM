using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Threading;
using SourceMovieMakerHost;

internal static class LauncherHostTests
{
    private static void Check(bool condition, string message)
    {
        if (!condition) throw new Exception(message);
    }

    private static Process Child(string args)
    {
        return Process.Start(new ProcessStartInfo(Assembly.GetExecutingAssembly().Location, args)
        { UseShellExecute = false, CreateNoWindow = true });
    }

    private static int Main(string[] args)
    {
        if (args.Length == 2 && args[0] == "--child-exit") return int.Parse(args[1]);
        if (args.Length == 2 && args[0] == "--child-wait")
        {
            using (var done = EventWaitHandle.OpenExisting(args[1])) done.WaitOne(10000);
            return 0;
        }
        try
        {
            foreach (int code in new[] { 0, 42, 259, unchecked((int)0xc0000135) })
            {
                using (var child = Child("--child-exit " + code))
                {
                    Check(child.WaitForExit(5000), "Fixture child did not exit.");
                    try
                    {
                        LaunchDiagnostics.ObserveStartup(child.Handle, (uint)child.Id, 20);
                        throw new Exception("An exited game was reported as running.");
                    }
                    catch (InvalidOperationException error)
                    {
                        Check(error.Message.Contains("0x" + unchecked((uint)code).ToString("X8")), "Wrong startup exit code.");
                        Check(error.Message.Contains("PID " + child.Id), "Missing game PID.");
                    }
                }
            }
            Console.WriteLine("PASS: immediate exits (0, 42, STILL_ACTIVE/259, loader failure) are detected.");

            string eventName = "Local\\SourceMvmLaunchTest_" + Guid.NewGuid().ToString("N");
            using (var done = new EventWaitHandle(false, EventResetMode.ManualReset, eventName))
            using (var child = Child("--child-wait " + eventName))
            {
                try { LaunchDiagnostics.ObserveStartup(child.Handle, (uint)child.Id, 20); }
                finally { done.Set(); }
                Check(child.WaitForExit(5000), "Fixture child did not finish.");
            }
            Console.WriteLine("PASS: a living process completes the observation without being terminated.");

            try
            {
                LaunchDiagnostics.ObserveStartup(IntPtr.Zero, 0, 0);
                throw new Exception("Invalid monitor handle was ignored.");
            }
            catch (Win32Exception) { }
            Console.WriteLine("PASS: monitoring errors are not reported as launch success.");

            // Reproduce error 5 on our own disposable executable, never on a game/user file.
            string blocked = Path.Combine(Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location), "blocked.exe");
            File.Copy(Assembly.GetExecutingAssembly().Location, blocked);
            FileSecurity original = File.GetAccessControl(blocked);
            try
            {
                FileSecurity denied = File.GetAccessControl(blocked);
                denied.AddAccessRule(new FileSystemAccessRule(WindowsIdentity.GetCurrent().User,
                    FileSystemRights.ExecuteFile, AccessControlType.Deny));
                File.SetAccessControl(blocked, denied);
                try
                {
                    using (var child = Process.Start(new ProcessStartInfo(blocked, "--child-exit 0")
                        { UseShellExecute = false, CreateNoWindow = true })) child.WaitForExit(5000);
                    throw new Exception("Expected Windows to deny executing the fixture.");
                }
                catch (Win32Exception error)
                {
                    Check(error.NativeErrorCode == 5, "Expected ERROR_ACCESS_DENIED, got " + error.NativeErrorCode);
                    string message = LaunchDiagnostics.CreateProcessError(error.NativeErrorCode, blocked).Message;
                    Check(message.StartsWith("Windows could not start the game"), "Failure must lead the message.");
                    Check(message.Contains("error 5"), "Missing original Windows error.");
                    Check(message.Contains(blocked), "Missing executable path.");
                    Check(message.Contains("Protection history"), "Missing actionable guidance.");
                }
            }
            finally { File.SetAccessControl(blocked, original); }
            Check(LaunchDiagnostics.CreateProcessError(740, "game.exe").Message.Contains("requires elevation"), "Missing elevation explanation.");
            Check(LaunchDiagnostics.CreateProcessError(2, "game.exe").Message.Contains("error 2"), "Other error codes must be retained.");
            Console.WriteLine("PASS: real execute-permission denial reproduces Windows error 5 and actionable diagnostics.");
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }
}
