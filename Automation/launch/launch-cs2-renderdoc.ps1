# Single RenderDoc launch entry point. Scene setup belongs to the caller.
#
# CS2 is started suspended by the native host, which injects the SOURCE:MVM hook and then renderdoc.dll,
# so RenderDoc wraps the D3D11 device from the first frame while the hook keeps working: it carries
# inline kernel32 hooks (AfxHookSource2/main.cpp) that RenderDoc's import-table rewrites cannot hide.
# renderdoccmd child capture and attaching to a running game do not work: the first leaves the hook
# unregistered, the second finds a device that already exists and captures nothing.
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [int]$Width = 854,
    [int]$Height = 480,
    [string]$RenderDocDll = $env:SOURCEMVM_RENDERDOC
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $RenderDocDll) {
    $candidates = @("$env:ProgramFiles\RenderDoc\renderdoc.dll")
    $candidates += @(Get-ChildItem "$env:USERPROFILE\tools\RenderDoc\RenderDoc_*\renderdoc.dll" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | ForEach-Object FullName)
    $RenderDocDll = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $RenderDocDll -or -not (Test-Path -LiteralPath $RenderDocDll)) { throw 'RenderDoc is missing. Pass -RenderDocDll with the installed renderdoc.dll path.' }
& (Join-Path $repo 'Automation\renderdoc\build-bridge.ps1') | Out-Host
$bridge = Join-Path $repo 'Automation\output\renderdoc-bridge-build\Release\SourceMvmRenderDocBridge.dll'
dotnet build (Join-Path $repo 'SourceMovieMakerLauncher\host\SourceMovieMakerHost.csproj') -c Release -o (Join-Path $repo 'SourceMovieMakerLauncher\host\build') | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Native launch host build failed.' }
$nativeHost = Join-Path $repo 'SourceMovieMakerLauncher\host\build\SourceMovieMakerHost.exe'

& (Join-Path $PSScriptRoot 'launch-cs2-netcon.ps1') -Port $Port -Width $Width -Height $Height -DebugHelper $RenderDocDll -NativeHostOverride $nativeHost -SettleSeconds 45

# The bridge only needs the RenderDoc API that is already resident, so it can attach to the running game.
$cs2 = @(Get-Process cs2 -ErrorAction Stop)
if ($cs2.Count -ne 1) { throw 'Expected exactly one running CS2 process.' }
$injector = Join-Path $repo 'build\source2-release\bin\x64\injector.exe'
& $nativeHost --inject-pid $cs2[0].Id --injector $injector --inject-dll $bridge
if ($LASTEXITCODE -ne 0) { throw 'Attaching the RenderDoc bridge failed.' }
$modules = @((Get-Process -Id $cs2[0].Id).Modules.ModuleName)
foreach ($required in @('renderdoc.dll', 'AfxHookSource2.dll', 'SourceMvmRenderDocBridge.dll')) {
    if ($modules -notcontains $required) { throw "CS2 PID $($cs2[0].Id) is missing $required." }
}
$captureDir = Join-Path $repo 'Automation\output\renderdoc-relight-audit'
New-Item -ItemType Directory -Force -Path $captureDir | Out-Null
$control = Join-Path $repo 'Automation\renderdoc\renderdoc-control.ps1'
& $control -ProcessId $cs2[0].Id -Action path -Value (Join-Path $captureDir 'source-mvm') | Out-Null
& $control -ProcessId $cs2[0].Id -Action status
Write-Host "RenderDoc and SOURCE:MVM are both loaded in CS2 PID $($cs2[0].Id); captures go to $captureDir."
