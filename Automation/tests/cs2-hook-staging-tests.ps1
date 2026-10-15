$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$automationRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $automationRoot 'lib/AutomationCommon.ps1')

function Assert-Equal($Actual, $Expected, [string]$Message) {
    if ($Actual -cne $Expected) { throw "${Message}: expected '$Expected', got '$Actual'" }
}

function New-FixtureDll([string]$Path, [string]$Content, [datetime]$Timestamp) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    [System.IO.File]::WriteAllText($Path, $Content)
    [System.IO.File]::SetLastWriteTimeUtc($Path, $Timestamp)
    Assert-Equal ([System.IO.File]::GetLastWriteTimeUtc($Path).Ticks) $Timestamp.Ticks 'Fixture timestamp precision'
}

function Assert-Dll([string]$Path, [string]$Content, [datetime]$Timestamp) {
    Assert-Equal ([System.IO.File]::ReadAllText($Path)) $Content 'DLL contents'
    Assert-Equal ([System.IO.File]::GetLastWriteTimeUtc($Path).Ticks) $Timestamp.Ticks 'Exact DLL timestamp'
}

function Invoke-Case([string]$Name, [scriptblock]$Body) {
    $fixture = Join-Path $testRoot $Name
    New-Item -ItemType Directory -Path $fixture | Out-Null
    $source2 = Join-Path $fixture 'build/x64-source2-release/AfxHookSource2/Release/AfxHookSource2.dll'
    $legacy = Join-Path $fixture 'build/x64-release/AfxHookSource2/Release/AfxHookSource2.dll'
    $staged = Join-Path $fixture 'staged runtime with spaces/x64/AfxHookSource2.dll'
    & $Body
    $script:passed++
    Write-Host "PASS $Name"
}

$baseTime = [datetime]::Parse('2020-02-03T04:05:06.1234567Z').ToUniversalTime()
$message = 'Staged newer AfxHookSource2.dll from the x64 release build.'
$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('cs2 hook staging tests ' + [guid]::NewGuid().ToString('N'))
$createdRoot = $false
$passed = 0

try {
    New-Item -ItemType Directory -Path $testRoot | Out-Null
    $createdRoot = $true

    Invoke-Case 'no build' {
        $result = Get-NewestCs2HookBuildDll -RepositoryRoot $fixture
        Assert-Equal $result $null 'No build result'
        Assert-Equal @($result).Count 0 'No build pipeline stays empty'
        New-FixtureDll $staged 'existing stage' $baseTime
        $output = @(Copy-NewerCs2HookBuildDll -SourceDll $result -StagedDll $staged 6>&1)
        Assert-Equal $output.Count 0 'No build is silent'
        Assert-Dll $staged 'existing stage' $baseTime
    }
    Invoke-Case 'single source2 build' {
        New-FixtureDll $source2 'source2' $baseTime
        Assert-Equal (Get-NewestCs2HookBuildDll -RepositoryRoot $fixture) $source2 'Single source2 build'
    }
    Invoke-Case 'single legacy build' {
        New-FixtureDll $legacy 'legacy' $baseTime
        Assert-Equal (Get-NewestCs2HookBuildDll -RepositoryRoot $fixture) $legacy 'Single legacy build'
    }
    Invoke-Case 'newest across roots' {
        New-FixtureDll $source2 'source2' $baseTime
        New-FixtureDll $legacy 'legacy' ($baseTime.AddTicks(1))
        Assert-Equal (Get-NewestCs2HookBuildDll -RepositoryRoot $fixture) $legacy 'Legacy newer by one tick'
        [System.IO.File]::SetLastWriteTimeUtc($source2, $baseTime.AddTicks(2))
        Assert-Equal (Get-NewestCs2HookBuildDll -RepositoryRoot $fixture) $source2 'Source2 newer by one tick'
    }
    Invoke-Case 'equal build timestamps' {
        New-FixtureDll $source2 'source2' $baseTime
        New-FixtureDll $legacy 'legacy' $baseTime
        $result = @(Get-NewestCs2HookBuildDll -RepositoryRoot $fixture)
        Assert-Equal $result.Count 1 'Equal builds select exactly one producer'
        Assert-Equal (@($source2, $legacy) -contains $result[0]) $true 'Equal builds select an existing producer'
    }
    Invoke-Case 'ignore other build roots' {
        New-FixtureDll (Join-Path $fixture 'build/x64-source2-debug/AfxHookSource2/Release/AfxHookSource2.dll') 'debug' ($baseTime.AddDays(1))
        New-FixtureDll $source2 'release' $baseTime
        Assert-Equal (Get-NewestCs2HookBuildDll -RepositoryRoot $fixture) $source2 'Only the two release roots are producers'
    }
    Invoke-Case 'older build does not copy' {
        New-FixtureDll $source2 'older source' ($baseTime.AddTicks(-1))
        New-FixtureDll $staged 'keep stage' $baseTime
        function Copy-Item { throw 'Unexpected copy of older build' }
        Assert-Equal @(Copy-NewerCs2HookBuildDll -SourceDll $source2 -StagedDll $staged 6>&1).Count 0 'Older build is silent'
        Assert-Dll $staged 'keep stage' $baseTime
    }
    Invoke-Case 'equal build does not copy' {
        New-FixtureDll $source2 'different content' $baseTime
        New-FixtureDll $staged 'keep equal stage' $baseTime
        function Copy-Item { throw 'Unexpected copy of equal build' }
        Assert-Equal @(Copy-NewerCs2HookBuildDll -SourceDll $source2 -StagedDll $staged 6>&1).Count 0 'Equal build is silent'
        Assert-Dll $staged 'keep equal stage' $baseTime
    }
    Invoke-Case 'newer build copies' {
        $newTime = $baseTime.AddTicks(1)
        New-FixtureDll $source2 'new source content' $newTime
        New-FixtureDll $staged 'old stage content' $baseTime
        [System.IO.File]::SetAttributes($staged, [System.IO.FileAttributes]::ReadOnly)
        try {
            $output = @(Copy-NewerCs2HookBuildDll -SourceDll $source2 -StagedDll $staged 6>&1)
            Assert-Equal $output.Count 1 'Exactly one staging message'
            Assert-Equal ([string]$output[0]) $message 'Exact staging message'
            Assert-Dll $staged 'new source content' $newTime
            Assert-Dll $source2 'new source content' $newTime
        } finally {
            [System.IO.File]::SetAttributes($staged, [System.IO.FileAttributes]::Normal)
        }
    }
    Invoke-Case 'null or empty source is a no-op' {
        function Get-Item { throw 'Unexpected lookup for absent source' }
        function Copy-Item { throw 'Unexpected copy for absent source' }
        Assert-Equal @(Copy-NewerCs2HookBuildDll -SourceDll $null -StagedDll $staged 6>&1).Count 0 'Null source is silent'
        Assert-Equal @(Copy-NewerCs2HookBuildDll -SourceDll '' -StagedDll $staged 6>&1).Count 0 'Empty source is silent'
        Assert-Equal (Test-Path -LiteralPath $staged) $false 'No destination created'
    }
    Invoke-Case 'missing selected source fails' {
        New-FixtureDll $source2 'build before shutdown' ($baseTime.AddTicks(1))
        New-FixtureDll $staged 'keep stage' $baseTime
        $selected = Get-NewestCs2HookBuildDll -RepositoryRoot $fixture
        Remove-Item -LiteralPath $selected
        $caught = $null
        try { Copy-NewerCs2HookBuildDll -SourceDll $selected -StagedDll $staged } catch { $caught = $_ }
        Assert-Equal ($null -ne $caught) $true 'Disappeared source throws rather than skipping'
        Assert-Equal $caught.CategoryInfo.Category ([System.Management.Automation.ErrorCategory]::ObjectNotFound) 'Missing source error category'
        Assert-Dll $staged 'keep stage' $baseTime
    }
    Invoke-Case 'missing staged dll fails' {
        New-FixtureDll $source2 'source' $baseTime
        $caught = $null
        try { Copy-NewerCs2HookBuildDll -SourceDll $source2 -StagedDll $staged } catch { $caught = $_ }
        Assert-Equal ($null -ne $caught) $true 'Missing stage throws rather than creating it'
        Assert-Equal $caught.CategoryInfo.Category ([System.Management.Automation.ErrorCategory]::ObjectNotFound) 'Missing stage error category'
        Assert-Equal (Test-Path -LiteralPath $staged) $false 'Stage still absent'
    }
    Invoke-Case 'transient copy failure retries' {
        $newTime = $baseTime.AddTicks(1)
        New-FixtureDll $source2 'retried source' $newTime
        New-FixtureDll $staged 'old stage' $baseTime
        $copyState = @{ Attempts = 0; Sleeps = @(); Clock = $baseTime }
        function Get-Date { return $copyState.Clock }
        function Start-Sleep([int]$Milliseconds) {
            $copyState.Sleeps += $Milliseconds
            $copyState.Clock = $copyState.Clock.AddMilliseconds($Milliseconds)
        }
        function Copy-Item([string]$LiteralPath, [string]$Destination, [switch]$Force) {
            $copyState.Attempts++
            Assert-Equal $LiteralPath $source2 'Literal copy source'
            Assert-Equal $Destination $staged 'Explicit copy destination'
            Assert-Equal $Force.IsPresent $true 'Forced copy'
            if ($copyState.Attempts -le 2) { throw 'simulated file lock' }
            Microsoft.PowerShell.Management\Copy-Item -LiteralPath $LiteralPath -Destination $Destination -Force
        }
        $output = @(Copy-NewerCs2HookBuildDll -SourceDll $source2 -StagedDll $staged 6>&1)
        Assert-Equal $copyState.Attempts 3 'Retry count before success'
        Assert-Equal ($copyState.Sleeps -join ',') '250,250' 'Retry intervals'
        Assert-Equal $output.Count 1 'Success prints only once after retry'
        Assert-Equal ([string]$output[0]) $message 'Retry success message'
        Assert-Dll $staged 'retried source' $newTime
    }
    Invoke-Case 'persistent copy failure reaches deadline' {
        New-FixtureDll $source2 'locked source' ($baseTime.AddTicks(1))
        New-FixtureDll $staged 'keep stage' $baseTime
        $copyState = @{ Attempts = 0; Sleeps = @(); Messages = @(); Clock = $baseTime }
        function Get-Date { return $copyState.Clock }
        function Start-Sleep([int]$Milliseconds) {
            $copyState.Sleeps += $Milliseconds
            $copyState.Clock = $copyState.Clock.AddMilliseconds($Milliseconds)
        }
        function Copy-Item { $copyState.Attempts++; throw 'simulated permanent file lock' }
        function Write-Host([string]$Object) { $copyState.Messages += $Object }
        $caught = $null
        try { Copy-NewerCs2HookBuildDll -SourceDll $source2 -StagedDll $staged } catch { $caught = $_ }
        Assert-Equal ($null -ne $caught) $true 'Deadline propagates copy error'
        Assert-Equal $caught.Exception.Message 'simulated permanent file lock' 'Original copy error'
        Assert-Equal $copyState.Attempts 41 'One initial attempt and forty retries'
        Assert-Equal $copyState.Sleeps.Count 40 'No sleep after deadline'
        Assert-Equal @($copyState.Sleeps | Where-Object { $_ -ne 250 }).Count 0 'Every wait is 250ms'
        Assert-Equal ($copyState.Clock - $baseTime).TotalSeconds 10 'Exact ten second deadline'
        Assert-Equal $copyState.Messages.Count 0 'Failure never announces staging'
        Assert-Dll $staged 'keep stage' $baseTime
    }
    foreach ($name in @('launch-cs2-netcon', 'launch-cs2-tools')) {
        $tokens = $null
        $parseErrors = $null
        $ast = [System.Management.Automation.Language.Parser]::ParseFile(
            (Join-Path $automationRoot "launch/$name.ps1"), [ref]$tokens, [ref]$parseErrors)
        Assert-Equal $parseErrors.Count 0 "$name parse errors"
        $statements = $ast.EndBlock.Statements
        $discovery = @($statements | Where-Object { $_.Extent.Text -eq '$builtDll = Get-NewestCs2HookBuildDll -RepositoryRoot $root' })
        $shutdown = @($statements | Where-Object { $_.Extent.Text -match '^if \(\$runningCs2.Count -gt 0\)' })
        $staging = @($statements | Where-Object { $_.Extent.Text -eq 'Copy-NewerCs2HookBuildDll -SourceDll $builtDll -StagedDll $dll' })
        Assert-Equal $discovery.Count 1 "$name shared discovery call"
        Assert-Equal $shutdown.Count 1 "$name shutdown block"
        Assert-Equal $staging.Count 1 "$name shared staging call"
        Assert-Equal ($discovery[0].Extent.EndOffset -lt $shutdown[0].Extent.StartOffset) $true "$name discovers before shutdown"
        Assert-Equal ($shutdown[0].Extent.EndOffset -lt $staging[0].Extent.StartOffset) $true "$name stages after shutdown"
        $passed++
        Write-Host "PASS $name / caller ordering"
    }
    Write-Host "PASS $passed isolated hook staging cases"
} finally {
    if ($createdRoot) { Remove-Item -LiteralPath $testRoot -Recurse -Force }
}
