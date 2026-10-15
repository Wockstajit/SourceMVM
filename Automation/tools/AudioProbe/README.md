# Output audio diagnostics

This optional Windows helper enumerates render endpoints and records a bounded WASAPI loopback WAV. It never opens a microphone or changes volume or routing. `--pid` selects the single endpoint with an active session for that process. Inactive sessions retained after device changes are listed but do not select a device. No match, or more than one active endpoint, is an error.

The recording includes all output mixed onto that endpoint, not just the selected process. Use a quiet baseline, and compare the known source waveform before attributing any output to the game. Do not infer audible output from a video screenshot or a successful playback API call.

Requires .NET SDK 8 or later and the pinned NAudio.Wasapi 2.3.0 NuGet dependency (development only). Build from the repository root, and keep all generated artifacts under the ignored build directory:

```powershell
dotnet build Automation/tools/AudioProbe/AudioProbe.csproj -c Release --artifacts-path build/automation/audio-probe
& build/automation/audio-probe/bin/AudioProbe/release/AudioProbe.exe --list --pid (Get-Process cs2).Id
& build/automation/audio-probe/bin/AudioProbe/release/AudioProbe.exe --pid (Get-Process cs2).Id --seconds 10 --out Automation/runs/audio-check.wav
```

Omitting `--pid` captures the default multimedia output endpoint. `--list` is read-only and does not record. The tool rejects existing output files. The duration must be finite and between 1 and 60 seconds. Run under the signed-in user so that the game process and audio sessions are visible.

The tool writes JSON readiness and result records to stdout. A companion `.wav.json` contains elapsed packet timestamps, byte counts, and peak and RMS measurements. The capture ends on wall-clock time, even if the endpoint supplies no packets. The WAV duration can therefore be shorter than the elapsed time, so check the packet timing before interpreting silent gaps. Capture errors produce a nonzero exit status. A partial WAV alone does not prove success. A capture with no packets reports zero captured duration, which is different from a full-duration silent recording.

The [NAudio loopback documentation](https://github.com/naudio/NAudio/blob/v2.3.0/Docs/WasapiLoopbackCapture.md) describes the endpoint capture and silence behavior. For game comparisons, read and record `snd_mute_losefocus` first. If you disable it temporarily to test a background game, restore its original value in a `finally` block. Also record the session and endpoint mute and volume from `--list`.
