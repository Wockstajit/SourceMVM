using System.Diagnostics;
using System.Text.Json;
using NAudio.CoreAudioApi;
using NAudio.Wave;

// Output-device loopback only. Never opens a microphone or changes routing/volume.
// A PID selects its render endpoint, NOT a process-isolated stream: other audio on that
// endpoint is mixed in. Compare a quiet baseline and correlate the known source waveform.
try
{
    int? targetPid = null;
    string? output = null;
    var seconds = 10.0;
    var list = false;
    for (var i = 0; i < args.Length; i++)
    {
        switch (args[i])
        {
            case "--list": list = true; break;
            case "--pid": targetPid = int.Parse(args[++i]); break;
            case "--out": output = Path.GetFullPath(args[++i]); break;
            case "--seconds": seconds = double.Parse(args[++i], System.Globalization.CultureInfo.InvariantCulture); break;
            default: throw new ArgumentException("Usage: AudioProbe --list [--pid PID] | --out file.wav [--pid PID] [--seconds 1..60]");
        }
    }
    if (!double.IsFinite(seconds) || seconds < 1 || seconds > 60)
        throw new ArgumentException("Capture duration must be between 1 and 60 seconds.");
    if (targetPid is <= 0) throw new ArgumentException("PID must be positive.");
    if (!list && output == null) throw new ArgumentException("Specify --list or --out.");
    if (output != null && (File.Exists(output) || File.Exists(output + ".json")))
        throw new IOException("Output already exists; choose a new evidence file.");

    using var enumerator = new MMDeviceEnumerator();
    var devices = enumerator.EnumerateAudioEndPoints(DataFlow.Render, DeviceState.Active).ToArray();
    try
    {
        var matching = new List<MMDevice>();
        foreach (var device in devices)
        {
            var sessions = device.AudioSessionManager.Sessions;
            var targetSessions = new List<object>();
            var activeTarget = false;
            for (var i = 0; i < sessions.Count; i++)
            {
                using var session = sessions[i];
                if (targetPid == null || session.GetProcessID != (uint)targetPid.Value) continue;
                targetSessions.Add(new { pid = session.GetProcessID, state = session.State.ToString(),
                    muted = session.SimpleAudioVolume.Mute, volume = session.SimpleAudioVolume.Volume });
                activeTarget |= session.State == NAudio.CoreAudioApi.Interfaces.AudioSessionState.AudioSessionStateActive;
            }
            if (activeTarget) matching.Add(device);
            if (list) Console.WriteLine(JsonSerializer.Serialize(new { kind = "endpoint", device.ID, device.FriendlyName,
                muted = device.AudioEndpointVolume.Mute, volume = device.AudioEndpointVolume.MasterVolumeLevelScalar, targetSessions }));
        }
        if (list) return 0;
        if (targetPid != null && matching.Count != 1)
            throw new InvalidOperationException($"PID {targetPid} has active sessions on {matching.Count} endpoints; one is required.");

        using var defaultDevice = targetPid == null ? enumerator.GetDefaultAudioEndpoint(DataFlow.Render, Role.Multimedia) : null;
        var selected = targetPid == null ? defaultDevice! : matching[0];
        Directory.CreateDirectory(Path.GetDirectoryName(output!)!);
        using var capture = new WasapiLoopbackCapture(selected);
        // Fixed float format allows quantitative comparison without interpreting extensible codecs.
        capture.WaveFormat = WaveFormat.CreateIeeeFloatWaveFormat(48000, 2);
        using var file = new FileStream(output!, FileMode.CreateNew, FileAccess.Write, FileShare.Read);
        using var writer = new WaveFileWriter(file, capture.WaveFormat);
        using var stopped = new ManualResetEventSlim();
        Exception? failure = null;
        long bytes = 0;
        double sumSquares = 0, peak = 0;
        var watch = Stopwatch.StartNew();
        var packets = new List<object>();
        capture.DataAvailable += (_, e) =>
        {
            writer.Write(e.Buffer, 0, e.BytesRecorded);
            double packetPeak = 0;
            for (var offset = 0; offset + 4 <= e.BytesRecorded; offset += 4)
            {
                var sample = BitConverter.ToSingle(e.Buffer, offset);
                sumSquares += sample * (double)sample;
                packetPeak = Math.Max(packetPeak, Math.Abs(sample));
            }
            peak = Math.Max(peak, packetPeak);
            bytes += e.BytesRecorded;
            packets.Add(new { atSeconds = watch.Elapsed.TotalSeconds, bytes = e.BytesRecorded, peak = packetPeak });
        };
        capture.RecordingStopped += (_, e) => { failure = e.Exception; stopped.Set(); };
        capture.StartRecording();
        Console.WriteLine(JsonSerializer.Serialize(new { kind = "ready", endpoint = selected.FriendlyName,
            targetPid, scope = "whole output endpoint", output, seconds }));
        Console.Out.Flush();
        try { stopped.Wait(TimeSpan.FromSeconds(seconds)); }
        finally { capture.StopRecording(); }
        if (!stopped.Wait(TimeSpan.FromSeconds(5))) throw new TimeoutException("Capture did not stop within five seconds.");
        if (failure != null) throw new IOException("Audio capture failed.", failure);
        writer.Flush();
        var result = new { kind = "result", wallSeconds = watch.Elapsed.TotalSeconds,
            capturedSeconds = bytes / (double)capture.WaveFormat.AverageBytesPerSecond,
            bytes, peak, rms = bytes > 0 ? Math.Sqrt(sumSquares / (bytes / 4)) : 0,
            note = "Silence may produce no packets; captured duration can be shorter than wall time.", packets };
        File.WriteAllText(output + ".json", JsonSerializer.Serialize(result, new JsonSerializerOptions { WriteIndented = true }));
        Console.WriteLine(JsonSerializer.Serialize(new { result.kind, result.wallSeconds, result.capturedSeconds, result.bytes, result.peak, result.rms }));
    }
    finally { foreach (var device in devices) device.Dispose(); }
    return 0;
}
catch (Exception e)
{
    Console.Error.WriteLine(e.Message);
    return 1;
}
