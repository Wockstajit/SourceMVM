using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.Serialization;
using advancedfx.injector.interop;

namespace SourceMovieMakerHost
{
    internal static class InjectorClient
    {
        public static void Inject(string injectorPath, uint processId, string hookPath)
        {
            using (var injector = new Process())
            {
                injector.StartInfo = new ProcessStartInfo
                {
                    FileName = injectorPath,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardInput = true,
                    RedirectStandardOutput = true,
                };

                // Process.Start builds the injector's stdin writer from the console input encoding, and on a
                // UTF-8 (65001) console that writes EF BB BF ahead of the binary message, which the injector
                // rejects. Start it with a BOM-free encoding, then put the console back.
                System.Text.Encoding previousInputEncoding = null;
                try
                {
                    previousInputEncoding = Console.InputEncoding;
                    Console.InputEncoding = System.Text.Encoding.GetEncoding(1252);
                }
                catch (IOException)
                {
                    previousInputEncoding = null; // no console attached: nothing to override
                }

                bool started;
                try
                {
                    started = injector.Start();
                }
                finally
                {
                    if (previousInputEncoding != null)
                    {
                        try { Console.InputEncoding = previousInputEncoding; } catch (IOException) { }
                    }
                }

                if (!started)
                    throw new InvalidOperationException("The injector process did not start.");

                IFormatter formatter = new advancedfx.injector.interop.Formatter();
                string lastError = null;

                using (Stream injectorIn = injector.StandardInput.BaseStream)
                using (Stream injectorOut = injector.StandardOutput.BaseStream)
                {
                    var injectMessage = new InjectMessage
                    {
                        ProcessId = processId,
                        DllPath = hookPath,
                    };

                    formatter.Serialize(injectorIn, injectMessage);
                    injectorIn.Flush();

                    while (true)
                    {
                        var message = formatter.Deserialize(injectorOut) as ProcessMessage;
                        if (message == null)
                            throw new InvalidOperationException("The injector returned an unknown message.");

                        if (message is ContinueWaitingQuestion)
                        {
                            var response = new ContinueWaiting { Response = false };
                            formatter.Serialize(injectorIn, response);
                            injectorIn.Flush();
                            lastError = "The injector requested an interactive wait.";
                            continue;
                        }

                        if (message is Error)
                            lastError = message.GetType().Name;

                        var injectResponse = message as InjectResponse;
                        if (injectResponse == null)
                            continue;

                        injector.WaitForExit(5000);
                        if (!injectResponse.Response)
                            throw new InvalidOperationException(
                                lastError == null
                                    ? "The injector rejected the hook."
                                    : $"The injector rejected the hook: {lastError}.");

                        return;
                    }
                }
            }
        }
    }
}
