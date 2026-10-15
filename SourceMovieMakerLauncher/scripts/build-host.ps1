$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$project = Join-Path $projectRoot 'host\SourceMovieMakerHost.csproj'
$output = Join-Path $projectRoot 'host\build'

if (-not (Test-Path $project)) {
    throw "Launcher host project was not found: $project"
}

$msbuild = Get-Command msbuild.exe -ErrorAction SilentlyContinue
if ($null -eq $msbuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $path = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
        if ($path) {
            $msbuild = Get-Command $path.Trim() -ErrorAction SilentlyContinue
        }
    }
}

if ($null -eq $msbuild) {
    throw 'MSBuild was not found. Install Visual Studio Build Tools with the .NET desktop build tools workload.'
}

New-Item -ItemType Directory -Force -Path $output | Out-Null
& $msbuild.Source $project `
    /restore `
    /t:Rebuild `
    /p:Configuration=Release `
    /p:Platform=AnyCPU `
    /p:DebugSymbols=false `
    /p:DebugType=None `
    "/p:OutDir=$($output.TrimEnd('\'))\"

if ($LASTEXITCODE -ne 0) {
    throw "Launcher host build failed with exit code $LASTEXITCODE."
}

$hostExecutable = Join-Path $output 'SourceMovieMakerHost.exe'
if (-not (Test-Path $hostExecutable)) {
    throw "MSBuild completed without producing $hostExecutable"
}

Write-Host "Built launcher host: $hostExecutable"
