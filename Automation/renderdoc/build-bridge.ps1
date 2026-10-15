param(
    [string]$RenderDocSource = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if ([string]::IsNullOrWhiteSpace($RenderDocSource)) {
    $RenderDocSource = Join-Path $repo 'Automation\output\renderdoc-src'
}
$header = Join-Path $RenderDocSource 'renderdoc\api\app\renderdoc_app.h'
if (-not (Test-Path -LiteralPath $header)) {
    throw "RenderDoc source/header missing at $header. Run Automation\renderdoc\sync-renderdoc.ps1 first."
}

$source = Join-Path $repo 'Automation\renderdoc\bridge'
$build = Join-Path $repo 'Automation\output\renderdoc-bridge-build'
cmake -S $source -B $build "-DRENDERDOC_SOURCE_DIR=$RenderDocSource"
if ($LASTEXITCODE -ne 0) { throw 'RenderDoc bridge configure failed.' }
cmake --build $build --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'RenderDoc bridge build failed.' }

$dll = Join-Path $build 'Release\SourceMvmRenderDocBridge.dll'
if (-not (Test-Path -LiteralPath $dll)) { throw "Bridge DLL missing after build: $dll" }
Write-Output $dll

