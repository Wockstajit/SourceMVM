param([string]$OutputDir='Automation/output/environment-shadow-detail')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'verify-environment-competitive-maps.ps1') -OutputDir $OutputDir -LibraryOnly
try {
    Send-Netcon @('getpos','mirv_filmmaker environment status') .2 |
        Set-Content -LiteralPath (Join-Path $output 'initial-state.txt')
    [void](Send-Netcon @('mvm_debug start','mirv_filmmaker environment on',
        'mirv_filmmaker environment climate enabled 1','mirv_filmmaker environment climate relighting 1',
        'mirv_filmmaker environment climate relight 0.62','mirv_filmmaker environment climate sunRelight 0.8',
        'mirv_filmmaker environment climate ambient 0.3','mirv_filmmaker environment climate sunElevation 45',
        'mirv_filmmaker environment climate sunAzimuth 135') .15)
    foreach($phase in @('unshadowed','shadowed','opposite')) {
        if($phase -eq 'opposite') {[void](Send-Netcon @('mirv_filmmaker environment climate sunAzimuth 315') .15)}
        else {[void](Send-Netcon @("mirv_filmmaker environment climate contactShadows $([int]($phase -eq 'shadowed'))") .15)}
        Start-Sleep -Seconds 2
        $capture=Capture-Frame 'de_mirage' $phase
        Write-Host $capture.path
        Send-Netcon @('mirv_filmmaker environment status','getpos') .15 |
            Set-Content -LiteralPath (Join-Path $output "$phase-state.txt")
    }
} finally {
    [void](Send-Netcon @('mirv_streams record end','mirv_filmmaker environment off','mvm_debug stop') .15)
}
