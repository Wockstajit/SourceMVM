$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $csc)) { throw '.NET Framework C# compiler was not found.' }
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('source-mvm-host-tests-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
try {
    $exe = Join-Path $testRoot 'LauncherHostTests.exe'
    & $csc /nologo /target:exe /platform:x64 "/out:$exe" `
        (Join-Path $repo 'SourceMovieMakerLauncher/host/LaunchDiagnostics.cs') `
        (Join-Path $PSScriptRoot 'launcher-host-tests.cs')
    if ($LASTEXITCODE -ne 0) { throw "Host tests did not compile ($LASTEXITCODE)." }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "Host tests failed ($LASTEXITCODE)." }
} finally {
    # Only remove the exact disposable directory created above.
    $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
    $tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\source-mvm-host-tests-'
    if (-not $resolvedTestRoot.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to clean a directory outside the host-test temporary folder.'
    }
    Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
}
