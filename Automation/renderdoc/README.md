# RenderDoc debug bridge

This folder is development tooling. `SourceMvmRenderDocBridge.dll` is not linked into or installed with `AfxHookSource2.dll`, the launcher package, or the full SOURCE:MVM build.

The bridge uses RenderDoc's official runtime application API. The API header and its MIT license come from a sparse checkout of `https://github.com/baldurk/renderdoc` under the ignored `Automation/output/renderdoc-src/` folder.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Automation/renderdoc/sync-renderdoc.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File Automation/renderdoc/build-bridge.ps1
```

## Launch CS2 with the mod and RenderDoc

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Automation/launch/launch-cs2-renderdoc.ps1 -Width 2560 -Height 1440
```

The native host starts CS2 suspended, injects the SOURCE:MVM hook, then injects `renderdoc.dll`. RenderDoc therefore wraps the D3D11 device from the first frame. The bridge attaches to the running game afterwards (`SourceMovieMakerHost.exe --inject-pid`). Two other routes do not work and should not be revived:

- `renderdoccmd capture --opt-hook-children`: the hook never registers `mirv_filmmaker`.
- Attaching RenderDoc to an already running game: the D3D11 device already exists, so nothing is captured.

RenderDoc rewrites the kernel32 import slots the hook relies on, including later, on its own schedule. The hook therefore also hooks `LoadLibraryExW` and `GetProcAddress` inline (`InstallInlineKernel32Hooks` in `AfxHookSource2/main.cpp`). Without that, `mirv_*` stays unregistered and the first client hook crashes.

The controller talks to a PID-specific, local-only named pipe. It does not focus the game or synthesize keyboard input.

```powershell
# Confirm the in-process RenderDoc API and current capture template.
powershell -NoProfile -ExecutionPolicy Bypass -File Automation/renderdoc/renderdoc-control.ps1 -Action status

# Capture the next frame and give it a title in RenderDoc.
powershell -NoProfile -ExecutionPolicy Bypass -File Automation/renderdoc/renderdoc-control.ps1 -Action capture -Value hallway-A-baked

# Change the capture filename template.
powershell -NoProfile -ExecutionPolicy Bypass -File Automation/renderdoc/renderdoc-control.ps1 -Action path -Value C:\captures\hallway
```

The bridge reports JSON on stdout. Captures made by the launcher default to `Automation/output/renderdoc-relight-audit/source-mvm_frame*.rdc`.
