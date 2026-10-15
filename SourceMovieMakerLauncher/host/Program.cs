using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace SourceMovieMakerHost
{
    internal static class Program
    {
        private const uint CreateSuspended = 0x00000004;
        private const uint DetachedProcess = 0x00000008;
        private const uint CreateNewProcessGroup = 0x00000200;
        private const string InteractiveDesktop = @"winsta0\default";

        private static int Main(string[] args)
        {
            // Electron decodes the captured streams as UTF-8, including localized errors/paths.
            Console.OutputEncoding = new UTF8Encoding(false);
            PROCESS_INFORMATION processInfo = default(PROCESS_INFORMATION);
            bool resumed = false;

            // Attach-only mode for tools that must load after the SOURCE:MVM hook (RenderDoc).
            if (args.Length > 0 && args[0] == "--inject-pid")
                return InjectOnly(args);

            try
            {
                Options options = Options.Parse(args);
                options.ResolveSteamPath();
                Validate(options);

                // CreateProcessW can target a different desktop from this helper. Supply the
                // Steam launch contract explicitly so the child does not lose it while crossing
                // from the automation desktop to winsta0\\default.
                Environment.SetEnvironmentVariable("SteamPath", options.SteamPath);
                Environment.SetEnvironmentVariable("SteamClientLaunch", "1");
                // The APP ID IS A PARAMETER, not a constant. It used to be hardcoded to CS2's 730,
                // which meant this host could only ever launch CS2: pointed at another Source 2
                // game it created the process, injected, resumed -- and the game exited
                // immediately and silently, because Steam had told it that it was a different
                // application. That failure looks exactly like a crashing hook and is not one.
                Environment.SetEnvironmentVariable("SteamGameId", options.SteamAppId);
                Environment.SetEnvironmentVariable("SteamAppId", options.SteamAppId);
                Environment.SetEnvironmentVariable("SteamOverlayGameId", options.SteamAppId);
				string processPath = Environment.GetEnvironmentVariable("Path") ?? string.Empty;
				if (processPath.IndexOf(options.SteamPath, StringComparison.OrdinalIgnoreCase) < 0)
					Environment.SetEnvironmentVariable("Path", options.SteamPath + ";" + processPath);
                Console.WriteLine("SteamPath: " + Environment.GetEnvironmentVariable("SteamPath")
                    + "  appid: " + options.SteamAppId);

                string programPath = Path.GetFullPath(options.Cs2Path);
                string hookPath = Path.GetFullPath(options.HookPath);
                string injectorPath = Path.GetFullPath(options.InjectorPath);
                string commandLine = $"\"{programPath}\" {options.CommandLine}".TrimEnd();
                string workingDirectory = Path.GetDirectoryName(programPath);
                Console.WriteLine("Game: " + programPath);
                Console.WriteLine("Hook: " + hookPath);
                Console.WriteLine("Injector: " + injectorPath);
                Console.WriteLine("Desktop: " + InteractiveDesktop);

                var startupInfo = new STARTUPINFOW
                {
                    cb = (uint)Marshal.SizeOf(typeof(STARTUPINFOW)),
                    // The automation host can be launched from a non-visible desktop. Explicitly
                    // target the interactive desktop so CS2 appears for the operator rather than
                    // only as a background process in Task Manager.
                    lpDesktop = InteractiveDesktop,
                };

                if (!CreateProcessW(
                    programPath,
                    new StringBuilder(commandLine),
                    null,
                    null,
                    // The game needs no parent handles. Inheriting captured stdout/stderr keeps
                    // automation callers waiting for EOF until the game exits, even after this
                    // host and its launcher have finished. Injection opens the child by PID.
                    false,
                    CreateSuspended
                        | DetachedProcess
                        | CreateNewProcessGroup,
                    IntPtr.Zero,
                    workingDirectory,
                    ref startupInfo,
                    out processInfo))
                {
                    int error = Marshal.GetLastWin32Error();
                    throw LaunchDiagnostics.CreateProcessError(error, programPath);
                }

                Console.WriteLine($"Created CS2 process {processInfo.dwProcessId} suspended.");
                // The hook goes in first, then the optional helper (renderdoc.dll for RenderDoc runs), both
                // while the process is still suspended so the helper is resident before the D3D11 device is
                // created. The hook keeps working next to RenderDoc through its inline kernel32 hooks.
                InjectorClient.Inject(injectorPath, processInfo.dwProcessId, hookPath);
                if (!string.IsNullOrWhiteSpace(options.DebugHelperPath))
                    InjectorClient.Inject(injectorPath, processInfo.dwProcessId, options.DebugHelperPath);

                if (options.SettleMilliseconds > 0)
                    Thread.Sleep(options.SettleMilliseconds);

                // RenderDoc's child hook can add one suspend count while it installs its
                // graphics interception. CreateProcess supplied the first count; release any
                // additional startup counts here so the exact child we injected can reach its
                // entry point. A normal launch returns 1 on the first call and exits this loop.
                for (int resumeCount = 0; resumeCount < 8; resumeCount++)
                {
                    uint priorSuspendCount = ResumeThread(processInfo.hThread);
                    if (priorSuspendCount == uint.MaxValue)
                        throw new InvalidOperationException(
                            $"ResumeThread failed with error {Marshal.GetLastWin32Error()}.");
                    if (priorSuspendCount <= 1)
                        break;
                    if (resumeCount == 7)
                        throw new InvalidOperationException("CS2 retained too many startup suspend counts.");
                }

                resumed = true;
                if (!string.IsNullOrWhiteSpace(options.ProcessIdFile))
                    File.WriteAllText(options.ProcessIdFile, processInfo.dwProcessId.ToString());
                Console.WriteLine("Hook injected; CS2 resumed.");
                if (options.WatchStartupMilliseconds == 0)
                    return 0;
                // Opt-in: callers that wait for this host's exit must not pay for the watch. The
                // launcher reports success on the line above and keeps reading. The original
                // handle catches even an exit with code 0 or 259.
                try
                {
                    LaunchDiagnostics.ObserveStartup(processInfo.hProcess, processInfo.dwProcessId,
                        options.WatchStartupMilliseconds);
                }
                catch (System.ComponentModel.Win32Exception exception)
                {
                    // The game may well be running; a broken watch is not a launch failure.
                    Console.WriteLine("Startup watch unavailable: " + exception.Message);
                    return 0;
                }
                Console.WriteLine("Game survived initial startup.");
                return 0;
            }
            catch (Exception exception)
            {
                Console.Error.WriteLine(exception.Message);
                return 1;
            }
            finally
            {
                if (processInfo.hProcess != IntPtr.Zero)
                {
                    if (!resumed)
                        TerminateProcess(processInfo.hProcess, 1);
                    CloseHandle(processInfo.hThread);
                    CloseHandle(processInfo.hProcess);
                }
            }
        }

        // --inject-pid <pid> --injector <injector.exe> --inject-dll <dll>: loads one DLL into a process
        // that is already running, through the same injector the launch path uses.
        private static int InjectOnly(string[] args)
        {
            uint processId = 0;
            string injectorPath = null;
            string dllPath = null;
            for (int i = 0; i + 1 < args.Length; i += 2)
            {
                switch (args[i])
                {
                    case "--inject-pid": processId = uint.Parse(args[i + 1]); break;
                    case "--injector": injectorPath = Path.GetFullPath(args[i + 1]); break;
                    case "--inject-dll": dllPath = Path.GetFullPath(args[i + 1]); break;
                    default: Console.Error.WriteLine("Unknown argument: " + args[i]); return 2;
                }
            }
            if (processId == 0 || injectorPath == null || dllPath == null)
            {
                Console.Error.WriteLine("Usage: --inject-pid <pid> --injector <injector.exe> --inject-dll <dll>");
                return 2;
            }
            try
            {
                InjectorClient.Inject(injectorPath, processId, dllPath);
                Console.WriteLine($"Injected {dllPath} into PID {processId}.");
                return 0;
            }
            catch (Exception exception)
            {
                Console.Error.WriteLine(exception.Message);
                return 1;
            }
        }

        private static void Validate(Options options)
        {
            RequireFile(options.Cs2Path, "the game executable");
            RequireFile(options.HookPath, "the hook DLL");
            RequireFile(options.InjectorPath, "injector.exe");
            RequireFile(Path.Combine(options.SteamPath, "steam.exe"), "Steam client executable");
            RequireFile(Path.Combine(options.SteamPath, "steamclient64.dll"), "Steamworks client library");
            if (System.Diagnostics.Process.GetProcessesByName("steam").Length == 0)
                throw new InvalidOperationException("Start Steam and sign in before launching the offline game.");
            if (!string.IsNullOrWhiteSpace(options.DebugHelperPath))
                RequireFile(options.DebugHelperPath, "the optional debug helper DLL");
            if (string.IsNullOrWhiteSpace(options.SteamPath) || !Directory.Exists(options.SteamPath))
                throw new DirectoryNotFoundException("Steam install directory was not found: " + options.SteamPath);
        }

        private static void RequireFile(string filePath, string label)
        {
            if (string.IsNullOrWhiteSpace(filePath) || !File.Exists(filePath))
                throw new FileNotFoundException($"{label} was not found.", filePath);
        }

        private sealed class Options
        {
            public string Cs2Path { get; private set; }
            public string HookPath { get; private set; }
            public string InjectorPath { get; private set; }
            public string SteamPath { get; private set; }
            public string DebugHelperPath { get; private set; }
            public string ProcessIdFile { get; private set; }
            public string CommandLine { get; private set; } = "-steam -insecure";
            // Defaults to CS2 so every existing caller keeps working without passing it.
            public string SteamAppId { get; private set; } = "730";
            public int SettleMilliseconds { get; private set; } = 2000;
            public uint WatchStartupMilliseconds { get; private set; }

            public void ResolveSteamPath()
            {
                var candidates = new List<string>();
                foreach (var process in System.Diagnostics.Process.GetProcessesByName("steam"))
                {
                    try { candidates.Add(Path.GetDirectoryName(process.MainModule.FileName)); }
                    catch (Exception) { }
                    finally { process.Dispose(); }
                }
                candidates.Add(SteamPath);
                candidates.Add(Environment.GetEnvironmentVariable("SteamPath"));
                candidates.Add(Microsoft.Win32.Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) as string);
                candidates.Add(Microsoft.Win32.Registry.GetValue(@"HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath", null) as string);
                foreach (string candidate in candidates)
                {
                    if (string.IsNullOrWhiteSpace(candidate)) continue;
                    string normalized = candidate.Replace('/', '\\');
                    if (File.Exists(Path.Combine(normalized, "steam.exe")) && File.Exists(Path.Combine(normalized, "steamclient64.dll")))
                    { SteamPath = Path.GetFullPath(normalized); return; }
                }
                throw new DirectoryNotFoundException("Cannot resolve Steam client directory containing steam.exe and steamclient64.dll. Start Steam or pass --steam-path.");
            }

            public static Options Parse(string[] args)
            {
                var options = new Options();
                var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

                for (int index = 0; index < args.Length; index++)
                {
                    string name = args[index];
                    if (!name.StartsWith("-", StringComparison.Ordinal) || index + 1 >= args.Length)
                        continue;
                    values[name] = args[++index];
                }

                options.Cs2Path = Value(values, "--cs2", "--cs2-exe", "-programPath");
                options.HookPath = Value(values, "--hook", "--hook-dll", "-hookDllPath");
                options.InjectorPath = Value(values, "--injector", "-injectorPath");
                options.SteamPath = Value(values, "--steam-path");
                options.DebugHelperPath = Value(values, "--debug-helper");
                options.ProcessIdFile = Value(values, "--pid-file");
                string commandLine = Value(values, "--cmd-line", "-cmdLine");
                string settle = Value(values, "--settle-ms");
                string appId = Value(values, "--steam-appid", "--appid");
                if (!string.IsNullOrWhiteSpace(appId))
                    options.SteamAppId = appId.Trim();
                if (!string.IsNullOrWhiteSpace(commandLine))
                    options.CommandLine = commandLine;
                if (int.TryParse(settle, out int settleMilliseconds))
                    options.SettleMilliseconds = Math.Max(0, settleMilliseconds);
                if (uint.TryParse(Value(values, "--watch-startup-ms"), out uint watchMilliseconds))
                    options.WatchStartupMilliseconds = watchMilliseconds;
                return options;
            }

            private static string Value(Dictionary<string, string> values, params string[] names)
            {
                foreach (string name in names)
                {
                    if (values.TryGetValue(name, out string value))
                        return value;
                }
                return string.Empty;
            }
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct STARTUPINFOW
        {
            public uint cb;
            public string lpReserved;
            public string lpDesktop;
            public string lpTitle;
            public uint dwX;
            public uint dwY;
            public uint dwXSize;
            public uint dwYSize;
            public uint dwXCountChars;
            public uint dwYCountChars;
            public uint dwFillAttribute;
            public uint dwFlags;
            public ushort wShowWindow;
            public ushort cbReserved2;
            public IntPtr lpReserved2;
            public IntPtr hStdInput;
            public IntPtr hStdOutput;
            public IntPtr hStdError;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct PROCESS_INFORMATION
        {
            public IntPtr hProcess;
            public IntPtr hThread;
            public uint dwProcessId;
            public uint dwThreadId;
        }

        [StructLayout(LayoutKind.Sequential)]
        private sealed class SECURITY_ATTRIBUTES
        {
            public uint nLength;
            public IntPtr lpSecurityDescriptor;
            public bool bInheritHandle;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
        private static extern bool CreateProcessW(
            string lpApplicationName,
            StringBuilder lpCommandLine,
            SECURITY_ATTRIBUTES lpProcessAttributes,
            SECURITY_ATTRIBUTES lpThreadAttributes,
            bool bInheritHandles,
            uint dwCreationFlags,
            IntPtr lpEnvironment,
            string lpCurrentDirectory,
            ref STARTUPINFOW lpStartupInfo,
            out PROCESS_INFORMATION lpProcessInformation);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint ResumeThread(IntPtr hThread);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool TerminateProcess(IntPtr hProcess, uint exitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CloseHandle(IntPtr hObject);
    }
}
